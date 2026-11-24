# Benchmark results

Reproduce: `make bench` (Linux, no root). Source: [bench.c](bench.c). It links the
real `mdp3_parser.c`, `seq_tracker.c` and `spsc_ring.h`.

**Machine:** Intel i7-12650H, WSL2 (Linux 6.18, gcc 11.4, `-O2`), producer and
consumer pinned to two distinct physical cores. WSL2 runs under Hyper-V, so
tail latencies include hypervisor preemption. Expect cleaner tails on bare metal.

## Real AF_XDP vs kernel UDP socket (veth, same handler binary logic)

Setup: veth pair, generator in a separate network namespace blasting 3M packets
with `--sleep-us 0 --drop-pct 0`, so any gap is **receiver-side loss**. The
handler ran once in `--mode sim` (ordinary UDP socket) and once in
`--mode afxdp --skb` (eBPF XDP redirect into an XSKMAP, hand-written UMEM and
rings). Three runs each, 3M packets per run.

| | UDP socket (`sim`) | AF_XDP (`afxdp`, generic/skb mode) |
|---|---|---|
| Time to push 3M packets | 10.0 to 10.4 s | 2.66 to 2.76 s |
| Sustained rate | about 290k pkt/s | at least 1.1M pkt/s (sender-limited) |
| Packets lost at the receiver | 4,847 / 5,747 / 15,493 (0.16 to 0.52%) | **0 of 9,000,000** |

Functional check (2,000 packets, 3% deliberate drop, seed 42): generator dropped
73, handler reported `missing_msgs=73` (1927 received), an exact match.

Caveats:
- This is **generic (skb) XDP mode**, not driver-native zero-copy: veth does not
  support `XDP_ZEROCOPY`, so the kernel still builds an skb and copies into UMEM.
  The win shown here is bypassing the UDP/IP stack and socket layer. Native
  zero-copy needs a real NIC.
- The AF_XDP side is sender-limited, so 1.1M pkt/s is a lower bound.
- I did not compare end-to-end latency between the two modes. The handler's
  processing thread does `usleep(50)` when idle, which dominates e2e latency
  (about 70 to 110 us) and is a function of arrival rate, so it would not isolate
  the receive path.
- Build with `make afxdp CLANG=clang-14` (the `clang` on this WSL has no BPF backend).

## Send-to-process latency over the veth link (`bench/run_veth_latency.sh`)

Timestamp taken by `packet_gen` just before `sendto()`, compared with the clock when
the processing thread pops the message. This includes the sender syscall, the veth
hop, the kernel receive path (UDP stack vs XDP redirect) and the SPSC handoff. It
excludes NIC/PHY time (veth has none). Both handlers `--busy-poll` on pinned cores;
generator paced at 50k pkt/s, 200k packets, 3 runs each, zero loss in every run.

| | p50 (3 runs) | p99 | p99.9 |
|---|---|---|---|
| UDP socket | 3.4 / 3.6 / 3.6 us | 78 to 121 us | 0.5 to 5.5 ms |
| AF_XDP (generic mode) | **2.1 / 2.1 / 2.1 us** | 81 to 214 us | 0.6 to 4.0 ms |

The median gap (about 1.5 us, roughly 40% lower) is consistent. The tails are
dominated by WSL2/Hyper-V scheduling jitter and are **not** distinguishable
between the two paths, so no tail claim is made.

## Recovery and filter correctness (real kernel, veth)

`tests/run_recovery_test.sh` (sim) and `tests/run_afxdp_veth_test.sh` (AF_XDP, root):

- Final handler book is **identical to the generator's ground truth** after random
  loss (3%), stale-packet replays, deterministic tail loss, and ring overflow forced by
  a throttled consumer. Verified both in sim mode and over AF_XDP
  (541 snapshots applied in the AF_XDP loss+dups run, every stale duplicate skipped).
- Negative controls: with duplicate-skipping disabled the dups-only run produces a
  different book; with recovery disabled the tail-loss run produces a different book.
  So the tests can fail.
- eBPF filter: matching multicast group redirected (received = sent + heartbeat, gap
  count = drops); other group, wrong VLAN id, and untagged-when-VLAN-required all
  redirected 0 packets.

Caveat: the exchange side is `packet_gen`, with a private snapshot protocol and
level-1 updates only, not CME's real recovery channel.

## Results (user-space hot path)

| Component | Metric | Result |
|---|---|---|
| MDP3/SBE parser | ns per message (1 / 4 / 16 msgs per packet) | 4.4 / 2.6 / 2.0 ns |
| MDP3/SBE parser | throughput | 226 / 392 / 504 M msg/s (single core, L1-hot) |
| Sequence tracker | cost per packet (wraparound-safe compare) | 1.9 ns |
| Sequence tracker | gap accounting, 40M packets, 0.1 to 20% random loss, with injected duplicates | 2,608,803 / 2,608,803 gaps detected, 0 mismatches |
| SPSC ring (cross-core) | throughput, ring saturated | 122 to 146 M ops/s |
| Mutex-queue baseline | same payload and capacity | 7.6 to 8.1 M ops/s |
| SPSC ring vs mutex | ratio | about 16x |
| Cross-core handoff | p50 latency, producer to consumer | about 155 to 165 ns |
| Full pipeline (parse, ring, book update) | sustained throughput, 4 msgs/packet | 21 to 24 M msg/s (5.4 to 6.0 M pkt/s) |
| Full pipeline | p50 latency at 0.5 and 1 M pkt/s | 163 / 168 ns |
| Full pipeline | p99 latency | 15 to 18 us (WSL2/Hyper-V jitter) |
| Kernel UDP baseline | `recvfrom()` cost per packet, loopback | 4,583 ns |
| Kernel UDP baseline | max single-core rate | 212 k pkt/s |

The pipeline latency is measured from just before `mdp3_parse_packet` to
consumer pop, so it excludes NIC and wire time.

## Caveats

- The parser numbers use a small, L1-resident packet set and a no-op callback. They are a
  ceiling, not a production figure.
- The 122 to 146 M ops/s ring figure is the saturated regime, where the producer and consumer are
  far apart in the ring. The realistic pipeline figure is the 21 to 24 M msg/s one,
  where they share cache lines.
- The mutex baseline is a plain `pthread_mutex` queue, not a tuned MPMC queue.
