/* Filter configuration shared by the eBPF program (xdp_kern.c) and the
 * loader (xdp_loader.c). Written into the one-entry `xdp_cfg` array map. */
#ifndef XDP_CFG_H
#define XDP_CFG_H

#include <linux/types.h>

struct xdp_cfg {
    __u32 group_be;   /* IPv4 destination to match, network byte order; 0 = any */
    __u16 port_be;    /* UDP destination port, network byte order               */
    __u16 vlan_id;    /* outer 802.1Q VID to require; 0 = don't care            */
};

enum {
    XDP_CNT_REDIRECTED = 0,
    XDP_CNT_WRONG_PORT = 1,
    XDP_CNT_WRONG_GROUP_OR_VLAN = 2,
    XDP_CNT_MAX = 4,
};

#endif /* XDP_CFG_H */
