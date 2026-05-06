#!/usr/bin/env bash
#
# tests/shots/net/run_kick_modal.sh — paired host+client run that
# captures the kick / ban confirmation modal for visual regression.
# Uses a temp soldut.cfg with auto_start_seconds=20 so the lobby
# stays open long enough to drive the modal directives.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built"; exit 1; }

H_SHOT="$REPO/tests/shots/net/kick_modal.host.shot"
C_SHOT="$REPO/tests/shots/net/kick_modal.client.shot"
[[ -f "$H_SHOT" && -f "$C_SHOT" ]] || { echo "fail: shot scripts missing"; exit 1; }

OUT="$REPO/build/shots/net"
rm -rf "$OUT/kick_modal_host" "$OUT/kick_modal_client"
mkdir -p "$OUT"

CFG="$REPO/soldut.cfg"
SAVED_CFG=""
if [[ -f "$CFG" ]]; then SAVED_CFG="$(mktemp)"; cp "$CFG" "$SAVED_CFG"; fi
cat > "$CFG" <<EOF
port=24107
auto_start_seconds=20
time_limit=30
score_limit=20
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

cd "$REPO"
"$BIN" --shot "$H_SHOT" >/dev/null 2>&1 &
PIDS+=($!)
sleep 0.7
"$BIN" --shot "$C_SHOT" >/dev/null 2>&1 &
PIDS+=($!)

TIMEOUT_S="${TEST_TIMEOUT_S:-30}"
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

HL=$(find "$OUT/kick_modal_host"   -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
CL=$(find "$OUT/kick_modal_client" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "host log exists"     '[[ -f "$HL" ]]'
asrt "client log exists"   '[[ -f "$CL" ]]'
asrt "host: kick_modal directive ran" \
    "grep -q 'shot: kick_modal slot=1' '$HL'"
asrt "host: ban_modal directive ran"  \
    "grep -q 'shot: ban_modal slot=1' '$HL'"
asrt "host wrote 4 modal-flow shots"  \
    "[[ \$(ls $OUT/kick_modal_host/*.png 2>/dev/null | wc -l) -ge 4 ]]"

# The client still in the lobby at every shot tick implies neither
# modal was confused for an actual kick — the modal-only directives
# don't disconnect peers.
asrt "client still connected at end"  \
    "! grep -q 'server connection lost' '$CL'"

echo
echo "== kick_modal summary: $PASS passed, $FAIL failed =="
echo "outputs at: $OUT"
[[ $FAIL -eq 0 ]] || exit 1
exit 0
