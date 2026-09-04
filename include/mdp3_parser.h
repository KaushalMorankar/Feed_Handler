#ifndef MDP3_PARSER_H
#define MDP3_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include "mdp3_proto.h"
#include "seq_tracker.h"

/* Flattened, already-decoded message handed to the processing thread.
 * This is what actually travels through the SPSC ring -- fixed size,
 * no pointers into the original packet buffer (which may be a UMEM
 * frame we're about to recycle back to the NIC). */
typedef struct {
    uint32_t packet_seq;
    uint64_t sending_time_ns;
    uint16_t template_id;
    seq_result_t seq_result;

    /* valid when template_id == TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK */
    mdp3_incremental_refresh_t book_update;
} decoded_message_t;

/* Parse one UDP datagram payload (len bytes at buf) belonging to the
 * given channel. For every SBE message found, invoke `on_message` with
 * a fully decoded_message_t. Returns the number of messages decoded,
 * or -1 on a malformed packet (truncated header/body -- these are
 * dropped and counted by the caller, never trusted). */
typedef void (*decoded_message_cb)(const decoded_message_t *msg, void *user_ctx);

int mdp3_parse_packet(const uint8_t *buf, size_t len, seq_tracker_t *tracker,
                       uint64_t now_ns, decoded_message_cb on_message, void *user_ctx);

#endif /* MDP3_PARSER_H */
