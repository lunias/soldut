#!/usr/bin/env bash
# M6 P06 — Linux perf record + flamegraph for the bench harness.
#
# Runs ./soldut --bench 30 --bench-csv <out>/bench.csv --perf-overlay
# under `perf record -F 99 -g`, then collapses the call stacks into an
# SVG via Brendan Gregg's FlameGraph scripts.
#
# Requirements (host side, not vendored):
#   - perf            (linux-tools-common + linux-tools-generic, or a
#                     custom build off the WSL2 kernel — see
#                     documents/m6/06-perf-profiling-and-optimization.md
#                     §"Path B").
#   - stackcollapse-perf.pl / flamegraph.pl on PATH
#                     (clone github.com/brendangregg/FlameGraph and
#                     export PATH).
#
# Usage:
#   ./tools/perf/flamegraph.sh [out.svg] [bench-secs] [extra --soldut-args ...]
#
# Defaults: out.svg = build/perf/flamegraph.svg ; bench-secs = 20.
#
# Caveats:
#   - WSL2 does not support hardware perf counters. -F 99 (software
#     sampling) works; -e cache-misses does not.
#   - The .data + .folded intermediates land next to the SVG. They are
#     large (tens of MB) and gitignored under build/.

set -euo pipefail

OUT_SVG="${1:-build/perf/flamegraph.svg}"
BENCH_SECS="${2:-20}"
shift || true
shift || true

OUT_DIR="$(dirname "$OUT_SVG")"
mkdir -p "$OUT_DIR"

if ! command -v perf >/dev/null 2>&1; then
    echo "perf: not on PATH. See documents/m6/06-perf-profiling-and-optimization.md §'Install commands'." >&2
    exit 2
fi
if ! command -v stackcollapse-perf.pl >/dev/null 2>&1 || \
   ! command -v flamegraph.pl >/dev/null 2>&1; then
    echo "FlameGraph: stackcollapse-perf.pl / flamegraph.pl not on PATH." >&2
    echo "  git clone github.com/brendangregg/FlameGraph and export PATH." >&2
    exit 2
fi

PERF_DATA="${OUT_DIR}/perf.data"
PERF_FOLDED="${OUT_DIR}/perf.folded"
BENCH_CSV="${OUT_DIR}/perf-flamegraph-bench.csv"

echo "[flamegraph] recording for ${BENCH_SECS}s..."
perf record -F 99 -g -o "$PERF_DATA" -- \
    ./soldut --bench "$BENCH_SECS" \
             --bench-csv "$BENCH_CSV" \
             --perf-overlay --window 3440x1440 "$@" || rc=$?

echo "[flamegraph] collapsing stacks..."
perf script -i "$PERF_DATA" | stackcollapse-perf.pl > "$PERF_FOLDED"

echo "[flamegraph] rendering SVG..."
flamegraph.pl --title="soldut --bench ${BENCH_SECS}s" "$PERF_FOLDED" > "$OUT_SVG"

echo "[flamegraph] -> $OUT_SVG"
echo "[flamegraph]    (intermediate: $PERF_FOLDED, $PERF_DATA)"
echo "[flamegraph]    (bench CSV:    $BENCH_CSV)"
