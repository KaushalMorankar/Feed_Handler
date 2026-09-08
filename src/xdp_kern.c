/*
 * xdp_kern.c
 *
 * eBPF program that runs in the NIC driver's XDP hook, i.e. before the
 * kernel builds an sk_buff or does any routing/socket lookup. This is
 * the actual "kernel bypass" mechanism: for packets we care about, we
 * never enter the normal Linux network stack at all.
 *
 * Logic per packet:
 *   1. Parse Ethernet -> IP -> UDP headers directly out of the frame
 *      (bounds-checked against data_end on every step, or the BPF
 *      verifier will reject the program).
 *   2. If it's UDP on our market-data multicast port, XDP_REDIRECT it
 *      into `xsks_map`, an XSKMAP keyed by RX queue index. Whichever
 *      AF_XDP socket is bound to that queue receives the frame
 *      zero-copy -- no skb allocation, no copy to a socket buffer, no
 *      involvement of the normal UDP/IP stack.
 *   3. Anything else: XDP_PASS, so the box keeps working as a normal
 *      Linux host for all other traffic (SSH, the recovery-feed TCP
 *      connection, etc.) -- we only bypass the stack for the specific
 *      flow we're optimizing.
 *
 * Build (on a box with clang/llvm + libbpf-dev + kernel headers):
 *   clang -O2 -g -target bpf -c xdp_kern.c -o xdp_kern.o
 *
 * Load with xdp_loader (uses libbpf) or `ip link set dev <if> xdp obj
 * xdp_kern.o sec xdp` / xdp-loader from the xdp-tools project.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

/* Must match the port the market data multicast feed publishes on;
 * set at build time or patched via bpf_program__set_attach_target /
 * a BPF global variable in a fancier version of this loader. */
#define MARKET_DATA_UDP_PORT 15000

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64); /* one slot per possible RX queue */
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

/* Per-queue counters, exposed to userspace for monitoring redirect
 * hit/miss rates without needing to touch the hot path's return value. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 2); /* 0 = redirected, 1 = passed */
    __type(key, __u32);
    __type(value, __u64);
} counters SEC(".maps");

static __always_inline void bump(__u32 idx) {
    __u64 *c = bpf_map_lookup_elem(&counters, &idx);
    if (c) __sync_fetch_and_add(c, 1);
}

SEC("xdp")
int xdp_market_data_redirect(struct xdp_md *ctx) {
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    if (ip->protocol != IPPROTO_UDP)
        return XDP_PASS;

    /* ip->ihl is in 32-bit words; handle IP options correctly rather
     * than assuming a bare 20-byte header */
    __u32 ip_hdr_len = ip->ihl * 4;
    if (ip_hdr_len < sizeof(struct iphdr))
        return XDP_PASS;

    struct udphdr *udp = (void *)ip + ip_hdr_len;
    if ((void *)(udp + 1) > data_end)
        return XDP_PASS;

    if (udp->dest != bpf_htons(MARKET_DATA_UDP_PORT)) {
        bump(1);
        return XDP_PASS;
    }

    bump(0);
    /* Redirect into the XSKMAP slot for this RX queue. bpf_redirect_map
     * returns XDP_REDIRECT on success or falls through per the map's
     * configured flags; XDP_PASS as the fallback keeps us safe if no
     * AF_XDP socket is currently bound to this queue. */
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
