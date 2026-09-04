#include "mdp3_parser.h"
#include <string.h>

int mdp3_parse_packet(const uint8_t *buf, size_t len, seq_tracker_t *tracker,
                       uint64_t now_ns, decoded_message_cb on_message, void *user_ctx) {
    if (len < MDP3_PACKET_HEADER_LEN) {
        return -1; /* truncated packet, can't even read the header */
    }

    mdp3_packet_header_t pkt_hdr;
    memcpy(&pkt_hdr, buf, MDP3_PACKET_HEADER_LEN);

    seq_result_t seq_result = seq_tracker_observe(tracker, pkt_hdr.sequence_number, now_ns);

    size_t offset = MDP3_PACKET_HEADER_LEN;
    int msg_count = 0;

    while (offset + SBE_HEADER_LEN <= len) {
        sbe_message_header_t sbe_hdr;
        memcpy(&sbe_hdr, buf + offset, SBE_HEADER_LEN);
        offset += SBE_HEADER_LEN;

        if (offset + sbe_hdr.block_length > len) {
            /* body would run past the datagram -- malformed/truncated,
             * stop parsing this packet rather than read out of bounds */
            return msg_count > 0 ? msg_count : -1;
        }

        decoded_message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.packet_seq = pkt_hdr.sequence_number;
        msg.sending_time_ns = pkt_hdr.sending_time_ns;
        msg.template_id = sbe_hdr.template_id;
        msg.seq_result = seq_result;

        switch (sbe_hdr.template_id) {
            case TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK:
                if (sbe_hdr.block_length >= sizeof(mdp3_incremental_refresh_t)) {
                    memcpy(&msg.book_update, buf + offset, sizeof(mdp3_incremental_refresh_t));
                }
                break;
            case TEMPLATE_ID_CHANNEL_RESET:
                seq_tracker_reset(tracker);
                break;
            case TEMPLATE_ID_HEARTBEAT:
            default:
                /* nothing to decode; header already advanced offset */
                break;
        }

        if (on_message) {
            on_message(&msg, user_ctx);
        }

        offset += sbe_hdr.block_length;
        msg_count++;
    }

    return msg_count;
}
