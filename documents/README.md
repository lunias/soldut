# Soldut — Design & Engineering Documents

A 2D side-scrolling multiplayer mech shooter in C, in the lineage of Soldat. Polygon mechs with bone-driven ragdolls, run-and-gun gameplay, lots of blood, 32 players over UDP, raylib for the foundation, stb for the sharp edges.

This folder is the project's **design canon and engineering plan**. It is meant to be read end to end before serious code is written, then revisited as the source of truth as we build. The documents are written in the spirit of the engineers who shaped the way we think about code:

- **Sean Barrett** — the stb single-header lineage. Boring code. Public domain. APIs that show their seams. One file, no build system if you can help it.
- **Casey Muratori** — Handmade Hero, compression-oriented programming. Make it usable before you make it reusable. Understand the machine. Don't pay for what you don't need.
- **Jonathan Blow** — game-feel above frameworks. Iteration speed is the killer feature. Allocate up front. The compiler is your tool, not your enemy.

We will channel them, then think for ourselves.

## How to read

Read in order. Each document is short enough to finish in one sitting. Cross-references use `[link](filename.md)` form.

| # | Doc | What it covers |
|---|---|---|
| 00 | [Vision](00-vision.md) | The pitch, design pillars, the *soul* of the game |
| 01 | [Philosophy](01-philosophy.md) | How Barrett/Muratori/Blow shape our codebase |
| 02 | [Game Design](02-game-design.md) | Modes, mechs, equipment, loadout, match flow, progression |
| 03 | [Physics & Mechs](03-physics-and-mechs.md) | Verlet skeletons, constraints, ragdolls, dismemberment |
| 04 | [Combat](04-combat.md) | Weapons, damage model, recoil/bink, blood, gore |
| 05 | [Networking](05-networking.md) | Authoritative server, prediction, snapshots, lobby, NAT |
| 06 | [Rendering & Audio](06-rendering-audio.md) | raylib usage, shaders, particles, decals, audio mix |
| 07 | [Level Design](07-level-design.md) | Map philosophy, exploration vs combat, level format |
| 08 | [Build & Distribution](08-build-and-distribution.md) | Toolchain, cross-compile, code signing, packaging |
| 09 | [Codebase Architecture](09-codebase-architecture.md) | Folders, modules, allocation, single-header style |
| 10 | [Performance Budget](10-performance-budget.md) | Frame, memory, network — concrete numbers |
| 11 | [Roadmap](11-roadmap.md) | Milestones M0..M7, what "done" means at each |

Reference material:

- [reference/soldat-constants.md](reference/soldat-constants.md) — raw constants pulled from the Soldat source
- [reference/sources.md](reference/sources.md) — every external URL we cite

Milestone breakdowns (sub-docs that scope a milestone into its tractable pieces):

- [m5/](m5/README.md) — M5 (Maps & content): `.lvl` format, level editor, 8 maps, pickups, CTF, grapple, sprite atlases, audio module, parallax. Read [m5/00-overview.md](m5/00-overview.md) first.

## Tone of these documents

These are **engineering documents**, not marketing copy. We are explicit about trade-offs. Where two choices are reasonable we say so and pick one with reasons. Where a number is a guess we mark it `[est.]`. Where we are wrong, we will be wrong on the page so the next reader can correct it.

We commit to numbers. "Fast" is not a target; **3 ms physics, 60 Hz simulation, 30 Hz snapshots, 1.6 Mbps host upstream at 32 players** is a target. Numbers can be debated; vibes cannot.

## Status

These documents describe a game **not yet built**. They are the plan, not the report. As code lands, we update the docs — the docs always reflect the system as it actually exists, not the system we wish we had. If the code disagrees with the doc, the doc is wrong until shown otherwise; fix the doc.
