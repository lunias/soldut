# M5 implementation prompts

18 implementation prompts (P01–P18), executed sequentially, that ship M5 — plus a P00 prologue to read first. Each is a self-contained brief for a fresh Claude Code session — you paste the prompt, Claude reads the listed context, then implements that one chunk of work.

## How to use

1. **Open Claude Code in the repo root.**
2. **Open the next prompt's `.md` file** in this folder (or `cat` it).
3. **Copy the entire file content** into Claude's input.
4. **Let Claude work.** It will read the context docs, write the code, run tests, update `CURRENT_STATE.md` and (where applicable) `TRADE_OFFS.md`.
5. **Verify.** Each prompt has a "Done when" section — confirm against it before moving on.
6. **Commit and move to the next prompt.**

## Order matters

The prompts have dependencies. **Do not skip.** If P05 (pickups) is run before P01 (.lvl format), the asset paths and spawner data won't load.

```
P00 ─ Prologue (read first; not an implementation prompt)

P01 ─ .lvl format ─────────────────────────────┐
                                                ├──► P04 ─ Level editor
P02 ─ Polygon collision + slope physics ───────┘

P03 ─ Render-side accumulator + interp alpha + reconcile smoothing
       (independent; can run any time after P00)

P04 ─ Level editor

P05 ─ Pickup system + Engineer + Burst SMG + practice dummy
P06 ─ Grappling hook
P07 ─ CTF mode
P08 ─ Map sharing across the network
P09 ─ BTN_FIRE_SECONDARY + keybinds + UI controls panel

P10 ─ Mech sprite atlas runtime + per-chassis bone distinctness
P11 ─ Per-weapon visible art + two-handed foregrip
P12 ─ Damage feedback (hit-flash, decals, stump caps, smoke)
P13 ─ Parallax + HUD final art + TTF font + halftone post + decal chunking
P14 ─ Audio module

P15 ─ ComfyUI setup + first chassis (Trooper) generation
P16 ─ Remaining chassis + weapon atlas + HUD icons (asset generation)
P17 ─ Author maps 1-4 (Foundry/Slipstream/Reactor as .lvl + Concourse)
P18 ─ Author maps 5-8 (Catwalk/Aurora/Crossfire/Citadel) + bake-test harness
```

The runtime work (P01–P14) can be done before any asset work. The asset work (P15–P18) needs the runtime in place to verify against. **Don't generate art before the renderer can use it.**

## Estimated time per prompt

| Prompt | Realistic single-session length |
|---|---|
| P01–P03 | 2–4 hours each |
| P04 (editor) | 6–10 hours (split across 2 sessions if needed) |
| P05–P08 | 2–4 hours each |
| P09 | 1–2 hours |
| P10–P12 | 2–4 hours each |
| P13–P14 | 4–6 hours each |
| P15 | 2–4 hours (plus iteration time you do, not Claude) |
| P16 | mostly your time generating in ComfyUI; Claude wires up the atlas |
| P17–P18 | 4–8 hours each |

Total Claude-time: ~50–80 hours. Plus your own ComfyUI iteration time for asset gen.

## Each prompt's structure

```
# P0N — <Title>

## What this prompt does
<1-paragraph goal>

## Required reading (in this order)
- Project conventions: CLAUDE.md
- Design canon: documents/00-vision.md, documents/01-philosophy.md
- Current state: CURRENT_STATE.md
- Active compromises: TRADE_OFFS.md
- M5 strategic view: documents/m5/00-overview.md
- This work's spec: documents/m5/<sub-doc>.md
- Existing code: <specific paths>

## Background
<recap of what's been done, what's pending, why this work matters>

## Concrete tasks
1. <Task with file:line references where useful>
2. ...

## Done when
- <verification criterion>
- ...

## Out of scope (don't do these here)
- <explicit non-tasks, often deferred to a later prompt>

## How to verify
- Build: `make`
- Tests: `make test-physics` / `tests/net/run.sh`
- Smoke: `./soldut --shot tests/shots/<scenario>.shot`

## Close-out
- Update CURRENT_STATE.md (add/move milestone bullet)
- Update TRADE_OFFS.md (delete resolved entries; add pre-disclosed new ones)
- Commit (only when explicitly asked)
```

## What if a prompt is too big for one session?

Split it. Each section (`Concrete tasks 1`, `Concrete tasks 2`, etc.) is roughly independent. You can run a prompt for "Tasks 1–3 only, defer 4–6 to a follow-up session." When you come back, point Claude at the same prompt and say "continue from task 4."

## What if Claude doesn't have ComfyUI access?

Claude Code can run shell commands but it can't drive a GUI like ComfyUI. The asset-generation prompts (P15, P16) tell Claude to **set up the workflow JSONs, the input skeletons, the build scripts, and the atlas-loader code**, then **describe the runs you should do interactively**. You drive ComfyUI yourself; Claude consumes the resulting PNGs and integrates them.

## Project files Claude needs to know about

In addition to the docs and CLAUDE.md, Claude should be aware of these key code files (it'll read them as needed per prompt):

- `src/main.c` — top-level loop
- `src/world.h` — all the data definitions
- `src/mech.{c,h}` — chassis + animation + damage
- `src/physics.c` — Verlet + constraints + collision
- `src/render.c` — current capsule renderer
- `src/weapons.{c,h}` — weapon table + fire dispatch
- `src/projectile.{c,h}` — projectile pool
- `src/level.{c,h}` — current tile-grid loader
- `src/maps.{c,h}` — current code-built maps
- `src/lobby.{c,h}` + `src/lobby_ui.{c,h}` — lobby state + UI
- `src/match.{c,h}` — match phases + scoring
- `src/net.{c,h}` — ENet wrapper + protocol
- `src/snapshot.{c,h}` — snapshot encode/decode
- `src/decal.{c,h}` — splat layer
- `src/particle.{c,h}` — FX pool
- `src/input.h` — input bitmask
- `src/platform.{c,h}` — raylib wrapper
- `src/ui.{c,h}` — small immediate-mode UI helpers
- `src/shotmode.{c,h}` — scriptable test runner
- `src/config.{c,h}` — soldut.cfg parser
- `Makefile` — build targets
- `tests/headless_sim.c` — physics regression
- `tests/net/run.sh` — networked smoke test
- `tests/shots/*.shot` — scriptable scene runs

## Don't worry about

- Claude not understanding raylib — it's vendored at `third_party/raylib` and Claude reads the headers as needed.
- Claude not understanding ENet — same.
- Tracking progress across prompts — the `CURRENT_STATE.md` updates after each prompt are the running log.

## When something breaks mid-prompt

If a prompt hits a blocker (missing dependency, ambiguity in the spec, code that doesn't compile), **stop the prompt and address the blocker in a side conversation** before continuing. Don't push past — Claude will start guessing and the result will need rework. The doc you're reading from is the source of truth; if there's a contradiction, the doc wins until you fix the doc.
