#!/usr/bin/env bash
#
# tests/test_play_ctf.sh — verifies that --test-play (the same
# code path the editor's F5 button takes) auto-detects CTF mode
# from a .lvl's META.mode_mask and runs CTF correctly.
#
# Pre-requisites: assets/maps/ctf_test.lvl exists (the chained
# test-ctf-editor-flow target builds it via the editor shot).
#
# Strategy: run ./soldut --test-play with a short timeout, grep
# soldut.log for the auto-detect log line + the round begin in
# CTF mode + flag init. The game window will appear briefly; we
# kill it after ~6 seconds (auto_start 1s + countdown 1s + a few
# seconds of round time).

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/soldut"
LVL="$REPO/assets/maps/ctf_test.lvl"

if [[ ! -x "$BIN" ]]; then
    echo "fail: $BIN not built" >&2
    exit 1
fi
if [[ ! -f "$LVL" ]]; then
    echo "fail: $LVL not present (run editor shot first)" >&2
    exit 1
fi

TMP="$(mktemp -d -t soldut-test-play-ctf-XXXXXX)"
cleanup() {
    [[ -d /proc/$PID ]] && kill "$PID" 2>/dev/null
    sleep 0.3
    [[ -d /proc/$PID ]] && kill -9 "$PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

# Run from a tmpdir so the game's soldut.log + soldut.cfg don't
# collide with the project's. test-play has its own hardcoded
# config (overridden via the META auto-detect).
cd "$TMP"
"$BIN" --test-play "$LVL" >/dev/null 2>&1 &
PID=$!

# Wait long enough for the round to begin + the auto-detect log
# line to flush.
sleep 5

LOG="$TMP/soldut.log"
echo "log: $LOG"
PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS+1));
    else                echo "FAIL: $1"; FAIL=$((FAIL+1)); fi
}

asrt "test-play detected CTF map" \
     "grep -q 'test-play: detected CTF map' '$LOG' 2>/dev/null"
asrt "round begins in CTF mode" \
     "grep -q 'match: round begin (mode=CTF' '$LOG' 2>/dev/null"
asrt "ctf_init_round populated flags from .lvl" \
     "grep -q 'ctf_init_round: flags at RED(200,332) BLUE(1400,332)' '$LOG' 2>/dev/null"
asrt "CTF score limit clamped to 5" \
     "grep -q 'mode=CTF, map=0, limit=5' '$LOG' 2>/dev/null"

echo
echo "== test-play CTF summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
