/*
 * seq_tracker.h
 *
 * Per-channel sequence gap detection, modeled on how real MDP3-style
 * feed handlers reconcile the "fast" UDP multicast feed against a
 * separate TCP recovery/retransmission feed.
 *
 * A gap is: we expected seq N, we received seq > N. Everything in
 * [N, seq-1] is missing. We don't silently skip forward -- we log the
 * gap, update stats, and enqueue a recovery request describing the
 * missing range so a (stubbed here) recovery-feed client can go fetch
 * it out of band. Duplicate/old packets (seq <= last delivered) are
 * dropped, which happens routinely on multicast (retransmits, kernel
 * requeue, etc.) and must never be treated as a gap.
 */

#ifndef SEQ_TRACKER_H
#define SEQ_TRACKER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define MAX_PENDING_RECOVERY_REQUESTS 256

typedef struct {
    uint32_t begin_seq;   /* first missing sequence number (inclusive) */
    uint32_t end_seq;     /* last missing sequence number (inclusive)  */
    uint64_t detected_at_ns;
} recovery_request_t;

typedef struct {
    uint32_t channel_id;
    uint32_t expected_seq;      /* next sequence number we expect       */
    bool     initialized;       /* false until first packet seen        */

    uint64_t packets_received;
    uint64_t packets_duplicate_or_stale;
    uint64_t packets_out_of_order_gap;
    uint64_t total_missing_messages;   /* sum of gap sizes              */
    uint64_t channel_resets;

    recovery_request_t pending[MAX_PENDING_RECOVERY_REQUESTS];
    uint32_t pending_count;
} seq_tracker_t;

typedef enum {
    SEQ_RESULT_OK,           /* in-order, deliver to book              */
    SEQ_RESULT_DUPLICATE,    /* seq <= expected-1, drop silently        */
    SEQ_RESULT_GAP_DETECTED, /* seq > expected, gap logged, deliver anyway
                                 (book will be stale until recovery)    */
} seq_result_t;

void seq_tracker_init(seq_tracker_t *t, uint32_t channel_id);

/* Feed the next observed sequence number through the tracker.
 * Returns the classification and, on a gap, appends a recovery_request_t
 * to the pending queue (best-effort; queue is capped and oldest is not
 * evicted -- a full queue means recovery is falling behind, which is
 * itself a stat worth alarming on in production). */
seq_result_t seq_tracker_observe(seq_tracker_t *t, uint32_t seq, uint64_t now_ns);

/* Explicit channel reset (MDP3 template 4): next expected seq becomes 1. */
void seq_tracker_reset(seq_tracker_t *t);

/* Pop the oldest pending recovery request. Returns false if none pending. */
bool seq_tracker_pop_recovery_request(seq_tracker_t *t, recovery_request_t *out);

void seq_tracker_print_stats(const seq_tracker_t *t);

#endif /* SEQ_TRACKER_H */
