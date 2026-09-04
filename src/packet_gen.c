/*
 * packet_gen.c
 *
 * Test traffic generator. Emits MDP3-shaped UDP packets (packet header
 * + one incremental-refresh SBE message each) to a target host:port
 * with a strictly increasing sequence number, then -- to actually
 * exercise the gap detector rather than just prove the happy path --
 * randomly *withholds* a configurable fraction of packets before
 * sending (simulating NIC/switch drops on the multicast feed) and
 * occasionally fires a channel-reset message.
 *
 * This does NOT talk AF_XDP; it's a bog-standard UDP sender, used to
 * drive the --mode sim feed handler (or, on a real box, an actual
 * multicast group that a real AF_XDP-based handler listens to).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mdp3_proto.h"

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
    const char *dst_addr = "127.0.0.1";
    uint16_t dst_port = 15000;
    int num_packets = 1000;
    double drop_pct = 2.0;       /* percent of packets silently withheld */
    int reset_every = 0;         /* 0 = never inject a channel reset */
    int usleep_between = 1000;   /* microseconds between sends */
    unsigned seed = (unsigned)time(NULL);

    static struct option opts[] = {
        {"dst", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"count", required_argument, 0, 'n'},
        {"drop-pct", required_argument, 0, 'r'},
        {"reset-every", required_argument, 0, 'R'},
        {"sleep-us", required_argument, 0, 's'},
        {"seed", required_argument, 0, 'S'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:p:n:r:R:s:S:", opts, NULL)) != -1) {
        switch (c) {
            case 'd': dst_addr = optarg; break;
            case 'p': dst_port = (uint16_t)atoi(optarg); break;
            case 'n': num_packets = atoi(optarg); break;
            case 'r': drop_pct = atof(optarg); break;
            case 'R': reset_every = atoi(optarg); break;
            case 's': usleep_between = atoi(optarg); break;
            case 'S': seed = (unsigned)strtoul(optarg, NULL, 10); break;
        }
    }
    srand(seed);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dst_port);
    inet_pton(AF_INET, dst_addr, &addr.sin_addr);

    uint32_t seq = 1;
    int sent = 0, dropped = 0, resets = 0;

    for (int i = 0; i < num_packets; i++) {
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

            seq++;
        }

        double roll = (rand() / (double)RAND_MAX) * 100.0;
        if (roll < drop_pct) {
            dropped++;
            /* deliberately do NOT sendto() -- simulates a dropped packet */
        } else {
            sendto(fd, buf, off, 0, (struct sockaddr *)&addr, sizeof(addr));
            sent++;
        }

        if (usleep_between > 0) usleep(usleep_between);
    }

    fprintf(stderr, "[gen] done: sent=%d dropped=%d (%.1f%% target) resets=%d seed=%u\n",
            sent, dropped, drop_pct, resets, seed);
    close(fd);
    return 0;
}
