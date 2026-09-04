#!/usr/bin/env bash
# Integration test: start the sim feed handler listening on loopback,
# fire a known number of packets at it with a fixed seed and drop rate,
# then check that the reported dropped-message count is in the
# ballpark of what the generator actually withheld. This proves the
# gap-detection logic works end-to-end over real UDP loopback traffic,
# not just against synthetic seq numbers in a unit test.
set -euo pipefail

BUILD_DIR="$(dirname "$0")/../build"
PORT=25123
COUNT=2000
DROP_PCT=3.0
SEED=42

"$BUILD_DIR/feed_handler_sim" --mode sim --bind 127.0.0.1 --port "$PORT" --channel 1 \
    > /tmp/feed_handler_stdout.log 2> /tmp/feed_handler_stderr.log &
FH_PID=$!

# give the listener a moment to bind
sleep 0.3

"$BUILD_DIR/packet_gen" --dst 127.0.0.1 --port "$PORT" --count "$COUNT" \
    --drop-pct "$DROP_PCT" --sleep-us 200 --seed "$SEED" 2> /tmp/packet_gen.log

sleep 0.5
kill -INT "$FH_PID"
wait "$FH_PID" 2>/dev/null || true

echo "--- generator log ---"
cat /tmp/packet_gen.log
echo "--- feed handler stats ---"
tail -n 20 /tmp/feed_handler_stderr.log

ACTUAL_DROPPED=$(grep -oP 'dropped=\K[0-9]+' /tmp/packet_gen.log)
REPORTED_MISSING=$(grep -oP 'missing_msgs=\K[0-9]+' /tmp/feed_handler_stderr.log | tail -1)

if [ -z "$REPORTED_MISSING" ]; then
    echo "FAIL: feed handler never printed missing_msgs stat"
    exit 1
fi

echo "generator withheld: $ACTUAL_DROPPED packets"
echo "feed handler detected as missing: $REPORTED_MISSING messages"

# each dropped packet in this test carries exactly one message, so the
# counts should match exactly (no reordering happens on loopback UDP
# at this send rate).
if [ "$ACTUAL_DROPPED" -eq "$REPORTED_MISSING" ]; then
    echo "PASS: gap detector correctly identified all $ACTUAL_DROPPED dropped packets"
    exit 0
else
    echo "FAIL: mismatch between dropped ($ACTUAL_DROPPED) and detected-missing ($REPORTED_MISSING)"
    exit 1
fi
