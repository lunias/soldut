# M5 — Overview

**Four calendar weeks (optimistic). Realistic: six.**

This is the strategic view of the milestone. The other docs in this folder are the tactical breakdown.

## What "done" means

The roadmap pins it: *all 8 maps pass the bake-test (heatmap of fights, no dead zones, no spawn imbalance)*. Concretely:

- A stranger downloads the binary, double-clicks it, picks "Browse Servers," joins a server, and the **first round they play** has art, sound, pickups they can grab, and a map that funnels them into fights.
- 8 maps render with parallax backgrounds and shipped audio environments.
- Pickups exist on every map, respawn correctly, and broadcast to all clients without desync.
- CTF runs end-to-end on at least Crossfire and Citadel.
- The level editor builds + opens + saves + reloads a `.lvl` file. A non-author can pick it up and ship a 9th map without touching code.
- HUD has its final art pass. The mech is no longer rendered as capsules.
- Audio mix is balanced: gunshots are the loudest thing, music ducks under combat, footsteps and servos modulate with movement.

This is not a "polish + balance" milestone. The work in M5 is *adding systems and content*. Balance is a continuous process that runs alongside.

## Why M5 is large

The roadmap's checklist for M5:

1. Level editor (`tools/editor/`)
2. 8 maps
3. Pickup system
4. Pickup respawn timers + audio
5. CTF mode + flag mechanics
6. Background art per map (parallax)
7. Audio mix pass
8. Music selection per map
9. HUD final art

Each of those is a real piece of work. Two of them (1 + 2) are coupled — the editor exists to make the maps. Two more (3 + 5) are gameplay systems that the maps need before you can author the maps. Three more (6 + 7 + 8) are the audio-visual layer that finally retires the "capsule mech / silent world" prototype look. (9) is HUD final art.

On top of that, **earlier milestones explicitly deferred work to M5**. We honor those debts here:

| Source | What | Where it lands in M5 |
|---|---|---|
| M1 trade-off | The hard-coded tutorial map | Replaced by `.lvl` files. See [01-lvl-format.md](01-lvl-format.md). |
| M1 trade-off | Tile grid only; no per-tile polygons | [03-collision-polygons.md](03-collision-polygons.md). |
| M3 trade-off | Grappling Hook is a stub | [05-grapple.md](05-grapple.md). |
| M3 trade-off | Engineer's BTN_USE heals self | Becomes a deployable repair pack in [04-pickups.md](04-pickups.md). |
| M3 trade-off | Burst SMG fires all rounds on the same tick | Small fix folded into [04-pickups.md](04-pickups.md) §"Residual fixes" — `burst_pending_*` state on Mech. |
| M4 trade-off | Three-card map vote picker UI | [07-maps.md](07-maps.md) §"Vote picker UI" — needs map preview thumbnails. |
| M4 trade-off | Host-controls panel (kick/ban buttons) | [07-maps.md](07-maps.md) §"Host controls" — surface the existing wired commands. |
| M4 trade-off | `bans.txt` persistence | [07-maps.md](07-maps.md) §"Host controls" — load on start, write on update. |
| M4 trade-off | Solo practice dummy | [04-pickups.md](04-pickups.md) §"Practice dummy" — special PickupSpawner kind. |
| M4 trade-off | Default raylib font | [08-rendering.md](08-rendering.md) §"TTF font" — vendor Atkinson Hyperlegible. |

## What M5 explicitly does **not** do

- **Snapshot delta encoding past M2's stub.** Stays in the M2 trade-off queue until 16+ player playtest pressures bandwidth.
- **HMAC-SHA256 handshake.** Stays deferred until we expose servers to the open internet.
- **Server-side entity culling.** Stays deferred. Ships at v0.2 alongside dedicated-server stretch goal.
- **Sim at 120 Hz.** P03 landed the fixed-step accumulator + render-side interpolation alpha + two-snapshot remote-mech interp; running the sim at 120 Hz is now a flag-flip, but stays at 60 because slope-physics tuning happened against it. Decide after authored-map playtest.
- **Left-arm IK / angle constraints.** Pushed to M6 polish pass.
- **Destructible geometry.** Explicitly out of scope per [07-level-design.md](../07-level-design.md). Maps are static at v1.
- **Procedurally generated maps.** Same.
- **Bots beyond what the bake-test needs.** A wandering-and-shooting bot is enough. Real AI is a stretch goal.

## What got added after first-pass review

A first pass on M5 produced docs 00–09. Successive review rounds added:

- **Map sharing across the network** ([10-map-sharing.md](10-map-sharing.md)) — a server should be able to load a custom map and stream it to the connecting client. Without this, only the 8 shipped maps can ever be played. The protocol is two new messages on `NET_CH_LOBBY` plus a content-addressed download cache.
- **Art direction & asset sourcing** ([11-art-direction.md](11-art-direction.md)) — the "Industrial Service Manual" aesthetic recipe. Battletech TRO + Patlabor + constructivist machinery, with a halftone post-process shader that does the visual lift. **Pivoted from "no AI" to "AI-assisted via ComfyUI with anti-AI-feel discipline"** after explicit user direction. The five hard constraints become the anti-AI-feel mechanism that wraps the ComfyUI workflow.
- **Slope physics for the Soldat run-up-slow / slide-down-fast feel** — landed inside [03-collision-polygons.md](03-collision-polygons.md) as the largest section. Every map now ships with explicit slope, hill, valley, and angled-ceiling features per [07-maps.md](07-maps.md) §"Slopes, hills, valleys, angled ceilings — discipline across the maps".
- **Caves and alcoves** for hidden / nook pickup placement — landed inside [07-maps.md](07-maps.md) §"Caves, alcoves, and hidden nooks". Each map specifies its alcove count and placement strategy.
- **Rigging, dismemberment & damage feedback** ([12-rigging-and-damage.md](12-rigging-and-damage.md)) — the assets are anchored to the Verlet bones, not static sprites. Specifies per-chassis bone-length distinctness (so Heavy / Scout / Sniper / Engineer read instantly different at gameplay distances), per-weapon visible art (a held Mass Driver looks like a Mass Driver, not a generic line), the two-handed foregrip pose (resolves part of the M1 "left arm dangles" trade-off), the per-part overlap-zone discipline that hides joint gaps, the exposed-end + stump-cap discipline that makes dismemberment look authored, and three runtime feedback layers (hit-flash tint, persistent damage decals, smoke from damaged limbs) that make hits feel impactful without multiplying asset count.
- **Controls + trade-off sweep** ([13-controls-and-residuals.md](13-controls-and-residuals.md)) — adds `BTN_FIRE_SECONDARY` (RMB) so the secondary slot fires one-shot without `BTN_SWAP`. Sweeps [TRADE_OFFS.md](../../TRADE_OFFS.md): 12 entries get resolved by other M5 sub-docs (delete on land); 4 are picked up here as new fixes (slope-aware post-physics anchor, render-side interpolation alpha + accumulator, snapshot smoothing + reconciliation visual offset, `is_dummy` bit on the wire); 11 stay deferred with reasons.

## Dependency graph

The work splits into **format/tools/content** vs. **gameplay systems** vs. **audio-visual layer**, and they merge at "8 maps that ship." The arrows show "must land before":

```
       ┌──────────────────────┐
       │ 01 .lvl format       │
       └─┬───────┬────────────┘
         │       │
         ▼       ▼
 ┌──────────┐  ┌──────────────────────┐
 │ 02 editor│  │ 03 collision polygons│
 └──────┬───┘  └────────┬─────────────┘
        │               │
        ▼               ▼
        ┌────────────────────────┐
        │ (editor + slopes ready)│
        └────────────┬───────────┘
                     │
                     ▼
                                                                           ┌──────────────┐
                            ┌────────────────┐  ┌──────────────┐           │ 08 rendering │
                            │ 04 pickups     │  │ 05 grapple   │           └──────┬───────┘
                            └────────┬───────┘  └──────┬───────┘                  │
                                     │                 │              ┌───────────┘
                                     ▼                 ▼              │       ┌──────────┐
                                     ┌──────────────────────┐         │       │ 09 audio │
                                     │ 06 CTF (uses spawns +│         │       └─────┬────┘
                                     │  flag-base zones)    │         │             │
                                     └──────────┬───────────┘         │             │
                                                │                     │             │
                                                ▼                     │             │
                                                ┌────────────────────────────────┐
                                                │ 07 the 8 maps + bake-test      │
                                                └────────────────────────────────┘
```

## Recommended order of attack

This is the order that keeps things shippable and minimizes "block on a dependency" days.

**Week 1 — format + skeleton**
- Land [01-lvl-format.md](01-lvl-format.md). New module `src/level_io.{c,h}`. Loader + saver round-trip a hard-coded test fixture.
- (Shipped at P17.) The four maps in batch 1 (Foundry / Slipstream / Reactor / Concourse) ship as `.lvl` via `tools/cook_maps/cook_maps.c` — a one-shot exporter that builds each map programmatically, calls `level_save`, and round-trips each emitted file through `level_load` + the polygon broadphase inside the cooker itself. The originally-planned editor-authoring path for Concourse was deferred; cook_maps ships a programmatic scaffold (tracked as a TRADE_OFFS entry). The `tools/editor/` path remains available for designers to iterate; running `make cook-maps` again would overwrite the editor-saved file unless the concourse builder is dropped from cook_maps. Runtime `map_build` loads `assets/maps/<short>.lvl` and falls back to the code-built path only if the file is missing or fails CRC.
- Begin [03-collision-polygons.md](03-collision-polygons.md). Free polygons in the world struct, broadphase grid, closest-point collision. Slopes drop in as polygons.

**Week 2 — editor + first new content**
- [02-level-editor.md](02-level-editor.md). `tools/editor/soldut_editor`. Tile paint, polygon draw, spawn/pickup placement, F5 test play. Save/load round-trip the format.
- ~~Build map #4 (Concourse) in the editor as the editor's own bake-test.~~ (Shipped at P17 as a programmatic `cook_maps` scaffold instead; editor-authoring of Concourse deferred — tracked as a TRADE_OFFS entry.)
- Land [04-pickups.md](04-pickups.md). Spawner pool, item state machine, respawn timers, network event protocol. No audio yet (that lands in week 3).

**Week 3 — gameplay systems + audio plumbing**
- [05-grapple.md](05-grapple.md). One-day implementation. Lets [07-maps.md](07-maps.md) author swing beats.
- [06-ctf.md](06-ctf.md). Flag entity, base zones in the level format, capture rule, drop/return, HUD compass. Wire protocol delta-encoded.
- Begin [09-audio.md](09-audio.md). New `src/audio.{c,h}` module, sample loading, alias pool, mix bus. Hook into existing fire/hit/explosion paths.

**Week 4 — content + visual layer**
- [07-maps.md](07-maps.md). Author the remaining 5 maps (Catwalk, Aurora, Crossfire, Citadel — Concourse is already in from week 2). Run bake-test on each.
- Begin [08-rendering.md](08-rendering.md). Mech sprite atlases (5 chassis × 1024² each), parallax (3 layers per map), HUD final art. The capsule mech goes away.
- Vote picker UI + host-controls panel + bans.txt + practice dummy + TTF font. Each is small; stagger across the week.

**Week 5–6 (slip budget)**
- Audio mix pass. Music selection per map. Final balance pass on pickup placement.
- Bake-test iteration: any map that fails (dead zone, spawn imbalance, ungrabbed pickup) gets layout adjustments.

## Risks

**Art bottleneck (high probability).** A single engineer can ship adequate hand-drawn line-art mech parts using the workflow in [08-rendering.md](08-rendering.md), but it will take longer than the 4-week estimate. Mitigation: ship "okay" art at week 4 and iterate during week 5–6. The parallax/atlas pipeline is the load-bearing part — the actual pixels can be re-sourced.

**Bake-test reveals layout problems (high probability).** Some of the 8 maps will fail their first bake. Layout fixes are cheap in the editor but cumulative: 8 maps × N iterations is real time. Mitigation: the bake-test bot is intentionally crude (wander + shoot anyone visible) so we can run it in <60 s per map and iterate fast.

**Pickup balance interacts with weapon balance (medium probability).** If a map has the Mass Driver in a too-easy spot, every fight is a Mass Driver fight. We accept some of this — power weapons are *supposed* to be contested — and document the bake-test reveal as a balance-pass note for M6.

**CTF wire format pulls in delta-encoding (low probability).** If the FlagState delta isn't enough and we need full snapshot delta encoding to fit, we promote that work from the M2 trade-off to in-scope. It's about a week of work; mitigation is to defer to M6 and absorb the cost on the M5 timeline.

**The editor never reaches "good enough" (low probability).** If the editor UX bogs down, fall back to **Tiled** (the third-party editor, https://www.mapeditor.org/) and write a one-way `.tmx → .lvl` exporter. Tiled handles tiles + polygons natively. Cost: 2 days vs. the 1 week the editor takes. Documented in [02-level-editor.md](02-level-editor.md) as the Plan B.

## Numeric targets to defend

These are the numbers M5 commits to. They are mostly carry-forwards from [10-performance-budget.md](../10-performance-budget.md); the M5-specific ones are flagged.

| Target | Value | Notes |
|---|---|---|
| `.lvl` file size | ≤500 KB | M5-specific. 50–500 KB band per [07-level-design.md](../07-level-design.md). |
| `.lvl` load time | <50 ms on NVMe | M5-specific. Read+CRC+arena copy only. |
| Editor cold start | <1 s | M5-specific. Same engine init path as the game. |
| Pickup count active per map | ≤40 | M5-specific. [07-level-design.md](../07-level-design.md) §"Map size targets". |
| Pickup state network cost | ≤5 KB/s/client | M5-specific. Event-only delta. |
| Flag state network cost | ≤200 B/s/client | M5-specific. 10 bytes × 2 flags × event-driven. |
| Audio sample memory | ≤30 MB | Per [10-performance-budget.md](../10-performance-budget.md). All SFX in RAM, music streamed. |
| Music streaming buffer | ≤1 MB | Same. OGG decode buffer. |
| Texture memory | ≤80 MB | Same. Now load-bearing — atlases + parallax can blow this. |
| Decal RT layer | ≤16 MB | Same. M5 chunks the layer when level >4096 px. |
| HUD frame budget | ≤0.8 ms | Same. Final art shouldn't regress this. |

## Trade-off entries we expect to add

These are pre-disclosed: when M5 ships, expect at least these entries to land in [TRADE_OFFS.md](../../TRADE_OFFS.md) unless we explicitly do the "right" version.

- **Mech atlas baked at build time, not loaded from a manifest.** Cuts a manifest format we'd have to maintain.
- **Editor undo is whole-tile-grid snapshot, not differential.** Wastes memory for big maps but is bullet-proof. *(Landed at P04.)*
- **Pickup transient state isn't persisted across host restarts.** A host crash mid-round resets all spawners to AVAILABLE; engineer-deployed transients vanish. Acceptable until rounds run long enough that mid-round restarts are a real concern. *(Landed at P05; supersedes the original "spawner state lives in level arena" pre-disclosure — pickups actually live on `World.pickups`.)*
- **Music plays only one track at a time (no crossfade).** Crossfade is double the streaming cost; a hard cut at round transitions is fine.
- **No actual UDP for audio (obviously).** Not a real trade-off, just calling out that audio cues fire client-side from network events; clients hear *their own world*, not a globally synchronized acoustic state.
- **CTF flag carrier-glow shader is pre-baked** rather than a runtime additive pass. The shader in [06-rendering-audio.md](../06-rendering-audio.md) §"Shaders" exists; this is just calling out the shape.

If we end up with more, that's a signal we missed something during planning.

## What's *next* after M5

[11-roadmap.md](../11-roadmap.md) has M6 (stability + ship prep) and M7 (launch + first patch cycle). M5 should leave M6 with:

- A `.lvl` editor that authors are willing to use (so the M6 polish pass can revise maps without engineering involvement).
- A pickup system robust enough to reskin (so M6 weapon-balance can move pickups without code changes).
- An audio bus structure that downmixes cleanly (so M6 platform builds don't surface audio glitches).
- An asset budget under cap (so M6 can add crash logging + a small font for non-Latin without exceeding 256 MB resident).

If those four things are true at M5 end, M6 is mostly stability/polish work and ships on time.
