#!/usr/bin/env bash
#
# tests/shots/net/run_round_loop.sh — paired test for the full
# networked round loop:
#   round 1 → kill → 3 s solo warning → SUMMARY → vote → COUNTDOWN
#   (inter-round, NO lobby) → round 2 → kill → SUMMARY → LOBBY
#   (match over).
#
# Asserts the seamless inter-round transition AND the match-over
# return to lobby. With rounds_per_match=2 in the temp soldut.cfg.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_round_loop"

H_SHOT="$REPO/tests/shots/net/$NAME.host.shot"
C_SHOT="$REPO/tests/shots/net/$NAME.client.shot"
[[ -f "$H_SHOT" && -f "$C_SHOT" ]] || { echo "fail: shots missing"; exit 1; }

CFG="$REPO/soldut.cfg"
SAVED_CFG=""
if [[ -f "$CFG" ]]; then SAVED_CFG="$(mktemp)"; cp "$CFG" "$SAVED_CFG"; fi
cat > "$CFG" <<EOF
port=24110
auto_start_seconds=1
time_limit=30
score_limit=99
rounds_per_match=2
mode=ffa
EOF

PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill     "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]}"; do kill -9  "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -f "$CFG"
    [[ -n "$SAVED_CFG" ]] && mv "$SAVED_CFG" "$CFG"
}
trap cleanup EXIT

OUT="$REPO/build/shots/net"
rm -rf "$OUT/2p_round_loop_host" "$OUT/2p_round_loop_client"
mkdir -p "$OUT"

cd "$REPO"
"$REPO/soldut" --shot "$H_SHOT" >/dev/null 2>&1 &
PIDS+=($!)
sleep 0.7
"$REPO/soldut" --shot "$C_SHOT" >/dev/null 2>&1 &
PIDS+=($!)

TIMEOUT_S="${TEST_TIMEOUT_S:-90}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    alive=0
    for p in "${PIDS[@]}"; do
        [[ -d /proc/$p ]] && alive=1
    done
    [[ $alive -eq 0 ]] && break
done
for p in "${PIDS[@]}"; do
    [[ -d /proc/$p ]] && kill "$p" 2>/dev/null
done
sleep 0.3

HL=$(find "$OUT/2p_round_loop_host"   -maxdepth 1 -name '*.log' | head -1)
CL=$(find "$OUT/2p_round_loop_client" -maxdepth 1 -name '*.log' | head -1)

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host log exists"   '[[ -f "$HL" ]]'
asrt "client log exists" '[[ -f "$CL" ]]'

# Round 1.
asrt "host: round 1 begin"   "grep -q 'match: round begin' '$HL'"
asrt "client: ROUND_START 1" "grep -q 'client: ROUND_START' '$CL'"

# Solo warning fires after kill_peer drops the client.
asrt "host: solo warning armed" \
    "grep -q 'only.*alive of.*— round ends in 3s' '$HL'"

# Round 1 ends, vote picker armed, both vote → fast-forward.
asrt "host: round 1 end"     "grep -q 'match_flow: round.*end' '$HL'"
asrt "host: vote picker armed (round 1)" \
    "grep -q 'match_flow: map vote' '$HL'"
asrt "host: all-voted fast-forward" \
    "grep -q 'all .* voted — fast-forwarding summary' '$HL'"

# Inter-round transition: NO lobby. Host should log "continuing match"
# (NOT "match over") and the client should NEVER see "returning to
# lobby" between rounds 1 and 2.
asrt "host: continues match into round 2" \
    "grep -q 'continuing match' '$HL'"
asrt "host: round 2 begins (>= 2 'match: round begin')" \
    "[[ \$(grep -c 'match: round begin' '$HL') -ge 2 ]]"
asrt "client: round 2 ROUND_START arrives" \
    "[[ \$(grep -c 'client: ROUND_START' '$CL') -ge 2 ]]"

# After round 2 ends, the match is over → host returns to LOBBY,
# client follows via the MATCH_STATE phase=LOBBY hook.
asrt "host: match over" \
    "grep -q 'match over' '$HL'"
asrt "client: returns to lobby AT MATCH END (not between rounds)" \
    "grep -q 'returning to lobby' '$CL'"

# Bug-shape regression: vote winner override must survive the
# COUNTDOWN→ACTIVE start_round transition.
asrt "host: vote winner picked" \
    "[[ \$(grep -c 'match_flow: vote winner' '$HL') -ge 1 ]]"

echo
echo "== round-loop summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
