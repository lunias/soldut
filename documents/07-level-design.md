# 07 — Level Design

This document specifies the **maps** — what they are made of, how they are authored, what shapes good ones, and what counts as a bad one. Levels are the third leg of the design tripod (alongside mechs and weapons). A great mech and great weapons in a bad map = a bad game.

## Level format

Maps are stored as a single **`.lvl`** binary file plus a paired **`.png`** for the background art. The `.lvl` file is a flat little-endian binary structure we own — no JSON, no XML, no third-party scene format. It loads in milliseconds, validates with a CRC, and is ~50–500 KB per map.

```
[ HEADER 64 bytes ]
[ TILE GRID  W * H * 2 bytes ]      // tile id + flags
[ POLYGON LIST  N * 32 bytes ]      // arbitrary polygons
[ SPAWN POINTS  M * 12 bytes ]
[ PICKUP SPAWNERS  K * 16 bytes ]
[ DECORATION SPRITES  D * 24 bytes ]
[ AMBIENT ZONES  A * 24 bytes ]
[ STRING TABLE  variable ]
[ FOOTER 16 bytes (CRC + version) ]
```

A 100×60 tile map (3200 px × 1920 px) with ~200 polygons, 32 spawns, 30 pickups, and 100 decorations weighs ~25 KB before background art.

## Geometry: tile grid + polygons

The map is a **tile grid** with **per-tile polygon sets** for fine collision, and a **separate list of free-floating polygons** for arbitrary shapes (ramps, slopes, floating platforms).

### Tile grid

```c
typedef struct {
    uint16_t id;       // tile graphic + collision class
    uint16_t flags;    // SOLID, ICE, DEADLY, ONE_WAY_TOP, etc.
} Tile;

typedef struct {
    uint16_t  width, height;
    uint16_t  tile_size;     // 32 px world space
    Tile     *tiles;          // width * height
} TileGrid;
```

Tiles handle the bulk of the map (floors, walls, ceilings). Each tile id maps to:
- A 32×32 sprite drawn at the tile's screen position.
- A **collision class**: empty, full block, slope-up, slope-down, half-block, etc.

Most tiles are flat squares or 45° slopes. The collision shape is generated procedurally from the class — we don't store geometry per tile.

### Free polygons

```c
typedef struct {
    Vec2     v[3];     // triangle (we triangulate at edit time)
    uint16_t kind;     // SOLID, ICE, DEADLY, ONE_WAY, BACKGROUND
    uint16_t group_id; // for destructible groups
    float    bounce;   // restitution 0..1
} TilePoly;
```

Free polygons cover the cases tiles can't: smooth curves, awkward angles, decorative geometry that blocks bullets, jump pads. The level editor triangulates user-drawn polygons before saving (we use a small earcut implementation in C).

### Why this hybrid

- **Tiles** are 99% of the map and give O(1) collision lookup, easy AI navigation, easy art scaling.
- **Polygons** handle the 1% that tiles can't, without forcing the entire collision system to run general polygon-vs-particle queries.

Soldat used pure polygon soup (`PolyMap.pas`); the trade-off is more flexible geometry but pricier collision and harder editor UX. We pay the small complexity cost of a hybrid for big editor and runtime wins.

## Ambient zones

Some areas of the map have **environmental effects**:

```c
typedef struct {
    Rectangle area;
    uint16_t  kind;       // WIND, ZERO_G, RAIN, FOG, ACID
    float     strength;
    Vec2      direction;
} AmbientZone;
```

Wind tunnels push mechs sideways. Zero-g sections turn off gravity inside the box. Acid does 5 HP/sec to anything inside. These are **rare** — used as level highlights, not as a constant gimmick.

## Spawn points

```c
typedef struct {
    Vec2     pos;
    uint8_t  team;        // 0=any, 1=red, 2=blue
    uint8_t  flags;       // PRIMARY, FALLBACK
} SpawnPoint;
```

Each spawn knows its team affinity. The respawn algorithm picks one that satisfies:

- Matches the spawning player's team.
- At least 800 px from the nearest enemy.
- Not currently occupied (telefrag prevention).

If no spawn satisfies all three, it relaxes the distance constraint. Maps must place enough spawns for these constraints to be satisfiable in worst-case scenarios — typically 8–16 spawns per team in TDM/CTF, 16–24 spawns total in FFA.

## Pickup spawners

```c
typedef struct {
    Vec2     pos;
    uint8_t  category;     // HEALTH, AMMO, ARMOR, WEAPON, POWERUP, JET_FUEL
    uint8_t  variant;      // small/medium/large for HEALTH; weapon id for WEAPON
    uint16_t respawn_ms;
} PickupSpawner;
```

Each spawner persistently exists; the actual pickup item is a transient spawned/despawned by the server.

## Map size targets

We commit to **small maps** in the Soldat tradition. Big sprawling maps lose the "frenetic" pillar and dilute combat density.

| Mode | Tile dimensions | World pixels | Approx pickup count |
|---|---|---|---|
| FFA (1v1) | 60 × 40 | 1920 × 1280 | 6–8 |
| FFA (8 players) | 100 × 60 | 3200 × 1920 | 12–18 |
| FFA (16+) | 140 × 80 | 4480 × 2560 | 18–30 |
| TDM (16v16) | 160 × 90 | 5120 × 2880 | 20–32 |
| CTF | 200 × 100 | 6400 × 3200 | 24–40 |

Bigger than ~6400 × 3200 = combat dilutes, exploration wins over fights. Smaller than ~1920 × 1280 = no room to flank, kills become first-shot-fired.

## Design pillars for maps

These are the contract for level designers (us, until/unless we have community contributions). A map fails to ship if it violates these.

### 1. Combat density: most spots get visited

Every walkable area should be traversed at least once per round per player on average. Dead zones (corners no one ever enters) are space wasted on the level budget. Use the "traffic heatmap" debug overlay to verify.

### 2. Three flow channels minimum

Every map has at least three viable paths from spawn to spawn (or flag to flag). If the only way to engage is one corridor, the map plays as one corridor.

### 3. Vertical layering

Mechs jet. Maps should reward vertical play. Each map has at least:
- A **high path** (rooftops, catwalks) — exposed but fast.
- A **mid path** (the main floor) — default traversal.
- A **low path** (basement, tunnels) — covered, slow, often holds the best pickups.

Pickups should reward mixing layers — running past a high pickup on the way to a mid fight is the rhythm we want.

### 4. Sightlines, not snipe-fests

Long sightlines are good (they create skill expression at range), but every sightline must have **cover within ~400 px**. The Rail Cannon should be *threatening*, not *omnipresent*. A map with one ten-mile sightline is a sniper-only map; we don't ship those.

### 5. Pickups bait, fights resolve

Place pickups in places that funnel multiple players. The **best pickup** (large health pack, rocket launcher) goes in the spot that creates the most fights — typically the contested midpoint, slightly off the main path so reaching it costs a beat.

### 6. No invisible walls

If a player can see space, they should be able to *go* there. This is a craft commitment. We use solid geometry (collapsed building, energy field) when we want to block — never invisible barriers.

### 7. Read the silhouette

A player's eye should resolve "is this floor / ceiling / wall / passable" from a glance. Color, shape, line weight all matter. We don't ship maps where players have to memorize what's solid.

## Authored content

At v1 we ship **8 maps**:

| # | Name | Size | Mode |
|---|---|---|---|
| 1 | Foundry | small (60×40) | FFA, 1v1 |
| 2 | Slipstream | small | FFA |
| 3 | Concourse | medium (100×60) | FFA, TDM |
| 4 | Reactor | medium | FFA, TDM |
| 5 | Catwalk | medium | TDM |
| 6 | Aurora | large (160×90) | TDM, FFA |
| 7 | Crossfire | large | TDM, CTF |
| 8 | Citadel | xl (200×100) | CTF |

All maps share a base art kit (the futuristic-mech aesthetic). Variation is in **architecture** (industrial, wreckage, urban, station-corridor, atrium), not in art-pack swap. One coherent visual identity.

## Level editor

We ship a **separate executable**, `soldut_editor`, alongside the game. It is built from the same engine code (raylib for rendering) plus an editor-specific module.

Features (v1):
- Tile painting (left-click paint, right-click erase, scroll for tile picker).
- Polygon drawing (click-click-click-close).
- Spawn / pickup / decoration placement (drag from palette).
- Ambient zone drag rectangles.
- Test play (F5 — opens the map in a single-player test session).
- Save / load `.lvl` files.
- Background art import (drop a PNG, sets parallax).

Features (v2 / community):
- Procedural decoration brushes.
- Region copy/paste.
- Multi-user collaborative editing (out of scope; if we do this, we use a real CRDT, not a custom hack).

The editor is a single C executable, ~5k LOC. It does not fork raylib; it links the same `libraylib.a`.

## Background art

Each map has a paired PNG file for parallax background. The level format references it by string-table index:

```
maps/
  foundry.lvl
  foundry_bg.png
```

The background PNG is up to 4096×2048 RGBA8 (≤32 MB), parallaxed per [06-rendering-audio.md](06-rendering-audio.md).

## Bullet-pass-through layer

Some decorative geometry (foreground silhouettes, dust clouds) doesn't block bullets but visually obscures sightlines. We mark these polygons with `kind = BACKGROUND`. They render but bullets pass through. Used sparingly.

## Destructible geometry — NOT at v1

We commit to **static maps for v1**. Destructible cover, breakable walls, deformable terrain are tempting and very expensive:
- Network state explodes (every chunk must sync).
- Map balance becomes ephemeral (a hot-spot that exists turn 1 doesn't turn 5).
- The level editor tooling triples in complexity.

Post-v1 we may add **predetermined break events** — specific walls that come down at scripted moments — but full destruction is not on the roadmap.

## Pickup placement heuristics

When designing a new map, follow these rules of thumb:

1. **Health packs** along the routes between spawn and combat. Reward survivors of a fight.
2. **Power weapons** (Rail Cannon, Mass Driver) at the *most contested* points. They should be hard-won.
3. **Armor** in transitional spots — between flow channels — so picking it up is a brief detour.
4. **Power-ups** (berserk, invisibility) at the ends of risky paths. Time-respawn them slowly.
5. **Jet fuel** at the bottom of vertical drops, so falling players can climb back up.

Verify by playtesting. If a pickup is grabbed every round by the same player or never grabbed at all, move it.

## Audio environments

Each map sets ambient audio in its header:

```c
typedef struct {
    StringId  music_path;       // optional
    StringId  ambient_loop;     // wind, machinery, etc.
    float     reverb_amount;    // 0..1
} AudioEnvironment;
```

This becomes the **mix bed** behind the action. Different maps feel different through audio alone — Reactor has machinery thrum, Aurora has open-air wind, Citadel has hall reverb.

## Testing

Each map has an automated playtest profile:
- Spawn 8 bots per team, run for 10 minutes, record:
  - Heatmap of player positions.
  - Heatmap of deaths.
  - Heatmap of pickup grabs.
  - Pickup respawn-to-grab times.
- A map fails the bake-test if:
  - Any quadrant has zero kills.
  - One team's spawn area gets >2× the deaths of the other's.
  - Any pickup goes ungrabbed for >50% of the playtest.

(Bot AI for testing can be cruder than competitive bots — a wandering-and-shooting bot is enough.)

## What we are NOT doing

- **Procedurally generated maps.** Hand-authored at v1.
- **Day-night cycles.** Maps are static.
- **Weather.** No rain physics, no wind affecting projectiles. Ambient zones are explicit, designer-placed.
- **Vehicles.** No mech-cars or turrets at v1. (Mounted weapons in fixed positions are a reasonable later addition.)
- **Loading screens.** Maps load fast enough that we go straight to the lobby briefing.
- **A "hub" map.** No persistent world, no overworld. The lobby is the hub.

## References

- Soldat maps (`ctf_Ash`, `Voland`, `Kampf`, `Run`, `Equinox`) for the small-and-dense tradition we're inheriting.
- Quake III maps (`Q3DM6 — The Camping Grounds`, `Q3DM17 — The Longest Yard`) for arena-FPS pillar exemplars.
- *The Level Design Book* — leveldesignbook.com — for cover/sightline taxonomy.
- Halo: Combat Evolved multiplayer maps (Blood Gulch, Hang 'Em High) for size-vs-density.
- Soldat's `MapFile.pas` for the original `.PMS` map format we are deliberately replacing with our own simpler binary.
