/*
 * recovery_proto.h
 *
 * Wire format of the TCP snapshot-recovery side channel, shared by the
 * feed handler (client) and packet_gen (the stand-in "exchange" server).
 *
 * This mirrors how real MDP3 recovery works: the fast multicast feed is
 * unreliable, so when a gap is detected the handler fetches a full book
 * snapshot over a separate, reliable transport and re-synchronises.
 *
 *   client -> server : 1 byte  'S'
 *   server -> client : snapshot_header_t, then header.count entries
 *
 * header.last_seq is the sequence number of the last incremental already
 * reflected in the snapshot. The client discards buffered live updates
 * with seq <= last_seq and replays the rest on top of the snapshot.
 */
#ifndef RECOVERY_PROTO_H
#define RECOVERY_PROTO_H

#include <stdint.h>

#pragma pack(push, 1)
typedef struct {
    uint32_t last_seq;
    uint32_t count;
} snapshot_header_t;

typedef struct {
    int32_t  security_id;
    int32_t  size;
    int64_t  px;
    uint8_t  side;    /* enum md_side */
    uint8_t  level;   /* 1-based */
    uint8_t  _pad[2];
} snapshot_entry_t;
#pragma pack(pop)

#define SNAP_MAX_ENTRIES 1280  /* 64 instruments x 2 sides x 10 levels */

#endif /* RECOVERY_PROTO_H */
