# M6 P06 — perf-baseline.md

Measurements log — not a design doc. Per
`documents/m6/06-perf-profiling-and-optimization.md` §5 checkpoint.
Captured 2026-05-14 on the user's WSL2/WSLg + Windows 11 native dev
box.

## Bench harness

`./soldut --bench <secs> --bench-csv <out>.csv --window 3440x1440 [...]`
Built from `m6-perf-profiling` HEAD with `src/profile.{c,h}` always-on,
14 zones bracketing the main-loop seams (see `src/profile.h`).

Offline-solo + 1 bot at Veteran tier on Reactor by default; CLI flags
`--bench-map citadel --bench-bots 8` swap to the stress scenario.
Local mech is given `powerup_godmode_remaining = 9999` each frame so
the round doesn't end on first hit and the rolling window captures
active combat instead of summary screens.

Window: 3440×1440 ultrawide unless noted. Internal cap default 1080
unless `--internal-h 0` (uncapped) is specified.

## 1v1 reactor — the user's reported scenario

| Zone          | WSL median | WSL p99 | Win median | Win p99 | Δ (WSL−Win) median |
|---|---:|---:|---:|---:|---:|
| **PROF_FRAME**    | **20.66 ms** | **22.95** | **10.01 ms** | **10.65** | **+10.65 ms** |
| input         |  0.005 |  0.009 |  0.001 |  0.002 |  +0.004 |
| sim           |  0.049 |  0.105 |  0.029 |  0.056 |  +0.020 |
| bots          |  0.008 |  0.022 |  0.003 |  0.011 |  +0.005 |
| decal_flush   |  0.001 |  0.002 |  0.000 |  0.000 |  +0.001 |
| draw_world    |  0.704 |  0.956 |  9.780 | 10.419 |  −9.08  |
| draw_post     |  0.023 |  0.047 |  0.009 |  0.014 |  +0.014 |
| draw_blit     | **4.263** | **5.261** |  0.000 |  0.000 |  **+4.26** |
| draw_hud      |  0.037 |  0.078 |  0.028 |  0.043 |  +0.009 |
| draw_overlay  |  0.038 |  0.071 |  0.041 |  0.064 |  −0.003 |
| **present**   | **15.527** | **19.006** |  **0.080** |  **0.564** | **+15.45** |
| median FPS    | 48.4 |    | 99.9 |    | |
| % > 16.6 ms   | 100% |    | 0% |    | |

CSV: `build/perf/{wsl,win}-1v1-3440-capped.csv`.

## 1v1 reactor — UNCAPPED (internal_h=0, world rasterises at 3440×1440)

| Zone          | WSL median | WSL p99 | Win median | Win p99 |
|---|---:|---:|---:|---:|
| PROF_FRAME    | 20.19 ms | 23.44 | 10.00 ms | 10.72 |
| draw_world    |  0.672 |  1.035 |  9.822 | 10.370 |
| draw_blit     |  3.645 |  5.449 |  0.000 |  0.000 |
| present       | 15.855 | 19.020 |  0.046 |  0.562 |

Internal-cap on vs off changes WSL frame time by ≤0.5 ms — the
internal RT cap **isn't the working knob** for the WSL dev-loop slowness.

CSV: `build/perf/{wsl,win}-1v1-3440-uncapped.csv`.

## 1v1 reactor @ 1920×1080 (budget reference window)

| Zone          | WSL median | WSL p99 | Win median | Win p99 |
|---|---:|---:|---:|---:|
| PROF_FRAME    | 12.39 ms | 15.60 | 10.01 ms | 10.75 |
| draw_world    |  0.690 |  0.953 |  9.829 | 10.471 |
| draw_blit     |  0.385 |  1.674 |  0.000 |  0.000 |
| present       | 11.120 | 14.411 |  0.052 |  0.691 |
| median FPS    | 80.7 |    | 99.9 |    |

At 1920×1080 the WSL build sits at 80.7 FPS median — comfortable
dev-loop pace. `draw_blit` scales superlinearly with output pixel
count (4.26 ms at 3440×1440 → 0.39 ms at 1920×1080, an ~11× drop for
a ~2.4× pixel-count drop), confirming a per-pixel cost in the
WSLg→D3D12 boundary blit.

CSV: `build/perf/wsl-1v1-1080-capped.csv` + `win-1v1-1080-capped.csv`.

## Stress — 1v8 citadel @ 3440×1440 (closer to the v1 32-mech reference)

| Zone          | WSL median | WSL p99 | Win median | Win p99 |
|---|---:|---:|---:|---:|
| PROF_FRAME    | 25.82 ms | 28.45 | 10.00 ms | 10.75 |
| sim           |  0.214 |  0.331 |  0.100 |  0.199 |
| draw_world    |  5.779 |  6.408 |  9.708 | 10.468 |
| draw_blit     |  0.135 |  0.462 |  0.000 |  0.000 |
| present       | 19.438 | 22.111 |  0.070 |  0.652 |
| median FPS    | 38.7 |    | 100.0 |    |

WSL hits **38.7 FPS** under stress — solidly in the "dipping into
the 30s" range the user reported, even though the per-zone breakdown
shows the rendering pipeline is well inside its 8 ms render budget
(draw_world 5.8 ms < 8.0 ms budget; sim 0.21 ms < 4.0 ms budget).
Frame time is **almost entirely PRESENT** (19.4 ms / 25.8 ms = 75 %).

Windows at the same stress: 10 ms / 100 FPS sustained. No regression.

CSV: `build/perf/{wsl,win}-stress-3440-capped.csv`.

## Prose summary (the Phase 2 checkpoint answer)

1. **Hot zones in the user's WSL scenario.** PROF_PRESENT (15.5 ms
   median, 75 % of frame) and PROF_DRAW_BLIT (4.3 ms median, 21 % of
   frame). Together they account for ~95 % of WSL frame time.
   Everything else — sim, bots, draw_world, draw_post, draw_hud — is
   under 1 ms.

2. **Is Windows native healthy?** Yes. 100 FPS sustained in every
   bench scenario including 8-bot stress at 3440×1440. PROF_FRAME
   median 10.0 ms, p99 ≤ 10.75 ms. Vsync-locked at 100 Hz; we're
   spending all our headroom in DRAW_WORLD because GL native flushes
   the GPU pipeline there, but the actual draw cost is small —
   that's confirmed by 1v1 vs 1v8 differing by only the world-pass
   cost (0.7 ms → 5.8 ms on WSL, where PRESENT doesn't absorb), and
   Windows DRAW_WORLD staying flat at 9.8 ms across both because
   it's vsync-bound at the GPU pipeline drain point.

3. **WSL/Windows delta shape.** **Shape 1** per §5c.1:
   *"WSL PROF_PRESENT ≫ Windows PROF_PRESENT, every other zone
   identical → WSLg compositor is the cost."* With the added wrinkle
   that PROF_DRAW_BLIT also shows real WSLg boundary cost (+4.3 ms
   median, super-linear in window pixels). The internal-RT cap and
   the halftone shader pass + upscale blit pipeline is paying two
   full-screen `DrawTexturePro` boundary crossings (g_internal →
   g_post; g_post → backbuffer); only one of those is the actual
   composite, the other is preparation.

## Hypotheses the data supports

In order of expected impact:

- **H10a — collapse halftone shader + upscale into one pass**:
  PROTOTYPED, NOT ADOPTED. Measured WSL 1v1 reactor 3440 capped:
  before frame median 20.66 ms (PRESENT 15.5 / BLIT 4.26 / POST
  0.02). After collapse frame median 20.37 ms (PRESENT 15.5 / BLIT
  0.00 / POST 4.12). **Net frame win: 0.3 ms, within noise.** The
  draw_blit cost just migrated into draw_post — the WSLg compositor
  stall (PROF_PRESENT 15.5 ms) is what dominates the frame, and the
  stall lands wherever there's a swap-or-blit syscall to absorb it.
  Removing one DrawTexturePro doesn't remove the stall; it just
  changes where the stall lands. Also broke shot-test byte-
  identicality (the merged pass produces slightly different pixel
  values via a different GL state path — visually indistinguishable
  but cmp-different). Plan §11 calls byte-identicality a hard
  requirement.  Reverted.

  **Lesson:** the WSLg "shape 1" delta is a *per-frame compositor
  pacing* cost, not a *per-draw-call boundary* cost. Reducing the
  number of full-screen DrawTexturePro calls doesn't help if each
  call is a tiny fraction of frame time and the compositor stall
  fills whatever gap is left.

- **H10b — WSL-specific lower internal_h default**: NOT PURSUED.
  At 1920×1080 windowed (effectively the same as 3440×1440 with
  internal_h=900-ish), WSL gets 80.7 FPS. So lowering internal_h on
  the user's 3440 window WOULD help — but the user wants to *play
  on the 3440 window*, and the dev loop is one-step-removed from
  the production binary's behavior. We can't unilaterally drop the
  internal_h default below 1080 without changing the production
  visual.

- **H6 — In-process server thread contention**: NOT PURSUED. Per
  §5d, A/B against a standalone dedi process would tell us if the
  in-process server thread contributes. With only 1 bot the dedi
  thread is essentially idle (sim 0.05 ms / tick from the bench
  data), so threading isn't plausible as the dominant cost at the
  scenarios we're measuring.

## Hypotheses the data does NOT support (skip)

- **H1 — Per-mech sprite draw count.** DRAW_WORLD = 0.7 ms in 1v1,
  5.8 ms in 1v8. The 8x mech count gives 8x draw work; that's linear
  and within budget. No batch flush problem here.
- **H2 — pose_compute / mech_ik_2bone cost.** sim = 0.05 ms in 1v1,
  0.21 ms in 1v8. Sim is 0.5 % of frame time. Not the cost.
- **H3 — mech_jet_fx_step spawn rate.** sim cost is tiny across
  scenarios. Not the cost.
- **H4 — Constraint solver iterations.** Same — sim is fast.
- **H5 — HUD text path.** DRAW_HUD = 0.04 ms. Tiny.
- **H7 — Decal chunk count.** decal_flush = 0.001 ms in 1v1, 0.111 ms
  p95 in stress. Even at stress, not the cost.
- **H9 — Default window size on first frame.** Possibly relevant for
  the first-second hitch, but the steady-state numbers dominate
  perceived FPS.

## What this tells us about pillar 6 (the budget)

- `documents/10-performance-budget.md` says 4.0 ms sim + 8.0 ms
  render at 32 mechs. We measure 0.21 ms sim + 5.8 ms draw_world at
  9 mechs — comfortably inside both. The budget is correct; we are
  not exceeding it on the .exe.
- The user's "30 FPS dips" complaint is **the WSLg dev-loop tax**,
  not a budget breach. The H10a prototype showed that the tax is a
  *compositor pacing stall*, not a *per-draw-call boundary cost* —
  so internal-pipeline optimizations don't move the needle on the
  WSL FPS number. Mitigations available to the user without code
  changes: shrink the window (1920×1080 sustains 80.7 FPS on WSL),
  or run the Windows .exe (100+ FPS at 3440×1440).
- Windows production path is **100 FPS at 32-mech-class stress**
  — well above the 60 FPS budget contract. Headroom is intact.
  No code changes were warranted by Phase 2's data, so the .exe
  stays at its current measured numbers.

## Outcome

**Phase 4 produced no shipped code change.** The Phase 1 + 2
profiler / bench harness / baseline doc *are* the shipped artifacts;
the optimization experiment (H10a) was prototyped, measured to
not-help on WSL while breaking shot-test byte-identicality, and
reverted. The data establishes that the WSL dev-loop slowness is a
compositor-pacing tax outside this codebase's control. Future
optimization work should target the .exe budget at higher mech
counts (where DRAW_WORLD scales linearly and would eventually
matter) rather than the WSL number.
