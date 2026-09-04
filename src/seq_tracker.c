#include "seq_tracker.h"
#include <stdio.h>
#include <string.h>

void seq_tracker_init(seq_tracker_t *t, uint32_t channel_id) {
    memset(t, 0, sizeof(*t));
    t->channel_id = channel_id;
    t->expected_seq = 1; /* MDP3 channels start sequencing at 1 */
    t->initialized = false;
}

void seq_tracker_reset(seq_tracker_t *t) {
    t->expected_seq = 1;
    t->initialized = false;
    t->channel_resets++;
}

static void enqueue_recovery_request(seq_tracker_t *t, uint32_t begin_seq,
                                      uint32_t end_seq, uint64_t now_ns) {
    if (t->pending_count >= MAX_PENDING_RECOVERY_REQUESTS) {
        /* Recovery is falling behind the gap rate. In production this
         * increments an alarm-worthy stat; here we just refuse to grow
         * unbounded and drop the request (it's still counted in
         * total_missing_messages). */
        return;
    }
    recovery_request_t *req = &t->pending[t->pending_count++];
    req->begin_seq = begin_seq;
    req->end_seq = end_seq;
    req->detected_at_ns = now_ns;
}

seq_result_t seq_tracker_observe(seq_tracker_t *t, uint32_t seq, uint64_t now_ns) {
    t->packets_received++;

    if (!t->initialized) {
        t->initialized = true;
        t->expected_seq = seq + 1;
        return SEQ_RESULT_OK;
    }

    if (seq < t->expected_seq) {
        /* stale/duplicate: multicast retransmit, or we already recovered
         * this range. Not an error condition on its own. */
        t->packets_duplicate_or_stale++;
        return SEQ_RESULT_DUPLICATE;
    }

    if (seq == t->expected_seq) {
        t->expected_seq = seq + 1;
        return SEQ_RESULT_OK;
    }

    /* seq > expected_seq: gap. [expected_seq, seq-1] is missing. */
    uint32_t missing_begin = t->expected_seq;
    uint32_t missing_end = seq - 1;
    uint32_t missing_count = missing_end - missing_begin + 1;

    t->packets_out_of_order_gap++;
    t->total_missing_messages += missing_count;
    enqueue_recovery_request(t, missing_begin, missing_end, now_ns);

    t->expected_seq = seq + 1;
    return SEQ_RESULT_GAP_DETECTED;
}

bool seq_tracker_pop_recovery_request(seq_tracker_t *t, recovery_request_t *out) {
    if (t->pending_count == 0) return false;
    *out = t->pending[0];
    memmove(&t->pending[0], &t->pending[1],
            (t->pending_count - 1) * sizeof(recovery_request_t));
    t->pending_count--;
    return true;
}

void seq_tracker_print_stats(const seq_tracker_t *t) {
    fprintf(stderr,
        "[channel %u] rx=%lu dup/stale=%lu gaps=%lu missing_msgs=%lu resets=%lu expected_next=%u\n",
        t->channel_id,
        (unsigned long)t->packets_received,
        (unsigned long)t->packets_duplicate_or_stale,
        (unsigned long)t->packets_out_of_order_gap,
        (unsigned long)t->total_missing_messages,
        (unsigned long)t->channel_resets,
        t->expected_seq);
}
