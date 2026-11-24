/*
 * xdp_kern.c
 *
 * eBPF program that runs in the NIC driver's XDP hook, i.e. before the
 * kernel builds an sk_buff or does any routing/socket lookup. This is
 * the actual "kernel bypass" mechanism: for packets we care about, we
 * never enter the normal Linux network stack at all.
 *
 * Logic per packet:
 *   1. Parse Ethernet (+ up to two VLAN tags: 802.1Q / 802.1ad) -> IPv4
 *      -> UDP directly out of the frame (bounds-checked against data_end
 *      at every step, or the BPF verifier will reject the program).
 *   2. Match the flow: UDP destination port, and -- if configured --
 *      the IPv4 destination (the multicast group) and the outer VLAN id.
 *      Matching packets are XDP_REDIRECTed into `xsks_map`, an XSKMAP
 *      keyed by RX queue index; whichever AF_XDP socket is bound to that
 *      queue receives the frame with no skb and no UDP/IP stack.
 *   3. Anything else: XDP_PASS, so the box keeps working as a normal
 *      Linux host for all other traffic (SSH, the recovery TCP
 *      connection, ...) -- only the one flow is bypassed.
 *
 * The match parameters live in the `xdp_cfg` map (written by the loader),
 * so the same object serves any group/port/VLAN without recompiling.
 *
 * Build:  clang -O2 -g -target bpf -Iinclude -c src/xdp_kern.c -o xdp_kern.o
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "xdp_cfg.h"

/* Fallback if the config map is somehow unreadable. */
#define DEFAULT_UDP_PORT 15000

struct vlan_hdr {
    __be16 tci;
    __be16 encap_proto;
};

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64); /* one slot per possible RX queue */
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct xdp_cfg);
} xdp_cfg SEC(".maps");

/* Per-CPU counters for monitoring redirect hit/miss rates. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, XDP_CNT_MAX);
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

    __u32 zero = 0;
    struct xdp_cfg *cfg = bpf_map_lookup_elem(&xdp_cfg, &zero);
    __u32 want_group = cfg ? cfg->group_be : 0;
    __u16 want_port  = cfg ? cfg->port_be : bpf_htons(DEFAULT_UDP_PORT);
    __u16 want_vlan  = cfg ? cfg->vlan_id : 0;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    __be16 proto = eth->h_proto;
    void *cur = (void *)(eth + 1);
    __u16 outer_vid = 0;

    /* up to two stacked VLAN tags (QinQ); bounded so the verifier is happy */
#pragma unroll
    for (int i = 0; i < 2; i++) {
        if (proto == bpf_htons(ETH_P_8021Q) || proto == bpf_htons(ETH_P_8021AD)) {
            struct vlan_hdr *vh = cur;
            if ((void *)(vh + 1) > data_end)
                return XDP_PASS;
            if (i == 0)
                outer_vid = bpf_ntohs(vh->tci) & 0x0FFF;
            proto = vh->encap_proto;
            cur = (void *)(vh + 1);
        }
    }

    if (proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *ip = cur;
    if ((void *)(ip + 1) > data_end)
        return XDP_PASS;

    if (ip->protocol != IPPROTO_UDP)
        return XDP_PASS;

    /* ip->ihl is in 32-bit words; handle IP options rather than
     * assuming a bare 20-byte header */
    __u32 ip_hdr_len = ip->ihl * 4;
    if (ip_hdr_len < sizeof(struct iphdr))
        return XDP_PASS;

    struct udphdr *udp = (void *)ip + ip_hdr_len;
    if ((void *)(udp + 1) > data_end)
        return XDP_PASS;

    if (udp->dest != want_port) {
        bump(XDP_CNT_WRONG_PORT);
        return XDP_PASS;
    }

    /* multicast group and VLAN scoping (0 = don't care) */
    if ((want_group && ip->daddr != want_group) ||
        (want_vlan && outer_vid != want_vlan)) {
        bump(XDP_CNT_WRONG_GROUP_OR_VLAN);
        return XDP_PASS;
    }

    bump(XDP_CNT_REDIRECTED);
    /* Redirect into the XSKMAP slot for this RX queue; XDP_PASS as the
     * fallback keeps us safe if no AF_XDP socket is bound to the queue. */
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
