#!/usr/bin/env bash
#
# tests/shots/net/run_kick_ban.sh — 3-player kick/ban end-to-end test.
#
# Spawns one host (HostA) + two clients (ClientB, ClientC) via
# shotmode networked. Host scripts a `kick 1` then `ban 2` directive.
# Asserts that both clients receive the forced disconnect and the
# main.c hook returns them to MODE_TITLE; that the host's slot table
# clears appropriately; and that ClientC's name lands in bans.txt.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built"; exit 1; }

H_SHOT="$REPO/tests/shots/net/3p_kick_ban.host.shot"
B_SHOT="$REPO/tests/shots/net/3p_kick_ban.clientB.shot"
C_SHOT="$REPO/tests/shots/net/3p_kick_ban.clientC.shot"
for f in "$H_SHOT" "$B_SHOT" "$C_SHOT"; do
    [[ -f "$f" ]] || { echo "fail: $f missing"; exit 1; }
done

OUT="$REPO/build/shots/net"
rm -rf "$OUT/3p_kick_ban_host" "$OUT/3p_kick_ban_clientB" "$OUT/3p_kick_ban_clientC"
mkdir -p "$OUT"

# bans.txt persistence is part of what we're testing. The host writes
# it to its cwd. We run all three processes from $REPO so the relative
# `out` paths in the shot scripts land under $REPO/build/shots/net/...
# and so they share a single bans.txt site. Wipe any stale file first;
# the test owns this path for its duration.
BANS_FILE="$REPO/bans.txt"
rm -f "$BANS_FILE"

PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill     "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${PIDS[@]}"; do kill -9  "$p" 2>/dev/null; done
    wait 2>/dev/null
}
trap cleanup EXIT

cd "$REPO"

# Host first — opens listening port.
"$BIN" --shot "$H_SHOT" >/dev/null 2>&1 &
PIDS+=($!)
sleep 0.7

# Clients — connect to the host.
"$BIN" --shot "$B_SHOT" >/dev/null 2>&1 &
PIDS+=($!)
sleep 0.4

"$BIN" --shot "$C_SHOT" >/dev/null 2>&1 &
PIDS+=($!)

# Wait for all to finish naturally (each script has `end` at tick 320 →
# ~5.3 seconds). 30 s timeout for safety.
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
sleep 0.5

# --- assertions ------------------------------------------------------
HL=$(find "$OUT/3p_kick_ban_host"   -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
BL=$(find "$OUT/3p_kick_ban_clientB" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
CL=$(find "$OUT/3p_kick_ban_clientC" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then
        echo "PASS: $1"; PASS=$((PASS + 1))
    else
        echo "FAIL: $1"; FAIL=$((FAIL + 1))
    fi
}

asrt "host log exists"      '[[ -f "$HL" ]]'
asrt "clientB log exists"   '[[ -f "$BL" ]]'
asrt "clientC log exists"   '[[ -f "$CL" ]]'

# Host scripted both directives.
asrt "host: scripted kick slot=1"  "grep -q 'shot: kick slot=1' '$HL'"
asrt "host: scripted ban slot=2"   "grep -q 'shot: ban slot=2'  '$HL'"

# Server-side announcements + slot cleanup.
asrt "host: kick chat 'ClientB was kicked'" \
    "grep -q 'ClientB was kicked by host' '$HL'"
asrt "host: ban  chat 'ClientC was banned'"  \
    "grep -q 'ClientC was banned by host' '$HL'"
asrt "host: clientB peer disconnects"       \
    "grep -q 'peer.*ClientB.*disconnect' '$HL' || grep -qE 'slot 1.*disconnected' '$HL'"
asrt "host: clientC peer disconnects"       \
    "grep -q 'peer.*ClientC.*disconnect' '$HL' || grep -qE 'slot 2.*disconnected' '$HL'"

# Forced-disconnect hook on the clients.
asrt "clientB: server connection lost"  \
    "grep -q 'server connection lost' '$BL'"
asrt "clientC: server connection lost"  \
    "grep -q 'server connection lost' '$CL'"

# bans.txt contents (ClientC banned by name).
asrt "host: bans.txt exists"            '[[ -f "$BANS_FILE" ]]'
asrt "host: bans.txt names ClientC"     "grep -q ClientC '$BANS_FILE' 2>/dev/null"
asrt "host: bans.txt does NOT name ClientB (kick != ban)" \
    "[[ -f '$BANS_FILE' ]] && ! grep -q ClientB '$BANS_FILE'"

# Screenshots written.
asrt "host wrote pre-kick + post-kick + post-ban shots" \
    "[[ \$(ls $OUT/3p_kick_ban_host/*.png 2>/dev/null | wc -l) -ge 3 ]]"
asrt "clientB wrote shots"  "[[ \$(ls $OUT/3p_kick_ban_clientB/*.png 2>/dev/null | wc -l) -ge 1 ]]"
asrt "clientC wrote shots"  "[[ \$(ls $OUT/3p_kick_ban_clientC/*.png 2>/dev/null | wc -l) -ge 1 ]]"

# --- phase 2: banned reconnect rejection -----------------------------
#
# Phase 1 left bans.txt with ClientC's name. Spin up a fresh host
# (which loads bans.txt via lobby_load_bans) and try to reconnect as
# ClientC. The server should reject at challenge-response time;
# from the client's perspective, ENet emits DISCONNECT and the new
# shotmode hook ends the script with "server connection lost".

P2_H_SHOT="$REPO/tests/shots/net/3p_kick_ban_phase2.host.shot"
P2_B_SHOT="$REPO/tests/shots/net/3p_kick_ban_phase2.banned.shot"

if [[ -f "$P2_H_SHOT" && -f "$P2_B_SHOT" && -f "$BANS_FILE" ]]; then
    rm -rf "$OUT/3p_kick_ban_phase2_host" "$OUT/3p_kick_ban_phase2_banned"
    P2_PIDS=()
    "$BIN" --shot "$P2_H_SHOT"   >/dev/null 2>&1 &
    P2_PIDS+=($!)
    PIDS+=($!)
    sleep 0.7
    "$BIN" --shot "$P2_B_SHOT"   >/dev/null 2>&1 &
    P2_PIDS+=($!)
    PIDS+=($!)

    P2_WAIT="${PHASE2_TIMEOUT_S:-15}"
    for ((i = 0; i < P2_WAIT; ++i)); do
        sleep 1
        alive=0
        for p in "${P2_PIDS[@]}"; do
            [[ -d /proc/$p ]] && alive=1
        done
        [[ $alive -eq 0 ]] && break
    done
    for p in "${P2_PIDS[@]}"; do
        [[ -d /proc/$p ]] && kill "$p" 2>/dev/null
    done
    sleep 0.3

    P2_HL=$(find "$OUT/3p_kick_ban_phase2_host"   -maxdepth 1 -name '*.log' 2>/dev/null | head -1)
    P2_BL=$(find "$OUT/3p_kick_ban_phase2_banned" -maxdepth 1 -name '*.log' 2>/dev/null | head -1)

    asrt "phase2: host loaded bans.txt"          \
        "grep -q 'lobby: loaded' '$P2_HL' 2>/dev/null"
    asrt "phase2: host rejected banned peer"     \
        "grep -q 'banned — rejecting' '$P2_HL' 2>/dev/null"
    # The banned client gets rejected at the connect handshake (before
    # MODE_LOBBY), so shotmode's connect path errors out — that's how
    # the rejection surfaces in the log. The per-tick "server connection
    # lost" hook is for clients ALREADY connected; this case never gets
    # that far.
    asrt "phase2: banned client got rejected at connect" \
        "grep -q 'REJECT (bad challenge)' '$P2_BL' 2>/dev/null"
fi

echo
echo "== 3p kick/ban summary: $PASS passed, $FAIL failed =="
echo "bans.txt:   $BANS_FILE"
echo "outputs at: $OUT"

# Always clean up the test-owned bans.txt so it doesn't leak into
# subsequent runs of soldut from the same cwd.
rm -f "$BANS_FILE"

[[ $FAIL -eq 0 ]] || exit 1
exit 0
