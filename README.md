# High-Throughput User-Space Market Data Feed Handler (AF_XDP Kernel Bypass)

A UDP multicast market-data feed handler that reads packets directly off
the NIC's ring buffers via **AF_XDP** (eBPF-driven kernel bypass),
decodes an MDP3-style sequenced binary protocol, detects and reports
sequence gaps, and hands decoded messages to a processing thread through
a lock-free ring buffer. On a NIC with native zero-copy support the RX
path has no per-packet syscall, skb allocation or kernel-to-user copy;
in generic (skb) mode, which is what was verified on veth, the stack's
UDP/socket layer is still bypassed but the kernel copies each frame into
UMEM. The processing thread currently sleeps (`usleep(50)`) when idle
rather than busy-polling.

## Why this exists

Standard `recv()` on a UDP socket costs you: an interrupt, an skb
allocation, a copy from skb to socket buffer, a syscall, and a copy
from socket buffer to your buffer — per packet, on the critical path of
every book update. At multicast feed rates (CME MDP3 alone can burst
well past 5M msgs/sec across all channels), that overhead dominates.
AF_XDP eliminates it by letting a small eBPF program in the NIC driver's
XDP hook redirect packets straight into a memory-mapped ring shared with
user space — the packet is DMA'd by the NIC directly into a buffer this
process already owns.

## Architecture

```
                    NIC ring buffer
                          |
                          v
        +--------------------------------+
        |  xdp_kern.c (eBPF, in-kernel)   |   <-- runs in the driver's XDP hook,
        |  parse Eth/IP/UDP, match port,  |       before any skb exists
        |  bpf_redirect_map -> XSKMAP     |
        +--------------------------------+
                          |
                 zero-copy DMA into UMEM
                          v
        +--------------------------------+
        |  afxdp_socket.c (user space)    |   <-- raw if_xdp.h UAPI, no libxdp
        |  UMEM + fill/comp/RX/TX rings   |
        +--------------------------------+
                          |
                          v
        +--------------------------------+
        |  mdp3_parser.c                  |   <-- walks packet header +
        |  + seq_tracker.c (gap detect)   |       repeating SBE messages
        +--------------------------------+
                          |
                push (memcpy, no malloc)
                          v
        +--------------------------------+
        |  spsc_ring.h (lock-free SPSC)   |   <-- cache-line padded,
        +--------------------------------+       acquire/release atomics
                          |
                pop (processing thread)
                          v
        +--------------------------------+
        |  feed_handler.c: book update,   |
        |  latency stats, recovery queue  |
        +--------------------------------+
```

Two RX backends feed the *identical* downstream pipeline
(`mdp3_parse_packet` → `seq_tracker` → `spsc_ring` → processing thread):

| Mode    | Path                                   | Requirements                                   |
|---------|-----------------------------------------|-------------------------------------------------|
| `afxdp` | Kernel-bypass via AF_XDP (zero-copy on supporting NICs, copy mode otherwise) | root, clang (BPF backend) + libbpf |
| `sim`   | Plain UDP socket (loopback or otherwise)| none — runs anywhere                             |

The `sim` backend isn't a toy stand-in bolted on for show — it exists so
the actual business logic (protocol parsing, gap detection, lock-free
handoff, book updates) can be built, run, and correctness-tested
deterministically without needing a kernel-bypass-capable box or real
multicast market data in front of you, including in CI.

## Design decisions worth discussing in an interview

- **Why AF_XDP over DPDK**: DPDK fully unbinds the NIC from the kernel
  driver (via UIO/VFIO), which means the box stops being a normal Linux
  host for that interface — no `ip`, no normal sockets on it, dedicated
  hugepages, a poll-mode driver replacing the kernel driver entirely.
  AF_XDP coexists with the kernel networking stack: this program can
  bypass the stack for the multicast feed on one queue while SSH, the
  TCP recovery-feed connection, and everything else on the box keeps
  working normally through the regular stack. For a single feed handler
  process on a shared box, that's usually the right tradeoff; DPDK wins
  when you're dedicating whole NICs/cores to packet processing.

- **Why hand-roll the AF_XDP socket setup instead of using libxdp's
  `xsk.h`**: partly to demonstrate the actual mechanism (UMEM
  registration, the four rings, `XDP_MMAP_OFFSETS`) rather than a
  library call, and partly practical — it means `afxdp_socket.c` has
  *zero* build dependencies beyond glibc + kernel UAPI headers and
  compiles in this sandbox right now (verified below). The one place
  this project does pull in libbpf (`xdp_loader.c`) is for loading and
  verifying raw eBPF bytecode into the kernel — reimplementing the BPF
  verifier invocation and ELF loading by hand isn't worthwhile.

- **Why a separate eBPF redirect program instead of relying on driver
  defaults**: there is no default "send UDP port X to this socket"
  behavior in AF_XDP — the `xsks_map` XSKMAP + `bpf_redirect_map` call
  in `xdp_kern.c` *is* the mechanism. It also means the redirect
  decision (which packets bypass the stack) is explicit, auditable, and
  narrow: only the configured UDP port (optionally scoped to one multicast
  group and VLAN) is redirected; everything else gets `XDP_PASS` and behaves
  like a normal Linux host.

- **Why gaps are detected but not silently patched**: on seeing
  `seq > expected`, the tracker records the missing range and the book is
  marked stale rather than just resetting `expected_seq` and moving on.
  Recovery runs on a separate reliable transport (TCP snapshot fetch, on its
  own thread) because the fast feed and the recovery feed have different
  reliability guarantees, and conflating them in one code path is a common
  source of subtle correctness bugs in real feed handlers. While stale,
  live updates are buffered, not applied; after the snapshot loads they are
  replayed on top of it (see "Book and sequencing semantics").

- **Why SPSC, not MPMC, for the ring buffer**: there is exactly one RX
  thread and one processing thread. A general MPMC ring buffer needs
  either locks or more expensive CAS loops on the hot path; SPSC needs
  only a relaxed load + acquire/release store handshake on head/tail,
  each padded to its own cache line to avoid false sharing between the
  two cores. If this were extended to multiple RX queues (multiple
  cores, multiple AF_XDP sockets), the right move is one SPSC ring per
  RX thread feeding a single processing thread, or partitioning
  instruments across processing threads — not a shared MPMC ring.

## Building

### `sim` mode — builds and runs anywhere, no privileges needed

```sh
make sim
./build/feed_handler_sim --mode sim --bind 127.0.0.1 --port 15000 --channel 1
# in another terminal:
./build/packet_gen --dst 127.0.0.1 --port 15000 --count 5000 --drop-pct 2 --sleep-us 200
```

Run the test suite (`make test`, no root):
- unit tests: gap detector (incl. `uint32` wraparound) and the depth book;
- gap-count integration test: real UDP traffic with a known drop rate, asserts the
  detected missing count equals what the generator withheld;
- **recovery end-to-end** (`tests/run_recovery_test.sh`): `packet_gen` acts as the
  exchange (drops packets, replays stale ones, serves snapshots over TCP) and the
  handler's final book must be identical to the generator's ground truth. Covers
  loss, duplicates, and ring overflow forced by `--slow-consumer-us`.

```sh
make test
```

Recovery in sim mode:

```sh
./build/feed_handler_sim --mode sim --port 15000 --recovery 127.0.0.1:15001
./build/packet_gen --dst 127.0.0.1 --port 15000 --count 20000 --drop-pct 3 \
    --dup-pct 5 --drop-tail 12 --serve-port 15001 --serve-ms 3000
# both print `[book] inst=..` lines at exit; they must match
```

### `afxdp` mode — the real kernel-bypass deliverable

Requires a Linux box with:
- `clang`/`llvm` (to compile the eBPF object)
- `libbpf-dev` (headers + library for the loader)
- a NIC/driver with native XDP support for true zero-copy (falls back
  to generic/SKB mode on any driver, which still works but allocates
  an skb first, losing the "no kernel copy" property)
- root / `CAP_NET_ADMIN` + `CAP_NET_RAW`

```sh
make afxdp
sudo ./build/feed_handler_afxdp --mode afxdp --iface eth0 --queue 0 \
    --bpf build/xdp_kern.o --channel 1 --port 15000 --group 239.1.1.1 --vlan 100
# add --skb to force generic mode on drivers without native XDP support
# --group / --vlan are optional: omit to match any destination IP / any VLAN
```

The eBPF program matches UDP destination port, and optionally the IPv4
destination (multicast group) and the outer 802.1Q VLAN id, through a config
map the loader fills in; it parses up to two stacked VLAN tags (802.1Q/802.1ad).
Non-matching traffic is `XDP_PASS`ed to the normal stack.

Real-kernel tests on a veth pair (root): `sudo bash tests/run_afxdp_veth_test.sh`
checks group match/mismatch, VLAN match/wrong-VID/untagged, and snapshot recovery
over AF_XDP. Latency sim-vs-AF_XDP: `sudo bash bench/run_veth_latency.sh`.

> Verified on a Linux 6.x kernel (WSL2) over a veth pair in generic (skb)
> XDP mode: the eBPF program loads, redirects through the XSKMAP, and the
> hand-written UMEM/ring code receives and parses the packets. Use
> `make afxdp CLANG=clang-14` if your default `clang` lacks the BPF backend.
> Native zero-copy mode needs a NIC whose driver supports `XDP_ZEROCOPY`
> (veth does not). Benchmarks and caveats are in [bench/RESULTS.md](bench/RESULTS.md).

## Book and sequencing semantics

- **Duplicates** (`seq` behind the expected value) are counted and skipped, never applied twice.
- **Sequence numbers use serial-number arithmetic**, so the `uint32` wrap is not
  misread as a stale packet or a huge gap.
- **Gaps and ring-full drops mark the channel's books stale** (`book_stale`), recording
  the sequence number a snapshot must reach (`need_seq`).
- **Snapshot recovery** (`--recovery host:port`): a recovery thread fetches a full book
  snapshot over TCP ([recovery_proto.h](include/recovery_proto.h)). While stale, live
  updates are buffered (not applied). A snapshot is accepted only if its `last_seq >=
  need_seq`; otherwise it is refetched. The snapshot is loaded, buffered updates with
  `seq > last_seq` are replayed on top, and the stale flag clears. Messages still queued
  in the ring that are older than the snapshot are skipped so they cannot overwrite it.
  Without `--recovery`, updates keep being applied but the book stays flagged stale
  until a channel reset.
- **Depth book (10 levels per side)** with MDP3 incremental semantics: NEW inserts and
  shifts deeper levels down, CHANGE overwrites, DELETE removes and promotes the levels
  below it ([order_book.h](include/order_book.h), tested in `tests/test_order_book.c`).
- **User-space frame parsing** honours IPv4 IHL and up to two VLAN tags, and matches the
  eBPF program's view of the frame.
- **Latency** is measured from the sender's timestamp in the packet header (set just
  before `sendto`) to the moment the processing thread pops the message. On the veth
  test that covers the send syscall, the veth hop, the kernel receive path and the
  ring handoff. It excludes NIC/PHY time; true NIC-to-user latency needs hardware
  timestamps. `--busy-poll` and `--cpu-rx/--cpu-proc` remove the idle `usleep`.

## What's simplified vs. a production system

This is scoped for an interview deliverable, not a drop-in production
feed handler. Explicitly out of scope, worth naming if asked:

- Recovery is snapshot-only, against the test exchange in `packet_gen`; it
  speaks a simple private protocol, not CME's real snapshot channel
  (template 38) or TCP replay, and fetches one snapshot per detected gap.
- The book is a fixed 10-level price-level book, not an order-by-order book, and
  the test exchange only emits level-1 updates.
- No TX path: the TX and completion rings are mapped (the kernel requires it) but unused.
- Core pinning is available (`--cpu-rx`, `--cpu-proc`) but there is no NUMA-aware
  allocation, `SCHED_FIFO`, or NIC interrupt-affinity tuning.
- The idle processing thread uses `usleep(50)` unless `--busy-poll` is set.
- Single RX queue / single AF_XDP socket. Scaling to line rate on a
  multi-queue NIC means one AF_XDP socket + one pinned RX thread per
  queue, with RSS (or a custom eBPF hash) steering flows to queues.
