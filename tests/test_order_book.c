#include <assert.h>
#include <stdio.h>
#include "order_book.h"

static mdp3_incremental_refresh_t upd(int side, int action, int level, int64_t px, int32_t sz) {
    mdp3_incremental_refresh_t u;
    memset(&u, 0, sizeof u);
    u.security_id = 1; u.side = (uint8_t)side; u.md_update_action = (uint8_t)action;
    u.md_price_level = (uint8_t)level; u.md_entry_px = px; u.md_entry_size = sz;
    return u;
}

static void test_new_inserts_and_shifts(void) {
    instrument_book_t b; book_init(&b, 1);
    mdp3_incremental_refresh_t u;
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, 1, 100, 10); book_apply(&b, &u);
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, 2, 99, 20);  book_apply(&b, &u);
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, 1, 101, 5);  book_apply(&b, &u); /* new best, shifts down */
    assert(b.bid[0].px == 101 && b.bid[1].px == 100 && b.bid[2].px == 99);
    printf("test_new_inserts_and_shifts: PASS\n");
}

static void test_delete_promotes_next_level(void) {
    instrument_book_t b; book_init(&b, 1);
    mdp3_incremental_refresh_t u;
    u = upd(MD_SIDE_ASK, MD_ACTION_NEW, 1, 102, 1); book_apply(&b, &u);
    u = upd(MD_SIDE_ASK, MD_ACTION_NEW, 2, 103, 2); book_apply(&b, &u);
    u = upd(MD_SIDE_ASK, MD_ACTION_NEW, 3, 104, 3); book_apply(&b, &u);
    u = upd(MD_SIDE_ASK, MD_ACTION_DELETE, 1, 0, 0); book_apply(&b, &u);
    assert(b.ask[0].px == 103 && b.ask[1].px == 104 && b.ask[2].px == 0); /* promoted, not zeroed */
    printf("test_delete_promotes_next_level: PASS\n");
}

static void test_change_in_place_and_deep_level(void) {
    instrument_book_t b; book_init(&b, 1);
    mdp3_incremental_refresh_t u;
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, 1, 100, 10); book_apply(&b, &u);
    u = upd(MD_SIDE_BID, MD_ACTION_CHANGE, 1, 100, 25); book_apply(&b, &u);
    assert(b.bid[0].sz == 25);
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, BOOK_DEPTH, 50, 7); book_apply(&b, &u); /* deepest level */
    assert(b.bid[BOOK_DEPTH - 1].px == 50);
    printf("test_change_in_place_and_deep_level: PASS\n");
}

static void test_bad_level_ignored_and_depth_cap(void) {
    instrument_book_t b; book_init(&b, 1);
    mdp3_incremental_refresh_t u;
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, 0, 1, 1);  assert(!book_apply(&b, &u));
    u = upd(MD_SIDE_BID, MD_ACTION_NEW, BOOK_DEPTH + 1, 1, 1); assert(!book_apply(&b, &u));
    assert(b.ignored == 2);
    for (int i = 0; i < BOOK_DEPTH + 3; i++) { /* overfill: deepest falls off, no overflow */
        u = upd(MD_SIDE_BID, MD_ACTION_NEW, 1, 1000 + i, 1); book_apply(&b, &u);
    }
    assert(b.bid[0].px == 1000 + BOOK_DEPTH + 2);
    printf("test_bad_level_ignored_and_depth_cap: PASS\n");
}

int main(void) {
    test_new_inserts_and_shifts();
    test_delete_promotes_next_level();
    test_change_in_place_and_deep_level();
    test_bad_level_ignored_and_depth_cap();
    printf("\nALL BOOK TESTS PASSED\n");
    return 0;
}
