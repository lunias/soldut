# 02 — Game Design

This document specifies the **game** — modes, characters, equipment, the loadout flow, the lobby, the match flow, and progression. Implementation belongs in later documents; this one answers "what is the player doing, when, and why."

## The match in one sentence

> Up to 32 players spawn into a 2D arena, choose a mech and loadout, and fight in 5–10-minute rounds across modes that range from straight free-for-all to capture-the-flag, with map pickups rewarding exploration between fights.

## Match flow

```
[Server browser / Direct connect]
        ↓
[Lobby — chat, ready up, player list, mech & loadout selection, map vote]
        ↓ (all ready, OR start-timer expires)
[Pre-round briefing, 5s — show map, show teams]
        ↓
[Round, 5–10 min — combat, pickups, respawns]
        ↓
[Round summary, 15s — kills, deaths, MVP]
        ↓
[Map vote / next-round lobby — loop until host disbands]
```

The **lobby is sticky.** Players stay between rounds. The match is the unit of session; the round is the unit of play.

## Modes

Three modes at v1. Each is implemented; mode-specific logic is a tiny module that consumes the shared world simulation.

### Free-for-All (FFA, "Deathmatch")
- Solo. First to N kills, or highest kill count when timer ends.
- Default: 25 kills, 10-minute timer.
- No friendly fire (there are no friends).
- Respawn after 3 seconds at a random spawn point.

### Team Deathmatch (TDM)
- Two teams (Red, Blue). Up to 16 v 16.
- First team to N kills, or higher count at timer end.
- Default: 80 kills, 10-minute timer.
- Friendly fire: optional, server config. Default off for casual servers, on for tournament settings.

### Capture the Flag (CTF)
- Two teams. Each base has a flag.
- Capture by carrying the enemy flag back to *your own* flag, which must be home.
- Score limit: 5 captures, 15-minute timer.
- Carrying the flag halves your jet thrust and disables your secondary weapon.
- Dropped flags return to base after 30 seconds.

These three are enough to ship. **Capture-and-Hold (KOTH)** and **Bunker (asymmetric assault)** are post-launch.

## Mechs (player characters)

Each player picks one of **five mech chassis** before each round. Chassis differ in mass, hitbox shape, jet curve, base health, and default movement feel. There is no "one is just better"; each is a different *play style*.

Numbers are starting targets and will be tuned.

| Chassis | Health | Mass | Run Speed | Jet Thrust | Jet Fuel | Hitbox | Identity |
|---|---|---|---|---|---|---|---|
| **Scout** | 100 | 0.8 | 1.20× | 1.30× | 1.20× | small | Fast, fragile, vertical |
| **Trooper** | 150 | 1.0 | 1.00× | 1.00× | 1.00× | medium | Balanced; the default |
| **Heavy** | 220 | 1.4 | 0.85× | 0.80× | 0.85× | large | Soak damage, slow |
| **Sniper** | 130 | 0.95 | 0.95× | 1.00× | 1.10× | medium | Long aim, extra zoom, lower spread when crouched |
| **Engineer** | 140 | 1.0 | 1.00× | 1.10× | 1.00× | medium | Repairs allies, deploys mines |

Each chassis has:
- A unique silhouette (so you can read at a glance who is who).
- A "passive" — a single mechanical perk (Heavy: -10% incoming explosion damage; Engineer: can drop a 50-HP repair pack on a 30s cooldown; Sniper: scoped zoom; Scout: double-jump dash; Trooper: faster reload).
- The same skeleton (see [03-physics-and-mechs.md](03-physics-and-mechs.md)) — only mass, scale, and visual sprites differ. **One physics model per game**, parameterized.

## Equipment slots

Each mech has **four slots**, set during loadout and partially mutable in-match via map pickups.

### 1. Primary Weapon
The big gun. Limited list (see [04-combat.md](04-combat.md)) — assault rifle, sniper rifle, shotgun, minigun, rocket launcher, plasma cannon, etc. About 8 primaries at v1.

### 2. Secondary Weapon
A sidearm or special. Pistol (semi-auto), SMG (full-auto burst), grappling hook, knife, frag grenades (3 charge), micro-rockets (5 charge). About 6 secondaries at v1.

### 3. Body Armor
Three tiers + variants:
- **None** — no penalty, no benefit. Lighter mass, slightly faster run.
- **Light Plating** — +30 HP equivalent, no movement penalty.
- **Heavy Plating** — +75 HP equivalent, -10% run speed, -10% jet thrust.
- **Reactive Plating** — variant of light: absorbs 50% of one explosion, then breaks. Niche; rewards prediction.

Armor takes damage **before** the mech HP. When armor is depleted, it drops off as a visible polygon and the mech is "naked" until it picks up new armor.

### 4. Jetpack
Mechs always have *some* jet capability — chassis-baseline. The jetpack slot is a **module** that modifies it:
- **None** — chassis baseline only.
- **Standard Jet** — +20% fuel capacity, +10% thrust.
- **Burst Jet** — same fuel, but a "boost" button that dumps 30% of fuel for 2× thrust over 0.4s. Great for evasion.
- **Glide Wing** — small fuel, but holding jet near zero fuel still gives lift (slow descent).
- **Jump Jet** — no thrust, but on touchdown reloads jump-impulse instantly; supports infinite re-jumps as long as fuel remains.

### Loadout UI (lobby)

A horizontal slot bar across the bottom of the lobby screen:

```
[ MECH ] [ PRIMARY ] [ SECONDARY ] [ ARMOR ] [ JETPACK ]
```

Click a slot, scroll a menu of options, click to confirm. Hover shows stat bars (damage, rate, accuracy, weight). The **Ready** button only enables when all slots are filled.

## Map pickups

In-match equipment swaps come from the map. Pickups are placed by the level designer (see [07-level-design.md](07-level-design.md)) and respawn on a timer.

Pickup categories:

| Category | Examples | Respawn |
|---|---|---|
| **Health pack** | small (+25), medium (+60), large (+full) | 20 / 30 / 60 s |
| **Ammo crate** | refills primary OR secondary | 25 s |
| **Armor pack** | adds 50 HP equivalent (capped at slot tier max) | 30 s |
| **Weapon swap** | a different primary or secondary on the ground | 30 s, fixed type per spawner |
| **Power-up** | brief buff: berserk (2× damage 10s), invisibility (8s), god-mode (5s, rare) | 90–180 s |
| **Jet fuel** | refills jetpack to max | 15 s |

Pickups are visible from a distance (they pulse). Sounds when picked up are distinct enough that other players can hear what you grabbed and react.

The fundamental design loop is:

> **You are always either fighting or running toward a fight, with a small detour available for advantage.**

Pickups are the detour. Maps are designed so the detour is fast (5–10 seconds), but during it your enemy is *also* moving. (See [07-level-design.md](07-level-design.md).)

## Health, damage, and death

- HP is a number per mech. Body armor is a separate number that depletes first.
- Damage is per-bullet, computed from weapon damage × hit-location multiplier × armor reduction. (See [04-combat.md](04-combat.md).)
- **Hit locations**: head (×1.6), torso (×1.0), arm (×0.7), leg (×0.7). Computed from which bone segment the ray hit.
- Death triggers ragdoll mode (see [03-physics-and-mechs.md](03-physics-and-mechs.md)). The body remains as physics geometry for ~30s or until far off-screen, then is recycled.
- **Limb dismemberment**: each limb (arm, leg, head) tracks accumulated damage. When a limb's damage exceeds its threshold (default 80 HP), the joint constraint breaks and the limb detaches. Dismemberment is a **death enhancer**, not a balance lever — surviving with one arm is rare; it makes for a great clip.

## Respawn

3-second countdown after death. Spawn at a randomly-chosen valid spawn point that is:
- At least 800 px from the nearest enemy.
- On a friendly side of the map (in TDM/CTF).
- Not currently occupied (no telefrags).

For the 3 seconds you are spectating the killer or the killcam (toggle).

## Scoring

- **Kill**: +1 point. (No "assists" at v1; over-engineering.)
- **Death**: -0 points. (We don't punish dying; matches end on kill caps anyway.)
- **Flag capture (CTF)**: +5 to your team, +1 to you.
- **Flag return (CTF)**: +1 to you.
- **Suicide**: -1 point. (Don't faceplant your own grenade.)

Round summary screen shows: kills, deaths, K/D, time alive, longest killstreak, MVP. Stats are local to the match — **no persistence at v1**.

## Lobby flow (detailed)

When a player joins:

1. **Server browser** lists active games. Each row: server name, host IP, current/max players, map, mode, ping. Refresh button. "Direct connect" button for IP+port.
2. Click a row → **Connect**. Handshake, version check (mismatched versions are kicked with a clear error). Map is downloaded if missing (see [07-level-design.md](07-level-design.md) — maps are <500 KB, transfer is fast).
3. Land in the **waiting room**:
   - Player list (scrollable on the right, with team selector for TDM/CTF)
   - Loadout strip (bottom)
   - Map preview + mode info (top-left)
   - Chat box (bottom-right) — text only, 256 char max, rate-limited
   - **Ready** button (toggle)
   - "Time to start: M:SS" countdown
4. Match starts when:
   - All connected players are Ready, OR
   - The host's configured `auto_start_seconds` (default 60) has elapsed since 50% of slots filled.
5. Anyone connecting *during* the round is in spectate-until-respawn-cycle, then joins next death.

### Chat

- Reliable channel (see [05-networking.md](05-networking.md)).
- Server scrubs to UTF-8, strips control chars, caps at 256 bytes.
- Rate limit: 1 message per 0.5 s per player. Extras are dropped silently with a small client-side animation.
- `/team`, `/all`, `/me`, `/me-too` — slash commands for team chat, emote, etc.
- Host can `/kick playername` and `/ban playername`. Bans persist in `bans.txt` next to the executable.

### Map vote

When a round ends, the lobby shows three randomly-chosen maps (excluding the one just played). 15-second vote window. Most votes wins; ties broken by random.

## Progression

There is **no meta-progression at v1**. No XP, no unlocks, no level-up. Every weapon and chassis is available immediately. This is on purpose:

- It removes a class of cheating (rank-grinding, smurfing).
- It lets a new player play *the actual game* in their first match.
- It removes a year of design debt (balance economies, unlock pacing).
- The skill ceiling is the depth, not the gear.

If we ever add progression post-launch, it is **cosmetic only** (mech paint jobs, kill-feed icons). We do not gate mechanics behind grinding.

## Bot mode (stretch goal)

A solo offline mode where the host plays against AI mechs. Bots use the same simulation pipeline as humans, with AI generating the input bitmask. Useful for:

- Practicing mechanics without a server.
- Testing maps in the editor.
- Filling out servers that have only a few human players.

Bots get their own document if/when we ship them. At v1 the player base lives on multiplayer.

## Failure modes we deliberately accept

- **Latency over 200 ms is bad.** If the host has a poor connection, gameplay degrades. We do not build relay infrastructure to fix this — players choose servers.
- **Cheaters exist.** We rely on authoritative-server invariants (see [05-networking.md](05-networking.md)). Hosts can ban. We do not build kernel anti-cheat.
- **Hosts can disappear.** If the host disconnects, the match ends. There is no host-migration at v1. Players move on.
- **The lobby is small.** No persistent matchmaking pools. The server browser is the matchmaker. This is a feature: it preserves the LAN-party feel of Soldat.

## What success looks like, from the player's seat

- A new player downloads a 25 MB executable.
- They double-click it. The window opens in 2 seconds.
- They click "Server browser." Servers appear.
- They click one. Twelve seconds later they're in a lobby, picking a mech.
- The match starts. Twenty seconds in, they kill someone. Thirty seconds later, they get killed in a way that feels fair and looks great.
- An hour later they're still playing.

Everything in this document is in service of that experience.
