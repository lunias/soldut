# 11 — Roadmap

This document defines what "done" means at each milestone. We work in milestones that each end with **a thing you can play**, not a checklist of features. If a milestone ends and the game isn't more fun than it was, the milestone failed.

Time estimates are calendar weeks for one or two engineers working seriously. They are estimates. They will move; we'll update this doc.

## M0 — Foundation

**Two weeks.**

**Goal**: We have a window, an empty world, and a build pipeline.

- [ ] Project scaffolding (`src/`, `third_party/`, `Makefile`, `build.sh`)
- [ ] raylib vendored and statically linked (Linux dev, Windows cross via zig cc, macOS native or cross)
- [ ] ENet vendored and statically linked
- [ ] `stb_ds.h`, `stb_sprintf.h` vendored
- [ ] Build matrix: `make`, `make windows`, `make macos` all produce binaries
- [ ] CI runs the build matrix on every push
- [ ] Window opens, clear-color renders, FPS counter
- [ ] Logger, arena allocator, pool allocator
- [ ] Math primitives

**Done when**: a fresh checkout, three commands, opens a window on Windows and macOS.

## M1 — One mech, no network

**Three weeks.**

**Goal**: One player can run, jet, jump, shoot at a static target, and see ragdoll on death.

- [ ] Particle pool + Verlet integrator
- [ ] Constraint pool + relaxation solver
- [ ] One chassis (Trooper) with the 16-particle skeleton
- [ ] Tile grid map format + loader
- [ ] Hard-coded test map (`maps/tutorial.lvl`)
- [ ] Body-vs-map collision
- [ ] Animation system: pose-driven, the 24-animation set, manual triggers (run, jump, fire)
- [ ] Camera2D with smoothed follow
- [ ] One weapon (Pulse Rifle) — hitscan, tracer, recoil impulse
- [ ] One target dummy that takes damage
- [ ] Death: drop pose drive, apply killshot impulse, ragdoll
- [ ] Limb dismemberment (arm only as a proof of concept)
- [ ] Blood particles + decal RT layer
- [ ] Hit-pause + screen shake
- [ ] HUD: health, ammo, crosshair

**Done when**: a player can play a 30-second loop — run, jet, shoot the dummy until it falls apart — and grin.

## M2 — Networking foundation

**Four weeks.**

**Goal**: Two players on a LAN can play the M1 loop together.

- [ ] `net.h` interface
- [ ] ENet host/client wrapping
- [ ] Packet header (protocol id, sequence, ack bitfield)
- [ ] Channels: STATE, EVENT, CHAT, LOBBY
- [ ] Client input upload (60 Hz) with sequence numbers
- [ ] Server simulate loop (60 Hz) consuming inputs
- [ ] Server snapshot broadcast (30 Hz) — uncompressed first
- [ ] Client snapshot interpolation for remote mechs
- [ ] Client-side prediction + reconciliation for local mech
- [ ] Lag compensation for hitscan (server rewinds bone history)
- [ ] Connection handshake (challenge nonce, version check)
- [ ] LAN broadcast for server discovery
- [ ] Direct-connect by IP+port

**Done when**: two laptops on the same WiFi can both run the binary, one types `:23073` to host, the other types the host's IP, and they shoot each other for ten minutes without a desync.

## M3 — Combat depth

**Three weeks.**

**Goal**: All v1 weapons. All v1 mech chassis. All damage and dismemberment systems.

- [ ] All 5 chassis with parameterized stats
- [ ] All 8 primaries
- [ ] All 6 secondaries
- [ ] All 4 armor variants
- [ ] All 4 jetpack variants
- [ ] Recoil + bink + self-bink fully wired
- [ ] Limb HP per limb, dismemberment of each
- [ ] Explosions: damage, impulse, line-of-sight
- [ ] Hit-location damage multipliers
- [ ] Friendly-fire toggle (server config)
- [ ] Kill feed UI

**Done when**: the weapon roster feels distinct in playtest — every gun has a use, none are dominant.

## M4 — Lobby & matches

**Three weeks.**

**Goal**: Players can join a server, pick a loadout, ready up, and play a 5-minute match with score and round summary.

- [ ] Server browser (LAN-only at first)
- [ ] Lobby UI: player list, chat, mech selector, loadout slots, ready button
- [ ] Map vote UI
- [ ] Round timer + score display
- [ ] FFA mode (kill counts, target score / time limit)
- [ ] TDM mode (team selector, two teams, friendly-fire toggle)
- [ ] Round summary: per-player stats, MVP
- [ ] Map cycle / next-round transition
- [ ] Server config file (port, max players, tick rate, mode rotation)
- [ ] Kick / ban (host-only)

**Done when**: 8 players can join a host's server, pick loadouts, play a TDM round, see scores, vote a new map, play another. Without crashes.

## M5 — Maps & content

**Four weeks.**

**Goal**: 8 ship-quality maps, art passes, audio mix, polish.

- [ ] Level editor (`tools/editor/`) — tile paint, polygon draw, spawn/pickup placement
- [ ] 8 maps: Foundry, Slipstream, Concourse, Reactor, Catwalk, Aurora, Crossfire, Citadel
- [ ] Pickup system: health, ammo, armor, weapon, power-up, jet fuel
- [ ] Pickup respawn timers, pickup audio
- [ ] CTF mode + flag mechanics
- [ ] Background art per map (parallax)
- [ ] Audio mix pass: gunshots, explosions, footsteps, mech servo loops, environment ambience
- [ ] Music selection per map
- [ ] HUD final art

**Done when**: all 8 maps pass the bake-test (heatmap of fights, no dead zones, no spawn imbalance).

## M6 — Stability & ship prep

**Three weeks.**

**Goal**: A binary players can reliably download, run, and play.

- [ ] Cross-platform builds work on every CI run
- [ ] macOS code signing + notarization in the release script
- [ ] Windows build verified on Win10 + Win11
- [ ] macOS build verified on Apple Silicon + Intel (universal)
- [ ] Crash logging to local file
- [ ] Player-friendly error messages on common failures (port conflict, map missing, version mismatch)
- [ ] Networking stress test (32 players for 30 min, no leaks, no hangs)
- [ ] Memory leak audit (heaptrack / address sanitizer clean for 30-min match)
- [ ] Performance pass against the budget in [10-performance-budget.md](10-performance-budget.md)
- [ ] Documentation pass (these docs reflect reality)
- [ ] Distribution: itch.io page, README, screenshots, trailer

**Done when**: a stranger can download from itch.io, double-click, and play a match without help.

## M7 — Launch + first patch cycle

**Two weeks.**

**Goal**: First players are playing. We watch and respond.

- [ ] Public release (v0.1.0)
- [ ] Bug triage from real players
- [ ] First balance pass on weapons (data-driven, with playtest evidence)
- [ ] First patch (v0.1.1) within 7 days of launch — bug fixes only
- [ ] Decide on next feature direction: bots? more maps? competitive ranked?

**Done when**: we have a player base of any size, a list of real complaints, and a v0.1.1 patch that addresses the urgent ones.

---

## Total time estimate (optimistic)

**~24 weeks** for one or two engineers, head-down. Six months. This is aggressive. Realistic: **9–12 months** with normal life and the inevitable scope creep we will refuse to accept.

## Stretch goals (post-v1)

These are *not* on the roadmap. They live here so we don't forget.

- **Bots** — AI-driven mechs to fill servers. Behavior tree + pathfinding on the tile grid.
- **Replays** — server logs inputs + RNG seed, viewer replays the match.
- **Spectator mode** — connect as observer, free camera.
- **More mech chassis** — community-friendly to add (data + sprites).
- **More maps** — same.
- **Master server / matchmaking** — optional internet server browser.
- **NAT hole-punching** — UDP rendezvous via master server.
- **Steam version** — Steamworks + GameNetworkingSockets + workshop maps.
- **Code hot-reload** — gameplay layer as dlopen'd shared lib.
- **Custom mods** — gameplay layer as a versioned API; community can build modes.
- **Linux distribution** — already runs natively in dev; we just need to ship.
- **Mobile / Steam Deck input** — controller + touch reasonable; UI scaling pass.
- **Voice chat** — opt-in, peer-to-peer.
- **Mounted weapons / vehicles** — stationary turrets feasible; mech-jeeps a stretch.
- **Destructible geometry** — designed to be hard to add safely; we'll see.

## What we will refuse to add

- **Microtransactions, loot boxes, battle passes, premium currencies.** Not negotiable.
- **Always-online DRM.** Game runs offline (single-player practice).
- **Account system.** No login. Identity is the IP address you connect from + the name you type.
- **Anti-cheat at kernel level.** Authoritative server is enough; servers are run by players.

---

## How we update this doc

When a milestone ends:
1. Mark it done with a date.
2. Move any unfinished items to the next milestone or to "Stretch."
3. Add notes if estimates were way off (so the next estimate is more honest).
4. Don't reshape past milestones — they're the historical record.

When a milestone is in flight:
1. Track work in the issue tracker, not in this doc.
2. Use this doc as the **strategic** view, not the daily todo list.
