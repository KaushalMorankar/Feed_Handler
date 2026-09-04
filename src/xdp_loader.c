/*
 * xdp_loader.c
 *
 * Loads xdp_kern.o (the compiled BPF object, see xdp_kern.c), attaches
 * it to the given interface's XDP hook, and wires the XSKMAP entry for
 * `queue_id` to point at an already-created AF_XDP socket fd (from
 * afxdp_socket.c). This is the one piece of this project that genuinely
 * needs libbpf -- loading and verifying raw BPF bytecode by hand isn't
 * something worth reinventing; libbpf's ELF loader + verifier
 * invocation is the standard, correct way to do it.
 *
 * Requires: libbpf-dev, and xdp_kern.o already built with:
 *   clang -O2 -g -target bpf -c xdp_kern.c -o xdp_kern.o
 *
 * Link with: -lbpf
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <net/if.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include "xdp_loader.h"

struct xdp_loader_ctx {
    struct bpf_object *obj;
    struct bpf_program *prog;
    int prog_fd;
    int xsks_map_fd;
    int ifindex;
};

static struct xdp_loader_ctx g_ctx;

int xdp_loader_attach(const char *ifname, const char *bpf_obj_path, bool skb_mode) {
    memset(&g_ctx, 0, sizeof(g_ctx));

    g_ctx.ifindex = if_nametoindex(ifname);
    if (g_ctx.ifindex == 0) {
        fprintf(stderr, "xdp_loader: unknown interface '%s'\n", ifname);
        return -1;
    }

    g_ctx.obj = bpf_object__open_file(bpf_obj_path, NULL);
    if (libbpf_get_error(g_ctx.obj)) {
        fprintf(stderr, "xdp_loader: failed to open %s\n", bpf_obj_path);
        return -1;
    }

    if (bpf_object__load(g_ctx.obj)) {
        fprintf(stderr, "xdp_loader: verifier rejected program (check dmesg / "
                         "libbpf debug log)\n");
        return -1;
    }

    g_ctx.prog = bpf_object__find_program_by_name(g_ctx.obj, "xdp_market_data_redirect");
    if (!g_ctx.prog) {
        fprintf(stderr, "xdp_loader: program 'xdp_market_data_redirect' not found in object\n");
        return -1;
    }
    g_ctx.prog_fd = bpf_program__fd(g_ctx.prog);

    struct bpf_map *xsks_map = bpf_object__find_map_by_name(g_ctx.obj, "xsks_map");
    if (!xsks_map) {
        fprintf(stderr, "xdp_loader: map 'xsks_map' not found in object\n");
        return -1;
    }
    g_ctx.xsks_map_fd = bpf_map__fd(xsks_map);

    /* Attach flags: prefer native (driver) XDP for zero-copy; fall back
     * to generic/SKB mode (works on any NIC driver, but the kernel still
     * allocates an skb first -- useful for testing on hardware/drivers
     * without native XDP support, at the cost of the copy we're trying
     * to avoid). */
    unsigned int flags = skb_mode ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;

    int err = bpf_xdp_attach(g_ctx.ifindex, g_ctx.prog_fd, flags, NULL);
    if (err) {
        fprintf(stderr, "xdp_loader: bpf_xdp_attach failed (%d): %s. If this is a "
                         "driver that lacks native XDP support, retry with skb_mode=true.\n",
                         err, strerror(-err));
        return -1;
    }

    return 0;
}

int xdp_loader_bind_queue(uint32_t queue_id, int xsk_fd) {
    int key = (int)queue_id;
    if (bpf_map_update_elem(g_ctx.xsks_map_fd, &key, &xsk_fd, BPF_ANY)) {
        fprintf(stderr, "xdp_loader: failed to update xsks_map[%u] = fd %d: %s\n",
                queue_id, xsk_fd, strerror(errno));
        return -1;
    }
    return 0;
}

void xdp_loader_detach(void) {
    if (g_ctx.ifindex) {
        bpf_xdp_attach(g_ctx.ifindex, -1, XDP_FLAGS_UPDATE_IF_NOEXIST, NULL);
    }
    if (g_ctx.obj) {
        bpf_object__close(g_ctx.obj);
    }
    memset(&g_ctx, 0, sizeof(g_ctx));
}
