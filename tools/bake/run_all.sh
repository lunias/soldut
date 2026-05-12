#!/bin/bash
# Bake-test every shipped map. Builds the harness if missing, then runs
# in sequence and summarizes per-map pass/fail at the end.

set -e

cd "$(dirname "$0")/../.."

if [ ! -x build/bake_runner ]; then
    make bake
fi

mkdir -p build/bake

MAPS=(foundry slipstream reactor concourse catwalk aurora crossfire citadel)
BOTS="${BAKE_BOTS:-16}"
DURATION="${BAKE_DURATION_S:-60}"
SEED="${BAKE_SEED:-0xC0FFEE}"

PASSED=0
FAILED=0
RESULTS=()

for i in "${!MAPS[@]}"; do
    name="${MAPS[$i]}"
    if [ ! -f "assets/maps/${name}.lvl" ]; then
        RESULTS+=("[$((i+1))/${#MAPS[@]}] ${name}  SKIP (no .lvl)")
        continue
    fi
    printf '[%d/%d] %s ... ' "$((i+1))" "${#MAPS[@]}" "$name"
    if ./build/bake_runner "$name" --bots "$BOTS" --duration_s "$DURATION" --seed "$SEED" \
            >"build/bake/${name}.stdout.txt" 2>&1; then
        line=$(grep '^bake\[' "build/bake/${name}.stdout.txt" | tail -1)
        RESULTS+=("[$((i+1))/${#MAPS[@]}] ${name}  PASS — ${line}")
        printf 'PASS\n'
        PASSED=$((PASSED+1))
    else
        line=$(grep '^bake\[' "build/bake/${name}.stdout.txt" | tail -1)
        RESULTS+=("[$((i+1))/${#MAPS[@]}] ${name}  FAIL — ${line}")
        printf 'FAIL\n'
        FAILED=$((FAILED+1))
    fi
done

echo
echo "=== Bake summary ==="
for r in "${RESULTS[@]}"; do
    echo "$r"
done
echo
echo "${PASSED} passed, ${FAILED} failed"

if [ "$FAILED" -gt 0 ]; then exit 1; fi
exit 0
