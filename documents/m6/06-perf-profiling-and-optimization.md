# 06 — Performance profiling & targeted optimization

---

## ⚠ Orientation — read this first (you are the implementing Claude)

This document is a self-contained plan written by an earlier Claude
session and handed to you to execute. You are doing all the
profiling and the perf-fix work. The user wants to be hands-off
beyond the prerequisite-install step at the start.

### What is already true when you arrive

- **Repository:** `/home/lunias/clones/soldut`. Already cloned,
  already builds. The Linux-side build command is `make` (release)
  or `./build.sh dev` (ASan/UBSan). The Windows-cross build is
  `make windows` (uses `zig` via `cross-windows.sh`). The user has
  all of these working today; you do not need to set up the build
  toolchain.
- **Branch:** `m6-perf-profiling`. Already created. **Stay on it
  for everything in this plan.** Do not merge to `main` until §8
  validation passes on both platforms.
- **This plan file** is the only thing the branch contains so far.
  No code changes yet. Phase 1 §4 is where you start typing.
- **User's host machine:** Windows + WSL2/WSLg. They develop in
  WSL, **play on Windows native**. The .exe runs at 100+ FPS at
  3440×1440 in their reported scenario; the WSL build dips into the
  30s. **Both targets are in scope — see §1b.**
- **The user's resolution:** **3440×1440** (ultrawide). All bench
  scenarios target this; don't pivot to 1920×1080 unless §5a's
  matrix explicitly calls for it.
- **CLAUDE.md** is at the repo root and contains the load-bearing
  build / test / convention summary. Re-read it before you start
  changing anything; it captures the M6 P03 / P05 state of the
  build that you're building on.

### What you do at the start of your session — STOP HERE FIRST

The plan needs four tools that aren't already part of the build
toolchain. Before you do *any* of §4 (Phase 1), check whether they
exist and ask the user to install the missing ones. **Run this
check as your first action of the session**, before you read the
rest of the plan, before you propose any code change.

Paste this exact verification block into Bash:

```bash
# (1) Linux perf — WSL2 uses a custom Microsoft kernel, so the
# standard linux-tools-generic may or may not match. Verify the
# binary runs AND understands `-g` (call graph).
perf --version 2>/dev/null && \
  perf record -F 99 -g -o /tmp/perf-probe.data -- /bin/true 2>/dev/null && \
  perf script -i /tmp/perf-probe.data >/dev/null 2>&1 && \
  echo "perf: OK" || echo "perf: MISSING or BROKEN"
rm -f /tmp/perf-probe.data

# (2) FlameGraph scripts on PATH (Brendan Gregg's repo).
command -v stackcollapse-perf.pl >/dev/null && \
  command -v flamegraph.pl       >/dev/null && \
  echo "FlameGraph: OK" || echo "FlameGraph: MISSING"

# (3) glxgears (mesa-utils) — sanity check for the WSLg pacing
# ceiling per §6c.
command -v glxgears >/dev/null && echo "glxgears: OK" || \
  echo "glxgears: MISSING"

# (4) zig — already required for `make windows`, but confirm.
command -v zig >/dev/null && echo "zig: OK" || echo "zig: MISSING"
```

**Then, before doing anything else, present the results to the
user and ask them to install whatever's missing.** Use the
`AskUserQuestion` tool with a single multi-select question listing
the missing items. Recommended phrasing:

> "Before I can run the profile/perf work in
> `documents/m6/06-perf-profiling-and-optimization.md`, I need
> these tools installed on your WSL2 box: \[list the
> missing ones\]. Install commands are in the orientation
> section of the plan. Want me to print them now, or have you
> already got these?"

Do not start Phase 1 until each of the four lines above prints
`OK` (or the user explicitly says "skip that one, we'll fall back
to the CSV path"). The `--bench-csv` path works with **none** of
these — but `perf record + flamegraph` (§6a) is the diagnostic
fallback for "the CSV says zone X is hot, but which line in X?"
and without it Phase 3 collapses to "guess."

### Install commands the user needs

Hand these to the user when they ask. Do **not** run them on the
user's behalf — they require `sudo` and involve a kernel-source
build; the user owns that decision.

#### (1) `perf` on WSL2 — the tricky one

WSL2 runs a custom Microsoft kernel. The `linux-tools-generic`
package from Ubuntu often **does not match** the running kernel.
Two paths:

**Path A — try the generic package first (5 minutes, may not
work):**

```bash
sudo apt update
sudo apt install -y linux-tools-common linux-tools-generic
# Then re-run the verify block. If perf complains about kernel
# version mismatch, fall through to Path B.
```

**Path B — build perf from the WSL2 kernel source (~10 minutes,
always works):**

```bash
sudo apt install -y build-essential flex bison libssl-dev \
                    libelf-dev libdw-dev libdwarf-dev pkg-config

# Clone the kernel source matching your WSL2 kernel.
# uname -r gives e.g. "6.6.114.1-microsoft-standard-WSL2";
# the tag in the WSL2-Linux-Kernel repo is "linux-msft-wsl-<X.Y.Z>".
WSL_KVER="$(uname -r | sed 's/-.*//')"   # → "6.6.114.1"
git clone --depth=1 \
  --branch "linux-msft-wsl-${WSL_KVER}" \
  https://github.com/microsoft/WSL2-Linux-Kernel.git ~/wsl-kernel
cd ~/wsl-kernel/tools/perf
make
sudo cp perf /usr/local/bin/
perf --version   # should now report a version matching uname -r
```

**WSL2-perf caveat:** WSL2 does not support hardware performance
counters. `perf record -F 99 -g` works (it samples software
events / time), but cache-miss / branch-miss counters are
unavailable. For our scope (call-graph + on-CPU time) that's
enough; for deeper µarch work you'd need a Linux host.

#### (2) FlameGraph scripts

```bash
git clone --depth=1 \
  https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
echo 'export PATH="$HOME/FlameGraph:$PATH"' >> ~/.bashrc
# Either source ~/.bashrc in the current shell or open a new shell.
source ~/.bashrc
```

License note: FlameGraph is CDDL-1.0 — we **do not vendor it**
into the repo. The plan's `tools/perf/flamegraph.sh` calls
through to whatever's on `PATH` (§4d).

#### (3) mesa-utils (for `glxgears` sanity check)

```bash
sudo apt install -y mesa-utils
```

#### (4) zig

Should already be installed (cross-windows.sh needs it). If
missing:

```bash
# Easiest: download a recent release into ~/bin
curl -L https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz \
  | tar -xJ -C ~/bin
ln -s ~/bin/zig-linux-x86_64-0.13.0/zig ~/bin/zig
```

(Pick the version `cross-windows.sh` actually invokes; check
the script's `ZIG_VERSION` line first.)

#### Optional — Windows Performance Recorder (WPR/WPA)

Only needed if Phase 3 §6b.2 says you need a Windows-side
flamegraph equivalent. Most likely you won't — the
`--bench-csv` zone breakdown should be enough. If you do:

- Free, MSDN download: search "Windows ADK" + add the "Windows
  Performance Toolkit" component.
- Installed onto the **Windows** host, not WSL.

### How you run the Windows benches (no extra install)

The user develops in WSL but plays on Windows. You can — and
should — run the cross-compiled `.exe` *from your WSL shell* via
**WSL interop**: when you invoke an .exe, WSL2 launches it on the
Windows host. The .exe's window appears on the Windows desktop;
its stdout pipes back to your WSL terminal; relative file paths
write to whatever the .exe's working directory is.

**Important constraint:** the .exe should live under `/mnt/c/...`
(a Windows-accessible path), not under `/home/...` (Linux-only
filesystem). When `make windows` finishes, copy the build tree to
a known `/mnt/c/...` location once, then run from there:

```bash
# One-time setup (after the first `make windows`):
rm -rf /mnt/c/Users/lunias/soldut-bench
cp -r build/windows /mnt/c/Users/lunias/soldut-bench
cp -r assets        /mnt/c/Users/lunias/soldut-bench/

# Each bench:
cd /mnt/c/Users/lunias/soldut-bench
./Soldut.exe --bench 30 --bench-csv win-1v1-3440-capped.csv \
             --perf-overlay --window 3440 1440
cd -
cp /mnt/c/Users/lunias/soldut-bench/win-1v1-3440-capped.csv \
   build/perf/
```

(Confirm the user's actual Windows username and preferred path
the first time. The user uses `lunias` per the repo config —
check `/mnt/c/Users/` to see what's there before assuming.)

A `tools/perf/run_win_bench.sh` helper that wraps this shuffle is
worth writing during Phase 2 — it makes per-fix Windows
regression checks (§8c) routine instead of a chore. Don't
pre-build it before you've done a manual run that proves the
shuffle works on the user's box.

**You can run benches autonomously this way.** The user will see
a window pop up on Windows for ~30 seconds during each Windows
bench, and a window pop up in WSLg during each WSL bench. That's
the only visible signal to them; everything else lives in CSVs
under `build/perf/`. Mention it to them once at the start so
they're not surprised.

### What you ask the user for (and what you do yourself)

**Ask the user:**

- To install the four prerequisite tools (above), at the start of
  the session. Wait for confirmation before Phase 1.
- To confirm the Windows-side path (`/mnt/c/Users/<them>/`) the
  first time you copy a build there.
- To review the §5 baseline numbers once you've captured them —
  you don't need their approval to proceed, but a "yep, that
  matches my anecdote of 30-FPS dips in WSL and 100+ in the .exe"
  confirms you're measuring the right thing.
- Before merging, to play the .exe for 30 seconds and confirm it
  *feels* the same as before (the §8c bench numbers are the
  contract; the user's eyeball is the sanity check).

**You do yourself, autonomously:**

- All `make`, `make windows`, `make test-*` runs.
- All `--bench` invocations on both platforms (via WSL interop
  for the .exe).
- All `perf record` + flamegraph generation.
- Writing `documents/m6/perf-baseline.md` and updating it after
  every Phase 4 fix.
- Every code change in §4 + §7.
- Every regression sweep in §8.
- All commits on the `m6-perf-profiling` branch (one fix per
  commit, with the measured delta in the commit body per §7a).

**Don't** ask the user for permission to run benches, run tests,
write CSVs, or commit on the branch. Those are routine. **Do**
ask before any destructive action (force-push, branch deletes,
distclean) or before changing anything outside `src/` /
`documents/` / `tools/perf/` / `tests/shots/`.

### Where to stop and re-check with the user

Two natural checkpoints to verify alignment:

1. **After Phase 2** (baseline data captured on both platforms).
   Post a one-paragraph summary: "Here's the WSL/Windows delta
   in the user's scenario. The hot zone is X. I'm about to start
   Phase 3 / Phase 4 on hypothesis Y." User can redirect.
2. **After Phase 4 lands its first fix**. Post the measured
   delta (WSL + Windows numbers, before/after). User confirms
   the fix shape feels right (e.g. "no, I don't want bot AI tick
   rate lowered — find another angle").

Otherwise: head down, work the plan, report results at the end of
each phase.

---

**Status:** plan, not built. Branch `m6-perf-profiling`.

**Trigger:** user reports the build dipping below 60 FPS — sometimes
down to ~30 — when their window is maximised on a 3440×1440 ultrawide
monitor while playing 1v1 against a single bot, hosting the server
on the same machine. The dev hardware is well above the reference
laptop in `documents/10-performance-budget.md:22`, so this is not
"the budget caught up with us"; it is "we exceeded the budget and
didn't notice."

This document is the **diagnosis-and-fix plan** for a single Claude
session — or a small number of sequenced sessions, with explicit
checkpoints — to bring the build back inside its frame budget at
the user's resolution. Read it end to end before touching code.

The key word is **measure**. The plan front-loads instrumentation
and baseline capture because we do not know which line of code is
costing us the missing milliseconds. M6 P03 (internal-RT cap) and
M6 P02 (jet FX + 8000-particle pool) and M6 P01 (procedural IK
pose) all landed in the same milestone without a budget pass.
Pillar 6 and rule 7 say we don't keep coasting on that.

---

## 0 — Required reading first

Three project files override every choice below if they disagree.
Read each in full before you start:

- **`documents/01-philosophy.md`** — how we write code here. Rule 1
  (data layout is API design), **rule 2 (pure functions where
  possible)**, **rule 7 (we commit to numbers)**, rule 9 (one way
  to do each thing), and the anti-pattern list. Anything proposed
  below that drifts from these — drop it.
- **`documents/00-vision.md`** — pillar 6: *"It runs on a laptop.
  Sixty frames per second on integrated graphics with 32 players in
  heavy combat. Memory budget under 256 MB resident."* Pillar 1
  ("movement is the protagonist") is the **regression bar**: if a
  fix slows the camera response, blunts the jet feel, or adds
  latency to recoil, it is wrong even if FPS goes up.
- **`documents/10-performance-budget.md`** — the **contract**.
  4.0 ms simulate, 8.0 ms render, 4.45 ms slack. The hot loops, in
  order, and the order of investigation (`:138-144`). The numbers
  in this doc are what we are bringing the code back to, not
  numbers we are negotiating against.

Two adjacent M6 docs are *context*, not law:

- **`documents/m6/03-perf-4k-enhancements.md`** — the prior round's
  plan. P03 capped the internal RT and dropped MSAA. The "this
  measured 8 % wall-clock on WSLg" note in `CLAUDE.md`'s M6 status
  paragraph is the only place we have *any* numbers on the current
  pipeline; everything else here re-establishes them.
- **`documents/m6/01-ik-and-pose-sync.md`** + **`02-jetpack-propulsion-fx.md`**
  — the two recent additions whose budgets we never measured. If
  one of them is the culprit, those docs are where the
  understanding-the-machine context lives.

If you don't have those four files open, stop and open them now.

---

## 1 — Symptom and what we already know

### 1a — The report

> "When I maximize my window in a simple 1v1 match against a bot
> (I am hosting the server on the same machine as well) my FPS
> dips below 60 and sometimes down to the 30s."

### 1b — The hardware and the *two* execution targets

The single most important thing to keep in mind while reading the
rest of this plan: **the user develops on Linux/WSL2 but plays on
Windows native**, and the two paths have very different
performance characteristics on the same machine.

- User's monitor: **3440×1440** (ultrawide QHD, ~5.0 Mpx — the same
  monitor that drove P03).
- **Dev path (the loop): Linux / WSL2 / WSLg** (per `CLAUDE.md`).
  WSL graphics go through the WSLg compositor / FreeRDP path. WSLg
  is known to **frame-pace** GL clients independent of vsync
  settings — bug reports (`microsoft/wslg#368`, `microsoft/wslg#38`,
  `forums.developer.nvidia.com/.../350278`) describe `glxgears`
  capped at ~72–82 FPS on otherwise-fast machines, with
  `vblank_mode=0`/`__GL_SYNC_TO_VBLANK=0` *not* moving the cap.
  This is where the user reports the **30-FPS dips** in the
  reported 1v1-vs-bot scenario.
- **Production path (the contract): native Windows** — the
  `Soldut.exe` produced by `make windows` (zig-cross via
  `cross-windows.sh`). On the **same hardware**, **same monitor**,
  **same 3440×1440 maximised window**, **same 1v1-vs-bot
  scenario**, the Windows .exe sustains **100+ FPS** (user-
  reported, 2026-05-14). This is what the user actually plays
  with friends.
- Per `CLAUDE.md`: P03's stress bench measured *22.7 s no-cap vs
  20.9 s capped* on this same WSLg machine — an 8 % wall-clock win,
  described in-source as "modest because WSLg compositor frame-paces
  both, not GPU-bound here." That observation now has a name: the
  WSLg layer is the dominant cost on the dev path, *not* on the
  production path.

**Consequence for this plan:** the work splits into two
intertwined goals.

1. **Protect the production path.** Windows native is the contract.
   Currently 100+ FPS at 3440×1440 — that is **not allowed to
   regress** under any fix this plan proposes. Every optimization
   gets measured on Windows as well as WSL.
2. **Close the dev-loop gap.** The WSL build dropping to 30 FPS
   makes interactive iteration painful — that's the user's actual
   complaint. The right metric isn't "match the .exe" (we can't
   defeat the WSLg compositor); it's "stop dropping below 60 in
   the user's normal play scenario, and stay at vsync most of the
   time." Anything in our code we can trim that helps WSL more
   than it hurts Windows is fair game; anything that helps WSL
   only by hurting Windows is **not**.

So: **every bench run is paired**. WSL number and Windows number,
side by side. The gap between them is what this plan exists to
investigate. The Windows number on its own tells us "the .exe is
healthy"; the WSL number tells us "the dev loop is sick"; the
delta tells us *what kind of cost* WSLg is adding (per-draw-call
submission overhead? per-pixel readback? per-frame compositor
stall?) and that diagnosis drives which §7 hypothesis we pursue.

### 1c — The current pipeline (post-M6 P03 ship)

`renderer_draw_frame` in `src/render.c:1377-1616` does, in order:

1. `decal_flush_pending()` (line 1438) — flushes any blood/scorch
   queue onto chunked decal RTs **outside** the world Begin/End
   pair. Chunked allocs are 1024×1024 each, capped at 64.
2. `BeginTextureMode(g_internal_target)` (line 1458) — world rasterises
   into the capped internal RT (`internal_w × internal_h`, default
   2580×1080 on a 3440×1440 window).
   - Parallax FAR + MID (screen space)
   - World-space draws via `BeginMode2D`:
     decoration layers 0/1 → level tiles → polys → decoration
     layer 2 → decals → jet plumes → pickups → **mech loop**
     (each mech emits 17 sprite parts + 1 weapon + 1 grapple rope)
     → projectiles → fx → background polys → decoration layer 3
     → CTF flags
   - Parallax NEAR (screen space)
3. `BeginTextureMode(g_post_target)` (line 1520) — halftone + shimmer
   shader samples `g_internal_target` into `g_post_target`, both at
   internal res. Fragment cost ~80–150 ALU/pix; ~2.79 Mpx at default
   cap.
4. `BeginDrawing()` (line 1561) — backbuffer pass.
   - Aspect-preserving bilinear upscale of `g_post_target` to the
     window (`internal → window` letterbox).
   - HUD draws on top at **window resolution**, no shader. The
     `hud_draw` body is ~20–40 `DrawTextEx` calls/frame
     (kill feed, HP/armor/ammo numerals, match banner, weapon icon).
   - Optional perf overlay (`g_shot_perf_overlay`): 1 rect + 3
     `DrawText` calls.
   - Overlay callbacks (`draw_diag`, `draw_summary_overlay`, audio
     mute icon).
5. `EndDrawing()` (line 1602) — `SwapScreenBuffer` + `PollInputEvents`.

The simulate step at 60 Hz (`src/simulate.c:116-493`) does:

1. Snapshot → `render_prev` copy.
2. Hit-pause / FX-only branch.
3. Remote-mech `inv_mass` zero/restore (wan-fixes-3).
4. `mech_step_drive` × N — pose drive + recoil + weapons predict.
5. `mech_try_fire` × N (auth only).
6. **Physics: gravity → integrate → `physics_constrain_and_collide`**
   (12-iteration relaxation, ~2048 constraint slots × all particles).
   This is the hottest known phase.
7. `mech_post_physics_anchor` × N.
8. **M6 P01: `pose_compute` → `pose_write_to_particles`** per alive
   mech (3 × `mech_ik_2bone` + 16-bone procedural skeleton).
9. **M6 P02: `mech_jet_fx_step`** per alive mech with the
   `MECH_JET_*` state set.
10. `physics_push_mech_out_of_terrain`.
11. `mech_apply_environmental_damage`.
12. `pickup_step` + `projectile_step` + `fx_update`.
13. Damaged-limb smoke (gated every 8th tick).
14. Kill-feed age + shake decay.
15. `snapshot_record_lag_hist` (auth).
16. `shot_dump_tick` (gated on `g_shot_mode`).

### 1d — Why this build *should* easily hold 60 FPS at the user's res

A 1v1 vs bot at 3440×1440 with a 1080-cap internal RT means:

- **2 mechs** in the world. Mech draws are budgeted for 32 mechs
  (`10-performance-budget.md:50`).
- **One** active jet (typical) → halftone+shimmer loop runs at
  N=1 zone, almost no cost.
- **~30** active pickups, 0–4 projectiles, 100–500 FX particles
  — well inside the budgeted pools (4096 particles, 200 bullets).
- Citadel-class map: ~3–8 active decal chunks, ~2000 visible tiles.
- 2580×1080 world rasterisation: 2.79 Mpx, 56 % of native at 3440×1440.

If we cannot hit 60 FPS in this configuration, **the budget
breakdown in `10-performance-budget.md:24-60` is wrong somewhere**
— *or* we are paying a per-frame WSLg overhead that the budget
doc's "mid-range laptop" reference machine never measured.

The user-reported **100+ FPS on native Windows in this exact
scenario** is the strong evidence for the second reading: the
code is inside budget; the WSLg path is adding cost.

### 1e — What we already know is *not* the problem

- It is **not** the internal-RT cap not being on. The cap *is* on
  by default (1080), set from `soldut.cfg` / `soldut-prefs.cfg` /
  `--internal-h`, and the perf overlay confirms it (the
  `internal=WxH` field in the once-per-second SHOT_LOG).
- It is **not** the halftone shader at peak shimmer load. 1v1 vs
  bot has ≤2 active jet zones at any moment; the shimmer loop is
  uniform-count-bounded (`halftone_post.fs.glsl:79-106`), so the
  fragment loop body is *not* entered when count=0.
- It is **not** MSAA fillrate. P03 removed `FLAG_MSAA_4X_HINT` from
  `platform.c:113-115`.
- It is **not** a known sim hot loop in 32-mech worst-case
  (`10-performance-budget.md:130-136`). 1v1 has 2 mechs.
- It is **not** decal RT memory pressure. A 1v1 round paints
  a handful of chunks; we're nowhere near the 64-chunk cap.

Additional fact (added 2026-05-14 from the user): **the same
binary path produces 100+ FPS on native Windows** at the same
resolution and scenario. This rules out:

- **A simulate-path regression** in P01 IK / P02 jet FX / P05 bot
  AI being intrinsically over-budget. The same code runs both
  places; if it were just our sim or render work, Windows would
  be slow too.
- **A draw-call count problem on its own.** Windows submits the
  same draw calls; Windows is fine.
- **A particle-pool / FX-update inefficiency.** Same code, same
  pool, fine on Windows.

So the candidates collapse to:

1. **The WSLg path adds per-draw-call or per-pixel cost** that
   the Windows-native GL→D3D path does not. WSLg routes GL through
   a CBL (Common Buffer Layer) into D3D12 via dxgkrnl; each
   `glDrawElements` (or each batch flush, or each
   `BeginTextureMode` round-trip) crosses that boundary. We do not
   control the boundary. We **can** control how often we cross it
   (= reduce draw call count / batch flushes / RT-target
   switches).
2. **The WSLg compositor frame-paces** below our render budget.
   We do not control that either. We **can** stop right before
   the pacing kicks in by keeping our frame work tight enough to
   fit inside whatever sub-vsync window WSLg gives us.
3. **Host-thread contention in WSL2 specifically.** wan-fixes-16's
   in-process server thread is the production design (Windows
   refuses cross-process UDP between spawn-tree-related
   processes); on WSL2 the same in-process model works fine but
   competes with the WSLg compositor for CPU in a way Windows's
   native compositor does not.

We will not guess which one(s). We will measure — **on both
platforms** — and let the side-by-side delta point us at the
right work.

---

## 2 — Why this plan is necessary (philosophy alignment)

Per **`documents/01-philosophy.md`** rule 7 ("we commit to numbers")
and `documents/10-performance-budget.md:202`:

> *"We don't merge regressions on faith. We don't merge optimizations
> on faith either."*

Three M6 phases (P01, P02, P05) added per-tick CPU work and per-pixel
GPU work without an attached measurement. The 4.45 ms slack
`10-performance-budget.md:62` is mortgageable on purpose — but we
mortgaged it without writing the number down. The user's report is
the first invoice we noticed.

The right response is **not** to start guessing which line is slow.
The right response — straight out of `10-performance-budget.md:138`
— is:

> *"1. Measure — never speculate."*

So that is what this plan does. Phase 1 builds the measurement.
Phases 2 and 3 capture data. Only Phase 4 spends any of it.

The plan is also intentionally *conservative*. Each fix in Phase 4
is gated on a measurement from Phase 2/3 confirming the
hypothesis. If a hypothesis isn't backed by data, it doesn't ship.
Rule 8 of the philosophy doc ("No abstraction we cannot delete in
an afternoon") applies doubly to optimization code — anything we
add must be possible to rip out cleanly if it turns out the *next*
profiling round shows it was solving the wrong problem.

---

## 3 — Methodology overview

```
Phase 1 — Instrumentation         (build the tools; no perf change)
Phase 2 — Baseline measurement    (capture data on BOTH PLATFORMS)
Phase 3 — Diagnose                (perf flamegraph; cross-platform A/B)
Phase 4 — Targeted optimization   (one hypothesis at a time)
Phase 5 — Validation & regression (every test passes; .exe still 60+)
```

Each phase has an explicit **checkpoint** before the next begins.
**Do not skip ahead.** A Phase 4 fix without a Phase 2 number is a
guess; a guess that ships becomes a future revisit-trigger in
`TRADE_OFFS.md`. We don't want more of those.

**Cross-platform discipline is built in from Phase 1.** The
`profile.{c,h}` API, the `--bench` flag, and the `--bench-csv`
output all have to work identically on Linux and Windows builds —
`make windows` produces `build/windows/Soldut.exe`, and the user
runs `.\Soldut.exe --bench 30 --bench-csv bench.csv …` on Windows
the same way they run `./soldut --bench 30 --bench-csv bench.csv …`
in WSL. No platform `#ifdef`s in the profiler. fopen, GetTime,
plain stdio. Per rule 9.

Total budget for this work: **two to three sessions**. Phase 1 is
~half a day. Phase 2 doubles in size compared to a single-platform
plan (every cell is run twice — once in WSL, once on Windows
native — so ~a half-day instead of "a couple of hours"). Phase 3
~half a day for the WSL flamegraph + the Windows ETW-or-vsync-
spreadsheet equivalent (see §6c). Phase 4 is "as long as the data
demands but not longer." Phase 5 includes a Windows-side
regression pass.

---

## 4 — Phase 1: Instrumentation

**Goal:** make the question *"where did the milliseconds go this
frame?"* answerable from a log file or a 30-second bench run. No
behaviour change in the renderer or the simulate step. Build the
tools, then walk away.

### 4a — The new module: `src/profile.{c,h}`

A new pair of files. One header, one C file. The complete public
API surface — and the only public API — is small:

```c
/* src/profile.h */
#pragma once

#include <stdint.h>

typedef enum {
    PROF_FRAME = 0,        /* total wall-clock for the frame */
    PROF_INPUT,            /* platform_sample_input + GetMousePosition */
    PROF_SIM_STEPS,        /* sum of simulate_step calls this frame */
    PROF_BOT_STEPS,        /* host-side bot_step (auth only) */
    PROF_NET_POLL,         /* net_client_poll / net_server_poll */
    PROF_SNAP_INTERP,      /* snapshot_interp_remotes */
    PROF_RECONCILE,        /* reconcile_push_input + smoothing */
    PROF_DECAL_FLUSH,      /* decal_flush_pending */
    PROF_DRAW_WORLD,       /* BeginTextureMode(internal) ... End */
    PROF_DRAW_POST,        /* halftone shader pass into g_post_target */
    PROF_DRAW_BLIT,        /* DrawTexturePro upscale to backbuffer */
    PROF_DRAW_HUD,         /* hud_draw on backbuffer */
    PROF_DRAW_OVERLAY,     /* draw_diag / draw_summary_overlay callback */
    PROF_PRESENT,          /* EndDrawing — SwapScreenBuffer + PollInputEvents */
    PROF_COUNT
} ProfSection;

void profile_init(void);              /* zero counters, latch start time */
void profile_frame_begin(void);       /* call once per frame before any zone */
void profile_frame_end(void);         /* call once per frame at the end */
void profile_zone_begin(ProfSection); /* GetTime() to start a zone */
void profile_zone_end  (ProfSection); /* GetTime() to end; accumulates ms */

/* Read accessors — used by the overlay + the CSV dump. */
float profile_zone_ms(ProfSection);   /* most recent frame's accumulated ms */
float profile_frame_ms(void);         /* PROF_FRAME's ms */
float profile_zone_p99_ms(ProfSection); /* over the rolling N-frame window */

/* CSV dump support — see §4d. */
void  profile_csv_open(const char *path);
void  profile_csv_close(void);

/* Optional: a one-shot snapshot for SHOT_LOG. */
void  profile_format_summary(char *buf, int buflen);
```

The implementation is ~120 lines of C. Internals:

- One `double zone_start[PROF_COUNT]` array, one
  `double zone_acc_s[PROF_COUNT]` array.
- A 256-frame ring buffer of `float zone_ms[256][PROF_COUNT]` for
  the p99 window. 256 × 14 floats = 14 KB — fits in permanent
  arena easily.
- No malloc. No mutex (the simulate-step seam is single-threaded;
  the only other thread is the in-process dedi server, which has
  its own `World` and never touches this profile state — see §5d).
- `GetTime()` from raylib is the time source. It is monotonic and
  cheap; the raylib source measures it as a single
  `clock_gettime(CLOCK_MONOTONIC, …)` on Linux.
- The cost of a zone_begin / zone_end pair is **two `GetTime()`
  calls + one subtract + one add** ≈ 30 ns total on the user's
  hardware. With 14 zones that's ~420 ns/frame of self-cost —
  three orders of magnitude under the budget. Always-on.

### 4b — Where the calls go (file:line moves)

All edits in `src/main.c` and `src/render.c`. The simulate step
itself stays unedited — `PROF_SIM_STEPS` brackets the
`simulate_step` *calls* from `main.c`, not their internals. (If
Phase 3 / 4 shows the sim itself needs decomposing, that's a Phase 4
addition to `profile.h`, not a Phase 1 one.)

Concrete moves:

1. **`src/main.c:2080-2710`** — wrap the main loop's per-frame
   body with `profile_frame_begin()` and `profile_frame_end()`.
   The natural place is right inside the
   `while (!WindowShouldClose())` (find via `grep -n
   WindowShouldClose src/main.c`); begin at the top of the iteration,
   end after the renderer call returns but before the
   `mode_switch` post-amble.

2. **`src/main.c:2696-2705`** (the `platform_sample_input`
   block inside the inner sim-step `while (accum >= TICK_DT)` loop)
   — bracket each iteration with `PROF_INPUT` before
   `platform_sample_input` and `PROF_SIM_STEPS` around the
   `simulate_step` call. **Important:** `PROF_SIM_STEPS` is
   accumulating, not single-shot — multiple ticks per frame are
   summed. The macro `profile_zone_end(PROF_SIM_STEPS)` is called
   after each tick.

3. **`src/main.c:2747` or thereabouts** (the server-mode branch's
   `bot_step` + `simulate_step` block) — same pattern, plus a
   `PROF_BOT_STEPS` bracket around the `bot_step` call.

4. **`src/main.c:2792`, `2852`** (the two `renderer_draw_frame`
   call sites) — no zone bracket here; the renderer does its own
   zones internally. But add `PROF_OVERLAY_DRIVE` if you find
   the overlay path needs decomposing later (don't pre-build).

5. **`src/render.c:1438`** — wrap `decal_flush_pending()` with
   `PROF_DECAL_FLUSH`.

6. **`src/render.c:1458` and `:1462`** — `PROF_DRAW_WORLD` bracket
   around the `BeginTextureMode(g_internal_target)` → `EndTextureMode`
   pair. (This zone covers the entire world draw including
   parallax, world-space mode-2d draws, and the
   `EndTextureMode` flush.)

7. **`src/render.c:1520` and `:1534`** — `PROF_DRAW_POST` bracket
   around the halftone shader pass. The "no shader" fallback path
   bypasses this whole block; its zone stays zero.

8. **`src/render.c:1561` (just after `BeginDrawing`)** — start
   `PROF_DRAW_BLIT` right before the
   `DrawTexturePro(g_post_target, …)` call (or `g_internal_target,
   …` in the no-shader path); end it right after.

9. **`src/render.c:~1577` (where `hud_draw` is called)** — wrap
   with `PROF_DRAW_HUD`.

10. **`src/render.c:~1583` (where the overlay callback fires)** —
    wrap with `PROF_DRAW_OVERLAY`.

11. **`src/render.c:1602` (`EndDrawing` line)** — `PROF_PRESENT`
    bracket around `EndDrawing`. This zone is what catches the
    vsync wait + swap chain present.

12. **`src/render.c:1608-1614` (the existing once-per-second
    SHOT_LOG perf line)** — extend the format string to include
    every zone's last-frame ms plus the rolling p99. The single
    line should fit in ~200 chars. Use `profile_format_summary` to
    build it.

13. **`src/main.c:209` (where `--perf-overlay` is parsed)** —
    extend the perf overlay code in `render.c:1586-1601` to render
    a small text column listing each zone's ms (e.g. 6-line vertical
    stack just under the existing FPS line). 14 numbers × 1
    DrawText call each ≈ 14 calls/frame at window res — that's
    cheap and self-monitoring.

14. **`src/main.c`** — accept a new CLI flag `--bench-csv
    <path>`. When set, call `profile_csv_open(path)` once at
    startup and `profile_csv_close()` once at clean shutdown.
    `profile_frame_end` writes one CSV row per frame: `frame_idx,
    frame_ms, input_ms, sim_steps_ms, …, present_ms`. The CSV
    is ~150 bytes/row × 60 FPS × 60 s = ~540 KB / minute. Small.
    Use `stdio` (`fopen` + `fprintf`) — no streaming-IO library.

### 4c — `--bench` mode (single new CLI flag)

A new flag `--bench <seconds>` that:

1. Opens the game in offline-solo mode with one bot (re-using the
   existing offline-solo path).
2. Forces a known map (default Reactor; override with `--bench-map
   <id>`).
3. Forces a known loadout (default Trooper / Auto-Cannon /
   Frag Grenades / Standard armor / Standard jet).
4. Runs for `<seconds>` of wall-clock then exits cleanly.
5. Writes the CSV (if `--bench-csv` is set), the final
   `documents/m6/perf-bench.summary.txt` (median, p50, p95, p99 ms
   per zone), and exits with code 0 if all frames hit ≤ 16.6 ms
   p99, else code 1.

**Don't** invent a script-format DSL. Don't wire it to shotmode.
This is a binary toggle in `main.c` that overrides the
title→host→lobby→match flow into a single fast path. Shotmode's
`perf_bench.shot` family stays for fine-grained reproducibility;
this `--bench` flag is for the developer running a quick interactive
profile.

Concrete file/line plan:

- **`src/main.c:120-220`** (arg parser block) — add the two flags
  to `Args`, parse, and store.
- **`src/main.c`** somewhere after `platform_init` and before the
  main loop — if `args.bench_secs > 0`, bypass the title screen and
  jump straight into `mode = MODE_MATCH` with the bench loadout and
  the offline-solo + bot flags wired.
- Default map: Reactor (matches `tests/net/run_3p.sh` since P17).
  `reactor.lvl` is small enough that the level cache fits inside
  the warm-up.

### 4d — Make targets

Add four targets to the `Makefile`:

```make
# Phase 1 / 2 — interactive profile run.
perf-bench: soldut
	./soldut --bench 30 --bench-csv build/perf/bench.csv \
	         --perf-overlay --window 3440 1440

# Phase 1 / 2 — same, with the internal cap disabled (A/B).
perf-bench-uncapped: soldut
	./soldut --bench 30 --bench-csv build/perf/bench-uncapped.csv \
	         --perf-overlay --window 3440 1440 --internal-h 0

# Phase 1 / 2 — citadel + 8 bots (closer to v1-spec worst case).
perf-bench-stress: soldut
	./soldut --bench 30 --bench-csv build/perf/bench-stress.csv \
	         --perf-overlay --window 3440 1440 \
	         --bench-map citadel --bench-bots 8

# Phase 3 — perf-record + flamegraph.
perf-flamegraph: soldut
	./tools/perf/flamegraph.sh build/perf/flamegraph.svg
```

`build/perf/` should be `.gitignore`-d.

`tools/perf/flamegraph.sh` is ~30 lines of shell that wraps
`perf record -F 99 -g -- ./soldut --bench 30 ...`, runs
`perf script | stackcollapse-perf.pl | flamegraph.pl`, and emits an
SVG. Document the host requirement (`linux-tools-common`,
`linux-tools-generic`, and a clone of `brendangregg/FlameGraph`
on PATH or in `third_party/`) in a new
`tools/perf/README.md`. **Don't vendor FlameGraph** — it's a
GPL-licensed perl script; we link to it from docs, we don't pull
it into the binary, and the philosophy doc's
`01-philosophy.md:14-15` ("public-domain or permissive") makes
vendoring it a non-starter.

### 4e — What instrumentation we explicitly do **not** add

Per rules 8, 9, and the anti-pattern list in `01-philosophy.md`:

- **No Tracy.** Tracy is great. It is also a 30k-line C++ vendored
  dependency, with a network protocol, a viewer GUI, and a runtime
  hook in every TU. Three transitive libraries deep is "we add a
  fifth without writing it down here." (rule from
  `01-philosophy.md:14`).
- **No `#ifdef PROFILE` gates.** The zone macros are always on. They
  are cheap enough; gating them adds a config knob (which we don't
  add — `01-philosophy.md:16`) and an "is the build slow because we
  forgot to define it" trap.
- **No per-function timing wrappers.** Zones bracket *phases*, not
  functions. A zone has a name in `ProfSection`. If you want to
  decompose a zone further, add a new `PROF_*` enum value — don't
  invent a function-decorator mechanism.
- **No GPU timer queries.** raylib does not expose `glQueryCounter`
  cleanly and adding them via `rlgl` is two days of GL plumbing for
  data that `PROF_PRESENT - PROF_DRAW_BLIT` already approximates on
  a vsync-locked frame. If Phase 3 shows we are GPU-bound and need
  it, add it then — not before.
- **No frame-time graph rendering.** A live FPS chart is a tool we
  don't have a use for yet. The bench-CSV + offline plot pipeline
  (gnuplot one-liner, or eyeball the numbers in `column -t`) is
  enough.

### Checkpoint after Phase 1

- `./soldut` launches normally; no perceptible perf change.
- F3 / perf overlay shows the 14-zone breakdown.
- `make perf-bench` runs for 30 s, produces `build/perf/bench.csv`
  with one row per rendered frame and a summary block at the end
  of stdout.
- `make test-*` (every target listed in `CLAUDE.md`) passes byte-
  identical — instrumentation must not alter sim output, snapshot
  bytes, or shot-mode PNGs. Verify this explicitly:
  - `make test-snapshot` — round-trip bytes unchanged.
  - `make shot SCRIPT=tests/shots/walk_right.shot` — PNGs byte-
    identical to pre-instrumentation.
  - `tests/shots/net/run.sh 2p_dismember` — paired-screenshot
    byte-identical.

If any of those fail, the bug is in Phase 1, not in the test. Roll
back, find it, redo.

---

## 5 — Phase 2: Baseline measurement

**Goal:** capture *what the user sees*, **on both platforms**.
No fixes yet. Just numbers.

### 5a — Test matrix

Run each row on **both platforms**:

- **WSL2** (Linux): `./soldut --bench 30 --bench-csv … …`,
  invoked from the WSL terminal.
- **Windows native**: `.\build\windows\Soldut.exe --bench 30
  --bench-csv … …`, invoked from a Windows PowerShell or `cmd`
  prompt. The .exe is the same one the user plays with friends.

Each row at the user's resolution (3440×1440 maximised window)
*and* at the budget reference (1920×1080 windowed). Both with
the default internal cap on (`internal_h=1080`) and off
(`internal_h=0`). 30-second `--bench` runs.

| Scenario               | Map     | Bots | Notes |
|---|---|---|---|
| Idle, no combat        | Reactor | 1    | Stand and watch. Pure render path. |
| 1v1 combat             | Reactor | 1    | The user's reported case. Walk-and-fight. |
| 1v3 small fight        | Foundry | 3    | Bot AI runs server-side too. |
| 1v7 medium stress      | Citadel | 7    | Closer to the budget's design point (32 mechs total). |
| Worst case (jet + AOE) | Slipstream | 3 | Player holds jet, fires Mass Driver. Pushes shimmer + FX + projectile. |

5 scenarios × 2 resolutions × 2 cap settings × **2 platforms** =
**40 bench runs**. Yes, that's twice the work; it's also the
whole point — the **delta between the WSL row and the Windows
row** is the data Phase 3 and Phase 4 act on. A median-frame-ms
table that doesn't include both platforms answers the wrong
question.

Each writes a CSV; the summary table goes into a new
`documents/m6/perf-baseline.md` (a sibling of `04-map-balance.md`
that documents iter-by-iter numbers — it is **not** a design doc,
it is a measurements log). Layout the doc so each scenario gets a
side-by-side row pair: WSL median/p95/p99 in one column, Windows
median/p95/p99 in the next, **delta** (WSL minus Windows) in the
third. The delta column is the artifact you stare at.

### 5a.1 — How to actually run the Windows side

The user develops in WSL but plays on Windows. The Windows .exe
is built via `make windows` (which calls `cross-windows.sh`),
producing `build/windows/Soldut.exe` plus the `assets/` tree. To
run a bench on Windows:

1. From WSL: `make windows` (rebuild the .exe if any source
   changed in this session).
2. From WSL: `cp -r build/windows /mnt/c/Users/<you>/soldut-bench`
   (or wherever the user keeps their Windows-side play tree).
   `cross-windows.sh` already bundles `assets/` per wan-fixes-2;
   verify the `assets/` dir is present in the copy.
3. From a Windows `cmd` or PowerShell:
   ```
   cd C:\Users\<you>\soldut-bench
   .\Soldut.exe --bench 30 --bench-csv bench.csv ^
                --perf-overlay --window 3440 1440
   ```
   (`^` is the cmd-line continuation; PowerShell uses backtick.)
4. After the run, the CSV is at `C:\Users\<you>\soldut-bench\bench.csv`.
   From WSL: `cp /mnt/c/Users/<you>/soldut-bench/bench.csv
   build/perf/win-1v1-3440-capped.csv` (or similar naming).
5. Open the WSL and Windows CSVs side by side; populate the row in
   `perf-baseline.md`.

This shuffle is annoying but it's the price of running the
production binary on the production platform. **Do not** try to
infer Windows numbers from WSL numbers — the whole point is that
they don't correspond. Do not skip step 4 because "it's just a
small change"; the WSL→Windows delta is **the** data this plan
needs.

(If the .exe deployment shuffle becomes a chronic friction, a
small `tools/perf/run_win_bench.sh` that takes a scenario name +
output path and does the copy + ssh-or-explorer-invocation +
copy-back automatically is worth ~30 lines of shell. Not Phase 1;
add when needed.)

### 5b — Numbers to compute per run

From the CSV, for each `PROF_*` zone:

- **Median** ms (the typical-frame cost).
- **p95** ms.
- **p99** ms (the spike cost — the one the user actually feels).
- **max** ms (the outlier that explains the "30 FPS" report).

Plus, frame-wide:

- **Median FPS** = 1000 / median frame ms.
- **% frames over 16.6 ms** (the budget breach rate).
- **% frames over 33.3 ms** (the 30-FPS-or-worse rate).

### 5c — What "good" looks like, *per platform*

For the user's reported scenario (3440×1440, 1v1, cap=1080):

**Windows native** — the contract. This is what the user plays;
the .exe is the binary that has to feel right.

```
PROF_FRAME       median ≤ 16.6 ms,  p99 ≤ 16.6 ms,  max ≤ 25 ms
                 (vsync-locked at the user's 60 Hz monitor)
                 OR sustained well above vsync, frame-time
                 floor near the GPU's draw cost.
                 Currently 100+ FPS → expect PROF_FRAME ≤ 10 ms.
```

A regression here is **a release blocker**. The user is currently
happy with the Windows perf; the only acceptable outcome of this
plan on Windows is "still at least as fast as today, ideally
with smaller p99 spikes."

**Linux / WSL2** — the dev loop. We do not get to control the
WSLg compositor, so this isn't a 60-FPS contract; it is a
*usable iteration speed* target.

```
PROF_FRAME       median ≤ 22 ms       (~45 FPS, comfortable iter)
                 p99    ≤ 33 ms       (never drop below 30 FPS)
                 max    ≤ 50 ms       (no visible hitches)
```

If WSL median sits at 60 FPS (16.6 ms) consistently, that's a
bonus. The target is "stop dropping into the 30s," which is the
visible-pain threshold the user reported. Anything above 45 FPS
sustained makes the dev loop pleasant; anything above 60 makes it
indistinguishable from the .exe for non-twitch playtesting.

**Per-zone budgets (apply on both platforms, with the caveat
that PRESENT will absorb the WSLg pacing differently from the
Windows compositor):**

```
PROF_SIM_STEPS   median ≤ 4.0 ms   (one tick / two frames at 60 FPS)
PROF_DRAW_WORLD  median ≤ 5.0 ms
PROF_DRAW_POST   median ≤ 1.5 ms
PROF_DRAW_BLIT   median ≤ 0.5 ms
PROF_DRAW_HUD    median ≤ 1.0 ms
PROF_PRESENT     varies        (on Windows: vsync slack; on WSL:
                                 also absorbs compositor pacing)
```

(These are guidelines, not contract. The contract is
`10-performance-budget.md:9`: total frame ≤ 16.6 ms. The breakdown
above is one valid way to allocate that, and a sane target for the
zones we just added.)

### 5c.1 — The expected shape of the WSL/Windows delta

A useful exercise before reading Phase-2 data: predict what the
delta will look like, so the actual data either confirms or
surprises.

Three plausible shapes (from the candidate list in §1e):

1. **WSL `PROF_PRESENT` ≫ Windows `PROF_PRESENT`**, every other
   zone identical → WSLg compositor is the cost. Our code is
   doing the same work; the present-blit through dxgkrnl is the
   slow path. Mitigation: tighten the *other* zones to make sure
   the WSLg presents have less per-frame work to push.
2. **WSL `PROF_DRAW_WORLD` ≫ Windows `PROF_DRAW_WORLD`**, with
   the rest similar → per-draw-call submission overhead in the
   WSL GL stack. Mitigation: reduce draw call count (H1, H7).
3. **WSL all zones ~equally inflated relative to Windows** →
   general WSL CPU overhead. Mitigation: reduce total CPU work
   (H2, H3, H4, H6).

If the data fits shape 1, Phase 4 leans heavily on H5 (HUD) and
H7 (decals) — they reduce *what we hand to PRESENT*. Shape 2 →
H1 (sprite batching). Shape 3 → H2/H3/H4 (sim hot loops).

This is a hypothesis, not a forecast; the actual data wins.

### 5d — Host UI threading caveat (platform-aware)

Per `wan-fixes-16`, the host UI process runs two threads: the
client (the main thread doing render + input + reconcile) and a
dedicated server thread (`dedicated_run` in `src/main.c` /
`src/net.c`). Both run a `Game` struct; they communicate **only**
over a kernel UDP socket pair on `127.0.0.1`.

The Phase 1 profiler **only sees the client thread**.
`profile.{c,h}` has no atomics, no mutex, no cross-thread aggregation.
That is correct: the rendering thread's frame budget is what the user
sees, and the server thread is an independent (single-core's worth
of) CPU consumer.

But this matters for **interpretation** of Phase 2 results, and the
interpretation is different per platform:

- **On Windows**: the in-process server thread is the *only* way
  to host with a UI, because Windows silently drops UDP between
  any pair of processes that share a spawn-tree ancestor (the
  whole reason wan-fixes-16 exists). We cannot A/B against a
  standalone dedi process on Windows. The Windows .exe's host-
  UI scenario *is* in-process-threaded by construction.
- **On Linux/WSL2**: standalone-dedi-process + client-process
  over UDP loopback works correctly. So Phase 2 on Linux **can**
  A/B in-process vs standalone-dedi:
  - In-process (production shape): default `--bench` flow.
  - Standalone (diagnostic shape): spawn `./soldut --dedicated
    7777` in the background, then `./soldut --bench 30 --client
    127.0.0.1:7777 --bench-csv …` from another terminal.
- If the Linux **standalone-dedi** numbers are materially better
  than the Linux **in-process** numbers, host-thread contention
  is a real WSL-side cost. That's a finding, not a fix — see
  §7 H6 for the constrained ways we can mitigate it (lower bot
  AI tick rate, sched_yield seam, snapshot rate floor under
  load). We **do not** revert wan-fixes-16; the Windows bug it
  solved is the production bug.
- Windows host-UI numbers serve as the "WSLg vs Windows" delta
  baseline for the in-process path *and* the implicit upper
  bound on what the threading model can deliver. If Linux
  standalone-dedi is still slow vs Windows in-process, threading
  isn't the issue at all — WSLg is.

### Checkpoint after Phase 2

- `documents/m6/perf-baseline.md` exists, populated with the
  40-cell matrix (5 scenarios × 2 resolutions × 2 cap settings × 2
  platforms) plus the Linux-only in-process vs standalone-dedi A/B.
- Side-by-side WSL/Windows median/p99 numbers for each scenario,
  with a **delta column** highlighting where the gap is.
- A *prose answer* to: "(a) what zone(s) are over budget in the
  user's WSL scenario? (b) is Windows native still healthy? (c)
  what shape does the WSL/Windows delta have — present-heavy,
  draw-world-heavy, or evenly inflated?" Three sentences. No fixes
  proposed yet — that's Phase 4.

**If Phase 2 shows Windows native has regressed**, stop and find
out why before touching anything else. The .exe was 100+ FPS in
the user's testing; if our build doesn't reproduce that, either
our bench harness is broken or something has changed since the
user last played. Reproduce the 100+ FPS on Windows before
anything else.

**If Phase 2 shows the user's WSL scenario is within budget on
every zone but FPS is still dropping**, stop. The bottleneck is
outside our profiler — the WSLg compositor itself. See §6c. Likely
mitigation is to reduce total CPU work (the H2/H3/H4/H6 hypotheses).

---

## 6 — Phase 3: Diagnose

Phase 2 tells us *which zone* is over budget. Phase 3 tells us
*which line in that zone* is the cost.

### 6a — Linux `perf record` + flamegraph

The standard, free, accurate-down-to-the-function tool. `make
perf-flamegraph` runs the bench scenario under `perf record -F 99
-g`, then collapses the call stacks into an SVG that puts the
hottest function on top with the widest bar.

Workflow:

1. `make perf-flamegraph` → `build/perf/flamegraph.svg`.
2. Open the SVG in a browser. The widest box at any depth is a
   function name; click to zoom.
3. Cross-reference with the Phase 2 hot zone(s). If
   `PROF_DRAW_WORLD` is the hot zone and the flamegraph shows
   `DrawTexturePro → rlgl::rlPushMatrix → glUniformMatrix4fv`
   eating 30 % of frame, that's a draw-call-count problem. If it
   shows `__fmod → modf → floor`, that's a CPU math hot loop.

### 6b — Native Windows cross-check (already largely known)

The user already reports 100+ FPS on the .exe at 3440×1440. Phase 2's
Windows-side numbers will quantify this — but the qualitative
answer ("WSLg is materially slower; Windows native is healthy")
is essentially confirmed before this plan starts. Phase 3's
job here is to **characterise the gap**, not re-discover it.

The three Phase 2 outcomes break down differently with this prior:

- **Expected (and pre-confirmed): Windows is materially faster.**
  Phase 2 quantifies *how* much (median delta, p99 delta, per-zone
  delta). The flamegraph then explains *which* part of the WSLg
  path is the cost. Most of Phase 4 is about giving the WSLg path
  less work to do, without hurting the Windows path. See §7's
  hypothesis ordering.
- **Unexpected: Windows is similar to WSL.** Then the user's
  Windows-FPS observation needs reproducing — different
  hardware? different driver state? a stale .exe? Phase 4 is
  blocked until this is resolved; the user is the source of
  truth on the Windows number, but our binary has to confirm it.
- **Unexpected: Windows is slower.** Implausible given the user
  report; if observed, the WSL .so path is somehow helping —
  almost certainly an experimental setup artifact. Investigate
  before changing anything.

#### 6b.1 — Tools available on the Windows side

WSL has `perf record`. Windows native does not — equivalents:

- **PIX for Windows** (free, Microsoft, GPU + CPU timeline,
  works on D3D11/D3D12). Since WSLg routes GL→D3D12, and the
  .exe uses raylib's native GL3.3 path on Windows, PIX won't
  help inside our process directly. **Skip.**
- **Windows Performance Recorder / Analyzer (WPR/WPA)** — the
  Microsoft-blessed ETW tracer. Has a "GPU activity" profile and
  a "CPU sampled" profile; the latter gives a callstack
  histogram comparable to `perf record -F 99 -g`. **Use for the
  CPU side.** Free, installs from the Windows SDK.
- **The profiler we just built.** `--bench-csv` works identically
  on Windows; the same `profile.{c,h}` zones are populated.
  This is the **primary** measurement source on Windows too.
  WPR/WPA is only needed if the CSV zone breakdown points at a
  zone that's hot on Windows in a way it isn't on WSL (which
  would be surprising given the prior).
- **Task Manager → Performance tab**, per-process CPU + memory.
  Coarse but useful for confirming our binary, not Soldut.exe's
  child or hosted process, is the consumer.
- **Frame-time line graphs in the existing `--perf-overlay`**.
  The user can eyeball this without any extra tooling.

`tools/perf/flamegraph.sh` stays Linux-only. The cross-platform
contract is the CSV.

#### 6b.2 — What to do if WPR/WPA is needed

(Only if the Windows-side CSV reveals an unexpected hot zone.)

1. Install Windows ADK + WPR (free, MSDN).
2. From a Windows admin cmd:
   ```
   wpr -start CPU
   .\Soldut.exe --bench 30 --window 3440 1440 --bench-csv bench.csv
   wpr -stop trace.etl
   ```
3. Open `trace.etl` in WPA, view "CPU Usage (Sampled) → Function
   weight" by `Soldut.exe`. The top functions in that table are
   the Windows equivalent of a flamegraph's widest boxes.
4. Cross-reference with the WSL flamegraph. Same function names
   should appear because the source is the same; relative
   weights tell us which calls cost differently per platform.

Document the WPA top-function list in `perf-baseline.md` next to
the WSL flamegraph reference.

### 6c — One more cross-check: `glxgears` and `vblank_mode=0`

Sanity: on the user's WSLg box, what does `glxgears` say? If it
caps at 73 FPS regardless of `__GL_SYNC_TO_VBLANK=0` (the known
WSLg ceiling), that single fact bounds the *maximum* FPS our build
can hit there. The user reports 30 FPS, so 73 isn't our problem
yet — but if Phase 2 says we have a frame that fits in 11 ms and
WSLg still won't go past 60, that's the bound we're hitting.

Run: `vblank_mode=0 __GL_SYNC_TO_VBLANK=0 glxgears` for 60 s and
record the average FPS. Note it in `perf-baseline.md`.

### Checkpoint after Phase 3

- `build/perf/flamegraph.svg` saved off-tree (gitignored), and a
  one-screen screenshot embedded in `perf-baseline.md`.
- A *prose answer* to: "what specific function / call chain in the
  hot zone is the cost?" One paragraph. With the WSL-vs-Windows
  delta characterised by shape (per §5c.1: present-heavy /
  draw-heavy / evenly-inflated).
- A *short list* of §7 hypotheses the data supports, with a
  one-line "why this one" each. Hypotheses the data **doesn't**
  support are listed as "not pursued (reason: …)" so the next
  session doesn't re-investigate them.

Now we know enough to fix something.

---

## 7 — Phase 4: Targeted optimization (data-driven, hypothesis-ranked)

The list below is **hypotheses**. Each one is a guess about what
the data *might* show. Only the ones the data *does* show enter
the implementation queue. Do not pre-build any of them.

For each hypothesis, the entry has:

- **Guess**: what we think might be slow.
- **Evidence to demand**: what Phase 2/3 must show before we touch
  this.
- **Fix shape**: the cheapest viable fix that preserves features.
- **Helps WSL? Helps Windows?**: per-platform impact prediction.
  A fix that meaningfully helps WSL and is neutral on Windows is
  a clear ship; a fix that helps both is even better; a fix that
  helps WSL but slows Windows is **not allowed** (Windows is the
  production contract — see §1b, §7b).
- **Risk**: what regresses if we get it wrong.
- **Test**: how we measure the fix worked — on **both
  platforms**.

### 7.0 — Cross-platform expected impact

Given the prior that Windows is healthy (100+ FPS) and WSL is
slow (30–60 FPS), some fix shapes are *inherently* asymmetric:

- **Reducing draw call count / batch flushes** disproportionately
  helps WSL because every `glDrawElements` crosses the
  CBL→D3D12 boundary in WSLg. Windows GL drivers don't pay this
  cost; they help, but the win is smaller. **High-value on the
  dev loop, harmless on production.**
- **Reducing per-pixel work** (smaller internal RT, cheaper
  shader) helps both platforms proportional to fill rate. On
  Windows native at 100+ FPS we're not limited by fill, so the
  Windows gain is small; on WSL it might be the dominant cost.
- **Reducing total CPU work** (sim loops, IK, FX update) helps
  both platforms ~equally. On Windows it just adds slack; on WSL
  it gives the WSLg compositor more headroom inside its pacing
  budget.
- **Caching expensive CPU work between frames** (HUD text,
  precomputed pose tables) helps both, with no per-platform
  asymmetry.

Phase 4 should prefer fixes from the first and third categories
because their WSL value is highest and their Windows risk is
lowest.

### H1 — Per-mech sprite draw count

- **Guess**: 17 `DrawTexturePro` calls per mech (`render.c:1338-1355`,
  via `draw_mech_sprites` walking the 17-entry `g_render_parts[]`
  z-order) submit to the raylib batch. If the batch is *flushing
  between parts* (different blend modes around grapple/jet plumes,
  state changes from intermediate `DrawCircle` calls), this becomes
  17 × 2 = 34 GL draw calls per mech. At 1v1 that's still small;
  at 32 mechs it's 1088. Each `glDrawElements` on WSLg crosses
  the GL→D3D12 boundary, so this is exactly the kind of cost
  that's amplified there but cheap on Windows.
- **Evidence to demand**: flamegraph shows `rlglDrawRenderBatch`
  / `rlSetTexture` / `glDrawElements` ≥ 25 % of `PROF_DRAW_WORLD`,
  *and* the WSL/Windows delta is concentrated in `PROF_DRAW_WORLD`
  (shape 2 in §5c.1).
- **Fix shape**: pre-order the per-mech draws so the *entire*
  block from sprite parts → held weapon → grapple rope shares
  state (texture + blend). Currently the grapple rope uses
  `DrawLineEx` which forces a batch flush between sprites and
  ropes. Consolidate into a textured rope quad on the weapons atlas
  if needed.
- **Helps WSL? Helps Windows?**: **strongly helps WSL** (every
  avoided draw call is one fewer GL→D3D12 boundary crossing).
  **Marginally helps Windows** (driver still benefits, but
  Windows isn't bottlenecked here so the visible delta is
  small). Asymmetric in the right direction.
- **Risk**: visual regression on grapple rope. Mitigate by
  byte-comparing `tests/shots/m5_weapon_held.shot` and the grapple
  shot tests pre/post.
- **Test**: `PROF_DRAW_WORLD` zone delta in the bench **on both
  platforms**; no diff in shot tests; .exe still 100+ FPS in the
  user's reported scenario.

### H2 — `pose_compute` + `mech_ik_2bone` per-tick cost

- **Guess**: P01 moved the procedural pose into the simulate-step
  hot path. Per alive mech: 16 bone writes, 3 × `mech_ik_2bone`
  solves (2 legs + foregrip), plus `pose_write_to_particles` which
  walks every mech particle. Budget said "Pose drive 0.10 ms"
  (`10-performance-budget.md:30`). Reality (M6 P01) does meaningfully
  more work than the budget assumed.
- **Evidence to demand**: `PROF_SIM_STEPS` over budget AND
  flamegraph shows `pose_compute` / `mech_ik_2bone` / sqrtf in the
  top 5.
- **Fix shape**: (a) lift invariants (the per-chassis bone-length
  table is read inside the inner loop today; precompute the rest
  lengths once); (b) drop the third IK solve (the foregrip pin)
  for offscreen mechs; (c) memoize the cosf/sinf for the same
  aim_angle within a tick. **Cheapest first**, in that order.
- **Risk**: pose visibly desyncs from the M6 P01 design intent.
  Cross-check with `test-pose-compute` + `tests/shots/m5_chassis_distinctness.shot`.
- **Test**: `PROF_SIM_STEPS` median ↓ ≥ 1 ms in the 1v8 stress
  scenario, no diff in pose shot tests.

### H3 — `mech_jet_fx_step` particle spawn rate

- **Guess**: P02's exhaust + ground-dust spawn rate (up to 12
  particles/tick per jetting mech at Burst class) is the dominant
  contributor to FX-pool churn. 8000-particle cap, ~500 alive
  average, but the spawn-and-die rate is per-tick.
- **Evidence to demand**: flamegraph shows `mech_jet_fx_step` or
  `fx_update` in the top 5 *and* `PROF_SIM_STEPS` over budget.
- **Fix shape**: stash a per-chassis spawn-cadence multiplier in
  `g_jetpack_defs` and tune Burst down 50 %. Visual impact: less
  dense plume; tunable via the existing per-jet table — a feel
  question, not a budget question.
- **Risk**: jet feels weak. Mitigate by checking
  `tests/shots/jet_burst.shot` (if it exists; if not, add one).
- **Test**: same FX-spawn-rate delta in `PROF_SIM_STEPS`, no
  perceptible feel change.

### H4 — Constraint solver iteration count

- **Guess**: `physics_constrain_and_collide` runs 12 iterations
  per tick (a hard-coded 12 in `physics.c`). The budget assumed
  8 (`10-performance-budget.md:36`). The +50 % iter count is
  unbudgeted and unmeasured.
- **Evidence to demand**: `PROF_SIM_STEPS` over budget AND
  flamegraph shows `physics_constrain_and_collide` in the top 3.
- **Fix shape**: drop to 8 and watch the stability tests. If
  visibly more "spongy" ragdolls, drop only the slope-edge pass to
  8 and keep the rest at 12.
- **Risk**: the rule-1-style "particles tunnel through corners"
  bug class. Mitigate by re-running every existing physics
  regression: the spawn / drift-isolate / m1_big_fall / 2p_legs
  shots.
- **Test**: `PROF_SIM_STEPS` median ↓ ≥ 1 ms, no shot-test diff.

### H5 — HUD text path at 3440×1440

- **Guess**: ~20–40 `DrawTextEx` calls per frame at native window
  resolution. `DrawTextEx` is per-glyph: each character does one
  `DrawTextureRec` from the font atlas, with measure + kerning
  recomputed on every call. At 40 calls × 8 chars avg = 320 per-
  glyph draws / frame. Probably batches into ~2 GL calls (one
  atlas, alpha blend), but the CPU side does the measure work
  every frame even when the text didn't change. On WSLg each of
  those 320 per-glyph submissions could be hitting the boundary
  cost.
- **Evidence to demand**: `PROF_DRAW_HUD` ≥ 2 ms in the WSL 1v1
  scenario, OR the `PROF_DRAW_HUD` WSL-vs-Windows delta is large.
- **Fix shape**: cache rendered text strings into a small
  `RenderTexture2D` keyed by hash(string + font + size + colour),
  evict LRU. Most HUD strings change a few times per match
  (player names, score totals); kill-feed strings are immutable
  once posted. Sub-millisecond invalidation.
- **Helps WSL? Helps Windows?**: **strongly helps WSL** (collapses
  ~320 per-glyph submissions to ~20 cached-quad submissions).
  **Mildly helps Windows** (still cheaper CPU, just not the
  bottleneck). Asymmetric in the right direction.
- **Risk**: stale text on a HUD update. Mitigate by checking the
  diag overlay (which displays live FPS via `GetFPS()` — that
  string DOES change every second and must not be cached).
- **Test**: `PROF_DRAW_HUD` ↓ ≥ 50 % at 3440×1440 **on both
  platforms**, kill-feed shot tests byte-identical, .exe still
  100+ FPS.

### H6 — In-process server thread contention (WSL-specific)

- **Guess**: on the user's WSL box the host UI process has two
  CPU-bound threads (client + dedi). Plus WSLg adds a hot
  compositor thread outside our process. On a 4-core box the
  scheduler interleaves them in a way Windows's native
  scheduler doesn't have to. (Windows native runs the same
  two-thread model and is fine — so this is asymmetric: it
  could be a WSL-only cost component.)
- **Evidence to demand**: §5d's Linux in-process-vs-standalone-
  dedi A/B shows a material gap. And: the Windows in-process
  number stays at 100+ FPS (it should — that's the path the
  user already plays).
- **Fix shape**: (a) lower the bot AI tick rate (it can run at
  20 Hz instead of 60 Hz — the bot-side budget is already small);
  (b) lower the server's snapshot rate floor under heavy host CPU
  via the existing fallback ladder (`10-performance-budget.md:108-115`);
  (c) `sched_yield` calls between major dedi sim phases on the
  dedi thread (no-ops on Windows, helpful under WSLg).
- **Helps WSL? Helps Windows?**: **may help WSL** (less per-frame
  thread contention with the compositor). **Neutral or marginal
  on Windows** (already healthy; less work for the server thread
  is fine). Acceptable if the WSL win is large.
- **Risk**: input → snapshot latency grows. Bot AI feels less
  reactive.
- **Test**: paired-dedi 1v1 bench on WSL shows reduced WSL/Windows
  gap; gameplay bake on Reactor (`tools/bake/run_bake.c`)
  confirms bot AI is still PASS at every tier; .exe perf
  unchanged.

### H10 — WSLg-specific renderer tweaks (new, post Windows datapoint)

- **Guess**: there are knobs we can flip that change *how WSLg
  presents our frames* without changing what the .exe does on
  Windows. Two cheap candidates worth measuring before deeper
  work:
  - The **GL→backbuffer blit** at `render.c:1561-1602`:
    `DrawTexturePro` of `g_post_target` to the window is one
    full-screen `glDrawElements` that WSLg has to route. If we
    can collapse the halftone shader pass *and* the upscale
    blit into one full-screen quad (sample
    `g_internal_target` directly, apply the shader, output to
    backbuffer in one pass), we save one RT round-trip per
    frame. This is the "one less RT bounce" optimization that
    `documents/m6/03-perf-4k-enhancements.md` Phase 2 step 4
    left on the table.
  - The **`internal_h` default** for WSL detected at startup —
    if the user's WSL launch is heavier than the Windows
    launch, defaulting WSL to a lower internal cap (e.g. 900
    lines instead of 1080) cheaply trims fill work in the
    common dev-loop case. **Only** the WSL default changes; the
    Windows default stays at 1080.
- **Evidence to demand**: data from §5c.1's "shape 1" case:
  `PROF_PRESENT` or `PROF_DRAW_BLIT` shows a large
  WSL/Windows delta.
- **Fix shape**: (a) fold the halftone shader + upscale into one
  pass (one new fragment shader, drops the `g_post_target` RT
  entirely). (b) probe an env var (`SOLDUT_WSL=1`) or detect
  `WSL_DISTRO_NAME` at startup; if set, default `internal_h`
  to 900. **Either** can ship independently.
- **Helps WSL? Helps Windows?**: (a) helps WSL (one fewer
  RT round-trip = one fewer boundary crossing) and helps
  Windows (one fewer fragment pass over `g_post_target`).
  **Symmetric, neutral or positive on both.** (b) helps WSL
  only (Windows keeps 1080 cap); zero Windows risk by
  construction.
- **Risk**: (a) halftone visual changes; pixel-byte-identical
  shot tests must hold (shot mode forces `internal_h=0`, so the
  collapsed pass still has to produce the same pixels when the
  internal RT is window-sized). (b) WSL users see a slightly
  softer halftone; opt-out via existing `--internal-h` CLI
  flag.
- **Test**: pixel-byte-identical shot tests, .exe 100+ FPS hold,
  WSL median FPS up.

### H7 — Decal chunk count

- **Guess**: at 3440×1440 maximised on Citadel, the viewport
  overlaps up to ~4 chunks. Decal_draw_layer iterates **all**
  allocated chunks regardless of viewport overlap.
- **Evidence to demand**: `PROF_DRAW_WORLD` includes a hot zone
  inside `decal_draw_layer`; flamegraph confirms.
- **Fix shape**: cull non-visible chunks. AABB-vs-camera-rect
  reject before the `DrawTextureRec` call. ~20 lines of math in
  `src/decal.c` around line 253-291.
- **Risk**: a chunk on the edge of viewport gets popped (visible
  one frame, gone next). Mitigate by inflating the cull rect by
  one chunk dimension.
- **Test**: `PROF_DRAW_WORLD` ↓ in the big-map scenarios.

### H8 — `EndDrawing` / vsync stall

- **Guess**: `PROF_PRESENT` should be the slack zone — under vsync
  it eats whatever frame time we didn't spend elsewhere. If it's
  *small* and `PROF_FRAME` is still over budget, we are CPU-bound
  by other zones. If it's *huge* and the other zones add up to
  fine, vsync is doing its job and we're at the WSLg compositor's
  ceiling.
- **Evidence to demand**: this is diagnostic, not actionable.
  Used to label rows in `perf-baseline.md`.
- **Fix shape**: none. PRESENT is a *measurement*, not a hot zone
  we own.

### H9 — Default window size on launch

- **Guess**: `wan-fixes-13` set the default window to 1920×1080
  (`src/main.c` around the platform_init call). If the user
  maximises after launch, raylib recreates the GL context with the
  3440×1440 backbuffer. `ensure_internal_targets(iw, ih)`
  reallocates the RTs on size change — and a *bilinear filter on
  re-creation* might cause sustained driver pressure on WSLg.
- **Evidence to demand**: `PROF_DRAW_BLIT` shows sustained
  outlier ms over the first ~10 frames after maximise; the
  steady-state is fine.
- **Fix shape**: pre-size the internal targets to the
  user's monitor resolution at startup via `GetMonitorWidth/Height`,
  rather than waiting for the first window resize.
- **Risk**: a few MB of VRAM allocated up front the user might
  never use.
- **Test**: maximise during `--bench`; first-second p99 drops.

### 7a — Fix-application discipline

Per `10-performance-budget.md:198`:

> *"Every commit that touches a performance-sensitive module
> includes a one-line note on the impact, measured."*

Each Phase 4 fix lands as a **separate commit** with the perf
delta in the commit body. Format (from the budget doc):

> "render: cache hud text strings into a 256-entry RT pool.
>  Measured PROF_DRAW_HUD median 2.1 → 0.4 ms at 3440x1440, 1v1
>  reactor (Trooper / Auto-Cannon) on user's WSLg machine."

No "minor cleanup" rolled into a perf commit. No "while I was in
there" refactors. **One fix per commit, with the number.**

### 7b — When to stop (per platform)

The plan ends when **all** of the following hold:

**Windows native (production contract):**

- The user's reported scenario (3440×1440 maximised, 1v1 vs bot)
  still hits **at least 60 FPS sustained at vsync lock**, and
  ideally retains the current ~100+ FPS headroom.
- No `--bench` scenario regresses by more than 2 % in median or
  p99 frame ms vs the Phase 2 Windows baseline.
- All asserting tests pass identically; shot-test PNGs byte-
  identical (shot mode forces `internal_h=0`).

**Linux / WSL2 (dev loop):**

- The user's reported scenario (3440×1440 maximised, 1v1 vs bot)
  sustains **median ≥ 45 FPS**, with **p99 ≤ 33 ms** (never
  drops into the 30s).
- Ideally **median = 60 FPS** at vsync, but that's a stretch
  target — WSLg's compositor pacing may prevent it regardless
  of our code.
- All asserting tests pass.

**Budget contract (`documents/10-performance-budget.md`):**

- The reference scenario (32 mechs, 200 bullets, 4000 particles
  — `10-performance-budget.md:24-60`) sustains 60 FPS at the
  budgeted breakdown OR the budget doc is updated to reflect the
  new measured breakdown with a one-paragraph justification.
  This is platform-agnostic — the budget is what the .exe
  delivers; if the .exe is over, we re-tune; if the .exe is
  under but WSL isn't, that's a known dev-loop-only gap not a
  budget breach.

Not before, not after. We do not chase 120 FPS
(`10-performance-budget.md:212`). We do not optimise zones that
are already inside budget. We do not break the Windows path to
help WSL.

---

## 8 — Phase 5: Validation & regression guards

**Goal:** every fix in Phase 4 ships without breaking anything.
Per `documents/01-philosophy.md` rule 10 ("the code is the spec")
the regression suite is the contract.

### 8a — Required test pass list (every fix in §7)

Each Phase 4 commit must run, in order, with each passing:

- `make` (host native, default `-O2 -Werror`).
- `./build.sh dev` (ASan/UBSan; the philosophy doc and
  `10-performance-budget.md:193` both call this out).
- `make windows` (cross-compile to Windows .exe).
- All assertion-based unit tests:
  `test-physics test-level-io test-pickups test-ctf test-snapshot
   test-spawn test-prefs test-map-chunks test-map-registry
   test-map-share test-editor test-spawn-e2e test-meet-named
   test-meet-custom test-ctf-editor-flow test-grapple-ceiling
   test-frag-grenade test-bot-nav test-bot-playtest test-mech-ik
   test-pose-compute test-riot-cannon-sfx test-audio-smoke`
- Paired-process: `tests/net/run.sh`, `tests/net/run_3p.sh`,
  every script under `tests/shots/net/run*.sh`.
- Shot regression: byte-identical PNGs for the existing
  `tests/shots/*.shot` files. Shot mode forces `internal_h=0`
  (`src/shotmode.c`), so pixel-identicality is the right bar.
- Visual regression for the new fixes: `make
  host-overlay-preview`, `lobby-overlay-preview`.

### 8b — New tests to add

One new shot test per landed Phase 4 fix that exercises the path
the fix touched. Cheaper than re-explaining what was risky in a
prose doc.

Plus one **always-on** test: `tests/shots/perf_1v1_native_capped.shot`
— a 1v1 vs bot Reactor scenario at 3440×1440 with the
default cap. SHOT_LOG asserts every frame is ≤ 16.6 ms p99 (use a
new `perf_assert max_ms <N>` directive — small extension to
shotmode parsed in `src/shotmode.c`'s directive table). If a future
change regresses the user's scenario, this test fails in CI.

### 8c — Windows regression pass

For every Phase 4 fix, **before** declaring the commit shippable:

1. `make windows` cross-builds `build/windows/Soldut.exe` clean.
2. Deploy + run the Phase 2 bench scenarios on Windows native.
3. Compare new Windows CSV against the Phase 2 Windows baseline.
4. Fail the fix if Windows median or p99 regresses > 2 %.

This is the **only** way to keep the .exe at 100+ FPS through
this round of work. Skipping the Windows regression check on the
assumption "WSL win shouldn't hurt Windows" is the bug that
breaks the user's actual play experience.

If the Windows-side bench shuffle (§5a.1) is too slow for fast
iteration, batch it: do 3–5 candidate fixes on WSL in a session,
then do one Windows pass on all of them at once. **Do not** push
a fix without a Windows confirmation, even if WSL looks good.

### 8d — Trade-off and budget bookkeeping

For each fix that **lowers fidelity to make the budget**:

- Add a TRADE_OFFS.md entry: what was lowered, why, what
  triggers a revisit. (e.g. "Burst jet spawn rate halved; revisit
  if the jet feels weak in user testing.")

For each fix that **brings the code back into the documented budget**:

- Delete the corresponding "we exceeded budget on purpose"
  TRADE_OFFS entry if one was added during M6 P01 / P02 / P05.
- Update `documents/10-performance-budget.md` if the breakdown
  shifted (e.g. if the pose-drive line is now 0.30 ms instead of
  0.10 ms, fix the number AND explain in the commit body that
  pose-drive grew because of M6 P01's procedural skeleton, and
  that 0.30 ms is the new contract).

### 8e — CURRENT_STATE.md update

The final commit updates `CURRENT_STATE.md` with:

- Pre/post numbers in the user's scenario, **both platforms
  side by side**. The "M6 P06 numbers" line in CURRENT_STATE
  should read like: "1v1 vs bot, 3440×1440, internal_h=1080 —
  WSL: 32 → 56 FPS median; Windows: 110 → 108 FPS median
  (within noise)."
- The flamegraph snapshot of the hottest zone (pre-fix), saved
  to `documents/m6/assets/perf-flamegraph-prefix.svg` (the only
  asset this plan ships — a static SVG, no binary blob).
- Which hypotheses from §7 the data did **not** support (kept in
  the doc for the next session so we don't re-investigate them).

---

## 9 — Anti-patterns we explicitly reject

Per `documents/01-philosophy.md` "Anti-patterns we forbid" plus
this round's specific traps:

- **No "while we're here" refactor**. The Phase 4 fixes are
  *single-purpose*. If during a fix you notice a code-smell in an
  adjacent function, write it down in a comment with `// PERF-M6:
  …` and move on. We do not bundle.
- **No premature SIMD**. The budget doc lists SIMD as the *last*
  recourse (`10-performance-budget.md:144`). Auto-vectorization is
  enough until measurement proves otherwise.
- **No "make it generic" framework moves**. A 2-bone IK in C is
  a 2-bone IK in C. We do not extract `IKSolver` interfaces. We
  do not abstract `RenderPass` into a vtable. (rule 8.)
- **No new threading.** Per `01-philosophy.md` anti-pattern 4
  (post-wan-fixes-16 wording), threads are added deliberately, one
  at a time, with explicit message queues and zero shared mutable
  state. None of the §7 hypotheses requires a thread to fix; if
  one ever does, that's a separate doc.
- **No new vendored dependencies.** `01-philosophy.md:14`. Tracy,
  microprofile, optick, easy_profiler, etc. — they are all
  rejected. `GetTime()` is enough.
- **No "feature flag" toggle.** `01-philosophy.md:16`. The Phase
  4 fix either ships or doesn't. No `#define ENABLE_HUD_CACHE`.
- **No hot-reload of profiler config.** A bench run is a bench
  run. Re-launch to change a flag.
- **No GPU profiler integration in v1.** RenderDoc, NSight, even
  raylib's `glQueryCounter` wrapper — out of scope. CPU-side
  bottleneck investigation comes first
  (`10-performance-budget.md:213`).
- **Do not bypass vsync as a "fix."** `SetTargetFPS(0)` + no
  `FLAG_VSYNC_HINT` could free PROF_PRESENT but adds tearing and
  produces FPS numbers we can't compare to other measurements.
  Vsync stays on. The bench scripts that disable vsync
  (`tests/shots/perf_*.shot`) are for raw-throughput A/B only;
  the user's play scenario keeps vsync.
- **Do not enable LTO.** Budget doc, `10-performance-budget.md:179`:
  "no LTO at v1." Per-TU compile speed is part of the contract,
  and LTO bites it.

---

## 10 — A list of things this plan is NOT

So the implementing session doesn't get pulled into adjacent work:

- It is **not** a rewrite of the post-process shader. The
  halftone+shimmer shader is already cheap when the zone count
  is 1. If the data says otherwise, the H1/H3/H5 plan above
  covers it.
- It is **not** a switch to instanced rendering. raylib's CPU-
  batch is enough by design (`10-performance-budget.md:154`).
  Instancing is a stretch reserve, not a Phase 4 plan.
- It is **not** an audio-mixer optimization. Audio runs on its
  own raylib thread; if a flamegraph names `miniaudio_*` as a hot
  CPU consumer **on the main render thread**, we re-open this.
  Otherwise, no.
- It is **not** a network protocol overhaul. We do not bump the
  protocol id. We do not change the snapshot wire format. Net is
  budget-internal at the moment (downstream ~60 kbps, well under
  the 80 kbps target).
- It is **not** a tile culling pass. `draw_level_tiles` iterates
  every tile in the level grid (`render.c:281-318`). That is
  noted as a separate concern in `documents/m6/03-perf-4k-enhancements.md`
  §6; it stays out of scope here unless the flamegraph names it.
- It is **not** a Steam Deck pass. We do not target Steam Deck at
  v1 (`10-performance-budget.md:206`).
- It is **not** an FSR1 / temporal upscaler refactor. Same
  reasoning as `documents/m6/03-perf-4k-enhancements.md:198-218`;
  the halftone aesthetic eats the detail those upscalers preserve.
- It is **not** a SIMD pass on the integrate loops.
  `10-performance-budget.md:144` explicitly puts SIMD last.

If the implementing session notices that any of these adjacent
problems *are* now in the critical path post-fix, write a new
`documents/m6/07-...md` document. Don't fold the work in.

---

## 11 — Hand-off summary for the next-session Claude

If you're the session implementing this: **read §1b, then §3-§5
carefully, then §7 in passing**. The mechanical work is five
phases, each isolated, each testable.

The single most important framing fact: **the user develops on
WSL2 but plays on Windows native, the .exe is at 100+ FPS in the
same scenario, and the WSL build is dropping into the 30s.** Every
piece of work here exists to close the WSL-side gap **without
regressing the Windows-side production path.**

Concrete starting move:

1. Create `src/profile.h` and `src/profile.c` per §4a.
2. Add `--bench` and `--bench-csv` to the arg parser (§4c).
3. Wire `profile_zone_begin/end` calls per §4b — start with
   `PROF_FRAME`, `PROF_SIM_STEPS`, `PROF_DRAW_WORLD`,
   `PROF_DRAW_HUD`, `PROF_PRESENT`. Those five tell you the
   coarse story; add the rest only if you need finer.
4. Run `make perf-bench` once **in WSL**. Look at the numbers.
   **Don't fix anything yet.** The instinct to start fixing in
   the same session is the trap.
5. Run the equivalent **on Windows**: `make windows`, deploy the
   built tree to a Windows path the user has (or ask them), run
   the same scenario, copy back the CSV. (See §5a.1 for the
   shuffle.)
6. Add `documents/m6/perf-baseline.md` with at minimum one row
   of WSL numbers + one row of Windows numbers + a **delta**.
   That's the artifact that proves Phase 1 + 2 are done.
7. Then, and only then, decide which Phase 4 hypothesis to pursue
   — gated on the *shape* of the WSL/Windows delta per §5c.1.

Three non-obvious things to remember while you work:

1. **`GetTime()` is monotonic and cheap.** Don't add a "clock
   skew" abstraction. raylib uses `clock_gettime(CLOCK_MONOTONIC)`
   on Linux and `QueryPerformanceCounter` on Windows; both are
   the right thing.

2. **Shot tests stay byte-identical.** Shot mode forces
   `internal_h = 0` (no cap) and never calls into the new bench
   path. If a shot test diffs after instrumentation, the bug is
   in your instrumentation, not in the test. Don't "fix" the
   test.

3. **Don't trust your eyes after 4 hours of profiling.** The
   `PROF_DRAW_*` and `PROF_SIM_STEPS` numbers are *the* truth.
   The flamegraph is the *secondary* truth. Anecdotal "feels
   smoother now" is *not* evidence and does not count as a
   measurement.

4. **The .exe is the contract. The WSL build is the iteration
   loop.** Never ship a Phase 4 fix without a Windows-side
   bench confirmation. The user plays on the .exe; if it
   regresses, this plan failed regardless of what the WSL
   numbers say.

---

## 12 — References

Tools and prior art actually used in this design:

- **Brendan Gregg, *Linux perf Examples*** —
  `https://www.brendangregg.com/perf.html`. The standard reference
  for `perf record -F 99 -g`. Used directly by §6a.
- **Brendan Gregg, *CPU Flame Graphs*** —
  `https://www.brendangregg.com/FlameGraphs/cpuflamegraphs.html`,
  + the FlameGraph repo at `https://github.com/brendangregg/FlameGraph`.
  We don't vendor FlameGraph (GPL); we document a host
  requirement and run it from PATH.
- **raylib `core_custom_frame_control` example** —
  `https://github.com/raysan5/raylib/blob/master/examples/core/core_custom_frame_control.c`.
  The canonical `GetTime()` instrumentation pattern. §4a follows
  it.
- **raylib batching limitations** —
  `https://github.com/raysan5/raylib/issues/4849`. Documents the
  state changes that flush the batch mid-frame; useful background
  for H1.
- **WSLg performance issues** — `https://github.com/microsoft/wslg/issues/368`,
  `https://github.com/microsoft/wslg/issues/38`,
  `https://forums.developer.nvidia.com/t/wsl2-opengl-d3d12-is-being-very-slow/350278`.
  Background for §6b/§6c. Confirms the WSLg frame-pacing ceiling
  is real and out of our control.
- **`documents/m6/03-perf-4k-enhancements.md`** — the prior
  round's plan + measured numbers. Read it before Phase 4.
- **`documents/10-performance-budget.md`** — the contract.
  Everything in this plan is in service of bringing the build back
  inside it.

---

**End of plan.** Implementation Phase 1 starts at `src/profile.h`
and `src/profile.c` (new files); the rest follows from §4b.
