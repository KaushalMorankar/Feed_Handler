#!/usr/bin/env bash
# AF_XDP end-to-end tests on a veth pair. Needs root, clang+libbpf built
# (`make afxdp CLANG=clang-14`), and iproute2/ethtool. Runs in generic
# (skb) XDP mode, which veth supports.
#
#   host:  vtest0 10.9.0.1/24          <- feed handler attaches XDP here
#   netns: vtest1 10.9.0.2/24          <- packet_gen sends from here
#          vtest1.100 (VLAN 100)       <- tagged traffic
#
# Checks:
#   1. multicast-group filter: matching group is redirected, other group is not
#   2. VLAN filter: matching VID is redirected; wrong VID and untagged are not
#   3. snapshot recovery over AF_XDP: final book == generator ground truth
set -uo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 2; }

B="$(cd "$(dirname "$0")/../build" && pwd)"
T="$(mktemp -d)"
FAIL=0
NS=gen

cleanup() {
    ip link del vtest0 2>/dev/null
    ip netns del $NS 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
cleanup_pre() { ip link del vtest0 2>/dev/null; ip netns del $NS 2>/dev/null; }
cleanup_pre

ip netns add $NS
ip link add vtest0 type veth peer name vtest1
ip link set vtest1 netns $NS
ip addr add 10.9.0.1/24 dev vtest0; ip link set vtest0 up
ip netns exec $NS ip addr add 10.9.0.2/24 dev vtest1
ip netns exec $NS ip link set vtest1 up
ip netns exec $NS ip link set lo up
# make VLAN tags travel inline in the frame (not as skb metadata) so XDP can see them
ip netns exec $NS ethtool -K vtest1 tx-vlan-hw-insert off tx-vlan-stag-hw-insert off 2>/dev/null
ip netns exec $NS ip link add link vtest1 name vtest1.100 type vlan id 100
ip netns exec $NS ip link set vtest1.100 up
ip netns exec $NS ip route add 239.1.1.0/24 dev vtest1
ip netns exec $NS ip route add 239.1.2.0/24 dev vtest1.100

FH_PID=""
start_handler() {  # extra args...
    # The kernel releases the previous XSK socket on the queue asynchronously
    # after the old process exits, so a back-to-back bind can briefly get
    # EBUSY. Retry the attach a few times.
    for attempt in 1 2 3 4 5; do
        "$B/feed_handler_afxdp" --mode afxdp --iface vtest0 --queue 0 --bpf "$B/xdp_kern.o" \
            --skb --channel 1 "$@" 2> "$T/fh.log" >/dev/null &
        FH_PID=$!
        for _ in $(seq 1 30); do
            grep -q '^\[afxdp\] attached' "$T/fh.log" && return 0
            kill -0 "$FH_PID" 2>/dev/null || break
            sleep 0.1
        done
        wait "$FH_PID" 2>/dev/null
        sleep 1
    done
    echo "  handler failed to attach:"; cat "$T/fh.log"; return 1
}
stop_handler() { sleep 1.2; kill -INT "$FH_PID" 2>/dev/null; wait "$FH_PID" 2>/dev/null; sleep 0.5; }
gen() { ip netns exec $NS "$B/packet_gen" "$@" 2> "$T/gen.log"; }
stat() { grep -oP "$1" "$T/fh.log" | tail -1; }
expect() {  # description actual expected
    if [ "$2" = "$3" ]; then echo "  PASS: $1 ($2)"; else echo "  FAIL: $1 (got $2, want $3)"; FAIL=1; fi
}

echo "== 1a. multicast group match (--group 239.1.1.1, send to 239.1.1.1)"
start_handler --group 239.1.1.1 || exit 1
gen --dst 239.1.1.1 --count 2000 --drop-pct 3 --sleep-us 100 --seed 42
stop_handler
sent=$(grep -oP 'sent=\K[0-9]+' "$T/gen.log"); dropped=$(grep -oP 'dropped=\K[0-9]+' "$T/gen.log")
expect "packets received == sent + heartbeat" "$(stat 'packets_parsed=\K[0-9]+')" "$((sent + 1))"
expect "gap detector missing == dropped" "$(stat 'missing_msgs=\K[0-9]+')" "$dropped"

echo "== 1b. multicast group mismatch (--group 239.1.1.1, send to 239.1.1.2)"
start_handler --group 239.1.1.1 || exit 1
gen --dst 239.1.1.2 --count 500 --drop-pct 0 --sleep-us 100 --seed 1
stop_handler
expect "other group NOT redirected" "$(stat 'packets_parsed=\K[0-9]+')" "0"

echo "== 2a. VLAN match (--vlan 100, tagged VLAN 100)"
start_handler --vlan 100 || exit 1
gen --dst 239.1.2.1 --count 1000 --drop-pct 0 --sleep-us 100 --seed 2
stop_handler
expect "tagged frames redirected (count + heartbeat)" "$(stat 'packets_parsed=\K[0-9]+')" "1001"

echo "== 2b. VLAN mismatch (--vlan 200, tagged VLAN 100)"
start_handler --vlan 200 || exit 1
gen --dst 239.1.2.1 --count 500 --drop-pct 0 --sleep-us 100 --seed 3
stop_handler
expect "wrong VID NOT redirected" "$(stat 'packets_parsed=\K[0-9]+')" "0"

echo "== 2c. untagged traffic while --vlan 100 is required"
start_handler --vlan 100 || exit 1
gen --dst 239.1.1.1 --count 500 --drop-pct 0 --sleep-us 100 --seed 4
stop_handler
expect "untagged NOT redirected" "$(stat 'packets_parsed=\K[0-9]+')" "0"

echo "== 3. snapshot recovery over AF_XDP (loss + stale replays + tail loss)"
start_handler --group 239.1.1.1 --recovery 10.9.0.2:25301 || exit 1
gen --dst 239.1.1.1 --count 20000 --drop-pct 3 --drop-tail 12 --dup-pct 5 --sleep-us 50 \
    --seed 7 --serve-port 25301 --serve-ms 2500
sleep 1
stop_handler
dups=$(grep -oP 'dups=\K[0-9]+' "$T/gen.log")
grep '^\[book\]' "$T/gen.log" | sort > "$T/g.book"
grep '^\[book\]' "$T/fh.log"  | sort > "$T/h.book"
expect "book not stale at exit" "$(stat 'book_stale=\K\w+')" "no"
expect "duplicates_skipped == dups sent" "$(stat 'duplicates_skipped=\K[0-9]+')" "$dups"
if [ -s "$T/g.book" ] && diff -q "$T/g.book" "$T/h.book" >/dev/null; then
    echo "  PASS: final book identical to ground truth (snapshots_applied=$(stat 'snapshots_applied=\K[0-9]+'))"
else
    echo "  FAIL: handler book != ground truth"; diff "$T/g.book" "$T/h.book"; FAIL=1
fi

if [ "$FAIL" = 0 ]; then echo "ALL AF_XDP VETH TESTS PASSED"; exit 0; fi
echo "AF_XDP VETH TESTS FAILED"; exit 1
