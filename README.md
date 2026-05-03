# Soldut

A 2D side-scrolling multiplayer mech shooter in C, in the lineage of Soldat.

The project is in **M3 — Combat depth**: 5 chassis (Trooper / Scout /
Heavy / Sniper / Engineer) with passives, 8 primaries + 6 secondaries,
projectile pool with explosions and line-of-sight, per-limb HP and
dismemberment, recoil + bink + self-bink, kill feed UI, friendly-fire
toggle. Built on top of M1's Verlet skeleton physics + ragdoll +
blood/decals and M2's authoritative server / client prediction /
snapshot interpolation / lag compensation. The lobby UI lands at M4.
See [documents/11-roadmap.md](documents/11-roadmap.md).

For the *current behavior* of the build (vs the design intent in
[documents/](documents/)) see [CURRENT_STATE.md](CURRENT_STATE.md) and
[TRADE_OFFS.md](TRADE_OFFS.md).

## Quick start

Three commands on a fresh Linux checkout:

```bash
sudo apt install -y build-essential libgl1-mesa-dev \
    libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libasound2-dev
make
./soldut
```

A 1280×720 window opens. You spawn on a small platform; a yellow target
dummy stands across the map.

Controls:

| Key            | Action |
|----------------|--------|
| `A` / `D`      | Run left / right |
| `W`            | Jet (consumes fuel) |
| `Space`        | Jump (when grounded) |
| `Shift`        | Dash (Scout chassis) / Burst-jet (Burst Jet module) |
| `R`            | Reload |
| `Q`            | Swap primary / secondary weapon |
| `F`            | Use ability (Engineer self-heal) |
| `Mouse`        | Aim |
| `Left Mouse`   | Fire |
| `Right Mouse`  | Melee swing (when knife is active) |

Hit the dummy enough and a) it ragdolls, b) limbs come off as their
per-limb HP drops to zero (head, both arms, both legs).

### Loadout & networking flags

```bash
# Single-player with a Heavy chassis carrying a Mass Driver:
./soldut --chassis Heavy --primary "Mass Driver" --armor Heavy --jetpack Standard

# Host a LAN server on default port 23073:
./soldut --host

# Connect to a host:
./soldut --connect 192.168.1.42:23073 --name "scout-1" --chassis Scout --primary "Plasma SMG"

# Tournament-style friendly-fire on:
./soldut --host --ff
```

Chassis names: `Trooper`, `Scout`, `Heavy`, `Sniper`, `Engineer`.
Primary weapons: `Pulse Rifle`, `Plasma SMG`, `Riot Cannon`,
`Rail Cannon`, `Auto-Cannon`, `Mass Driver`, `Plasma Cannon`, `Microgun`.
Secondaries: `Sidearm`, `Burst SMG`, `Frag Grenades`, `Micro-Rockets`,
`Combat Knife`, `Grappling Hook`. Armor: `None`, `Light`, `Heavy`,
`Reactive`. Jetpack: `Baseline`, `Standard`, `Burst`, `Glide`, `JumpJet`.
(Case-insensitive prefix match for weapons, exact for the rest.)

## Cross-compiling

`zig cc` is the cross compiler. With Zig on `PATH`:

```bash
./cross-windows.sh    # → build/windows/Soldut.exe
./cross-macos.sh      # → build/macos/Soldut.app   (requires macOS SDK; see script)
```

## Visual debugging — shot mode

For physics / motion / weapon questions, the binary takes a
`--shot path/to/script.txt` flag that drives a scripted scene through
the real renderer + sim and writes PNGs (and a paired `.log`) to
`build/shots/`. See `tests/shots/m3_*.shot` for examples covering the
Mass Driver, Plasma SMG, Frag Grenade, Riot Cannon spread, Rail Cannon
charge, and the armor bar.

```bash
./soldut --shot tests/shots/m3_mass_driver.shot
make shot                                       # default script
make shot SCRIPT=tests/shots/m3_grenade.shot    # any other .shot
```

The log is paired 1:1 with the PNGs — an LLM (or you) can read both
together. See `src/shotmode.h` for the script grammar (including the
`loadout` directive added at M3).

## Layout

```
soldut/
├── src/                 # game source (~25 .c/.h files)
├── third_party/
│   ├── raylib/          # vendored, built into libraylib.a per platform
│   ├── enet/            # vendored, built into libenet.a
│   ├── stb_ds.h         # the only stb header we vendor directly
│   └── stb_sprintf.h
├── documents/           # design canon — read these before reading code
├── tools/               # support utilities (level cooker, replay extractor)
├── tests/               # unit / regression tests
│   └── shots/           # scripted scenes for the --shot harness
├── Makefile             # native build
├── build.sh             # one-shot orchestration
├── cross-windows.sh     # cross-compile to Windows via zig cc
├── cross-macos.sh       # cross-compile to macOS via zig cc
└── README.md
```

The design documents in [documents/](documents/) are the source of truth
for *intent*; the code is the source of truth for *behavior*. Read them in
order — they cross-reference each other.

## Status

### M0 — Foundation (done)

- [x] Project scaffolding, `Makefile`, `build.sh`
- [x] raylib + ENet + stb headers vendored, statically linked
- [x] Build matrix (`make`, `make windows`, `make macos`)
- [x] CI runs the build matrix on every push
- [x] Window, clear-color, FPS counter
- [x] Logger, arena allocator, pool allocator, math primitives

### M1 — One mech, no network (done)

- [x] Particle pool (SoA) + Verlet integrator
- [x] Constraint pool + 12-iter solver, constraints + map collisions interleaved per iter
- [x] Trooper chassis with the 16-particle skeleton + 21 distance constraints
- [x] Tile-grid map format + hard-coded tutorial level
- [x] Body-vs-map collision (per-particle vs tile rect, neighbour-aware exit)
- [x] Pose-driven animation (kinematic translate; layout-consistent targets): Stand, Run, Jet, Fall, Fire, Death
- [x] Post-physics anchor that holds standing pose without sag (see [TRADE_OFFS.md](TRADE_OFFS.md))
- [x] `Camera2D` smoothed follow + screen shake
- [x] Pulse Rifle hitscan, tracer, recoil impulse
- [x] Static target dummy that takes damage
- [x] Death: drop pose drive, apply killshot impulse, ragdoll
- [x] Limb dismemberment (left arm; constraint deletion → free ragdoll piece)
- [x] Blood particles + decal RT splat layer
- [x] Hit-pause + screen shake
- [x] HUD: health bar, jet fuel gauge, ammo, crosshair, kill feed
- [x] Headless physics tester (`make test-physics && ./build/headless_sim`)

### M2 — Networking foundation (done; two-laptop bake pending)

- [x] ENet wrapper (`src/net.{h,c}`), 4 channels (STATE / EVENT / CHAT / LOBBY)
- [x] Connection handshake (CONNECT_REQUEST / CHALLENGE / CHALLENGE_RESPONSE / ACCEPT)
- [x] LAN broadcast discovery (`DISCOVERY_QUERY` / `DISCOVERY_REPLY`)
- [x] Direct connect by IP+port (`--connect host[:port]`)
- [x] Client input upload (60 Hz) with sequence numbers
- [x] Server simulate loop (60 Hz) consuming inputs
- [x] Server snapshot broadcast (30 Hz) — full snapshots, no delta yet
- [x] Client-side prediction + reconciliation for the local mech
- [x] Lag compensation for hitscan (12-tick bone-history ring per mech, capped at 200 ms)

### M3 — Combat depth (done)

- [x] All 5 chassis with parameterized stats (Trooper / Scout / Heavy / Sniper / Engineer)
- [x] All 8 primaries (Pulse Rifle, Plasma SMG, Riot Cannon, Rail Cannon, Auto-Cannon, Mass Driver, Plasma Cannon, Microgun)
- [x] All 6 secondaries (Sidearm, Burst SMG, Frag Grenades, Micro-Rockets, Combat Knife, Grappling Hook stub)
- [x] Projectile pool (`src/projectile.{h,c}`, capacity 512) with gravity, drag, swept tile + bone collision
- [x] Explosions: damage falloff (`1 - (d/r)²`), line-of-sight halving, impulse to live mechs and ragdolls
- [x] Hit-location damage multipliers (head ×1.6, arms/legs ×0.7, hands/feet ×0.5)
- [x] Per-limb HP for all 5 limbs; dismemberment via constraint deactivation; head detach is also lethal
- [x] All 4 armor variants (None / Light / Heavy / Reactive); Reactive eats one explosion fully
- [x] All 4 jetpack variants (Standard / Burst / Glide Wing / Jump Jet)
- [x] Recoil (hand impulse), bink (incoming-fire flinch on nearby targets), self-bink (rapid-fire jitter)
- [x] Friendly-fire toggle (`--ff` server flag), default off
- [x] Kill feed UI with HEADSHOT / GIB / OVERKILL / RAGDOLL / SUICIDE flags
- [x] Loadout via CLI flags (`--chassis`, `--primary`, `--secondary`, `--armor`, `--jetpack`)
- [x] Snapshot wire format widened (+5 bytes/mech) to carry chassis + armor + jetpack + secondary; protocol bumped to `S0LE`

## License

The game is MIT-licensed (see `LICENSE` once added). Vendored dependencies
keep their own licenses:

- raylib — zlib/libpng
- ENet — MIT-style
- stb_ds.h, stb_sprintf.h — public domain (Sean Barrett)
