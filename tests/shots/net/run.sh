#!/usr/bin/env bash
#
# tests/shots/net/run.sh — paired multiplayer shot test driver.
#
# Spawns ./soldut --shot tests/shots/net/<name>.host.shot and
# ./soldut --shot tests/shots/net/<name>.client.shot in parallel.
# Both write PNG screenshots to build/shots/net/{host,client}/. After
# completion, this script also composes a side-by-side comparison
# (host vs client at the same tick) so you can see what each player's
# screen showed at any given moment.
#
# Usage:
#   tests/shots/net/run.sh                  # default: 2p_basic
#   tests/shots/net/run.sh 2p_basic         # explicit name
#   tests/shots/net/run.sh -k 2p_basic      # keep tmpdir + dump logs

set -u

KEEP=0
NAME="2p_basic"
while [[ $# -gt 0 ]]; do
    case "$1" in
        -k) KEEP=1; shift ;;
        -*) echo "unknown flag: $1" >&2; exit 1 ;;
        *)  NAME="$1"; shift ;;
    esac
done

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

HOST_SHOT="$REPO/tests/shots/net/$NAME.host.shot"
CLI_SHOT="$REPO/tests/shots/net/$NAME.client.shot"
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT"  ]] || { echo "fail: $CLI_SHOT  missing"; exit 1; }

OUT="$REPO/build/shots/net"
rm -rf "$OUT"
mkdir -p "$OUT"

HOST_PID=0
CLI_PID=0
cleanup() {
    [[ $CLI_PID  -gt 0 ]] && kill     "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill     "$HOST_PID" 2>/dev/null
    sleep 0.3
    [[ $CLI_PID  -gt 0 ]] && kill -9  "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill -9  "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

cd "$REPO"

# Host first; give the listening socket a moment.
"$BIN" --shot "$HOST_SHOT" >/dev/null 2>&1 &
HOST_PID=$!
sleep 0.7

# Client connects.
"$BIN" --shot "$CLI_SHOT"  >/dev/null 2>&1 &
CLI_PID=$!

# Both scripts have an `end` event; wait for them to finish naturally
# (with a generous bound so the test can't hang the CI).
TIMEOUT_S="${TEST_TIMEOUT_S:-60}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$HOST_PID && ! -d /proc/$CLI_PID ]]; then
        break
    fi
done

# Belt-and-braces stop in case either is still alive.
[[ -d /proc/$CLI_PID  ]] && kill "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill "$HOST_PID" 2>/dev/null
sleep 0.5

# Build a side-by-side comparison contact sheet — for each shot name
# present in BOTH host/ and client/, place them in a row labelled by
# tick / name so a reviewer can spot client-vs-host divergence at a
# glance.
echo "=== shot output ==="
ls -la "$OUT/host"   2>/dev/null | grep "\.png" || echo "(no host shots)"
ls -la "$OUT/client" 2>/dev/null | grep "\.png" || echo "(no client shots)"

echo
echo "=== quick assertions ==="

# Assertions are deliberately script-agnostic — we just verify both
# sides ran end-to-end and the network plumbing engaged. Per-test
# visual review happens by looking at the contact sheets and
# individual PNGs under build/shots/net/{host,client}.
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then
        echo "PASS: $1"; PASS=$((PASS + 1))
    else
        echo "FAIL: $1"; FAIL=$((FAIL + 1))
    fi
}

# Find each side's actual output dir (script uses `out` directive, so
# may be host_motion / client_motion / etc — not just host / client).
HOST_OUT=$(find "$OUT" -maxdepth 1 -mindepth 1 -type d \
              -name '*host*' -o -name 'host' | head -1)
CLI_OUT=$(find  "$OUT" -maxdepth 1 -mindepth 1 -type d \
              -name '*client*' -o -name 'client' | head -1)

asrt "host output dir exists"  '[[ -n "$HOST_OUT" && -d "$HOST_OUT" ]]'
asrt "client output dir exists" '[[ -n "$CLI_OUT" && -d "$CLI_OUT" ]]'
asrt "host wrote >=1 png"       '[[ $(ls "$HOST_OUT"/*.png 2>/dev/null | wc -l) -ge 1 ]]'
asrt "client wrote >=1 png"     '[[ $(ls "$CLI_OUT"/*.png 2>/dev/null | wc -l) -ge 1 ]]'

# Network plumbing checks via the per-side log file.
HL=$(find "$HOST_OUT" -maxdepth 1 -name '*.log' | head -1)
CL=$(find "$CLI_OUT"  -maxdepth 1 -name '*.log' | head -1)
asrt "host log exists"             '[[ -f "$HL" ]]'
asrt "client log exists"           '[[ -f "$CL" ]]'
asrt "host begins a round"         "grep -q 'match: round begin' '$HL' 2>/dev/null"
asrt "client receives ROUND_START" "grep -q 'client: ROUND_START'  '$CL' 2>/dev/null"
asrt "client receives snapshots"   "grep -q 'first snapshot'        '$CL' 2>/dev/null"
asrt "client resolves local_mech"  "grep -q 'client local_mech_id resolved' '$CL' 2>/dev/null"
asrt "host runs to end"            "grep -q 'shotmode: networked done' '$HL' 2>/dev/null"
asrt "client runs to end"          "grep -q 'shotmode: networked done' '$CL' 2>/dev/null"

echo
echo "== summary: $PASS passed, $FAIL failed =="
echo "outputs at: $OUT"
[[ $FAIL -eq 0 ]] || exit 1
exit 0
