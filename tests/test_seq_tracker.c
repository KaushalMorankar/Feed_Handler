#include <assert.h>
#include <stdio.h>
#include "seq_tracker.h"

static void test_in_order(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    assert(seq_tracker_observe(&t, 1, 100) == SEQ_RESULT_OK);
    assert(seq_tracker_observe(&t, 2, 200) == SEQ_RESULT_OK);
    assert(seq_tracker_observe(&t, 3, 300) == SEQ_RESULT_OK);
    assert(t.packets_received == 3);
    assert(t.packets_out_of_order_gap == 0);
    assert(t.expected_seq == 4);
    printf("test_in_order: PASS\n");
}

static void test_single_gap(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    seq_tracker_observe(&t, 1, 100);
    /* seq 2, 3 never arrive */
    seq_result_t r = seq_tracker_observe(&t, 4, 400);
    assert(r == SEQ_RESULT_GAP_DETECTED);
    assert(t.total_missing_messages == 2); /* missing 2 and 3 */
    assert(t.packets_out_of_order_gap == 1);

    recovery_request_t rr;
    assert(seq_tracker_pop_recovery_request(&t, &rr));
    assert(rr.begin_seq == 2);
    assert(rr.end_seq == 3);
    assert(!seq_tracker_pop_recovery_request(&t, &rr)); /* queue now empty */
    printf("test_single_gap: PASS\n");
}

static void test_multiple_gaps_queued_in_order(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    seq_tracker_observe(&t, 1, 100);
    seq_tracker_observe(&t, 3, 300);   /* gap: missing 2 */
    seq_tracker_observe(&t, 6, 600);   /* gap: missing 4,5 */
    assert(t.total_missing_messages == 3);
    assert(t.pending_count == 2);

    recovery_request_t rr;
    seq_tracker_pop_recovery_request(&t, &rr);
    assert(rr.begin_seq == 2 && rr.end_seq == 2);
    seq_tracker_pop_recovery_request(&t, &rr);
    assert(rr.begin_seq == 4 && rr.end_seq == 5);
    printf("test_multiple_gaps_queued_in_order: PASS\n");
}

static void test_duplicate_is_not_a_gap(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    seq_tracker_observe(&t, 1, 100);
    seq_tracker_observe(&t, 2, 200);
    seq_result_t r = seq_tracker_observe(&t, 2, 250); /* replay/retransmit */
    assert(r == SEQ_RESULT_DUPLICATE);
    assert(t.packets_duplicate_or_stale == 1);
    assert(t.packets_out_of_order_gap == 0);
    assert(t.expected_seq == 3); /* unaffected by the duplicate */
    printf("test_duplicate_is_not_a_gap: PASS\n");
}

static void test_channel_reset(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    seq_tracker_observe(&t, 1, 100);
    seq_tracker_observe(&t, 2, 200);
    seq_tracker_reset(&t);
    assert(t.channel_resets == 1);
    /* after reset, next observed seq is treated as the new starting point */
    seq_result_t r = seq_tracker_observe(&t, 1, 300);
    assert(r == SEQ_RESULT_OK);
    assert(t.expected_seq == 2);
    printf("test_channel_reset: PASS\n");
}

static void test_first_packet_never_flagged_as_gap(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    /* even if the very first packet we ever see has a high seq number
     * (we joined the multicast group mid-stream), that's not treated
     * as a gap -- there's nothing to recover, we just start tracking
     * from here. Real recovery-from-snapshot is a separate concern. */
    seq_result_t r = seq_tracker_observe(&t, 5000, 100);
    assert(r == SEQ_RESULT_OK);
    assert(t.packets_out_of_order_gap == 0);
    printf("test_first_packet_never_flagged_as_gap: PASS\n");
}

static void test_wraparound(void) {
    seq_tracker_t t;
    seq_tracker_init(&t, 1);
    assert(seq_tracker_observe(&t, 0xFFFFFFFEu, 1) == SEQ_RESULT_OK);
    assert(seq_tracker_observe(&t, 0xFFFFFFFFu, 2) == SEQ_RESULT_OK);
    assert(seq_tracker_observe(&t, 0u, 3) == SEQ_RESULT_OK);   /* wrap, in order */
    assert(seq_tracker_observe(&t, 1u, 4) == SEQ_RESULT_OK);
    assert(t.packets_duplicate_or_stale == 0 && t.packets_out_of_order_gap == 0);
    /* a gap that straddles the wrap: expected 2, got 4 */
    assert(seq_tracker_observe(&t, 4u, 5) == SEQ_RESULT_GAP_DETECTED);
    assert(t.total_missing_messages == 2);
    /* a stale packet from before the wrap is a duplicate, not a gap */
    assert(seq_tracker_observe(&t, 0xFFFFFFFFu, 6) == SEQ_RESULT_DUPLICATE);
    /* gap that begins before the wrap and ends after it */
    seq_tracker_init(&t, 1);
    seq_tracker_observe(&t, 0xFFFFFFFEu, 1);
    assert(seq_tracker_observe(&t, 1u, 2) == SEQ_RESULT_GAP_DETECTED);
    assert(t.total_missing_messages == 2); /* 0xFFFFFFFF and 0 */
    printf("test_wraparound: PASS\n");
}

int main(void) {
    test_wraparound();
    test_in_order();
    test_single_gap();
    test_multiple_gaps_queued_in_order();
    test_duplicate_is_not_a_gap();
    test_channel_reset();
    test_first_packet_never_flagged_as_gap();
    printf("\nALL TESTS PASSED\n");
    return 0;
}
