#include "afxdp_socket.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <net/if.h>

/* atomics on the shared producer/consumer indices: the kernel and this
 * process both touch these words across the mmap, so treat them like
 * any other cross-thread shared state */
#include <stdatomic.h>

static uint32_t ring_load_u32(uint32_t *p) {
    return atomic_load_explicit((_Atomic uint32_t *)p, memory_order_acquire);
}
static void ring_store_u32(uint32_t *p, uint32_t v) {
    atomic_store_explicit((_Atomic uint32_t *)p, v, memory_order_release);
}

static int mmap_ring(int fd, xsk_ring_t *r, struct xdp_ring_offset *off,
                      uint32_t num_entries, size_t desc_size, __u64 pgoff) {
    size_t map_size = off->desc + (size_t)num_entries * desc_size;
    void *m = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_POPULATE, fd, (off_t)pgoff);
    if (m == MAP_FAILED) {
        perror("mmap ring");
        return -1;
    }
    r->map = m;
    r->map_size = map_size;
    r->producer = (uint32_t *)((uint8_t *)m + off->producer);
    r->consumer = (uint32_t *)((uint8_t *)m + off->consumer);
    r->flags    = (uint32_t *)((uint8_t *)m + off->flags);
    r->ring     = (uint8_t *)m + off->desc;
    r->size = num_entries;
    r->cached_prod = 0;
    r->cached_cons = 0;
    return 0;
}

int xsk_socket_create(xsk_socket_t *xsk, const char *ifname, uint32_t queue_id, bool zerocopy) {
    memset(xsk, 0, sizeof(*xsk));

    xsk->ifindex = if_nametoindex(ifname);
    if (xsk->ifindex == 0) {
        fprintf(stderr, "afxdp: unknown interface '%s': %s\n", ifname, strerror(errno));
        return -1;
    }
    xsk->queue_id = queue_id;

    /* ---- 1. UMEM: anonymous mmap, the DMA target for the NIC ---- */
    xsk->umem_size = XSK_UMEM_SIZE;
    xsk->umem_area = mmap(NULL, xsk->umem_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (xsk->umem_area == MAP_FAILED) {
        perror("afxdp: mmap umem");
        return -1;
    }

    /* seed free list with every frame offset */
    for (uint32_t i = 0; i < XSK_NUM_FRAMES; i++) {
        xsk->free_frames[i] = (uint64_t)i * XSK_FRAME_SIZE;
    }
    xsk->free_frame_count = XSK_NUM_FRAMES;

    /* ---- 2. AF_XDP socket ---- */
    xsk->fd = socket(AF_XDP, SOCK_RAW, 0);
    if (xsk->fd < 0) {
        perror("afxdp: socket(AF_XDP) -- needs root/CAP_NET_RAW and a kernel with "
               "CONFIG_XDP_SOCKETS");
        munmap(xsk->umem_area, xsk->umem_size);
        return -1;
    }

    /* ---- 3. register UMEM ---- */
    struct xdp_umem_reg umem_reg;
    memset(&umem_reg, 0, sizeof(umem_reg));
    umem_reg.addr = (uint64_t)(uintptr_t)xsk->umem_area;
    umem_reg.len = xsk->umem_size;
    umem_reg.chunk_size = XSK_FRAME_SIZE;
    umem_reg.headroom = 0;

    if (setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof(umem_reg)) < 0) {
        perror("afxdp: setsockopt(XDP_UMEM_REG)");
        goto fail;
    }

    /* ---- 4. size the four rings ---- */
    uint32_t ring_size = XSK_RING_SIZE;
    if (setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_FILL_RING, &ring_size, sizeof(ring_size)) < 0 ||
        setsockopt(xsk->fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_size, sizeof(ring_size)) < 0 ||
        setsockopt(xsk->fd, SOL_XDP, XDP_RX_RING, &ring_size, sizeof(ring_size)) < 0 ||
        setsockopt(xsk->fd, SOL_XDP, XDP_TX_RING, &ring_size, sizeof(ring_size)) < 0) {
        perror("afxdp: setsockopt(ring sizes)");
        goto fail;
    }

    /* ---- 5. fetch mmap offsets and map all four rings ---- */
    struct xdp_mmap_offsets off;
    socklen_t off_len = sizeof(off);
    if (getsockopt(xsk->fd, SOL_XDP, XDP_MMAP_OFFSETS, &off, &off_len) < 0) {
        perror("afxdp: getsockopt(XDP_MMAP_OFFSETS)");
        goto fail;
    }

    if (mmap_ring(xsk->fd, &xsk->fill_ring, &off.fr, ring_size, sizeof(uint64_t),
                  XDP_UMEM_PGOFF_FILL_RING) < 0) goto fail;
    if (mmap_ring(xsk->fd, &xsk->comp_ring, &off.cr, ring_size, sizeof(uint64_t),
                  XDP_UMEM_PGOFF_COMPLETION_RING) < 0) goto fail;
    if (mmap_ring(xsk->fd, &xsk->rx_ring, &off.rx, ring_size, sizeof(struct xdp_desc),
                  XDP_PGOFF_RX_RING) < 0) goto fail;
    if (mmap_ring(xsk->fd, &xsk->tx_ring, &off.tx, ring_size, sizeof(struct xdp_desc),
                  XDP_PGOFF_TX_RING) < 0) goto fail;

    /* ---- 6. bind to (ifindex, queue_id) ---- */
    struct sockaddr_xdp sxdp;
    memset(&sxdp, 0, sizeof(sxdp));
    sxdp.sxdp_family = AF_XDP;
    sxdp.sxdp_ifindex = xsk->ifindex;
    sxdp.sxdp_queue_id = queue_id;
    sxdp.sxdp_flags = (uint16_t)(XDP_USE_NEED_WAKEUP | (zerocopy ? XDP_ZEROCOPY : XDP_COPY));

    if (bind(xsk->fd, (struct sockaddr *)&sxdp, sizeof(sxdp)) < 0) {
        perror("afxdp: bind() -- if this is ENOTSUP/EOPNOTSUPP the NIC driver likely "
               "lacks zerocopy XDP support; retry with zerocopy=false for generic/SKB mode");
        goto fail;
    }

    return 0;

fail:
    close(xsk->fd);
    munmap(xsk->umem_area, xsk->umem_size);
    return -1;
}

void xsk_populate_fill_ring(xsk_socket_t *xsk) {
    xsk_ring_t *fr = &xsk->fill_ring;
    uint32_t prod = ring_load_u32(fr->producer);
    uint64_t *descs = (uint64_t *)fr->ring;

    uint32_t to_post = xsk->free_frame_count;
    if (to_post > fr->size) to_post = fr->size;

    for (uint32_t i = 0; i < to_post; i++) {
        uint64_t frame = xsk->free_frames[--xsk->free_frame_count];
        descs[(prod + i) & (fr->size - 1)] = frame;
    }
    if (to_post > 0) {
        ring_store_u32(fr->producer, prod + to_post);
    }
}

uint32_t xsk_rx_poll(xsk_socket_t *xsk, struct xdp_desc *out_descs, uint32_t max_descs) {
    xsk_ring_t *rx = &xsk->rx_ring;
    uint32_t prod = ring_load_u32(rx->producer);
    uint32_t cons = ring_load_u32(rx->consumer);
    uint32_t avail = prod - cons;
    if (avail == 0) return 0;
    if (avail > max_descs) avail = max_descs;

    struct xdp_desc *ring = (struct xdp_desc *)rx->ring;
    for (uint32_t i = 0; i < avail; i++) {
        out_descs[i] = ring[(cons + i) & (rx->size - 1)];
    }
    ring_store_u32(rx->consumer, cons + avail);
    return avail;
}

void xsk_release_frame(xsk_socket_t *xsk, uint64_t addr) {
    /* frame addr may include a chunk offset if XDP_UMEM_UNALIGNED_CHUNK_FLAG
     * were used; we don't use that mode, so addr is already frame-aligned */
    if (xsk->free_frame_count < XSK_NUM_FRAMES) {
        xsk->free_frames[xsk->free_frame_count++] = addr - (addr % XSK_FRAME_SIZE);
    }
}

void xsk_socket_destroy(xsk_socket_t *xsk) {
    if (xsk->fd >= 0) close(xsk->fd);
    if (xsk->fill_ring.map) munmap(xsk->fill_ring.map, xsk->fill_ring.map_size);
    if (xsk->comp_ring.map) munmap(xsk->comp_ring.map, xsk->comp_ring.map_size);
    if (xsk->rx_ring.map) munmap(xsk->rx_ring.map, xsk->rx_ring.map_size);
    if (xsk->tx_ring.map) munmap(xsk->tx_ring.map, xsk->tx_ring.map_size);
    if (xsk->umem_area) munmap(xsk->umem_area, xsk->umem_size);
}
