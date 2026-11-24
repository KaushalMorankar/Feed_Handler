/*
 * feed_handler.c
 *
 * Ties the whole pipeline together:
 *
 *   RX thread                         Processing thread
 *   ----------                        ------------------
 *   read raw packet    -----push--->  pop decoded_message_t
 *   (AF_XDP zero-copy                 apply to book / stats
 *    OR sim UDP socket)               track send-to-process latency
 *   parse + gap-detect                (no locks: SPSC ring)
 *   push decoded msgs
 *
 *   Recovery thread (only when --recovery host:port is given)
 *   ---------------------------------------------------------
 *   woken when the book goes stale; fetches a full book snapshot over
 *   TCP and hands it to the processing thread, which re-synchronises.
 *
 * Two RX backends share the identical downstream pipeline
 * (mdp3_parse_packet -> seq_tracker -> spsc_ring -> processing thread):
 *
 *   --mode afxdp   kernel-bypass path via AF_XDP sockets (zero-copy on
 *                  NICs whose driver supports it, copy mode otherwise).
 *                  Requires root and xdp_kern.o (see README).
 *
 *   --mode sim     plain UDP socket (works anywhere, no privileges).
 *
 * Book consistency model
 * ----------------------
 * A gap in the sequence numbers, or a message lost to ring overflow,
 * makes the book untrustworthy. With recovery enabled the handler then:
 *   1. stops applying live updates and buffers them instead,
 *   2. fetches a snapshot whose last_seq covers everything that was lost,
 *   3. loads the snapshot, replays buffered updates with seq > last_seq,
 *      and clears the stale flag.
 * Without recovery, updates keep being applied but the book stays
 * flagged stale until a channel reset.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mdp3_proto.h"
#include "mdp3_parser.h"
#include "seq_tracker.h"
#include "spsc_ring.h"
#include "order_book.h"
#include "recovery_proto.h"

#ifdef USE_AFXDP
#include "afxdp_socket.h"
#include "xdp_loader.h"
#endif

SPSC_RING_DEFINE(msg_ring_t, decoded_message_t, 4096)

#define MAX_INSTRUMENTS   64
#define REPLAY_CAPACITY   (1u << 16)   /* live updates buffered while stale */
#define LAT_SAMPLE_CAP    (1u << 22)   /* ~4M latency samples kept for percentiles */

typedef struct {
    snapshot_header_t hdr;
    snapshot_entry_t entries[SNAP_MAX_ENTRIES];
} snapshot_t;

typedef struct {
    msg_ring_t ring;
    seq_tracker_t tracker;

    instrument_book_t book[MAX_INSTRUMENTS];
    uint32_t book_count;

    /* ---- consistency state (processing-thread owned) ------------- */
    bool book_stale;
    uint32_t need_seq;               /* snapshot must reach at least this seq */
    uint64_t stale_since_ns;
    uint64_t stale_events;
    uint64_t duplicates_skipped;
    uint64_t updates_applied_while_stale;   /* only without recovery */
    uint64_t updates_buffered;
    uint64_t replay_overflows;
    uint64_t snapshots_applied;
    uint64_t snapshots_too_old;
    uint64_t recovery_time_sum_ns;
    uint64_t recovery_time_max_ns;
    uint32_t replay_n;
    decoded_message_t replay[REPLAY_CAPACITY];

    /* After a snapshot, messages still queued in the ring may be OLDER
     * than the snapshot; applying them would overwrite newer state, so
     * everything with seq <= floor_seq is skipped until the first newer one. */
    bool floor_valid;
    uint32_t floor_seq;
    uint64_t skipped_covered_by_snapshot;

    /* ---- RX -> consumer: ring overflow notification -------------- */
    atomic_bool ring_overflow;
    atomic_uint overflow_seq;        /* packet_seq of the latest dropped msg */

    /* ---- consumer <-> recovery thread ---------------------------- */
    bool recovery_enabled;
    struct sockaddr_in recovery_addr;
    atomic_bool snap_request;        /* consumer -> recovery thread */
    atomic_bool snap_ready;          /* recovery thread -> consumer */
    snapshot_t snap;                 /* owned by whoever holds the flag */
    uint64_t snapshot_fetch_failures;

    /* ---- options ------------------------------------------------- */
    bool busy_poll;
    int cpu_rx, cpu_proc;
    uint32_t slow_consumer_us;       /* test hook: simulate a slow consumer */

    volatile sig_atomic_t running;

    uint64_t packets_parsed;
    uint64_t messages_processed;
    uint64_t ring_full_drops;

    /* send-time -> processed-time latency (generator/exchange timestamp
     * in the packet header, same machine clock) */
    uint64_t latency_sum_ns;
    uint64_t latency_max_ns;
    uint64_t latency_samples;
    uint32_t lat_ns[LAT_SAMPLE_CAP];
} app_ctx_t;

static app_ctx_t g_app;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_sigint(int sig) {
    (void)sig;
    g_app.running = 0;
}

static void pin_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof(s), &s) != 0) perror("sched_setaffinity");
}

/* serial-number comparisons, safe across the uint32 wrap */
static inline bool seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

/* ---- shared downstream pipeline -------------------------------- */

static void on_decoded_message(const decoded_message_t *msg, void *ctx) {
    app_ctx_t *app = (app_ctx_t *)ctx;
    if (!msg_ring_t_push(&app->ring, msg)) {
        app->ring_full_drops++;
        /* A dropped message is a gap the sequence tracker can't see (the
         * packet seq was fine). Record which seq was lost and tell the
         * consumer, so the book goes stale instead of silently diverging. */
        atomic_store_explicit(&app->overflow_seq, msg->packet_seq, memory_order_relaxed);
        atomic_store_explicit(&app->ring_overflow, true, memory_order_release);
    }
}

static instrument_book_t *find_or_create_book(app_ctx_t *app, int32_t security_id) {
    for (uint32_t i = 0; i < app->book_count; i++) {
        if (app->book[i].security_id == security_id) return &app->book[i];
    }
    if (app->book_count >= MAX_INSTRUMENTS) return NULL;
    instrument_book_t *b = &app->book[app->book_count++];
    book_init(b, security_id);
    return b;
}

static void apply_book_update(app_ctx_t *app, const decoded_message_t *msg) {
    if (msg->template_id != TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK) return;
    const mdp3_incremental_refresh_t *u = &msg->book_update;
    instrument_book_t *b = find_or_create_book(app, u->security_id);
    if (!b) return;
    book_apply(b, u);
}

static void request_snapshot(app_ctx_t *app) {
    if (app->recovery_enabled)
        atomic_store_explicit(&app->snap_request, true, memory_order_release);
}

/* Book can no longer be trusted: everything up to `need_seq` must be
 * covered by a snapshot before we are consistent again. */
static void mark_stale(app_ctx_t *app, uint32_t need_seq) {
    if (!app->book_stale) {
        app->book_stale = true;
        app->stale_events++;
        app->stale_since_ns = now_ns();
        app->need_seq = need_seq;
        request_snapshot(app);
    } else if (seq_gt(need_seq, app->need_seq)) {
        app->need_seq = need_seq;
    }
}

static void reset_books(app_ctx_t *app) {
    app->book_count = 0;
    app->book_stale = false;
    app->replay_n = 0;
    app->floor_valid = false;
}

static void buffer_live_update(app_ctx_t *app, const decoded_message_t *msg) {
    if (app->replay_n == REPLAY_CAPACITY) {
        /* Can't hold any more. Drop the buffer and demand a snapshot that
         * reaches at least this message; everything older is then covered
         * by the snapshot itself. */
        app->replay_n = 0;
        app->replay_overflows++;
        if (seq_gt(msg->packet_seq, app->need_seq)) app->need_seq = msg->packet_seq;
    }
    app->replay[app->replay_n++] = *msg;
    app->updates_buffered++;
}

/* Called by the processing thread when the recovery thread has delivered
 * a snapshot in app->snap. */
static void apply_snapshot(app_ctx_t *app) {
    const snapshot_t *s = &app->snap;

    if (!app->book_stale) {
        /* a channel reset already made us fresh; nothing to do */
    } else if (!seq_ge(s->hdr.last_seq, app->need_seq)) {
        /* snapshot is older than what we lost: ask again */
        app->snapshots_too_old++;
        atomic_store_explicit(&app->snap_ready, false, memory_order_release);
        request_snapshot(app);
        return;
    } else {
        app->book_count = 0;
        for (uint32_t i = 0; i < s->hdr.count; i++) {
            const snapshot_entry_t *e = &s->entries[i];
            instrument_book_t *b = find_or_create_book(app, e->security_id);
            if (b) book_set_level(b, e->side, e->level, e->px, e->size);
        }
        for (uint32_t i = 0; i < app->replay_n; i++) {
            if (seq_gt(app->replay[i].packet_seq, s->hdr.last_seq))
                apply_book_update(app, &app->replay[i]);
        }
        app->replay_n = 0;
        app->book_stale = false;
        app->floor_valid = true;
        app->floor_seq = s->hdr.last_seq;
        app->snapshots_applied++;
        uint64_t dt = now_ns() - app->stale_since_ns;
        app->recovery_time_sum_ns += dt;
        if (dt > app->recovery_time_max_ns) app->recovery_time_max_ns = dt;
    }
    atomic_store_explicit(&app->snap_ready, false, memory_order_release);
}

static void *processing_thread_main(void *arg) {
    app_ctx_t *app = (app_ctx_t *)arg;
    decoded_message_t msg;
    pin_cpu(app->cpu_proc);

    while (app->running) {
        if (atomic_load_explicit(&app->snap_ready, memory_order_acquire)) {
            apply_snapshot(app);
        }

        if (!msg_ring_t_pop(&app->ring, &msg)) {
            /* Ring empty. Default: yield briefly so the demo doesn't peg a
             * core. --busy-poll spins instead (pin it with --cpu-proc),
             * trading a core for the last bit of wakeup latency. */
            if (!app->busy_poll) usleep(50);
            continue;
        }

        app->messages_processed++;

        /* A message was lost between parser and here (ring full). */
        if (atomic_exchange_explicit(&app->ring_overflow, false, memory_order_acq_rel)) {
            mark_stale(app, atomic_load_explicit(&app->overflow_seq, memory_order_relaxed));
        }

        /* Stale/duplicate packets (retransmits) must not be applied twice. */
        if (msg.seq_result == SEQ_RESULT_DUPLICATE) {
            app->duplicates_skipped++;
            continue;
        }

        uint64_t proc_time = now_ns();
        if (msg.sending_time_ns > 0 && proc_time > msg.sending_time_ns) {
            uint64_t lat = proc_time - msg.sending_time_ns;
            app->latency_sum_ns += lat;
            if (app->latency_samples < LAT_SAMPLE_CAP)
                app->lat_ns[app->latency_samples] = lat > UINT32_MAX ? UINT32_MAX : (uint32_t)lat;
            app->latency_samples++;
            if (lat > app->latency_max_ns) app->latency_max_ns = lat;
        }

        if (msg.template_id == TEMPLATE_ID_CHANNEL_RESET) {
            /* Exchange says: discard state, restart sequencing. */
            reset_books(app);
            continue;
        }

        if (app->floor_valid) {
            if (!seq_gt(msg.packet_seq, app->floor_seq)) {
                app->skipped_covered_by_snapshot++;   /* already reflected in the snapshot */
                continue;
            }
            app->floor_valid = false;
        }

        if (msg.seq_result == SEQ_RESULT_GAP_DETECTED) {
            fprintf(stderr, "[gap] packet_seq=%u template=%u -- books stale until %s\n",
                    msg.packet_seq, msg.template_id,
                    app->recovery_enabled ? "snapshot recovery" : "reset");
            mark_stale(app, msg.packet_seq - 1);   /* missing range ends just before this packet */
        }

        if (app->book_stale && app->recovery_enabled) {
            buffer_live_update(app, &msg);          /* replayed on top of the snapshot */
        } else {
            apply_book_update(app, &msg);
            if (app->book_stale) app->updates_applied_while_stale++;
        }

        if (app->slow_consumer_us) usleep(app->slow_consumer_us);
    }
    return NULL;
}

/* ---- recovery thread: TCP snapshot client ----------------------- */

static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

static int fetch_snapshot(app_ctx_t *app) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    int rc = -1;
    snapshot_header_t hdr;
    if (connect(fd, (struct sockaddr *)&app->recovery_addr, sizeof(app->recovery_addr)) == 0 &&
        write(fd, "S", 1) == 1 &&
        read_full(fd, &hdr, sizeof hdr) == 0 &&
        hdr.count <= SNAP_MAX_ENTRIES &&
        read_full(fd, app->snap.entries, hdr.count * sizeof(snapshot_entry_t)) == 0) {
        app->snap.hdr = hdr;
        rc = 0;
    }
    close(fd);
    return rc;
}

static void *recovery_thread_main(void *arg) {
    app_ctx_t *app = (app_ctx_t *)arg;
    while (app->running) {
        /* never touch app->snap while the consumer may still be reading it */
        if (atomic_load_explicit(&app->snap_ready, memory_order_acquire) ||
            !atomic_exchange_explicit(&app->snap_request, false, memory_order_acq_rel)) {
            usleep(200);
            continue;
        }
        if (fetch_snapshot(app) == 0) {
            atomic_store_explicit(&app->snap_ready, true, memory_order_release);
        } else {
            app->snapshot_fetch_failures++;
            usleep(100000);
            atomic_store_explicit(&app->snap_request, true, memory_order_release);
        }
    }
    return NULL;
}

/* ---- stats -------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void print_stats(app_ctx_t *app) {
    fprintf(stderr, "\n=== stats ===\n");
    fprintf(stderr, "packets_parsed=%lu messages_processed=%lu ring_full_drops=%lu\n",
            (unsigned long)app->packets_parsed, (unsigned long)app->messages_processed,
            (unsigned long)app->ring_full_drops);
    if (app->latency_samples > 0) {
        fprintf(stderr, "e2e_latency_avg_ns=%lu max_ns=%lu (samples=%lu)\n",
                (unsigned long)(app->latency_sum_ns / app->latency_samples),
                (unsigned long)app->latency_max_ns, (unsigned long)app->latency_samples);
        uint64_t n = app->latency_samples < LAT_SAMPLE_CAP ? app->latency_samples : LAT_SAMPLE_CAP;
        qsort(app->lat_ns, n, sizeof(uint32_t), cmp_u32);
        fprintf(stderr, "e2e_latency_p50_ns=%u p99_ns=%u p999_ns=%u\n",
                app->lat_ns[n * 50 / 100], app->lat_ns[n * 99 / 100], app->lat_ns[n * 999 / 1000]);
    }
    seq_tracker_print_stats(&app->tracker);
    fprintf(stderr, "book_stale=%s stale_events=%lu duplicates_skipped=%lu "
                    "applied_while_stale=%lu\n",
            app->book_stale ? "YES" : "no", (unsigned long)app->stale_events,
            (unsigned long)app->duplicates_skipped,
            (unsigned long)app->updates_applied_while_stale);
    if (app->recovery_enabled) {
        fprintf(stderr, "recovery: snapshots_applied=%lu too_old=%lu fetch_failures=%lu "
                        "buffered=%lu replay_overflows=%lu covered_skipped=%lu",
                (unsigned long)app->snapshots_applied, (unsigned long)app->snapshots_too_old,
                (unsigned long)app->snapshot_fetch_failures,
                (unsigned long)app->updates_buffered, (unsigned long)app->replay_overflows,
                (unsigned long)app->skipped_covered_by_snapshot);
        if (app->snapshots_applied)
            fprintf(stderr, " recovery_time_avg_us=%lu max_us=%lu",
                    (unsigned long)(app->recovery_time_sum_ns / app->snapshots_applied / 1000),
                    (unsigned long)(app->recovery_time_max_ns / 1000));
        fprintf(stderr, "\n");
    }
    for (uint32_t i = 0; i < app->book_count; i++) {
        instrument_book_t *b = &app->book[i];
        int bd = 0, ad = 0;
        while (bd < BOOK_DEPTH && b->bid[bd].sz > 0) bd++;
        while (ad < BOOK_DEPTH && b->ask[ad].sz > 0) ad++;
        fprintf(stderr, "  instrument %d: bid=%ld@%ld (%d lvls) ask=%ld@%ld (%d lvls) updates=%lu\n",
                b->security_id, (long)b->bid[0].sz, (long)b->bid[0].px, bd,
                (long)b->ask[0].sz, (long)b->ask[0].px, ad, (unsigned long)b->updates);
    }
    /* machine-readable, same format packet_gen prints for its ground truth */
    for (int id = 1; id < 8; id++) {
        for (uint32_t i = 0; i < app->book_count; i++) {
            instrument_book_t *b = &app->book[i];
            if (b->security_id != id) continue;
            fprintf(stderr, "[book] inst=%d bid=%d@%ld ask=%d@%ld\n", id,
                    b->bid[0].sz, (long)b->bid[0].px, b->ask[0].sz, (long)b->ask[0].px);
        }
    }
    recovery_request_t rr;
    int pending = 0;
    seq_tracker_t snapshot = app->tracker; /* stats-only copy so draining doesn't mutate live state */
    while (seq_tracker_pop_recovery_request(&snapshot, &rr)) pending++;
    fprintf(stderr, "  gap ranges detected (queued, cap %d): %d\n",
            MAX_PENDING_RECOVERY_REQUESTS, pending);
}

/* ---- SIM backend: plain UDP socket ------------------------------ */

static int sim_run(app_ctx_t *app, const char *bind_addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    int rcvbuf = 8 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    fprintf(stderr, "[sim] listening on %s:%u\n", bind_addr, port);
    pin_cpu(app->cpu_rx);

    uint8_t buf[2048];
    while (app->running) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf), app->busy_poll ? MSG_DONTWAIT : 0, NULL, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        app->packets_parsed++;
        mdp3_parse_packet(buf, (size_t)n, &app->tracker, now_ns(), on_decoded_message, app);
    }

    close(fd);
    return 0;
}

#ifdef USE_AFXDP
/* ---- AF_XDP backend: kernel-bypass path -------------------------- */

/* Locate the MDP3 payload in a raw Ethernet frame: Ethernet, up to two
 * VLAN tags (802.1Q / 802.1ad), IPv4 honouring IHL, UDP. Returns the
 * payload offset, or 0 if the frame is not IPv4/UDP. */
static size_t mdp3_payload_offset(const uint8_t *pkt, size_t len) {
    if (len < 14) return 0;
    size_t off = 14;
    uint16_t et = (uint16_t)((pkt[12] << 8) | pkt[13]);
    for (int i = 0; i < 2 && (et == 0x8100 || et == 0x88A8); i++) {
        if (len < off + 4) return 0;
        et = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        off += 4;
    }
    if (et != 0x0800 || len < off + 20) return 0;
    size_t ihl = (size_t)(pkt[off] & 0x0F) * 4;
    if ((pkt[off] >> 4) != 4 || ihl < 20 || pkt[off + 9] != 17 || len < off + ihl + 8) return 0;
    off += ihl + 8;
    return len > off ? off : 0;
}

static int afxdp_run(app_ctx_t *app, const char *ifname, uint32_t queue_id,
                      const char *bpf_obj_path, bool skb_mode) {
    xsk_socket_t xsk;
    if (xsk_socket_create(&xsk, ifname, queue_id, !skb_mode) != 0) {
        return -1;
    }
    xsk_populate_fill_ring(&xsk);

    if (xdp_loader_attach(ifname, bpf_obj_path, skb_mode) != 0) {
        xsk_socket_destroy(&xsk);
        return -1;
    }
    if (xdp_loader_bind_queue(queue_id, xsk.fd) != 0) {
        xdp_loader_detach();
        xsk_socket_destroy(&xsk);
        return -1;
    }

    fprintf(stderr, "[afxdp] attached to %s queue %u (%s mode)\n",
            ifname, queue_id, skb_mode ? "skb/generic" : "native/zero-copy");
    pin_cpu(app->cpu_rx);

    struct xdp_desc descs[64];
    while (app->running) {
        uint32_t n = xsk_rx_poll(&xsk, descs, 64);
        if (n == 0) { if (!app->busy_poll) usleep(50); continue; }

        for (uint32_t i = 0; i < n; i++) {
            uint8_t *pkt = xsk_umem_ptr(&xsk, descs[i].addr);
            size_t off = mdp3_payload_offset(pkt, descs[i].len);
            if (off) {
                app->packets_parsed++;
                mdp3_parse_packet(pkt + off, descs[i].len - off,
                                   &app->tracker, now_ns(), on_decoded_message, app);
            }
            xsk_release_frame(&xsk, descs[i].addr);
        }
        xsk_populate_fill_ring(&xsk);
    }

    xdp_loader_detach();
    xsk_socket_destroy(&xsk);
    return 0;
}
#endif

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s --mode sim --bind 127.0.0.1 --port 15000 --channel 1\n"
        "  common: [--recovery host:port] [--busy-poll] [--cpu-rx N] [--cpu-proc N]\n"
        "          [--slow-consumer-us N]  (test hook: throttle the consumer)\n", prog);
#ifdef USE_AFXDP
    fprintf(stderr,
        "       %s --mode afxdp --iface eth0 --queue 0 --bpf xdp_kern.o [--skb]\n"
        "          [--port 15000] [--group 239.1.1.1] [--vlan 100] --channel 1\n", prog);
#endif
}

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.running = 1;
    g_app.cpu_rx = g_app.cpu_proc = -1;
    msg_ring_t_init(&g_app.ring);
    atomic_init(&g_app.ring_overflow, false);
    atomic_init(&g_app.overflow_seq, 0);
    atomic_init(&g_app.snap_request, false);
    atomic_init(&g_app.snap_ready, false);

    const char *mode = "sim";
    const char *bind_addr = "127.0.0.1";
    uint16_t port = 15000;
    uint32_t channel_id = 1;
    const char *recovery = NULL;
#ifdef USE_AFXDP
    const char *iface = NULL;
    uint32_t queue_id = 0;
    const char *bpf_obj = "xdp_kern.o";
    bool skb_mode = false;
    const char *group = NULL;
    uint16_t vlan = 0;
#endif

    enum { OPT_RECOVERY = 256, OPT_BUSY, OPT_CPU_RX, OPT_CPU_PROC, OPT_SLOW, OPT_GROUP, OPT_VLAN };
    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"bind", required_argument, 0, 'b'},
        {"port", required_argument, 0, 'p'},
        {"channel", required_argument, 0, 'c'},
        {"recovery", required_argument, 0, OPT_RECOVERY},
        {"busy-poll", no_argument, 0, OPT_BUSY},
        {"cpu-rx", required_argument, 0, OPT_CPU_RX},
        {"cpu-proc", required_argument, 0, OPT_CPU_PROC},
        {"slow-consumer-us", required_argument, 0, OPT_SLOW},
#ifdef USE_AFXDP
        {"iface", required_argument, 0, 'i'},
        {"queue", required_argument, 0, 'q'},
        {"bpf", required_argument, 0, 'x'},
        {"skb", no_argument, 0, 's'},
        {"group", required_argument, 0, OPT_GROUP},
        {"vlan", required_argument, 0, OPT_VLAN},
#endif
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
#ifdef USE_AFXDP
    const char *optstring = "m:b:p:c:i:q:x:sh";
#else
    const char *optstring = "m:b:p:c:h";
#endif
    while ((c = getopt_long(argc, argv, optstring, opts, NULL)) != -1) {
        switch (c) {
            case 'm': mode = optarg; break;
            case 'b': bind_addr = optarg; break;
            case 'p': port = (uint16_t)atoi(optarg); break;
            case 'c': channel_id = (uint32_t)atoi(optarg); break;
            case OPT_RECOVERY: recovery = optarg; break;
            case OPT_BUSY: g_app.busy_poll = true; break;
            case OPT_CPU_RX: g_app.cpu_rx = atoi(optarg); break;
            case OPT_CPU_PROC: g_app.cpu_proc = atoi(optarg); break;
            case OPT_SLOW: g_app.slow_consumer_us = (uint32_t)atoi(optarg); break;
#ifdef USE_AFXDP
            case 'i': iface = optarg; break;
            case 'q': queue_id = (uint32_t)atoi(optarg); break;
            case 'x': bpf_obj = optarg; break;
            case 's': skb_mode = true; break;
            case OPT_GROUP: group = optarg; break;
            case OPT_VLAN: vlan = (uint16_t)atoi(optarg); break;
#endif
            case 'h': default: usage(argv[0]); return 0;
        }
    }

    if (recovery) {
        char host[64];
        const char *colon = strchr(recovery, ':');
        if (!colon || (size_t)(colon - recovery) >= sizeof(host)) {
            fprintf(stderr, "--recovery expects host:port\n");
            return 1;
        }
        memcpy(host, recovery, (size_t)(colon - recovery));
        host[colon - recovery] = '\0';
        memset(&g_app.recovery_addr, 0, sizeof(g_app.recovery_addr));
        g_app.recovery_addr.sin_family = AF_INET;
        g_app.recovery_addr.sin_port = htons((uint16_t)atoi(colon + 1));
        if (inet_pton(AF_INET, host, &g_app.recovery_addr.sin_addr) != 1) {
            fprintf(stderr, "bad recovery host '%s' (IPv4 only)\n", host);
            return 1;
        }
        g_app.recovery_enabled = true;
    }

    seq_tracker_init(&g_app.tracker, channel_id);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    pthread_t proc_thread, rec_thread;
    pthread_create(&proc_thread, NULL, processing_thread_main, &g_app);
    if (g_app.recovery_enabled) pthread_create(&rec_thread, NULL, recovery_thread_main, &g_app);

    int rc;
    if (strcmp(mode, "sim") == 0) {
        rc = sim_run(&g_app, bind_addr, port);
    }
#ifdef USE_AFXDP
    else if (strcmp(mode, "afxdp") == 0) {
        if (!iface) { fprintf(stderr, "--iface required for afxdp mode\n"); usage(argv[0]); return 1; }
        uint32_t group_be = 0;
        if (group && inet_pton(AF_INET, group, &group_be) != 1) {
            fprintf(stderr, "bad --group '%s'\n", group);
            return 1;
        }
        xdp_loader_set_filter(group_be, port, vlan);
        rc = afxdp_run(&g_app, iface, queue_id, bpf_obj, skb_mode);
    }
#endif
    else {
        fprintf(stderr, "unknown mode '%s'\n", mode);
        usage(argv[0]);
        return 1;
    }

    g_app.running = 0;
    pthread_join(proc_thread, NULL);
    if (g_app.recovery_enabled) pthread_join(rec_thread, NULL);
    print_stats(&g_app);
    return rc;
}
