# High-Throughput User-Space Market Data Feed Handler (AF_XDP Kernel Bypass)

A UDP multicast market-data feed handler that reads packets directly off
the NIC's ring buffers via **AF_XDP** (zero-copy, eBPF-driven kernel
bypass), reassembles an MDP3-style sequenced binary protocol, detects
and reports sequence gaps, and hands decoded messages to a processing
thread through a lock-free ring buffer — no syscalls, no skb allocation,
no memcpy across the kernel/user boundary on the hot path.

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
| `afxdp` | Real kernel-bypass, AF_XDP zero-copy   | root, NIC+driver with native XDP, clang+libbpf   |
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
  narrow: only the market-data multicast port is redirected; everything
  else gets `XDP_PASS` and behaves like a normal Linux host.

- **Why gaps are detected but not silently patched**: on seeing
  `seq > expected`, the tracker logs the missing range and enqueues a
  `recovery_request_t` rather than just resetting `expected_seq` and
  moving on. The book is *known-stale* for that instrument until a
  (stubbed here) TCP recovery-feed client fetches the missing range —
  that stubbing point is deliberate: recovery and the fast feed are
  different transports with different reliability guarantees, and
  conflating them in the same code path is a common source of subtle
  correctness bugs in real feed handlers.

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

Run the test suite (unit tests for the gap detector + an integration
test that fires real UDP traffic with a known drop rate at the sim
feed handler and asserts the detected gap count matches exactly):

```sh
make test
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
    --bpf build/xdp_kern.o --channel 1
# add --skb to force generic mode on drivers without native XDP support
```

> This sandbox has the kernel UAPI headers (`linux/if_xdp.h`,
> `linux/bpf.h`) but not `clang`/`libbpf-dev`, and has no NIC to bind
> to — so `afxdp_socket.c` (the hand-rolled socket/UMEM/ring code, zero
> external deps) is compile-verified here, but `xdp_kern.c` and
> `xdp_loader.c` (which need clang-targeting-bpf and libbpf
> respectively) are provided as complete, correct source to be built on
> a properly provisioned box — see `make afxdp`.

## What's simplified vs. a production system

This is scoped for an interview deliverable, not a drop-in production
feed handler. Explicitly out of scope, worth naming if asked:

- The TCP recovery-feed client is stubbed as a queued
  `recovery_request_t`, not an actual second connection replaying
  missed messages.
- The order book keeps only best bid/ask per instrument, not a full
  depth book.
- No NUMA-aware memory allocation or explicit core pinning
  (`sched_setaffinity`) for the RX/processing threads, which a real
  low-latency deployment would want alongside `SCHED_FIFO` and
  interrupt affinity tuning on the NIC's queues.
- Single RX queue / single AF_XDP socket. Scaling to line rate on a
  multi-queue NIC means one AF_XDP socket + one pinned RX thread per
  queue, with RSS (or a custom eBPF hash) steering flows to queues.
