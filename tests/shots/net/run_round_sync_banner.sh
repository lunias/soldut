#!/usr/bin/env bash
#
# tests/shots/net/run_round_sync_banner.sh — visual capture of the
# inter-round COUNTDOWN banner on host vs client. Pairs with the
# log-driven assertions in run_round_sync.sh.
#
# After running, eyeball:
#   build/shots/net/2p_round_sync_banner_host/t460_countdown.png
#   build/shots/net/2p_round_sync_banner_client/t460_countdown.png
# The "Round X / Y starts in N s" banner round number must match.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
NAME="2p_round_sync_banner"

H_SHOT="$REPO/tests/shots/net/$NAME.host.shot"
C_SHOT="$REPO/tests/shots/net/$NAME.client.shot"
[[ -f "$H_SHOT" && -f "$C_SHOT" ]] || { echo "fail: shots missing"; exit 1; }

CFG="$REPO/soldut.cfg"
SAVED_CFG=""
if [[ -f "$CFG" ]]; then SAVED_CFG="$(mktemp)"; cp "$CFG" "$SAVED_CFG"; fi
cat > "$CFG" <<EOF
port=24114
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
rm -rf "$OUT/${NAME}_host" "$OUT/${NAME}_client"
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

HL=$(find "$OUT/${NAME}_host"   -maxdepth 1 -name '*.log' | head -1)
CL=$(find "$OUT/${NAME}_client" -maxdepth 1 -name '*.log' | head -1)

echo "host log:    $HL"
echo "client log:  $CL"
echo "host shots:  $OUT/${NAME}_host/"
echo "client shots: $OUT/${NAME}_client/"
echo
echo "host  rounds samples:"
grep -E 'tag=(begin_countdown|begin_round|end_round)' "$HL" | tail -6 || true
echo
echo "client rounds samples:"
grep -E 'tag=rx_' "$CL" | tail -6 || true
