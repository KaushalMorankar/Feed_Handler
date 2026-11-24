#!/usr/bin/env bash
# Send-to-process latency over a veth link, kernel UDP socket vs AF_XDP.
#
# Timestamp is taken by packet_gen immediately before sendto() and compared
# with the clock when the processing thread pops the decoded message (same
# machine, CLOCK_REALTIME). So it INCLUDES the sender's syscall, the veth
# hop, the kernel receive path (UDP stack or XDP redirect) and the SPSC
# handoff. It does not include NIC/PHY time: veth has none, and true
# NIC-to-user latency needs hardware timestamps.
#
# Both handlers run with --busy-poll on pinned cores (no usleep), and the
# generator is rate-paced and pinned, so the two paths are compared fairly.
#
# usage: sudo bash bench/run_veth_latency.sh [rate_pps] [count] [runs]
set -uo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 2; }

RATE=${1:-50000}; COUNT=${2:-200000}; RUNS=${3:-3}
B="$(cd "$(dirname "$0")/../build" && pwd)"
T="$(mktemp -d)"
CPU_RX=${CPU_RX:-4}; CPU_PROC=${CPU_PROC:-6}; CPU_GEN=${CPU_GEN:-8}
cleanup() { ip link del vtest0 2>/dev/null; ip netns del gen 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
ip link del vtest0 2>/dev/null; ip netns del gen 2>/dev/null

ip netns add gen
ip link add vtest0 type veth peer name vtest1
ip link set vtest1 netns gen
ip addr add 10.9.0.1/24 dev vtest0; ip link set vtest0 up
ip netns exec gen ip addr add 10.9.0.2/24 dev vtest1
ip netns exec gen ip link set vtest1 up

run() {  # mode
    local mode=$1 FH
    if [ "$mode" = sim ]; then
        "$B/feed_handler_sim" --mode sim --bind 10.9.0.1 --port 15000 --channel 1 \
            --busy-poll --cpu-rx $CPU_RX --cpu-proc $CPU_PROC 2> "$T/fh.log" >/dev/null &
    else
        "$B/feed_handler_afxdp" --mode afxdp --iface vtest0 --queue 0 --bpf "$B/xdp_kern.o" --skb \
            --channel 1 --busy-poll --cpu-rx $CPU_RX --cpu-proc $CPU_PROC 2> "$T/fh.log" >/dev/null &
    fi
    FH=$!
    sleep 1.5
    ip netns exec gen "$B/packet_gen" --dst 10.9.0.1 --port 15000 --count "$COUNT" --drop-pct 0 \
        --rate-pps "$RATE" --cpu $CPU_GEN 2> "$T/gen.log"
    sleep 1; kill -INT $FH; wait $FH 2>/dev/null; sleep 0.8
    local p50 p99 p999 avg missing
    p50=$(grep -oP 'p50_ns=\K[0-9]+' "$T/fh.log"); p99=$(grep -oP 'p99_ns=\K[0-9]+' "$T/fh.log")
    p999=$(grep -oP 'p999_ns=\K[0-9]+' "$T/fh.log"); avg=$(grep -oP 'e2e_latency_avg_ns=\K[0-9]+' "$T/fh.log")
    missing=$(grep -oP 'missing_msgs=\K[0-9]+' "$T/fh.log")
    printf "%-6s p50=%6.1f us  p99=%7.1f us  p99.9=%8.1f us  avg=%6.1f us  lost=%s\n" \
        "$mode" "$(echo "$p50/1000" | bc -l)" "$(echo "$p99/1000" | bc -l)" \
        "$(echo "$p999/1000" | bc -l)" "$(echo "$avg/1000" | bc -l)" "$missing"
}

echo "rate=${RATE} pps  packets=${COUNT}  runs=${RUNS}  (rx cpu $CPU_RX, proc cpu $CPU_PROC, gen cpu $CPU_GEN)"
for i in $(seq 1 "$RUNS"); do
    echo "-- run $i"
    run sim
    run afxdp
done
