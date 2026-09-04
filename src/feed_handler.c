/*
 * feed_handler.c
 *
 * Ties the whole pipeline together:
 *
 *   RX thread                         Processing thread
 *   ----------                        ------------------
 *   read raw packet    -----push--->  pop decoded_message_t
 *   (AF_XDP zero-copy                 apply to book / stats
 *    OR sim UDP socket)               track e2e latency
 *   parse + gap-detect                (no locks: SPSC ring)
 *   push decoded msgs
 *
 * Two RX backends share the identical downstream pipeline
 * (mdp3_parse_packet -> seq_tracker -> spsc_ring -> processing thread):
 *
 *   --mode afxdp   real kernel-bypass path via AF_XDP zero-copy sockets.
 *                  Requires root, a NIC/driver with XDP support, and
 *                  xdp_kern.o built+loaded (see README). This is the
 *                  actual deliverable.
 *
 *   --mode sim     plain UDP socket (works anywhere, no privileges,
 *                  no special NIC). Exists so the parsing / gap
 *                  detection / lock-free ring / book-update logic can
 *                  be built, run, and tested deterministically without
 *                  needing real market-data multicast hardware --
 *                  and so this can be demoed live without a
 *                  kernel-bypass-capable box in front of you.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
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

#ifdef USE_AFXDP
#include "afxdp_socket.h"
#include "xdp_loader.h"
#endif

SPSC_RING_DEFINE(msg_ring_t, decoded_message_t, 4096)

#define MAX_INSTRUMENTS 64

typedef struct {
    int32_t security_id;
    int64_t best_bid_px;
    int64_t best_ask_px;
    int32_t best_bid_sz;
    int32_t best_ask_sz;
    uint64_t updates;
} instrument_book_t;

typedef struct {
    msg_ring_t ring;
    seq_tracker_t tracker;

    instrument_book_t book[MAX_INSTRUMENTS];
    uint32_t book_count;

    volatile sig_atomic_t running;

    uint64_t packets_parsed;
    uint64_t messages_processed;
    uint64_t ring_full_drops;

    /* latency: sending_time (exchange) -> processed_time (us), running max/sum */
    uint64_t latency_sum_ns;
    uint64_t latency_max_ns;
    uint64_t latency_samples;
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

/* ---- shared downstream pipeline -------------------------------- */

static void on_decoded_message(const decoded_message_t *msg, void *ctx) {
    app_ctx_t *app = (app_ctx_t *)ctx;
    if (!msg_ring_t_push(&app->ring, msg)) {
        app->ring_full_drops++;
    }
}

static instrument_book_t *find_or_create_book(app_ctx_t *app, int32_t security_id) {
    for (uint32_t i = 0; i < app->book_count; i++) {
        if (app->book[i].security_id == security_id) return &app->book[i];
    }
    if (app->book_count >= MAX_INSTRUMENTS) return NULL;
    instrument_book_t *b = &app->book[app->book_count++];
    memset(b, 0, sizeof(*b));
    b->security_id = security_id;
    return b;
}

static void apply_book_update(app_ctx_t *app, const decoded_message_t *msg) {
    if (msg->template_id != TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK) return;
    const mdp3_incremental_refresh_t *u = &msg->book_update;
    instrument_book_t *b = find_or_create_book(app, u->security_id);
    if (!b) return;

    b->updates++;
    if (u->md_update_action == MD_ACTION_DELETE) {
        if (u->side == MD_SIDE_BID && u->md_price_level == 1) { b->best_bid_px = 0; b->best_bid_sz = 0; }
        if (u->side == MD_SIDE_ASK && u->md_price_level == 1) { b->best_ask_px = 0; b->best_ask_sz = 0; }
        return;
    }
    /* NEW or CHANGE at top of book: track best bid/ask for the demo */
    if (u->md_price_level == 1) {
        if (u->side == MD_SIDE_BID) { b->best_bid_px = u->md_entry_px; b->best_bid_sz = u->md_entry_size; }
        else                        { b->best_ask_px = u->md_entry_px; b->best_ask_sz = u->md_entry_size; }
    }
}

static void *processing_thread_main(void *arg) {
    app_ctx_t *app = (app_ctx_t *)arg;
    decoded_message_t msg;

    while (app->running) {
        if (!msg_ring_t_pop(&app->ring, &msg)) {
            /* ring empty: brief yield rather than a hard spin. In a
             * latency-critical production build this would instead be
             * a pinned-core busy-poll with no sleep at all -- trading
             * CPU for the last few hundred ns of wakeup latency. Kept
             * as usleep here so the demo doesn't peg a core at 100%. */
            usleep(50);
            continue;
        }

        app->messages_processed++;

        uint64_t proc_time = now_ns();
        if (msg.sending_time_ns > 0 && proc_time > msg.sending_time_ns) {
            uint64_t lat = proc_time - msg.sending_time_ns;
            app->latency_sum_ns += lat;
            app->latency_samples++;
            if (lat > app->latency_max_ns) app->latency_max_ns = lat;
        }

        if (msg.seq_result == SEQ_RESULT_GAP_DETECTED) {
            fprintf(stderr, "[gap] packet_seq=%u template=%u -- book may be stale until recovery\n",
                    msg.packet_seq, msg.template_id);
        }

        apply_book_update(app, &msg);
    }
    return NULL;
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
    }
    seq_tracker_print_stats(&app->tracker);
    for (uint32_t i = 0; i < app->book_count; i++) {
        instrument_book_t *b = &app->book[i];
        fprintf(stderr, "  instrument %d: bid=%ld@%ld ask=%ld@%ld updates=%lu\n",
                b->security_id, (long)b->best_bid_sz, (long)b->best_bid_px,
                (long)b->best_ask_sz, (long)b->best_ask_px, (unsigned long)b->updates);
    }
    recovery_request_t rr;
    int pending = 0;
    seq_tracker_t snapshot = app->tracker; /* stats-only copy for draining without mutating live state oddly */
    while (seq_tracker_pop_recovery_request(&snapshot, &rr)) {
        fprintf(stderr, "  pending recovery: [%u..%u]\n", rr.begin_seq, rr.end_seq);
        pending++;
    }
    if (pending == 0) fprintf(stderr, "  no pending recovery requests\n");
}

/* ---- SIM backend: plain UDP socket ------------------------------ */

static int sim_run(app_ctx_t *app, const char *bind_addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

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

    uint8_t buf[2048];
    while (app->running) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
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
/* ---- AF_XDP backend: real kernel-bypass path -------------------- */

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

    struct xdp_desc descs[64];
    while (app->running) {
        uint32_t n = xsk_rx_poll(&xsk, descs, 64);
        if (n == 0) { usleep(50); continue; }

        for (uint32_t i = 0; i < n; i++) {
            uint8_t *pkt = xsk_umem_ptr(&xsk, descs[i].addr);
            /* NOTE: this is the raw Ethernet frame; skip past the
             * Ethernet(14) + IP(20+) + UDP(8) headers the kernel
             * program already validated to reach the MDP3 payload.
             * Using a fixed 42-byte offset here (no IP options) to
             * keep the demo path simple -- a production parser would
             * read ip->ihl like xdp_kern.c does. */
            const size_t L2_L3_L4_HDR = 14 + 20 + 8;
            if (descs[i].len > L2_L3_L4_HDR) {
                app->packets_parsed++;
                mdp3_parse_packet(pkt + L2_L3_L4_HDR, descs[i].len - L2_L3_L4_HDR,
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
        "usage: %s --mode sim   --bind 127.0.0.1 --port 15000 --channel 1\n", prog);
#ifdef USE_AFXDP
    fprintf(stderr,
        "       %s --mode afxdp --iface eth0 --queue 0 --bpf xdp_kern.o [--skb] --channel 1\n", prog);
#endif
}

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.running = 1;
    msg_ring_t_init(&g_app.ring);

    const char *mode = "sim";
    const char *bind_addr = "127.0.0.1";
    uint16_t port = 15000;
    uint32_t channel_id = 1;
#ifdef USE_AFXDP
    const char *iface = NULL;
    uint32_t queue_id = 0;
    const char *bpf_obj = "xdp_kern.o";
    bool skb_mode = false;
#endif

    static struct option opts[] = {
        {"mode", required_argument, 0, 'm'},
        {"bind", required_argument, 0, 'b'},
        {"port", required_argument, 0, 'p'},
        {"channel", required_argument, 0, 'c'},
#ifdef USE_AFXDP
        {"iface", required_argument, 0, 'i'},
        {"queue", required_argument, 0, 'q'},
        {"bpf", required_argument, 0, 'x'},
        {"skb", no_argument, 0, 's'},
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
#ifdef USE_AFXDP
            case 'i': iface = optarg; break;
            case 'q': queue_id = (uint32_t)atoi(optarg); break;
            case 'x': bpf_obj = optarg; break;
            case 's': skb_mode = true; break;
#endif
            case 'h': default: usage(argv[0]); return 0;
        }
    }

    seq_tracker_init(&g_app.tracker, channel_id);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    pthread_t proc_thread;
    pthread_create(&proc_thread, NULL, processing_thread_main, &g_app);

    int rc;
    if (strcmp(mode, "sim") == 0) {
        rc = sim_run(&g_app, bind_addr, port);
    }
#ifdef USE_AFXDP
    else if (strcmp(mode, "afxdp") == 0) {
        if (!iface) { fprintf(stderr, "--iface required for afxdp mode\n"); usage(argv[0]); return 1; }
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
    print_stats(&g_app);
    return rc;
}
