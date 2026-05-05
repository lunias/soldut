#!/usr/bin/env bash
#
# tests/test_play_mode_override.sh — verifies that --test-play +
# --mode <name> forces the requested match-mode regardless of the .lvl's
# META.mode_mask.
#
# Pre-requisites: assets/maps/ctf_test.lvl exists (a CTF-flagged map;
# auto-detect would normally pick CTF on it). We pass --mode ffa to
# force FFA, which proves the override beats the auto-detect.

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

PASS=0; FAIL=0
run_case() {
    local label="$1"; shift
    local mode_arg="$1"; shift
    local expect_log_re="$1"; shift

    local TMP
    TMP="$(mktemp -d -t soldut-test-play-mode-XXXXXX)"
    (
        cd "$TMP"
        if [[ -n "$mode_arg" ]]; then
            "$BIN" --test-play "$LVL" --mode "$mode_arg" >/dev/null 2>&1 &
        else
            "$BIN" --test-play "$LVL" >/dev/null 2>&1 &
        fi
        local PID=$!
        sleep 5
        [[ -d /proc/$PID ]] && kill "$PID" 2>/dev/null
        sleep 0.3
        [[ -d /proc/$PID ]] && kill -9 "$PID" 2>/dev/null
        wait 2>/dev/null
    )
    local LOG="$TMP/soldut.log"
    if grep -qE "$expect_log_re" "$LOG" 2>/dev/null; then
        echo "PASS: $label"
        PASS=$((PASS+1))
    else
        echo "FAIL: $label (expected /$expect_log_re/ in $LOG)"
        FAIL=$((FAIL+1))
    fi
    rm -rf "$TMP"
}

# Case 1: no --mode → auto-detect picks CTF (the map's META.mode_mask
# has the CTF bit and there are 2 flags placed).
run_case "default → CTF auto-detect" \
    "" \
    "match: round begin \(mode=CTF"

# Case 2: --mode ffa → override beats the auto-detect.
run_case "--mode ffa overrides CTF auto-detect" \
    "ffa" \
    "match: round begin \(mode=FFA"

# Case 3: --mode tdm.
run_case "--mode tdm overrides auto-detect" \
    "tdm" \
    "match: round begin \(mode=TDM"

# Case 4: --mode ctf is a no-op (auto-detect would already pick CTF on
# this map) but the override-log line should still appear.
run_case "--mode ctf logs override" \
    "ctf" \
    "test-play: --mode override .* mode=2"

echo
echo "== test-play mode-override summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
