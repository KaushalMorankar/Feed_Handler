/*
 * packet_gen.c
 *
 * Test "exchange". Emits MDP3-shaped UDP packets (packet header + one SBE
 * message each) with a strictly increasing sequence number and then
 * deliberately misbehaves, so every recovery path in the feed handler
 * gets exercised end to end:
 *
 *   --drop-pct   withhold a fraction of packets (simulated multicast loss)
 *   --drop-tail  always drop the last N data packets (makes a missed update
 *                visible in the final book, so recovery is provably needed)
 *   --dup-pct    re-send an OLD, already-delivered packet (stale replay).
 *                Its payload is the original one, so a handler that applies
 *                it a second time overwrites newer state and ends up with
 *                a book that differs from ours -- duplicates are detectable.
 *   --reset-every inject a channel reset every N packets
 *   --rate-pps   busy-wait paced sending (precise, for latency tests)
 *
 * It also keeps a shadow of the true book state and:
 *   - serves it as a snapshot over TCP (--serve-port), the stand-in for
 *     the exchange's reliable recovery channel, and
 *   - prints it at exit as `[book] inst=.. bid=sz@px ask=sz@px`, in the
 *     same format the handler prints, so a test can diff the two.
 *
 * A final heartbeat (never dropped) carries the next sequence number, so
 * a gap at the very tail of the run is still detectable -- exactly what a
 * real exchange's heartbeats are for.
 *
 * Plain UDP sender: no AF_XDP here. Works against --mode sim or against
 * a real multicast group that an AF_XDP handler listens to.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <poll.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mdp3_proto.h"
#include "recovery_proto.h"

#define MAX_SEC_ID 8
#define HIST 16

typedef struct { int64_t px; int32_t sz; int have; } shadow_level_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static shadow_level_t g_shadow[MAX_SEC_ID][2];  /* [security_id][side], level 1 only */
static uint32_t g_last_seq;                      /* last data seq reflected in shadow */
static volatile int g_stop;
static int g_snapshots_served;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- snapshot server (the "recovery channel") ------------------- */

static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void serve_one(int cfd) {
    struct timeval tv = {.tv_sec = 1};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req;
    if (read(cfd, &req, 1) != 1 || req != 'S') return;

    snapshot_entry_t entries[MAX_SEC_ID * 2];
    snapshot_header_t hdr;
    uint32_t n = 0;

    pthread_mutex_lock(&g_mu);
    hdr.last_seq = g_last_seq;
    for (int id = 1; id < MAX_SEC_ID; id++) {
        for (int side = 0; side < 2; side++) {
            if (!g_shadow[id][side].have) continue;
            memset(&entries[n], 0, sizeof entries[n]);
            entries[n].security_id = id;
            entries[n].side = (uint8_t)side;
            entries[n].level = 1;
            entries[n].px = g_shadow[id][side].px;
            entries[n].size = g_shadow[id][side].sz;
            n++;
        }
    }
    pthread_mutex_unlock(&g_mu);
    hdr.count = n;

    if (write_full(cfd, &hdr, sizeof hdr) == 0)
        write_full(cfd, entries, n * sizeof entries[0]);
    g_snapshots_served++;
}

static void *server_main(void *arg) {
    int port = *(int *)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(lfd, 8) < 0) {
        perror("snapshot server bind/listen");
        close(lfd);
        return NULL;
    }
    while (!g_stop) {
        struct pollfd pfd = {.fd = lfd, .events = POLLIN};
        if (poll(&pfd, 1, 100) <= 0) continue;
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        serve_one(cfd);
        close(cfd);
    }
    close(lfd);
    return NULL;
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *dst_addr = "127.0.0.1";
    uint16_t dst_port = 15000;
    int num_packets = 1000;
    double drop_pct = 2.0;
    double dup_pct = 0.0;
    int drop_tail = 0;
    int reset_every = 0;
    int usleep_between = 1000;
    long rate_pps = 0;
    int serve_port = 0;
    int serve_ms = 0;
    int cpu = -1;
    unsigned seed = (unsigned)time(NULL);

    static struct option opts[] = {
        {"dst", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"count", required_argument, 0, 'n'},
        {"drop-pct", required_argument, 0, 'r'},
        {"dup-pct", required_argument, 0, 'D'},
        {"drop-tail", required_argument, 0, 'L'},
        {"reset-every", required_argument, 0, 'R'},
        {"sleep-us", required_argument, 0, 's'},
        {"rate-pps", required_argument, 0, 'P'},
        {"serve-port", required_argument, 0, 'T'},
        {"serve-ms", required_argument, 0, 'M'},
        {"cpu", required_argument, 0, 'C'},
        {"seed", required_argument, 0, 'S'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:p:n:r:D:L:R:s:P:T:M:C:S:", opts, NULL)) != -1) {
        switch (c) {
            case 'L': drop_tail = atoi(optarg); break;
            case 'd': dst_addr = optarg; break;
            case 'p': dst_port = (uint16_t)atoi(optarg); break;
            case 'n': num_packets = atoi(optarg); break;
            case 'r': drop_pct = atof(optarg); break;
            case 'D': dup_pct = atof(optarg); break;
            case 'R': reset_every = atoi(optarg); break;
            case 's': usleep_between = atoi(optarg); break;
            case 'P': rate_pps = atol(optarg); break;
            case 'T': serve_port = atoi(optarg); break;
            case 'M': serve_ms = atoi(optarg); break;
            case 'C': cpu = atoi(optarg); break;
            case 'S': seed = (unsigned)strtoul(optarg, NULL, 10); break;
        }
    }
    srand(seed);

    if (cpu >= 0) {
        cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
        sched_setaffinity(0, sizeof s, &s);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    inet_pton(AF_INET, dst_addr, &addr.sin_addr);

    pthread_t srv;
    int have_srv = 0;
    if (serve_port > 0) {
        have_srv = (pthread_create(&srv, NULL, server_main, &serve_port) == 0);
    }

    uint8_t hist[HIST][128];
    size_t hist_len[HIST];
    int hist_n = 0, hist_pos = 0;

    uint32_t seq = 1;
    int sent = 0, dropped = 0, resets = 0, dups = 0;
    uint64_t gap_ns = rate_pps > 0 ? 1000000000ull / (uint64_t)rate_pps : 0;
    uint64_t next_send = mono_ns();

    for (int i = 0; i < num_packets; i++) {
        if (gap_ns) {
            next_send += gap_ns;
            while (mono_ns() < next_send) { /* busy-wait for precise pacing */ }
        }

        uint8_t buf[128];
        size_t off = 0;

        mdp3_packet_header_t pkt_hdr;
        pkt_hdr.sequence_number = seq;
        pkt_hdr.sending_time_ns = now_ns();
        memcpy(buf + off, &pkt_hdr, sizeof(pkt_hdr));
        off += sizeof(pkt_hdr);

        int do_reset = (reset_every > 0 && i > 0 && i % reset_every == 0);

        sbe_message_header_t sbe_hdr;
        memset(&sbe_hdr, 0, sizeof(sbe_hdr));
        sbe_hdr.schema_id = 1;
        sbe_hdr.version = 1;

        if (do_reset) {
            sbe_hdr.template_id = TEMPLATE_ID_CHANNEL_RESET;
            sbe_hdr.block_length = sizeof(mdp3_channel_reset_t);
            memcpy(buf + off, &sbe_hdr, sizeof(sbe_hdr));
            off += sizeof(sbe_hdr);
            mdp3_channel_reset_t reset_body = { .reset_channel_id = 1 };
            memcpy(buf + off, &reset_body, sizeof(reset_body));
            off += sizeof(reset_body);
            resets++;
            pthread_mutex_lock(&g_mu);
            memset(g_shadow, 0, sizeof g_shadow);
            g_last_seq = 0;
            pthread_mutex_unlock(&g_mu);
            seq = 1; /* generator's own seq resets to match the reset it just sent */
        } else {
            sbe_hdr.template_id = TEMPLATE_ID_MD_INCREMENTAL_REFRESH_BOOK;
            sbe_hdr.block_length = sizeof(mdp3_incremental_refresh_t);
            memcpy(buf + off, &sbe_hdr, sizeof(sbe_hdr));
            off += sizeof(sbe_hdr);

            mdp3_incremental_refresh_t upd;
            memset(&upd, 0, sizeof(upd));
            upd.security_id = 1 + (i % 3);                 /* 3 synthetic instruments */
            upd.side = (i % 2 == 0) ? MD_SIDE_BID : MD_SIDE_ASK;
            upd.md_price_level = 1;
            upd.md_update_action = MD_ACTION_CHANGE;
            upd.md_entry_px = 100000000000LL + (i % 50) * 1000000LL; /* fake fixed-point px */
            upd.md_entry_size = 100 + (i % 10) * 10;
            memcpy(buf + off, &upd, sizeof(upd));
            off += sizeof(upd);

            /* shadow = ground truth, updated whether or not the packet is dropped */
            pthread_mutex_lock(&g_mu);
            shadow_level_t *sl = &g_shadow[upd.security_id][upd.side];
            sl->px = upd.md_entry_px; sl->sz = upd.md_entry_size; sl->have = 1;
            g_last_seq = seq;
            pthread_mutex_unlock(&g_mu);

            seq++;
        }

        double roll = (rand() / (double)RAND_MAX) * 100.0;
        int force_drop = (drop_tail > 0 && i >= num_packets - drop_tail);
        if (force_drop || roll < drop_pct) {
            dropped++;
            /* deliberately do NOT sendto() -- simulates a dropped packet */
        } else {
            sendto(fd, buf, off, 0, (struct sockaddr *)&addr, sizeof(addr));
            sent++;
            memcpy(hist[hist_pos], buf, off);
            hist_len[hist_pos] = off;
            hist_pos = (hist_pos + 1) % HIST;
            if (hist_n < HIST) hist_n++;

            /* stale replay of an older delivered packet (never the one just sent) */
            if (dup_pct > 0.0 && hist_n >= 2 &&
                (rand() / (double)RAND_MAX) * 100.0 < dup_pct) {
                int back = 1 + rand() % (hist_n - 1);           /* 1..hist_n-1 packets ago */
                int idx = (hist_pos - 1 - back + 2 * HIST) % HIST;
                sendto(fd, hist[idx], hist_len[idx], 0, (struct sockaddr *)&addr, sizeof(addr));
                dups++;
            }
        }

        if (!gap_ns && usleep_between > 0) usleep(usleep_between);
    }

    /* Tail replay: a mid-run duplicate only damages a price level until its
     * next real update, which would let a handler that wrongly applies
     * duplicates still finish with the right book. Re-sending the whole
     * recent window right at the end makes that bug visible in the final
     * state: applying these would roll every level back to old values. */
    if (dup_pct > 0.0) {
        for (int k = 0; k < hist_n; k++) {
            int idx = (hist_pos - 1 - k + 2 * HIST) % HIST;     /* newest first */
            sendto(fd, hist[idx], hist_len[idx], 0, (struct sockaddr *)&addr, sizeof(addr));
            dups++;
        }
    }

    /* Final heartbeat, never dropped: lets the receiver notice a gap at the tail. */
    {
        uint8_t buf[32];
        mdp3_packet_header_t ph = {.sequence_number = seq, .sending_time_ns = now_ns()};
        sbe_message_header_t sh = {.block_length = 0, .template_id = TEMPLATE_ID_HEARTBEAT,
                                   .schema_id = 1, .version = 1};
        memcpy(buf, &ph, sizeof ph);
        memcpy(buf + sizeof ph, &sh, sizeof sh);
        sendto(fd, buf, sizeof ph + sizeof sh, 0, (struct sockaddr *)&addr, sizeof(addr));
        /* snapshots must also cover the heartbeat's seq, or a handler that
         * lost the heartbeat to ring overflow could never be satisfied */
        pthread_mutex_lock(&g_mu);
        g_last_seq = seq;
        pthread_mutex_unlock(&g_mu);
    }

    /* Keep serving snapshots for a while so a recovering handler can fetch one. */
    if (have_srv && serve_ms > 0) {
        for (int waited = 0; waited < serve_ms; waited += 50) usleep(50000);
    }
    g_stop = 1;
    if (have_srv) pthread_join(srv, NULL);

    fprintf(stderr, "[gen] done: sent=%d dropped=%d (%.1f%% target) resets=%d dups=%d "
                    "snapshots_served=%d seed=%u\n",
            sent, dropped, drop_pct, resets, dups, g_snapshots_served, seed);

    pthread_mutex_lock(&g_mu);
    for (int id = 1; id < MAX_SEC_ID; id++) {
        if (!g_shadow[id][0].have && !g_shadow[id][1].have) continue;
        fprintf(stderr, "[book] inst=%d bid=%d@%ld ask=%d@%ld\n", id,
                g_shadow[id][MD_SIDE_BID].sz, (long)g_shadow[id][MD_SIDE_BID].px,
                g_shadow[id][MD_SIDE_ASK].sz, (long)g_shadow[id][MD_SIDE_ASK].px);
    }
    pthread_mutex_unlock(&g_mu);
    close(fd);
    return 0;
}
