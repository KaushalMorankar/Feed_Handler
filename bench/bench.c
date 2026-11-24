/*
 * bench.c -- micro + end-to-end benchmarks for the feed handler hot path.
 *
 * Uses the REAL project code (mdp3_parser.c, seq_tracker.c, spsc_ring.h);
 * nothing is re-implemented here except a mutex-queue baseline for contrast.
 *
 *   1. parser      : ns/msg + Mmsg/s decoding multi-message MDP3 packets
 *   2. seq_tracker : ns/op, and exactness of gap accounting over N packets
 *   3. ring        : cross-core SPSC throughput vs a pthread-mutex queue
 *   4. ring_lat    : cross-core one-way handoff latency percentiles
 *   5. pipeline    : parse -> SPSC ring -> book update, throughput + latency
 *   6. udp_path    : kernel UDP loopback recvfrom() path (what AF_XDP removes)
 *
 * Output is one `KEY value` line per metric so scripts can scrape it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mdp3_proto.h"
#include "mdp3_parser.h"
#include "seq_tracker.h"
#include "spsc_ring.h"

SPSC_RING_DEFINE(msg_ring_t, decoded_message_t, 4096)

static int CPU_A = 0, CPU_B = 2; /* two distinct physical cores */

static inline uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static void report_pct(const char *tag, uint64_t *v, size_t n) {
    qsort(v, n, sizeof(*v), cmp_u64);
    printf("%s_p50_ns %lu\n", tag, (unsigned long)v[n * 50 / 100]);
    printf("%s_p99_ns %lu\n", tag, (unsigned long)v[n * 99 / 100]);
    printf("%s_p999_ns %lu\n", tag, (unsigned long)v[n * 999 / 1000]);
    printf("%s_max_ns %lu\n", tag, (unsigned long)v[n - 1]);
}

/* ---- packet builder --------------------------------------------- */
static size_t build_packet(uint8_t *buf, uint32_t seq, int nmsg, int salt) {
    size_t off = 0;
    mdp3_packet_header_t ph = {.sequence_number = seq, .sending_time_ns = 1};
    memcpy(buf, &ph, sizeof ph); off += sizeof ph;
    for (int m = 0; m < nmsg; m++) {
        sbe_message_header_t sh = {.block_length = sizeof(mdp3_incremental_refresh_t),
            .template_id = TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK, .schema_id = 1, .version = 1};
        memcpy(buf + off, &sh, sizeof sh); off += sizeof sh;
        mdp3_incremental_refresh_t u;
        memset(&u, 0, sizeof u);
        u.security_id = 1 + ((salt + m) % 16);
        u.side = (salt + m) & 1;
        u.md_price_level = 1;
        u.md_update_action = MD_ACTION_CHANGE;
        u.md_entry_px = 100000000000LL + ((salt + m) % 50) * 1000000LL;
        u.md_entry_size = 100 + ((salt + m) % 10) * 10;
        memcpy(buf + off, &u, sizeof u); off += sizeof u;
    }
    return off;
}

/* ---- 1. parser --------------------------------------------------- */
static uint64_t g_cb_count;
static void count_cb(const decoded_message_t *m, void *c) { (void)m; (void)c; g_cb_count++; }

static void bench_parser(void) {
    pin(CPU_A);
    static seq_tracker_t t;
    for (int nmsg = 1; nmsg <= 16; nmsg *= 4) {
        enum { NPK = 64 };
        static uint8_t pk[NPK][512];
        size_t plen[NPK];
        for (int i = 0; i < NPK; i++) plen[i] = build_packet(pk[i], 0, nmsg, i);
        seq_tracker_init(&t, 1);
        g_cb_count = 0;
        const uint64_t iters = 20000000ull / nmsg;
        uint32_t seq = 1;
        for (int w = 0; w < 100000; w++) { /* warm */
            memcpy(pk[w % NPK], &seq, 4); seq++;
            mdp3_parse_packet(pk[w % NPK], plen[w % NPK], &t, 0, count_cb, NULL);
        }
        g_cb_count = 0;
        uint64_t t0 = mono_ns();
        for (uint64_t i = 0; i < iters; i++) {
            uint8_t *p = pk[i % NPK];
            memcpy(p, &seq, 4); seq++;
            mdp3_parse_packet(p, plen[i % NPK], &t, 0, count_cb, NULL);
        }
        uint64_t dt = mono_ns() - t0;
        double ns_msg = (double)dt / (double)g_cb_count;
        printf("parser_msgs_per_pkt_%d_ns_per_msg %.2f\n", nmsg, ns_msg);
        printf("parser_msgs_per_pkt_%d_ns_per_pkt %.2f\n", nmsg, (double)dt / iters);
        printf("parser_msgs_per_pkt_%d_Mmsg_s %.2f\n", nmsg, 1e3 / ns_msg);
    }
}

/* ---- 2. seq tracker ---------------------------------------------- */
static uint64_t xs = 88172645463325252ull;
static inline uint64_t xr(void) { xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17; return xs; }

static void bench_seq(void) {
    pin(CPU_A);
    static seq_tracker_t t;
    /* speed: in-order stream */
    seq_tracker_init(&t, 1);
    const uint64_t N = 200000000ull;
    uint64_t t0 = mono_ns();
    for (uint64_t i = 1; i <= N; i++) seq_tracker_observe(&t, (uint32_t)i, 0);
    uint64_t dt = mono_ns() - t0;
    printf("seq_observe_ns_per_op %.2f\n", (double)dt / N);

    /* exactness: random drops at several rates, 10M packets each; compare
     * tracker's missing-count against ground truth. Also inject duplicates
     * to ensure they never count as gaps. */
    double rates[] = {0.1, 1.0, 5.0, 20.0};
    uint64_t total_pkts = 0, total_dropped = 0, total_mismatch = 0;
    for (int r = 0; r < 4; r++) {
        seq_tracker_init(&t, 1);
        const uint32_t M = 10000000u;
        uint64_t dropped = 0, last_sent = 0;
        uint32_t thr = (uint32_t)(rates[r] / 100.0 * 4294967295.0);
        for (uint32_t s = 1; s <= M; s++) {
            if ((uint32_t)xr() < thr) { dropped++; continue; }
            seq_tracker_observe(&t, s, 0);
            last_sent = s;
            if ((xr() & 0xFF) == 0) seq_tracker_observe(&t, s, 0); /* dup */
            t.pending_count = 0; /* drain recovery queue (cap would drop reqs, not counts) */
        }
        /* trailing drops after last delivered packet are undetectable by design */
        uint64_t trailing = M - last_sent;
        uint64_t truth = dropped - trailing;
        total_pkts += M; total_dropped += truth;
        if (t.total_missing_messages != truth) total_mismatch++;
        printf("seq_exact_%.1fpct_truth %lu detected %lu\n", rates[r],
               (unsigned long)truth, (unsigned long)t.total_missing_messages);
    }
    printf("seq_exact_total_packets %lu\n", (unsigned long)total_pkts);
    printf("seq_exact_total_gaps_truth %lu\n", (unsigned long)total_dropped);
    printf("seq_exact_mismatched_runs %lu\n", (unsigned long)total_mismatch);
}

/* ---- 3. ring throughput: SPSC vs mutex --------------------------- */
typedef struct { msg_ring_t *r; uint64_t n; } ring_arg_t;

static void *prod_spsc(void *a) {
    ring_arg_t *x = a; pin(CPU_A);
    decoded_message_t m; memset(&m, 0, sizeof m);
    for (uint64_t i = 0; i < x->n; i++) { m.packet_seq = (uint32_t)i; while (!msg_ring_t_push(x->r, &m)) ; }
    return NULL;
}
static void *cons_spsc(void *a) {
    ring_arg_t *x = a; pin(CPU_B);
    decoded_message_t m; uint64_t got = 0;
    while (got < x->n) if (msg_ring_t_pop(x->r, &m)) got++;
    return NULL;
}

/* mutex baseline: same capacity, same payload, plain pthread_mutex */
typedef struct {
    decoded_message_t slots[4096]; uint64_t head, tail; pthread_mutex_t mu; uint64_t n;
} mq_t;
static void *prod_mq(void *a) {
    mq_t *q = a; pin(CPU_A);
    decoded_message_t m; memset(&m, 0, sizeof m);
    for (uint64_t i = 0; i < q->n; ) {
        pthread_mutex_lock(&q->mu);
        if (q->tail - q->head < 4096) { q->slots[q->tail & 4095] = m; q->tail++; i++; }
        pthread_mutex_unlock(&q->mu);
    }
    return NULL;
}
static void *cons_mq(void *a) {
    mq_t *q = a; pin(CPU_B);
    decoded_message_t m; uint64_t got = 0;
    while (got < q->n) {
        pthread_mutex_lock(&q->mu);
        if (q->head != q->tail) { m = q->slots[q->head & 4095]; q->head++; got++; }
        pthread_mutex_unlock(&q->mu);
    }
    return NULL;
}

static void bench_ring(void) {
    const uint64_t N = 50000000ull;
    static msg_ring_t ring;
    printf("msg_slot_bytes %zu\n", sizeof(decoded_message_t));
    for (int rep = 0; rep < 3; rep++) {
        msg_ring_t_init(&ring);
        ring_arg_t a = {&ring, N};
        pthread_t p, c;
        uint64_t t0 = mono_ns();
        pthread_create(&c, NULL, cons_spsc, &a); pthread_create(&p, NULL, prod_spsc, &a);
        pthread_join(p, NULL); pthread_join(c, NULL);
        double s = (double)(mono_ns() - t0) / 1e9;
        printf("ring_spsc_run%d_Mops_s %.2f\n", rep, N / s / 1e6);
    }
    static mq_t q;
    for (int rep = 0; rep < 3; rep++) {
        memset(&q, 0, sizeof q); pthread_mutex_init(&q.mu, NULL); q.n = N / 10; /* 5M: mutex is slow */
        pthread_t p, c;
        uint64_t t0 = mono_ns();
        pthread_create(&c, NULL, cons_mq, &q); pthread_create(&p, NULL, prod_mq, &q);
        pthread_join(p, NULL); pthread_join(c, NULL);
        double s = (double)(mono_ns() - t0) / 1e9;
        printf("ring_mutex_run%d_Mops_s %.2f\n", rep, q.n / s / 1e6);
    }
}

/* ---- 4. ring one-way latency ------------------------------------- */
static void *lat_prod(void *a) {
    ring_arg_t *x = a; pin(CPU_A);
    decoded_message_t m; memset(&m, 0, sizeof m);
    for (uint64_t i = 0; i < x->n; i++) {
        uint64_t t = mono_ns();
        while (mono_ns() - t < 2000) ; /* pace ~2us so ring stays near-empty */
        m.sending_time_ns = mono_ns();
        while (!msg_ring_t_push(x->r, &m)) ;
    }
    return NULL;
}
static uint64_t *g_lat;
static void *lat_cons(void *a) {
    ring_arg_t *x = a; pin(CPU_B);
    decoded_message_t m; uint64_t got = 0;
    while (got < x->n) {
        if (msg_ring_t_pop(x->r, &m)) { g_lat[got++] = mono_ns() - m.sending_time_ns; }
    }
    return NULL;
}
static void bench_ring_lat(void) {
    const uint64_t N = 1000000;
    static msg_ring_t ring; msg_ring_t_init(&ring);
    g_lat = malloc(N * sizeof(uint64_t));
    ring_arg_t a = {&ring, N};
    pthread_t p, c;
    pthread_create(&c, NULL, lat_cons, &a); pthread_create(&p, NULL, lat_prod, &a);
    pthread_join(p, NULL); pthread_join(c, NULL);
    report_pct("ring_handoff", g_lat, N);
    free(g_lat);
}

/* ---- 5. full pipeline: parse -> ring -> book update --------------- */
typedef struct {
    msg_ring_t ring; uint64_t n_msgs; uint64_t arrive_ns; uint64_t *lat; uint64_t drops;
    int msgs_per_pkt; uint64_t rate_pps; /* 0 = unpaced */
} pipe_t;

static void pipe_cb(const decoded_message_t *m, void *c) {
    pipe_t *p = c;
    decoded_message_t tmp = *m; tmp.sending_time_ns = p->arrive_ns; /* "NIC arrival" stamp */
    while (!msg_ring_t_push(&p->ring, &tmp)) p->drops++;
}
typedef struct { int32_t id; int64_t bpx, apx; int32_t bsz, asz; uint64_t upd; } book_t;
static void *pipe_prod(void *a) {
    pipe_t *p = a; pin(CPU_A);
    static seq_tracker_t t; seq_tracker_init(&t, 1);
    enum { NPK = 256 };
    static uint8_t pk[NPK][512]; size_t plen[NPK];
    for (int i = 0; i < NPK; i++) plen[i] = build_packet(pk[i], 0, p->msgs_per_pkt, i);
    uint64_t npk = p->n_msgs / p->msgs_per_pkt;
    uint64_t gap_ns = p->rate_pps ? 1000000000ull / p->rate_pps : 0;
    uint64_t next = mono_ns();
    for (uint64_t i = 0; i < npk; i++) {
        uint8_t *b = pk[i % NPK];
        uint32_t seq = (uint32_t)(i + 1); memcpy(b, &seq, 4);
        if (gap_ns) { next += gap_ns; while (mono_ns() < next) ; }
        if (p->lat) p->arrive_ns = mono_ns();
        mdp3_parse_packet(b, plen[i % NPK], &t, 0, pipe_cb, p);
    }
    return NULL;
}
static void *pipe_cons(void *a) {
    pipe_t *p = a; pin(CPU_B);
    book_t book[16]; memset(book, 0, sizeof book);
    decoded_message_t m; uint64_t got = 0;
    while (got < p->n_msgs) {
        if (!msg_ring_t_pop(&p->ring, &m)) continue;
        if (p->lat) p->lat[got] = mono_ns() - m.sending_time_ns;
        got++;
        const mdp3_incremental_refresh_t *u = &m.book_update;
        book_t *b = &book[u->security_id & 15];
        b->id = u->security_id; b->upd++;
        if (u->side == MD_SIDE_BID) { b->bpx = u->md_entry_px; b->bsz = u->md_entry_size; }
        else                        { b->apx = u->md_entry_px; b->asz = u->md_entry_size; }
    }
    return NULL;
}
static void run_pipe(const char *tag, int mpp, uint64_t rate_pps, uint64_t nmsgs, int want_lat) {
    static pipe_t p;
    memset(&p, 0, sizeof p); msg_ring_t_init(&p.ring);
    p.n_msgs = nmsgs - nmsgs % mpp; p.msgs_per_pkt = mpp; p.rate_pps = rate_pps;
    if (want_lat) p.lat = malloc(p.n_msgs * sizeof(uint64_t));
    pthread_t pr, co;
    uint64_t t0 = mono_ns();
    pthread_create(&co, NULL, pipe_cons, &p); pthread_create(&pr, NULL, pipe_prod, &p);
    pthread_join(pr, NULL); pthread_join(co, NULL);
    double s = (double)(mono_ns() - t0) / 1e9;
    printf("%s_Mmsg_s %.2f\n", tag, p.n_msgs / s / 1e6);
    printf("%s_Mpkt_s %.2f\n", tag, p.n_msgs / mpp / s / 1e6);
    if (want_lat) { report_pct(tag, p.lat, p.n_msgs); free(p.lat); }
}
static void bench_clock(void) {
    uint64_t t0 = mono_ns(), x = 0;
    for (int i = 0; i < 5000000; i++) x += mono_ns();
    printf("clock_gettime_ns_per_call %.1f (sink %lu)\n", (double)(mono_ns() - t0) / 5e6, (unsigned long)(x & 1));
}
static void bench_pipeline(void) {
    bench_clock();
    run_pipe("pipe_throughput", 4, 0, 40000000ull, 0);
    run_pipe("pipe_lat_500kpps", 1, 500000, 2000000ull, 1);
    run_pipe("pipe_lat_1Mpps", 1, 1000000, 4000000ull, 1);
}

/* ---- 6. UDP loopback recvfrom() path ------------------------------ */
static volatile int g_snd_done;
static void *udp_sender(void *a) {
    (void)a; pin(CPU_A);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ad = {.sin_family = AF_INET, .sin_port = htons(26001)};
    inet_pton(AF_INET, "127.0.0.1", &ad.sin_addr);
    uint8_t pk[256]; size_t n = build_packet(pk, 0, 1, 0);
    for (uint32_t s = 1; s <= 3000000u; s++) {
        memcpy(pk, &s, 4);
        sendto(fd, pk, n, 0, (struct sockaddr *)&ad, sizeof ad);
        if ((s & 0x7F) == 0) { struct timespec ts = {0, 20000}; nanosleep(&ts, NULL); }
    }
    g_snd_done = 1; close(fd); return NULL;
}
static void bench_udp(void) {
    pin(CPU_B);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int rb = 64 << 20; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
    struct sockaddr_in ad = {.sin_family = AF_INET, .sin_port = htons(26001)};
    inet_pton(AF_INET, "127.0.0.1", &ad.sin_addr);
    if (bind(fd, (struct sockaddr *)&ad, sizeof ad) < 0) { perror("bind"); return; }
    struct timeval tv = {0, 200000}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static seq_tracker_t t; seq_tracker_init(&t, 1);
    pthread_t s; g_snd_done = 0; pthread_create(&s, NULL, udp_sender, NULL);
    uint8_t buf[2048]; uint64_t pk = 0, recv_ns = 0, parse_ns = 0;
    uint64_t wall0 = mono_ns();
    for (;;) {
        uint64_t a = mono_ns();
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, NULL, NULL);
        uint64_t b = mono_ns();
        if (n < 0) { if (g_snd_done) break; continue; }
        g_cb_count = 0;
        mdp3_parse_packet(buf, (size_t)n, &t, 0, count_cb, NULL);
        uint64_t c = mono_ns();
        recv_ns += b - a; parse_ns += c - b; pk++;
    }
    double wall = (double)(mono_ns() - wall0) / 1e9;
    pthread_join(s, NULL);
    printf("udp_pkts_received %lu\n", (unsigned long)pk);
    printf("udp_recvfrom_ns_per_pkt %.1f\n", (double)recv_ns / pk);
    printf("udp_parse_ns_per_pkt %.1f\n", (double)parse_ns / pk);
    printf("udp_pps_k %.1f\n", pk / wall / 1e3);
    printf("udp_loss_detected %lu\n", (unsigned long)t.total_missing_messages);
}

int main(int argc, char **argv) {
    const char *w = argc > 1 ? argv[1] : "all";
    if (argc > 3) { CPU_A = atoi(argv[2]); CPU_B = atoi(argv[3]); }
    int all = !strcmp(w, "all");
    if (all || !strcmp(w, "parser")) bench_parser();
    if (all || !strcmp(w, "seq")) bench_seq();
    if (all || !strcmp(w, "ring")) bench_ring();
    if (all || !strcmp(w, "lat")) bench_ring_lat();
    if (all || !strcmp(w, "pipe")) bench_pipeline();
    if (all || !strcmp(w, "udp")) bench_udp();
    return 0;
}
