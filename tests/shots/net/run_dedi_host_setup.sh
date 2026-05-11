#!/usr/bin/env bash
#
# tests/shots/net/run_dedi_host_setup.sh — verifies wan-fixes-6.
#
# Spawns a dedicated child with explicit CLI args (mode + map + score
# + time + ff) and connects a single shot client. Asserts the server
# applied each setting (visible in soldut-server.log's dedicated
# startup line and the match-state broadcast).
#
# Pre-fix: bootstrap_host_via_dedicated only forwarded --dedicated
# PORT. The dedicated child read its own cfg defaults regardless of
# what the host UI's setup screen picked. Result: lobby ignored the
# user's choices.
#
# Post-fix: every host_setup-derived field flows as a CLI flag.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

PORT=$((24300 + RANDOM % 200))
TMP="$(mktemp -d -t soldut-dedi-setup-XXXXXX)"
SRV_DIR="$TMP/server"
mkdir -p "$SRV_DIR"

SRV_PID=0
CLI_PID=0
cleanup() {
    [[ $CLI_PID -gt 0 ]] && kill     "$CLI_PID" 2>/dev/null
    [[ $SRV_PID -gt 0 ]] && kill     "$SRV_PID" 2>/dev/null
    sleep 0.3
    [[ $CLI_PID -gt 0 ]] && kill -9  "$CLI_PID" 2>/dev/null
    [[ $SRV_PID -gt 0 ]] && kill -9  "$SRV_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

cd "$REPO"

# Run the dedicated server with explicit forwarded settings.
( cd "$SRV_DIR" && exec "$BIN" --dedicated "$PORT" \
    --mode tdm \
    --map reactor \
    --score 30 \
    --time 420 \
    --ff ) >/dev/null 2>&1 &
SRV_PID=$!

# Wait for the server to bind.
for ((i = 0; i < 60; ++i)); do
    sleep 0.05
    if grep -q "dedicated: listening on $PORT" "$SRV_DIR/soldut-server.log" 2>/dev/null; then
        break
    fi
done

S_LOG="$SRV_DIR/soldut-server.log"

echo "=== host-setup-forward assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1));
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1)); fi
}

asrt "server started" \
    "grep -q 'dedicated: listening on $PORT' '$S_LOG'"
asrt "--mode tdm applied" \
    "grep -qE 'dedicated: --mode (TDM|tdm)' '$S_LOG'"
asrt "--map reactor applied (id resolved)" \
    "grep -qE \"dedicated: --map reactor \\(id=[0-9]+\\)\" '$S_LOG'"
asrt "--score 30 applied" \
    "grep -q 'dedicated: --score 30' '$S_LOG'"
asrt "--time 420 applied" \
    "grep -q 'dedicated: --time 420' '$S_LOG'"
asrt "--ff applied" \
    "grep -q 'dedicated: --ff 1' '$S_LOG'"
asrt "server built the reactor map (not foundry default)" \
    "grep -q 'map_build(reactor)' '$S_LOG' || grep -q 'map: reactor built' '$S_LOG'"

echo
echo "== host-setup-forward summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
