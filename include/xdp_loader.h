#ifndef XDP_LOADER_H
#define XDP_LOADER_H

#include <stdint.h>
#include <stdbool.h>

/* Optional flow filter, applied by xdp_loader_attach() before the program
 * goes live. group_be = IPv4 destination in network byte order (0 = any),
 * port = UDP destination port (host order), vlan_id = outer 802.1Q VID
 * to require (0 = don't care). Defaults: any group, port 15000, any VLAN. */
void xdp_loader_set_filter(uint32_t group_be, uint16_t port, uint16_t vlan_id);

/* Load bpf_obj_path (compiled from xdp_kern.c), attach to ifname's XDP
 * hook. skb_mode=true uses generic XDP (works everywhere, no zero-copy);
 * false requests native driver XDP (required for true zero-copy). */
int xdp_loader_attach(const char *ifname, const char *bpf_obj_path, bool skb_mode);

/* Point the XSKMAP slot for queue_id at an AF_XDP socket fd, so packets
 * the kernel program redirects for that queue land in that socket. */
int xdp_loader_bind_queue(uint32_t queue_id, int xsk_fd);

/* Detach the program from the interface and release libbpf resources. */
void xdp_loader_detach(void);

#endif /* XDP_LOADER_H */
