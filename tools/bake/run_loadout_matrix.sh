#!/usr/bin/env bash
#
# tools/bake/run_loadout_matrix.sh — sweep (chassis × primary) × maps.
#
# For each map × each (chassis, primary) combination, runs a 30-second
# bake with ALL bots wearing that loadout. Records aggregate kills /
# fires / pickups into build/bake/loadout/<tag>.tsv and emits per-axis
# summary tables (per-chassis, per-primary, per-map).
#
# Usage:
#   ./tools/bake/run_loadout_matrix.sh                    # tag=iter0, 30 s, 8 bots, elite
#   ./tools/bake/run_loadout_matrix.sh iter1 60 8 elite

set -u

TAG="${1:-iter0}"
DUR="${2:-30}"
BOTS="${3:-8}"
TIER="${4:-elite}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/build/bake_runner"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make bake'"; exit 1; }

OUT="$REPO/build/bake/loadout/$TAG"
mkdir -p "$OUT"

MAPS=(foundry slipstream reactor concourse catwalk aurora crossfire citadel)
CHASSIS=(trooper scout heavy sniper engineer)
PRIMARIES=("Pulse Rifle" "Plasma SMG" "Riot Cannon" "Rail Cannon"
           "Auto-Cannon" "Mass Driver" "Plasma Cannon" "Microgun")

TSV="$OUT/matrix.tsv"
printf "map\tchassis\tprimary\ttotal_kills\ttotal_fires\ttotal_pickups\tavg_alive_s\n" > "$TSV"

total=$(( ${#MAPS[@]} * ${#CHASSIS[@]} * ${#PRIMARIES[@]} ))
done=0
echo "running $total combinations..."

for m in "${MAPS[@]}"; do
  for c in "${CHASSIS[@]}"; do
    for p in "${PRIMARIES[@]}"; do
      "$BIN" "$m" --bots "$BOTS" --duration_s "$DUR" --tier "$TIER" \
                  --chassis "$c" --primary "$p" \
                  > "$OUT/_run.log" 2>&1
      # Parse the per-mech TSV the bake just wrote to build/bake/<map>.per_mech.tsv
      PER="$REPO/build/bake/${m}.per_mech.tsv"
      if [[ -f "$PER" ]]; then
        # Sum kills, fires, pickups; avg alive_s.
        SUMS=$(awk -F'\t' 'NR>1 { k+=$8; d+=$9; f+=$10; pk+=$11; a+=$12; n++ }
                          END  { if(n>0) printf "%d\t%d\t%d\t%.1f", k,f,pk,a/n; else print "0\t0\t0\t0" }' \
               "$PER")
        printf "%s\t%s\t%s\t%s\n" "$m" "$c" "$p" "$SUMS" >> "$TSV"
      fi
      done=$((done+1))
      if (( done % 20 == 0 )); then echo "  ...$done/$total"; fi
    done
  done
done

echo
echo "=== per-chassis totals (sum across maps × primaries) ==="
awk -F'\t' 'NR>1 { k[$2]+=$4; f[$2]+=$5; p[$2]+=$6; n[$2]++ }
            END  { printf "%-9s %8s %8s %8s\n", "chassis","kills","fires","pickups";
                   for (c in n) printf "%-9s %8d %8d %8d\n", c, k[c], f[c], p[c] }' \
    "$TSV" | sort -k2 -nr

echo
echo "=== per-primary totals (sum across maps × chassis) ==="
awk -F'\t' 'NR>1 { k[$3]+=$4; f[$3]+=$5; p[$3]+=$6; n[$3]++ }
            END  { printf "%-14s %8s %8s %8s\n", "primary","kills","fires","pickups";
                   for (w in n) printf "%-14s %8d %8d %8d\n", w, k[w], f[w], p[w] }' \
    "$TSV" | sort -k2 -nr

echo
echo "=== per-map totals (sum across chassis × primaries) ==="
awk -F'\t' 'NR>1 { k[$1]+=$4; f[$1]+=$5; p[$1]+=$6 }
            END  { printf "%-12s %8s %8s %8s\n", "map","kills","fires","pickups";
                   for (m in k) printf "%-12s %8d %8d %8d\n", m, k[m], f[m], p[m] }' \
    "$TSV" | sort -k2 -nr

echo
echo "full matrix → $TSV"
