/*
 * afxdp_socket.h
 *
 * Hand-rolled AF_XDP socket + UMEM setup, talking directly to the
 * kernel via <linux/if_xdp.h> and raw socket()/setsockopt()/mmap()/
 * bind() calls -- deliberately NOT using libxdp's xsk.h wrapper, so
 * this compiles and is auditable with nothing but glibc + kernel
 * UAPI headers. (The eBPF *redirect* program that steers packets into
 * this socket's fill/RX rings still needs to be loaded separately --
 * see xdp_kern.c and xdp_loader.c, which do use libbpf, since loading
 * verified BPF bytecode by hand is not something worth reinventing.)
 *
 * Zero-copy data path summary:
 *   1. UMEM: one big mmap'd region, carved into fixed-size frames.
 *      This is the *only* memory packets ever live in on the RX path --
 *      the NIC DMAs directly into UMEM frames, so there is no
 *      kernel-space skb allocation and no copy into a socket buffer.
 *   2. Fill ring: userspace tells the kernel "here are UMEM frame
 *      addresses you may DMA into next."
 *   3. RX ring: kernel tells userspace "here's a descriptor (addr+len)
 *      for a frame that just arrived."
 *   4. Completion ring / TX ring: symmetric machinery for the transmit
 *      side. This feed handler is receive-only, so TX/completion rings
 *      are set up (the kernel requires valid ring registrations) but
 *      unused in the hot path.
 */

#ifndef AFXDP_SOCKET_H
#define AFXDP_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <linux/if_xdp.h>

#define XSK_RING_SIZE       2048     /* entries per ring, must be power of 2 */
#define XSK_FRAME_SIZE       4096     /* must match NIC page/frame size, typically 4096 */
#define XSK_NUM_FRAMES       4096     /* total UMEM frames */
#define XSK_UMEM_SIZE        ((size_t)XSK_FRAME_SIZE * XSK_NUM_FRAMES)

/* Thin, explicit wrapper around one mmap'd ring (RX, TX, fill, or
 * completion). We track producer/consumer cache pointers ourselves
 * rather than pulling in libxdp's xsk_ring_prod/xsk_ring_cons -- same
 * idea, no extra dependency. */
typedef struct {
    void *map;              /* mmap'd base */
    size_t map_size;
    uint32_t *producer;     /* -> shared producer index */
    uint32_t *consumer;     /* -> shared consumer index */
    uint32_t *flags;
    void *ring;              /* -> start of descriptor array */
    uint32_t cached_prod;
    uint32_t cached_cons;
    uint32_t size;           /* ring capacity (entries) */
} xsk_ring_t;

typedef struct {
    int fd;                  /* AF_XDP socket fd */
    void *umem_area;         /* mmap'd UMEM backing memory */
    size_t umem_size;

    xsk_ring_t fill_ring;    /* uint64_t frame addr descriptors */
    xsk_ring_t comp_ring;    /* uint64_t frame addr descriptors */
    xsk_ring_t rx_ring;      /* struct xdp_desc descriptors */
    xsk_ring_t tx_ring;      /* struct xdp_desc descriptors (unused, RX-only handler) */

    uint32_t ifindex;
    uint32_t queue_id;

    /* simple free-list of UMEM frame offsets available to hand to the
     * fill ring; avoids needing a general allocator on the hot path */
    uint64_t free_frames[XSK_NUM_FRAMES];
    uint32_t free_frame_count;
} xsk_socket_t;

/* Create the UMEM, the AF_XDP socket, register rings, and bind to
 * (ifname, queue_id). Does NOT load/attach the eBPF redirect program
 * or populate the XSKMAP -- that's xdp_loader's job, since a socket
 * only receives packets once traffic is actually being redirected to
 * it at the XDP layer. Returns 0 on success. */
int xsk_socket_create(xsk_socket_t *xsk, const char *ifname, uint32_t queue_id,
                       bool zerocopy);

/* Push every currently-free UMEM frame onto the fill ring so the NIC
 * has somewhere to DMA incoming packets. Call once after bind, and
 * again after recycling frames post-processing. */
void xsk_populate_fill_ring(xsk_socket_t *xsk);

/* Poll the RX ring for newly arrived descriptors. Writes up to
 * max_descs entries into out_descs, returns the count (0 if none
 * ready). Caller must release each frame back to the fill ring via
 * xsk_release_frame() once done reading it, or the UMEM will starve. */
uint32_t xsk_rx_poll(xsk_socket_t *xsk, struct xdp_desc *out_descs, uint32_t max_descs);

/* Return a UMEM frame (by its addr, as reported in an xdp_desc) to the
 * free list so it can be re-posted to the fill ring. */
void xsk_release_frame(xsk_socket_t *xsk, uint64_t addr);

/* Convert a descriptor's addr into a real pointer into UMEM. */
static inline uint8_t *xsk_umem_ptr(xsk_socket_t *xsk, uint64_t addr) {
    return (uint8_t *)xsk->umem_area + addr;
}

void xsk_socket_destroy(xsk_socket_t *xsk);

#endif /* AFXDP_SOCKET_H */
