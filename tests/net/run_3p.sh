#!/usr/bin/env bash
#
# tests/net/run_3p.sh — 3-player TDM smoke test.
#
# Spawns one host + two clients, runs through a single round,
# verifies every peer reaches the match phase. Uses the same
# log-driven assertion pattern as run.sh.

set -u

KEEP=0
[[ "${1:-}" == "-k" ]] && KEEP=1

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built"; exit 1; }

PORT=$((23000 + RANDOM % 1000))
TMP="$(mktemp -d -t soldut-net3-XXXXXX)"
HOST_DIR="$TMP/host"
C1_DIR="$TMP/c1"
C2_DIR="$TMP/c2"
mkdir -p "$HOST_DIR" "$C1_DIR" "$C2_DIR"

for d in "$HOST_DIR" "$C1_DIR" "$C2_DIR"; do
    cat > "$d/soldut.cfg" <<EOF
port=$PORT
auto_start_seconds=2
time_limit=4
score_limit=5
mode=tdm
map_rotation=reactor
mode_rotation=tdm
EOF
done

PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill     "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]}"; do kill -9  "$p" 2>/dev/null; done
    wait 2>/dev/null
    if [[ "$KEEP" == "0" ]]; then rm -rf "$TMP"; else echo "kept: $TMP"; fi
}
trap cleanup EXIT

( cd "$HOST_DIR" && exec "$BIN" --listen-host "$PORT" --name HostA ) > "$HOST_DIR/stdout.log" 2>&1 &
PIDS+=($!)
sleep 0.6

( cd "$C1_DIR" && exec "$BIN" --connect "127.0.0.1:$PORT" --name ClientB ) > "$C1_DIR/stdout.log" 2>&1 &
PIDS+=($!)
sleep 0.4

( cd "$C2_DIR" && exec "$BIN" --connect "127.0.0.1:$PORT" --name ClientC ) > "$C2_DIR/stdout.log" 2>&1 &
PIDS+=($!)

WAIT_S="${TEST_WAIT_S:-20}"
for ((i = 0; i < WAIT_S; ++i)); do
    sleep 1
    # If any died, bail early.
    for p in "${PIDS[@]}"; do
        [[ -d /proc/$p ]] || break 2
    done
done

for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
sleep 0.5

PASS=0
FAIL=0
assert_log() {
    local label="$1"; local file="$2"; local pattern="$3"
    if grep -qE "$pattern" "$file"; then
        echo "PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label  (file: $file, pattern: $pattern)"
        FAIL=$((FAIL + 1))
    fi
}

HL="$HOST_DIR/soldut.log"
C1L="$C1_DIR/soldut.log"
C2L="$C2_DIR/soldut.log"

assert_log "host listens"             "$HL"  "listening on port $PORT"
assert_log "host sees 2 peers"        "$HL"  "ACCEPT peer 1 → lobby slot 2"
assert_log "host spawns 3 mechs"      "$HL"  "lobby_spawn_round_mechs: 3 mech\\(s\\) spawned"
assert_log "host begins TDM round"    "$HL"  "match: round begin .mode=TDM"

assert_log "client B connects"        "$C1L" "ACCEPT client_id=0 mech_id=1"
assert_log "client B reaches MATCH"   "$C1L" "ROUND_START map=2 mode=TDM"
assert_log "client B resolves mech"   "$C1L" "local_mech_id resolved → 1"

assert_log "client C connects"        "$C2L" "ACCEPT client_id=1 mech_id=2"
assert_log "client C reaches MATCH"   "$C2L" "ROUND_START map=2 mode=TDM"
assert_log "client C resolves mech"   "$C2L" "local_mech_id resolved → 2"

echo
echo "== 3p summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
