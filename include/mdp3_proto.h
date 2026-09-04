/*
 * mdp3_proto.h
 *
 * Binary wire structures modeled on CME Group's publicly documented
 * MDP 3.0 market data protocol (Simple Binary Encoding / SBE framing).
 * This is a deliberately simplified re-implementation for demonstration
 * purposes -- field widths and header layout mirror the public spec,
 * but the message catalog is trimmed to what's needed to show a
 * realistic parse -> gap-detect -> orderbook-update pipeline.
 *
 * Every multicast UDP datagram on the wire looks like:
 *
 *   [ PacketHeader (12 bytes) ][ Msg1 ][ Msg2 ] ... [ MsgN ]
 *
 * Each Msg is itself:
 *
 *   [ SbeMessageHeader (8 bytes) ][ message body (BlockLength bytes) ]
 *
 * A single UDP packet can (and routinely does) carry multiple SBE
 * messages -- this is why simple "1 packet = 1 update" assumptions
 * break, and why the reassembly/parsing loop below has to walk the
 * packet using BlockLength rather than trusting datagram boundaries.
 */

#ifndef MDP3_PROTO_H
#define MDP3_PROTO_H

#include <stdint.h>

#pragma pack(push, 1)

/* ---- Packet-level header (once per UDP datagram) ---------------- */
typedef struct {
    uint32_t sequence_number;  /* monotonically increasing per channel   */
    uint64_t sending_time_ns;  /* exchange send timestamp, ns since epoch */
} mdp3_packet_header_t;

#define MDP3_PACKET_HEADER_LEN (sizeof(mdp3_packet_header_t))

/* ---- SBE message header (once per message within the packet) ---- */
typedef struct {
    uint16_t block_length;  /* length of the fixed-size message body   */
    uint16_t template_id;   /* identifies the message schema           */
    uint16_t schema_id;     /* identifies the overall schema           */
    uint16_t version;       /* schema version                          */
} sbe_message_header_t;

#define SBE_HEADER_LEN (sizeof(sbe_message_header_t))

/* ---- Template IDs we care about ---------------------------------- */
#define TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK   32
#define TEMPLATE_ID_CHANNEL_RESET                 4
#define TEMPLATE_ID_HEARTBEAT                     12

enum md_update_action {
    MD_ACTION_NEW    = 0,
    MD_ACTION_CHANGE = 1,
    MD_ACTION_DELETE = 2,
};

enum md_side {
    MD_SIDE_BID = 0,
    MD_SIDE_ASK = 1,
};

/* Simplified incremental book update (template 32). Real MDP3 wraps
 * these in a repeating group with NumInGroup; we flatten to a single
 * entry per message for clarity -- the parser below still walks
 * multiple such messages per packet correctly. */
typedef struct {
    int64_t  md_entry_px;     /* fixed-point price, scaled by 1e-9      */
    int32_t  md_entry_size;   /* quantity                               */
    int32_t  security_id;     /* instrument id                          */
    uint8_t  md_update_action;/* enum md_update_action                  */
    uint8_t  md_price_level;  /* 1 = top of book, etc.                  */
    uint8_t  side;            /* enum md_side                           */
    uint8_t  _pad;
} mdp3_incremental_refresh_t;

/* Channel Reset (template 4): tells us to reset expected sequence to 1 */
typedef struct {
    uint32_t reset_channel_id;
} mdp3_channel_reset_t;

#pragma pack(pop)

#endif /* MDP3_PROTO_H */
