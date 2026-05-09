#!/usr/bin/env bash
#
# tests/shots/net/run_round_sync.sh — round-counter sync regression.
#
# Runs the existing 2p_round_loop paired host/client shots and grep-
# asserts that the per-transition `match_state: tag=... rounds=X/Y`
# diagnostics emitted by `match_shot_log_phase` agree between host and
# client at every phase transition.
#
# Pre-fix this fails: the host increments `rounds_played` after each
# SUMMARY but `match_encode`/`match_decode` doesn't carry it on the
# wire, so the client stays at 0/N while the host advances to 1/N
# → 2/N. The visible symptom is the lobby_ui "Round X / Y starts in
# N s" banner during the inter-round COUNTDOWN: host says "Round 2"
# while client still says "Round 1".

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
port=24112
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

# Override the network port in the shots to avoid the 24110 collision
# with run_round_loop.sh if both run in CI back-to-back.
H_SHOT_TMP="$(mktemp --suffix=.shot)"
C_SHOT_TMP="$(mktemp --suffix=.shot)"
sed 's/24110/24112/g' "$H_SHOT" > "$H_SHOT_TMP"
sed 's/24110/24112/g' "$C_SHOT" > "$C_SHOT_TMP"
trap 'cleanup; rm -f "$H_SHOT_TMP" "$C_SHOT_TMP"' EXIT

cd "$REPO"
"$REPO/soldut" --shot "$H_SHOT_TMP" >/dev/null 2>&1 &
PIDS+=($!)
sleep 0.7
"$REPO/soldut" --shot "$C_SHOT_TMP" >/dev/null 2>&1 &
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

# Extract the rounds=X/Y for each side at the second begin_countdown
# (the inter-round transition into round 2). Pre-fix host says 1/2
# while client says 0/2; post-fix both should say 1/2.
extract_rounds() {
    # $1 = log path, $2 = grep pattern (anchors the line), $3 = match index (1-based)
    local log="$1" pattern="$2" idx="$3"
    grep -oE "$pattern" "$log" | sed -n "${idx}p" | grep -oE 'rounds=[0-9]+/[0-9]+'
}

# Round transitions in 2p_round_loop:
#   tag=begin_countdown #1 (lobby→round 1)            : rounds=0/2
#   tag=begin_round     #1                            : rounds=0/2
#   tag=end_round       #1                            : rounds=0/2
#   tag=begin_countdown #2 (inter-round → round 2)    : rounds=1/2  ← bug shows here
#   tag=begin_round     #2                            : rounds=1/2
#   tag=end_round       #2                            : rounds=1/2

# The client side: rx_match_state arrives whenever the host broadcasts.
# We assert by *content* (does a `rounds=1/2` line exist on each side
# at the right tag?) instead of by index — the first rx_match_state of
# the run can race with the connection-establishment logic and isn't a
# reliable anchor for "round 2 countdown".

# Pre-fix the client's rx_match_state never carried `rounds=1/2`
# (rounds_played wasn't on the wire) — so this grep returns false and
# the assertion fails. Post-fix the wire carries it; both sides see
# `rounds=1/2` at the inter-round transition.
asrt "host: begin_countdown #2 carries rounds=1/2" \
    "grep -q 'tag=begin_countdown phase=1.*rounds=1/2' '$HL'"
asrt "client: rx_match_state phase=1 carries rounds=1/2" \
    "grep -q 'tag=rx_match_state phase=1.*rounds=1/2' '$CL'"
asrt "host: begin_round #2 carries rounds=1/2" \
    "grep -q 'tag=begin_round phase=2.*rounds=1/2' '$HL'"
asrt "client: rx_round_start phase=2 carries rounds=1/2" \
    "grep -q 'tag=rx_round_start phase=2.*rounds=1/2' '$CL'"

echo
echo "  host  rounds=1/2 lines:"
grep -E 'tag=(begin_countdown|begin_round|end_round).*rounds=1/2' "$HL" | sed 's/^/    /'
echo "  client rounds=1/2 lines:"
grep -E 'tag=rx_.*rounds=1/2' "$CL" | sed 's/^/    /'

# Sanity: rounds_per_match must match too. If the client's soldut.cfg
# differs from the host's, the host's value must win on the wire.
H_RPM=$(grep -oE 'rounds=[0-9]+/[0-9]+' "$HL" | head -1 | grep -oE '/[0-9]+')
C_RPM=$(grep -oE 'rounds=[0-9]+/[0-9]+' "$CL" | head -1 | grep -oE '/[0-9]+')
asrt "rounds_per_match agrees (host=$H_RPM client=$C_RPM)" \
    '[[ "$H_RPM" == "$C_RPM" && "$H_RPM" == "/2" ]]'

echo
echo "== round-sync summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
