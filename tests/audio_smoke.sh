#!/bin/bash
#
# tests/audio_smoke.sh — P19 smoke gate.
#
# Boots a 12-tick scripted run on Foundry, then greps soldut.log
# (the per-shot log written next to the shot's output PNGs) for any
# "audio:.*missing" line. A clean run = every manifest entry loaded
# from disk, no fallback no-ops in real play.
#
# Exit code: 0 if no `missing` lines, 1 otherwise.

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SHOT="tests/shots/audio_smoke.shot"
LOG="build/shots/audio_smoke/audio_smoke.log"

# Re-run from scratch so a stale log can't mask a regression.
rm -rf build/shots/audio_smoke

./soldut --shot "$SHOT" >/dev/null 2>&1 || true

if [ ! -f "$LOG" ]; then
    echo "FAIL: $LOG not produced (shotmode did not run)"
    exit 1
fi

if grep -q "audio:.*missing" "$LOG"; then
    echo "FAIL: missing audio file(s) reported in $LOG:"
    grep "audio:.*missing" "$LOG"
    exit 1
fi

# Sanity: confirm audio_init actually loaded everything.
if grep -E "audio_init: [0-9]+/[0-9]+ samples loaded" "$LOG"; then
    loaded=$(grep -oE "audio_init: [0-9]+/[0-9]+" "$LOG" | tail -1)
    case "$loaded" in
        *0/*)
            echo "FAIL: audio_init loaded 0 samples — assets/sfx/ missing?"
            exit 1 ;;
    esac
fi

echo "PASS: audio smoke — no missing audio at boot"
