# M5 — Maps & content

The roadmap calls M5 "**Four weeks. 8 ship-quality maps, art passes, audio mix, polish.**" That undersells the actual surface area: M5 is the milestone where every system that has been *plumbed* finally gets *content*, and where several "carry-forwards" from M1–M4 reach their natural pairing point.

This folder is the breakdown of M5 into tractable sub-documents. Read [00-overview.md](00-overview.md) first — it is the strategic map of the milestone and the ordering rationale. Each subsequent document is one self-contained chunk of work.

| # | Doc | What it covers |
|---|---|---|
| 00 | [Overview](00-overview.md) | Orientation, dependency graph, "done" criteria, risks, residual carry-forwards |
| 01 | [.lvl binary format](01-lvl-format.md) | Header + section table, every section's record layout, CRC32, version handling |
| 02 | [Level editor](02-level-editor.md) | `tools/editor/` — UX, undo/redo, polygon drawing, palette, file I/O, hot reload |
| 03 | [Collision polygons, slopes, ceilings](03-collision-polygons.md) | Free polygons, slope-tangent run velocity, slope-aware friction, angled ceilings — the Soldat slope feel |
| 04 | [Pickup system](04-pickups.md) | Spawners, items, respawn timers, audio cues, network protocol, Engineer repair pack |
| 05 | [Grappling hook](05-grapple.md) | Projectile head, anchor + retract, contracting distance constraint, release |
| 06 | [CTF mode](06-ctf.md) | Flags, base zones, capture rule, drop/return, carrier penalties, HUD compass |
| 07 | [The 8 maps](07-maps.md) | Foundry / Slipstream / Concourse / Reactor / Catwalk / Aurora / Crossfire / Citadel + slope vocabulary + caves/alcoves + bake-test |
| 08 | [Rendering & art](08-rendering.md) | Mech sprite atlases, 3-layer parallax, HUD final art, TTF font, decal chunking |
| 09 | [Audio module](09-audio.md) | `src/audio.{c,h}`, sample loading, alias pool, mix bus, ducking, music streaming |
| 10 | [Map sharing across the network](10-map-sharing.md) | Wire protocol for streaming a `.lvl` from server to client, CRC validation, cache, mid-round join |
| 11 | [Art direction & asset sourcing](11-art-direction.md) | The "Industrial Service Manual" identity, five hard constraints, **ComfyUI workflows + AI-tells defeat-list**, asset source registry, halftone post-process, AI licensing |
| 12 | [Rigging, dismemberment & damage feedback](12-rigging-and-damage.md) | Per-chassis bone structures, per-weapon visible art, two-handed foregrip pose, overlap zones, exposed ends, stump caps, hit-flash tint, damage decals, smoke from broken limbs, ragdoll-mode tolerances |
| 13 | [Controls and residual fixes](13-controls-and-residuals.md) | `BTN_FIRE_SECONDARY` (RMB), updated keybind table, trade-off sweep — what M5 resolves, picks up, and explicitly defers |

Each sub-document follows the design-canon convention: explicit numbers, concrete trade-offs, code references, and a "Done when" criterion. New trade-offs go to the project-wide [TRADE_OFFS.md](../../TRADE_OFFS.md), not into these sub-docs.

## How to use

- **Read 00-overview first.** It tells you the right order to land the work and which pieces unlock which.
- **Stay inside one sub-doc at a time.** If you find yourself wanting to expand the scope of one (e.g. CTF needs a brand-new spawn system), check whether that work belongs in another sub-doc and stop.
- **When a sub-doc graduates to shipped:** update [CURRENT_STATE.md](../../CURRENT_STATE.md), add a line to [11-roadmap.md](../11-roadmap.md)'s M5 section, and close out any TRADE_OFFS entry the work resolves.
- **When a sub-doc collects a "for now" hack:** open a TRADE_OFFS entry *before* commit, and link it from the relevant section here.

## Where this lands in the documents tree

```
documents/
├── 00..11                 # design canon (read first)
├── reference/             # external sources + Soldat constants
└── m5/                    # this folder — milestone breakdown
    ├── README.md          # ← you are here
    ├── 00-overview.md
    ├── 01..13 …
    └── prompts/           # P00–P18: implementation briefs, run sequentially
```

The canonical design docs (00–11) describe what the game **is**. The M5 sub-docs describe what we **build during M5** so the canon catches up to the code. When M5 ships, the canon docs absorb the relevant facts and these sub-docs become historical.
