#!/usr/bin/env bash
#
# tests/shots/net/run_input_redundancy.sh — verifies Phase 3.
#
# Phase 3 packs the client's last N=4 inputs into every NET_MSG_INPUT
# datagram so a single dropped UDP packet doesn't cause a missed sim
# tick on the server. The server deduplicates by seq.
#
# Drives 2p_basic for ~10 seconds of normal play and asserts:
#
#   - the server logs `input batch peer=0 count=N` lines (proving the
#     count-prefixed wire format is in use; pre-Phase-3 the handler
#     parsed a single input directly with no count field).
#   - after warmup (the first 3 batches: count=1, count=2, count=3),
#     every batch is count=4 (ring full).
#   - in steady state, `applied=1` (one fresh seq per batch + three
#     redundant resends that get skipped — proves the dedup filter
#     runs).
#   - sim still progresses normally (basic-smoke asserts come along
#     for the ride via the inner run.sh).

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_basic"

"$REPO/tests/shots/net/run.sh" "$NAME"
RC=$?

echo
echo "=== input-redundancy assertions ==="

OUT="$REPO/build/shots/net"
HOST_OUT=$(find "$OUT" -maxdepth 1 -mindepth 1 -type d -name '*host*' | head -1)
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' | head -1)
[[ -n "$HL" ]] || { echo "fail: host log missing"; exit 1; }

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "server logs input batches in the new wire format" \
    "grep -qE 'net: input batch peer=0 count=[0-9]+ applied=[0-9]+' '$HL'"

asrt "redundancy ring fills (sees count=4 batches)" \
    "grep -qE 'net: input batch peer=0 count=4 ' '$HL'"

# At least 8 batches reach count=4 — a noisy ENet schedule can
# still hit count=3 occasionally (multi-tick gap), so we just want
# evidence of steady-state. 2p_basic runs ~900 ticks ≈ many batches.
asrt "steady-state has many count=4 batches (>= 8)" \
    "[ \"\$(grep -cE 'net: input batch peer=0 count=4 ' '$HL')\" -ge 8 ]"

# applied counts: every batch reports `applied=N` where N is how many
# NEW seqs the server hadn't already seen. In normal (no-loss)
# operation N=1 because three of the four batched inputs are dupes
# from prior batches. If applied=4 appears it means the server is
# missing 3 in a row — a real loss event; rare on loopback.
asrt "steady-state dedup: applied=1 dominates" \
    "[ \"\$(grep -cE 'net: input batch peer=0 .*applied=1 ' '$HL')\" -gt \"\$(grep -cE 'net: input batch peer=0 .*applied=4 ' '$HL')\" ]"

# Throttle log line confirms the server applied the Phase 3 ENet tune
# on accept (1 s interval, 5..15 s timeout band).
asrt "server tuned ENet throttle on accept" \
    "grep -qE 'throttle_interval=1000ms timeout=5\\.\\.15s' '$HL'"

echo
echo "== input-redundancy summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 && $RC -eq 0 ]] || exit 1
exit 0
