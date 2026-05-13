#!/usr/bin/env bash
#
# tools/bake/run_matrix.sh — iterate bake_runner across the 8 ship maps
# at multiple difficulty tiers and write a TSV summary to
# build/bake/<tag>/summary.tsv (tag = $1 or "iter0").
#
# Usage:
#   ./tools/bake/run_matrix.sh              # tag=iter0, 8 bots, 60 s
#   ./tools/bake/run_matrix.sh iter1 30 4   # tag=iter1, 30 s, 4 bots

set -u

TAG="${1:-iter0}"
DUR="${2:-60}"
BOTS="${3:-8}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/build/bake_runner"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make bake'"; exit 1; }

OUT="$REPO/build/bake/$TAG"
mkdir -p "$OUT"

MAPS=(foundry slipstream reactor concourse catwalk aurora crossfire citadel)
TIERS=(veteran elite champion)

printf "map\ttier\tbots\tduration\tnav_nodes\tfires\tpickups\tunique_pickups\tungrabbed\tdead_cells\tkills\tverdict\n" > "$OUT/summary.tsv"

for tier in "${TIERS[@]}"; do
  for m in "${MAPS[@]}"; do
    echo "→ $m / $tier / ${DUR}s / $BOTS bots"
    LOG=$("$BIN" "$m" --bots "$BOTS" --duration_s "$DUR" --tier "$tier" 2>&1)
    HEAD=$(echo "$LOG" | grep -E "^bake\[.*\]: $BOTS bots" || true)
    TAIL=$(echo "$LOG" | grep -E "^bake\[.*\]:.*(PASS|FAIL)" | tail -1)
    NAV=$(echo "$HEAD" | sed -nE 's/.*, ([0-9]+) nav nodes.*/\1/p')
    FIRES=$(echo "$TAIL" | sed -nE 's/.*fires=([0-9]+).*/\1/p')
    PICKUPS=$(echo "$TAIL" | sed -nE 's/.*pickups=([0-9]+) \(.*/\1/p')
    UNIQ=$(echo "$TAIL" | sed -nE 's/.*\(([0-9]+) unique.*/\1/p')
    UNGR=$(echo "$TAIL" | sed -nE 's/.*([0-9]+) ungrabbed.*/\1/p')
    DEAD=$(echo "$TAIL" | sed -nE 's/.*dead-cells=([0-9]+).*/\1/p')
    KILLS=$(echo "$TAIL" | sed -nE 's/.*kills=([A-Za-z0-9\/]+).*/\1/p')
    VERDICT=$(echo "$TAIL" | grep -oE "(PASS|FAIL)" | tail -1)
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "$m" "$tier" "$BOTS" "$DUR" "${NAV:-0}" "${FIRES:-0}" "${PICKUPS:-0}" \
      "${UNIQ:-0}" "${UNGR:-0}" "${DEAD:-0}" "${KILLS:-0/0/0}" "${VERDICT:-?}" \
      >> "$OUT/summary.tsv"
    # Preserve the heatmap with a tier-tagged filename so we can compare.
    if [[ -f "$REPO/build/bake/${m}.heatmap.png" ]]; then
      cp "$REPO/build/bake/${m}.heatmap.png" "$OUT/${m}_${tier}.heatmap.png"
    fi
    if [[ -f "$REPO/build/bake/${m}.summary.txt" ]]; then
      cp "$REPO/build/bake/${m}.summary.txt" "$OUT/${m}_${tier}.summary.txt"
    fi
  done
done

echo
echo "=== $OUT/summary.tsv ==="
column -t -s $'\t' "$OUT/summary.tsv"
