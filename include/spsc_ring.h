/*
 * spsc_ring.h
 *
 * Fixed-capacity, single-producer/single-consumer lock-free ring buffer.
 *
 * Design notes (why it looks like this):
 *  - Capacity is a power of two so index wraparound is a bitmask, not a
 *    modulo (branch-free, no division on the hot path).
 *  - head/tail are separate cache lines (padded) to avoid false sharing
 *    between the producer (RX thread) and consumer (processing thread)
 *    hammering the same cache line from different cores.
 *  - Slots are pre-allocated inline in the ring (an array of the message
 *    struct, not an array of pointers), so pushing a message is a single
 *    memcpy into an already-resident, already-hot slot -- no malloc/free
 *    on the hot path, no pointer chasing on the consumer side.
 *  - Uses C11 atomics with acquire/release ordering: the producer
 *    publishes the new tail with a release store after writing the
 *    payload; the consumer acquires that store before reading the
 *    payload. This is the standard SPSC handshake and is cheaper than
 *    a full seq_cst fence on every push/pop.
 */

#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define CACHE_LINE_SIZE 64

#define SPSC_RING_DEFINE(NAME, TYPE, CAPACITY_POW2)                        \
typedef struct {                                                           \
    TYPE slots[CAPACITY_POW2];                                             \
    _Alignas(CACHE_LINE_SIZE) atomic_uint_fast64_t head; /* consumer owns*/\
    char _pad0[CACHE_LINE_SIZE - sizeof(atomic_uint_fast64_t)];            \
    _Alignas(CACHE_LINE_SIZE) atomic_uint_fast64_t tail; /* producer owns*/\
    char _pad1[CACHE_LINE_SIZE - sizeof(atomic_uint_fast64_t)];            \
} NAME;                                                                     \
                                                                             \
static inline void NAME##_init(NAME *r) {                                  \
    atomic_init(&r->head, 0);                                              \
    atomic_init(&r->tail, 0);                                              \
}                                                                           \
                                                                             \
static inline bool NAME##_push(NAME *r, const TYPE *item) {                \
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed); \
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_acquire); \
    if (tail - head >= (CAPACITY_POW2)) {                                  \
        return false; /* ring full: consumer too slow, caller decides */   \
    }                                                                      \
    r->slots[tail & ((CAPACITY_POW2) - 1)] = *item;                        \
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);       \
    return true;                                                           \
}                                                                           \
                                                                             \
static inline bool NAME##_pop(NAME *r, TYPE *out) {                        \
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_relaxed); \
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire); \
    if (head == tail) {                                                    \
        return false; /* empty */                                         \
    }                                                                      \
    *out = r->slots[head & ((CAPACITY_POW2) - 1)];                         \
    atomic_store_explicit(&r->head, head + 1, memory_order_release);       \
    return true;                                                           \
}                                                                           \
                                                                             \
static inline uint_fast64_t NAME##_size(NAME *r) {                         \
    uint_fast64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire); \
    uint_fast64_t head = atomic_load_explicit(&r->head, memory_order_acquire); \
    return tail - head;                                                    \
}

#endif /* SPSC_RING_H */
