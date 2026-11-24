/*
 * order_book.h
 *
 * Fixed-depth, price-level book per instrument, applying MDP3
 * incremental-refresh semantics (md_price_level is 1-based):
 *
 *   NEW    at level L: insert at L, levels L.. shift one deeper
 *                      (the deepest level falls off the book)
 *   CHANGE at level L: overwrite level L in place
 *   DELETE at level L: remove level L, deeper levels shift up one
 *
 * Fixed arrays, no allocation. An empty level has size 0.
 */
#ifndef ORDER_BOOK_H
#define ORDER_BOOK_H

#include <stdint.h>
#include <string.h>
#include "mdp3_proto.h"

#define BOOK_DEPTH 10

typedef struct {
    int64_t px;
    int32_t sz;
} book_level_t;

typedef struct {
    int32_t security_id;
    book_level_t bid[BOOK_DEPTH];
    book_level_t ask[BOOK_DEPTH];
    uint64_t updates;
    uint64_t ignored;   /* out-of-range level / unknown action */
} instrument_book_t;

static inline void book_init(instrument_book_t *b, int32_t security_id) {
    memset(b, 0, sizeof(*b));
    b->security_id = security_id;
}

/* Overwrite one level directly (used to load a recovery snapshot).
 * Returns 1 if set, 0 if the level is out of range. */
static inline int book_set_level(instrument_book_t *b, int side, unsigned level,
                                 int64_t px, int32_t sz) {
    if (level < 1 || level > BOOK_DEPTH) return 0;
    book_level_t *s = (side == MD_SIDE_BID) ? b->bid : b->ask;
    s[level - 1].px = px;
    s[level - 1].sz = sz;
    return 1;
}

/* Returns 1 if applied, 0 if ignored (bad level/action). */
static inline int book_apply(instrument_book_t *b, const mdp3_incremental_refresh_t *u) {
    book_level_t *side = (u->side == MD_SIDE_BID) ? b->bid : b->ask;
    unsigned lvl = u->md_price_level;           /* 1-based */
    if (lvl < 1 || lvl > BOOK_DEPTH) { b->ignored++; return 0; }
    unsigned i = lvl - 1;

    switch (u->md_update_action) {
    case MD_ACTION_NEW:
        memmove(&side[i + 1], &side[i], (BOOK_DEPTH - 1 - i) * sizeof(book_level_t));
        side[i].px = u->md_entry_px;
        side[i].sz = u->md_entry_size;
        break;
    case MD_ACTION_CHANGE:
        side[i].px = u->md_entry_px;
        side[i].sz = u->md_entry_size;
        break;
    case MD_ACTION_DELETE:
        memmove(&side[i], &side[i + 1], (BOOK_DEPTH - 1 - i) * sizeof(book_level_t));
        side[BOOK_DEPTH - 1].px = 0;
        side[BOOK_DEPTH - 1].sz = 0;
        break;
    default:
        b->ignored++;
        return 0;
    }
    b->updates++;
    return 1;
}

#endif /* ORDER_BOOK_H */
