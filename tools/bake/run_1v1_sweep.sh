#!/usr/bin/env bash
# tools/bake/run_1v1_sweep.sh — 1v1 acceptance test for Phase 1+2 of M6 P05.
# Runs 1v1 bakes over the 8 ship maps at every tier (or single tier if given).
#
# Usage:
#   ./tools/bake/run_1v1_sweep.sh                    # all tiers, 60s
#   ./tools/bake/run_1v1_sweep.sh veteran 60         # one tier
#   ./tools/bake/run_1v1_sweep.sh all   60  baseline # tag the output dir
set -u

TIER_ARG="${1:-all}"
DUR="${2:-60}"
TAG="${3:-iter8}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/build/bake_runner"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make bake'"; exit 1; }

OUT="$REPO/build/bake/1v1/$TAG"
mkdir -p "$OUT"

MAPS=(foundry slipstream reactor concourse catwalk aurora crossfire citadel)
case "$TIER_ARG" in
  recruit|veteran|elite|champion) TIERS=("$TIER_ARG") ;;
  all|*) TIERS=(recruit veteran elite champion) ;;
esac

TSV="$OUT/1v1.tsv"
printf "tier\tmap\tfires\tkills\tverdict\n" > "$TSV"

for tier in "${TIERS[@]}"; do
  for m in "${MAPS[@]}"; do
    LOG=$("$BIN" "$m" --bots 2 --duration_s "$DUR" --tier "$tier" 2>&1)
    LINE=$(echo "$LOG" | grep -E "^bake\[.*\]:.*(PASS|FAIL)" | tail -1)
    FIRES=$(echo "$LINE" | sed -nE 's/.*fires=([0-9]+).*/\1/p')
    K_FFA=$(echo "$LINE" | sed -nE 's/.*kills=R[0-9]+\/B[0-9]+\/F([0-9]+).*/\1/p')
    VERDICT=$(echo "$LINE" | grep -oE "(PASS|FAIL)" | tail -1)
    printf "%-10s %-12s FIRES=%-6s KILLS=%-3s %s\n" "$tier" "$m" "${FIRES:-0}" "${K_FFA:-0}" "${VERDICT:-?}"
    printf "%s\t%s\t%s\t%s\t%s\n" "$tier" "$m" "${FIRES:-0}" "${K_FFA:-0}" "${VERDICT:-?}" >> "$TSV"
  done
done

echo
echo "TSV → $TSV"
