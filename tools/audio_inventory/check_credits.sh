#!/bin/bash
#
# tools/audio_inventory/check_credits.sh — P19 CC0 compliance gate.
#
# Verifies that every audio file shipped under assets/sfx/ +
# assets/music/ has a matching row in assets/credits.txt.
# Returns non-zero (with the missing list) if any file lacks
# attribution; protects the project's CC0 hygiene per
# documents/01-philosophy.md §"Public domain or permissive".
#
# A "matching row" means the credits file contains the asset's
# relative path (e.g., `sfx/pulse_rifle.wav`) as a token somewhere
# in the row. Exact-match would be brittle (rows have variable
# whitespace + source URLs); substring is enough.

set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CREDITS="assets/credits.txt"

if [ ! -f "$CREDITS" ]; then
    echo "FAIL: $CREDITS missing"
    exit 1
fi

missing=0
shopt -s nullglob

check_file() {
    local f="$1"
    # Match on the path stripped of the "assets/" prefix.
    local rel="${f#assets/}"
    if ! grep -qF "$rel" "$CREDITS"; then
        echo "MISSING: $rel"
        missing=$((missing + 1))
    fi
}

for f in assets/sfx/*.wav assets/sfx/*.ogg assets/music/*.ogg; do
    check_file "$f"
done

if [ "$missing" -gt 0 ]; then
    echo
    echo "audio-credits: $missing unattributed file(s) — append rows to $CREDITS"
    exit 1
fi

# Count what we verified.
n=$( (ls -1 assets/sfx/*.wav assets/sfx/*.ogg assets/music/*.ogg 2>/dev/null || true) | wc -l | tr -d ' ')
echo "audio-credits: all $n assets attributed in $CREDITS"
