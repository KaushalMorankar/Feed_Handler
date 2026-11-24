#!/usr/bin/env bash
# End-to-end recovery test (sim mode, loopback, no root).
#
# packet_gen acts as the exchange: it drops packets, replays stale ones,
# and serves book snapshots over TCP. The handler runs with --recovery.
# After each run the handler's final book must EQUAL the generator's
# ground-truth book, which is only possible if gap detection, snapshot
# fetch, buffered-update replay and duplicate skipping are all correct.
set -uo pipefail

B="$(cd "$(dirname "$0")/../build" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
FAIL=0

run_case() {  # name  gen-args...  --  handler-args...
    local name="$1"; shift
    local gen_args=() fh_args=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do gen_args+=("$1"); shift; done
    shift
    fh_args=("$@")
    local P_UDP=25210 P_TCP=25211

    "$B/feed_handler_sim" --mode sim --bind 127.0.0.1 --port $P_UDP --channel 1 \
        --recovery 127.0.0.1:$P_TCP "${fh_args[@]}" 2> "$T/fh.log" >/dev/null &
    local FH=$!
    sleep 0.4
    "$B/packet_gen" --dst 127.0.0.1 --port $P_UDP "${gen_args[@]}" \
        --serve-port $P_TCP --serve-ms 2500 2> "$T/gen.log"
    sleep 1.5
    kill -INT $FH; wait $FH 2>/dev/null

    local dropped dups drops stale snaps dskip
    dropped=$(grep -oP 'dropped=\K[0-9]+' "$T/gen.log")
    dups=$(grep -oP 'dups=\K[0-9]+' "$T/gen.log")
    drops=$(grep -oP 'ring_full_drops=\K[0-9]+' "$T/fh.log")
    stale=$(grep -oP 'book_stale=\K\w+' "$T/fh.log")
    snaps=$(grep -oP 'snapshots_applied=\K[0-9]+' "$T/fh.log")
    dskip=$(grep -oP 'duplicates_skipped=\K[0-9]+' "$T/fh.log")

    grep '^\[book\]' "$T/gen.log" | sort > "$T/gen.book"
    grep '^\[book\]' "$T/fh.log"  | sort > "$T/fh.book"

    echo "--- $name: dropped=$dropped dups=$dups ring_full_drops=$drops snapshots_applied=$snaps dup_skipped=$dskip stale=$stale"
    local ok=1
    [ "$stale" = "no" ] || { echo "  FAIL: book still stale"; ok=0; }
    [ -s "$T/gen.book" ] && diff -q "$T/gen.book" "$T/fh.book" >/dev/null \
        || { echo "  FAIL: handler book != ground truth"; diff "$T/gen.book" "$T/fh.book"; ok=0; }
    [ "$dskip" = "$dups" ] || { echo "  FAIL: duplicates_skipped ($dskip) != dups sent ($dups)"; ok=0; }
    if [ "$ok" = 1 ]; then
        echo "  PASS: final book identical to ground truth"
    else
        FAIL=1
    fi
    LAST_DROPS=$drops; LAST_SNAPS=$snaps; LAST_DROPPED=$dropped
}

# 1. network loss + stale replays: gaps -> snapshot recovery, dups skipped
run_case "loss+dups" --count 20000 --drop-pct 3 --drop-tail 12 --dup-pct 5 --sleep-us 50 --seed 7 --
[ "${LAST_SNAPS:-0}" -ge 1 ] || { echo "  FAIL: expected at least one snapshot"; FAIL=1; }

# 1b. stale replays only (no loss, so no snapshot can mask a duplicate bug):
#     the book matches only if duplicates are skipped, not applied
run_case "dups-only" --count 20000 --drop-pct 0 --dup-pct 5 --sleep-us 50 --seed 11 --

# 2. consumer far slower than the feed: ring overflow -> stale -> recovery
run_case "ring-overflow" --count 30000 --drop-pct 0 --sleep-us 0 --seed 9 -- --slow-consumer-us 50
[ "${LAST_DROPS:-0}" -gt 0 ] || { echo "  FAIL: test did not actually overflow the ring"; FAIL=1; }
[ "${LAST_SNAPS:-0}" -ge 1 ] || { echo "  FAIL: expected a snapshot after overflow"; FAIL=1; }

if [ "$FAIL" = 0 ]; then echo "ALL RECOVERY TESTS PASSED"; exit 0; fi
echo "RECOVERY TESTS FAILED"; exit 1
