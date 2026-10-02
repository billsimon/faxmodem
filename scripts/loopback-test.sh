#!/usr/bin/env bash
# End-to-end smoke test: one faxmodem answers, another calls it, a real fax
# crosses real SIP signalling and RTP on the loopback interface.
#
#   scripts/loopback-test.sh [path/to/faxmodem]
set -euo pipefail

BIN=${1:-build/faxmodem}
WORK=$(mktemp -d -t faxmodem-loopback)
RX_PORT=${RX_PORT:-5080}
TX_PORT=${TX_PORT:-5070}
RX_RTP=${RX_RTP:-4100}
TX_RTP=${TX_RTP:-4200}
PAGES=${PAGES:-2}

cleanup() {
    [ -n "${RX_PID:-}" ] && kill "$RX_PID" 2>/dev/null || true
    wait "${RX_PID:-}" 2>/dev/null || true
    echo "logs and output in $WORK"
}
trap cleanup EXIT

[ -x "$BIN" ] || {
    echo "no faxmodem binary at $BIN - run: cmake -S . -B build && cmake --build build" >&2
    exit 1
}

echo "==> building a $PAGES page test document"
"$(dirname "$0")/make-test-page.sh" "$WORK/testpage.tif" "$PAGES" >/dev/null
"$BIN" probe "$WORK/testpage.tif"

echo "==> starting the receiver on port $RX_PORT"
"$BIN" receive \
    --server 127.0.0.1 \
    --username rx \
    --no-register \
    --local-port "$RX_PORT" \
    --rtp-port "$RX_RTP" \
    --station-id "RX-STATION" \
    --timeout 1800 \
    --output-dir "$WORK/inbox" >"$WORK/receive.log" 2>&1 &
RX_PID=$!

for _ in $(seq 1 50); do
    grep -q "waiting for inbound faxes" "$WORK/receive.log" && break
    sleep 0.2
done

echo "==> sending"
set +e
"$BIN" send "sip:rx@127.0.0.1:$RX_PORT" "$WORK/testpage.tif" \
    --server 127.0.0.1 \
    --username tx \
    --no-register \
    --local-port "$TX_PORT" \
    --rtp-port "$TX_RTP" \
    --station-id "+15550001111" | tee "$WORK/send.log"
SEND_RC=${PIPESTATUS[0]}
set -e

sleep 2
RECEIVED=$(find "$WORK/inbox" -name '*.tif' | head -1)

echo
echo "==> send exit code: $SEND_RC"
if [ "$SEND_RC" -ne 0 ] || [ -z "$RECEIVED" ]; then
    echo "FAILED"
    tail -20 "$WORK/receive.log"
    exit 1
fi

SENT_PAGES=$(grep -c "TIFF Directory" <(tiffinfo "$WORK/testpage.tif" 2>/dev/null) || echo "?")
GOT_PAGES=$(grep -c "TIFF Directory" <(tiffinfo "$RECEIVED" 2>/dev/null) || echo "?")
echo "==> received $RECEIVED ($GOT_PAGES pages, sent $SENT_PAGES)"
[ "$SENT_PAGES" = "$GOT_PAGES" ] || {
    echo "FAILED: page count mismatch"
    exit 1
}

# Page counts only prove pages arrived, not that they arrived intact.
if command -v tiffcmp >/dev/null 2>&1; then
    if tiffcmp "$WORK/testpage.tif" "$RECEIVED" >"$WORK/tiffcmp.log" 2>&1; then
        echo "==> pixel comparison: identical across all $GOT_PAGES pages"
    else
        echo "FAILED: received pages differ from what was sent"
        head -20 "$WORK/tiffcmp.log"
        exit 1
    fi
else
    echo "==> tiffcmp not installed, page count checked only"
fi
echo "PASS"
