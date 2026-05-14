# tools/perf

M6 P06 perf tooling. Host-side scripts that aren't bundled into the
binary. See `documents/m6/06-perf-profiling-and-optimization.md` for
the full plan.

## flamegraph.sh

Linux-only. Runs `./soldut --bench <N>` under `perf record -F 99 -g`,
then collapses the call stacks into an SVG.

```
./tools/perf/flamegraph.sh build/perf/flamegraph.svg 30
```

### Host requirements (not vendored)

- `perf` — `linux-tools-common` + `linux-tools-generic`. On WSL2 the
  generic package often doesn't match the Microsoft kernel; if it
  doesn't work, build perf from the WSL2 kernel source (see plan
  doc §"Path B").
- `stackcollapse-perf.pl` and `flamegraph.pl` from
  `github.com/brendangregg/FlameGraph` — clone and add to PATH.
  FlameGraph is CDDL-1.0 licensed; we deliberately don't vendor it.

### Output

The SVG opens in any browser. Widest bar at the top of the stack is
the hottest function; click to zoom. Cross-reference the `PROF_*`
zone names from the `--bench-csv` zone breakdown with the flamegraph
hierarchy.

### Caveats

- WSL2 does not support hardware performance counters
  (`cache-misses`, `branch-misses`, etc.). `-F 99 -g` (software
  sampling) works.
- `.data` + `.folded` intermediates land in `build/perf/`. They are
  large (tens of MB) and gitignored.

## What's NOT here

- Tracy / microprofile / optick / easy_profiler — rejected by
  `documents/01-philosophy.md` rule 14 (no new vendored deps).
- A Windows-side flamegraph helper — use WPR/WPA on Windows; CSV
  + zone breakdown are the cross-platform contract.
