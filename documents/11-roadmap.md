# 11 — Roadmap

This document defines what "done" means at each milestone. We work in milestones that each end with **a thing you can play**, not a checklist of features. If a milestone ends and the game isn't more fun than it was, the milestone failed.

Time estimates are calendar weeks for one or two engineers working seriously. They are estimates. They will move; we'll update this doc.

## M0 — Foundation

**Two weeks.** **Status: done (2026).**

**Goal**: We have a window, an empty world, and a build pipeline.

- [x] Project scaffolding (`src/`, `third_party/`, `Makefile`, `build.sh`)
- [x] raylib vendored and statically linked (Linux dev, Windows cross via zig cc, macOS native or cross)
- [x] ENet vendored and statically linked
- [x] `stb_ds.h`, `stb_sprintf.h` vendored
- [x] Build matrix: `make`, `make windows`, `make macos` all produce binaries
- [x] CI runs the build matrix on every push
- [x] Window opens, clear-color renders, FPS counter
- [x] Logger, arena allocator, pool allocator
- [x] Math primitives

**Done when**: a fresh checkout, three commands, opens a window on Windows and macOS.

## M1 — One mech, no network

**Three weeks.** **Status: done (2026-05-03).**

**Goal**: One player can run, jet, jump, shoot at a static target, and see ragdoll on death.

- [x] Particle pool + Verlet integrator
- [x] Constraint pool + relaxation solver (12 iters, constraint+collide interleaved per iter)
- [x] One chassis (Trooper) with the 16-particle skeleton
- [x] Tile grid map format + loader
- [x] Hard-coded test map (built in code, `level_build_tutorial`)
- [x] Body-vs-map collision (per-particle vs tile rect, neighbour-aware exit)
- [x] Animation system: pose-driven (Stand, Run, Jet, Fall, Fire, Death — 6 of the 24 ship at M1)
- [x] Camera2D with smoothed follow + cursor lookahead + screen shake
- [x] One weapon (Pulse Rifle) — hitscan, tracer, recoil impulse
- [x] One target dummy that takes damage
- [x] Death: drop pose drive, apply killshot impulse, ragdoll
- [x] Limb dismemberment (left arm only as proof of concept)
- [x] Blood particles + decal RT layer
- [x] Hit-pause + screen shake
- [x] HUD: health, jet fuel, ammo, crosshair, kill feed

**Done when**: a player can play a 30-second loop — run, jet, shoot the dummy until it falls apart — and grin. *Met.*

**Carried forward (deliberate, not finished here):**

- 120 Hz fixed-step accumulator and render-side interpolation alpha. M1 ships at 60 Hz with one sim tick per render frame. (Picked up before or alongside M2.)
- Full 24-animation set. M1 ships 6.
- IK on the left arm. The left hand currently dangles.
- Angle constraints in active use. Implemented but not registered.

See [CURRENT_STATE.md](../CURRENT_STATE.md) and [TRADE_OFFS.md](../TRADE_OFFS.md) for the full list with revisit triggers.

## M2 — Networking foundation

**Four weeks.** **Status: foundation in 2026-05-03; bake test pending.**

**Goal**: Two players on a LAN can play the M1 loop together.

- [x] `net.h` interface
- [x] ENet host/client wrapping
- [x] Packet header (tag byte + per-message body; ENet handles
      sequencing & ack bitfield internally on reliable channels)
- [x] Channels: STATE, EVENT, CHAT, LOBBY
- [x] Client input upload (60 Hz) with sequence numbers
- [x] Server simulate loop (60 Hz) consuming inputs
- [x] Server snapshot broadcast (30 Hz) — uncompressed
- [ ] Client snapshot **interpolation** for remote mechs (snapshots
      apply instantly today; the interp_delay path is a TRADE_OFFS
      entry — fine on LAN, needed once we're across the internet)
- [x] Client-side prediction + reconciliation for local mech
- [x] Lag compensation for hitscan (server rewinds bone history,
      capped at 200 ms)
- [x] Connection handshake (challenge nonce, version check) —
      uses keyed FNV1a today, HMAC-SHA256 deferred (see TRADE_OFFS.md)
- [x] LAN broadcast for server discovery
- [x] Direct-connect by IP+port

**Done when**: two laptops on the same WiFi can both run the binary, one types `:23073` to host, the other types the host's IP, and they shoot each other for ten minutes without a desync.

**Carried forward (deliberate, not finished here):**

- Mid-tick interpolation of remote mechs (the "render 100 ms in the past" path).
- Snapshot delta encoding (full snapshots only at M2 — fine for
  small player counts, will hit the bandwidth budget at 16+ players).
- Server-side entity culling (also a bandwidth + anti-ESP feature).
- Visual smoothing of reconciliation jumps on the client.
- HMAC-SHA256 for handshake tokens.
- In-game lobby UI (M4).

See [CURRENT_STATE.md](../CURRENT_STATE.md) and [TRADE_OFFS.md](../TRADE_OFFS.md) for the full picture.

## M3 — Combat depth

**Three weeks.** **Status: shipped 2026-05-03.**

**Goal**: All v1 weapons. All v1 mech chassis. All damage and dismemberment systems.

- [x] All 5 chassis with parameterized stats (Trooper / Scout / Heavy / Sniper / Engineer)
- [x] All 8 primaries (Pulse Rifle, Plasma SMG, Riot Cannon, Rail Cannon,
      Auto-Cannon, Mass Driver, Plasma Cannon, Microgun)
- [x] All 6 secondaries (Sidearm, Burst SMG, Frag Grenades, Micro-Rockets,
      Combat Knife, Grappling Hook — grapple is a stub, see TRADE_OFFS.md)
- [x] All 4 armor variants (None, Light, Heavy, Reactive)
- [x] All 4 jetpack variants (Standard, Burst, Glide Wing, Jump Jet)
- [x] Recoil + bink + self-bink fully wired
- [x] Limb HP per limb, dismemberment of each (head, both arms, both legs)
- [x] Explosions: damage falloff, line-of-sight halving, impulse to ragdolls
- [x] Hit-location damage multipliers (head 1.6×, arms/legs 0.7×, hands/feet 0.5×)
- [x] Friendly-fire toggle (server config — `--ff` CLI flag)
- [x] Kill feed UI (HEADSHOT / GIB / OVERKILL / RAGDOLL / SUICIDE flags)

**Done when**: the weapon roster feels distinct in playtest — every gun has a use, none are dominant. *Met for the implementation; balance pass will run alongside M5 maps.*

**Carried forward (deliberate, not finished here):**

- Grappling Hook is a stub (cooldown ticks, but no projectile / anchor /
  pull). Lands at M5 paired with map content that needs swing beats.
- Engineer's BTN_USE heals the engineer (50 HP, 30 s CD). The proper
  "drop a deployable repair pack" lands at M5 with the pickup system.
- Burst SMG fires all 3 rounds on the same tick; per-round 70 ms cadence
  is a TRADE_OFFS entry.
- No lobby UI for picking loadouts — CLI flags only (`--chassis`,
  `--primary`, etc). M4 wires the lobby.

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
