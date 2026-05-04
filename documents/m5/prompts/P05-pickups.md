# P05 — Pickup system + Engineer deployable + Burst SMG cadence + practice dummy

## What this prompt does

Implements the pickup runtime: spawner pool populated from `LvlPickup` records, item touch detection, respawn timers, audio-cue triggers, network protocol (`NET_MSG_PICKUP_STATE` event-only), powerup application + state. Plus three smaller tied items: the Engineer's `BTN_USE` becomes a deployable repair pack (resolves an M3 trade-off); Burst SMG fires its 3 rounds at proper 70 ms cadence (resolves another M3 trade-off); practice dummy gets a `PICKUP_PRACTICE_DUMMY` spawner kind (resolves an M4 trade-off).

Depends on P01 (`.lvl` format defines `LvlPickup`).

## Required reading

1. `CLAUDE.md`
2. `documents/02-game-design.md` §"Map pickups", §"Health, damage, and death"
3. `documents/04-combat.md` — weapon stats relevant to pickups
4. **`documents/m5/04-pickups.md`** — the spec
5. `documents/m5/01-lvl-format.md` §"PICK — pickup spawners"
6. `TRADE_OFFS.md` — "Engineer ability heals self instead of dropping a deployable", "Burst SMG fires all rounds on the same tick", "Single-player mode has no practice dummy"
7. `src/world.h` — Mech struct (you'll add powerup timers + burst pending fields)
8. `src/mech.{c,h}` — `mech_apply_damage`, `mech_step_drive`, the M3 `apply_engineer_use` path
9. `src/weapons.{c,h}` — Burst SMG firing path
10. `src/net.{c,h}` — channels + message tags
11. `src/snapshot.{c,h}` — EntitySnapshot widening for powerup state (`SNAP_STATE_BERSERK/INVIS/GODMODE`)
12. `src/lobby.c` — `lobby_spawn_round_mechs` (you'll add the practice dummy spawn after pickup_init_round)

## Background

After P01 the `Level` struct has `pickups` records but the runtime ignores them. The Engineer's BTN_USE heals self (50 HP, 30 s CD) per M3 — the canon doc wants it to drop a deployable repair pack. Burst SMG fires all 3 rounds on the same tick. Single-player mode has no opposition.

## Concrete tasks

### Task 1 — `src/pickup.{h,c}`

New module per `documents/m5/04-pickups.md` §"Two entities, one map record":

```c
typedef enum {
    PICKUP_HEALTH = 0,
    PICKUP_AMMO_PRIMARY, PICKUP_AMMO_SECONDARY,
    PICKUP_ARMOR, PICKUP_WEAPON, PICKUP_POWERUP, PICKUP_JET_FUEL,
    PICKUP_REPAIR_PACK,        // engineer-deployed
    PICKUP_PRACTICE_DUMMY,     // single-player only
    PICKUP_KIND_COUNT
} PickupKind;

typedef struct PickupSpawner { ... } PickupSpawner;
typedef struct { PickupSpawner items[64]; int count; } PickupPool;
```

Add `PickupPool pickups;` to World.

Implement:
- `pickup_init_round(World*, const Level*)` — populate from `level->pickups`.
- `pickup_step(World*, float dt)` — server-only state machine + touch detection per the spec.
- `pickup_spawn_transient(PickupPool*, PickupSpawner)` — for the Engineer's repair pack.
- `apply_pickup(World*, int mech_id, const PickupSpawner*)` — returns false when the mech can't use the item (full health, etc.).
- `apply_powerup(World*, int mech_id, PowerupVariant)` — sets timer fields.

Default respawn timers per the doc.

### Task 2 — Powerup state on Mech + snapshot

Add to `Mech`:

```c
float powerup_berserk_remaining;
float powerup_invis_remaining;
float powerup_godmode_remaining;
```

Tick down in `mech_step_drive`. Damage paths consult them:
- `mech_apply_damage`: skip damage if `powerup_godmode_remaining > 0`.
- Outgoing damage (in `weapons_fire_*`, `projectile_step`) doubled if shooter has `powerup_berserk_remaining > 0`.
- Render: alpha-mod the mech's draw color by 0.2 if `powerup_invis_remaining > 0`. Local mech sees alpha 0.5 for legibility.

Widen `EntitySnapshot.state_bits` from `u8` to `u16`. Add bits per `documents/m5/04-pickups.md` §"Powerups (timed buffs)":

```c
SNAP_STATE_BERSERK    = 1u << 8,
SNAP_STATE_INVIS      = 1u << 9,
SNAP_STATE_GODMODE    = 1u << 10,
```

Bump `SOLDUT_PROTOCOL_ID` from `S0LF` to `S0LG`. EntitySnapshot grows from 27 to 28 bytes.

### Task 3 — Wire protocol: `NET_MSG_PICKUP_STATE`

New message tag at `NET_CH_EVENT` (reliable, ordered):

```c
struct {
    uint8_t  msg_type;           // NET_MSG_PICKUP_STATE
    uint8_t  spawner_id;         // index into world.pickups.items
    uint8_t  state;              // PickupState
    uint8_t  reserved;
    uint64_t available_at_tick;
};
```

12 bytes. Server broadcasts on every state transition. Initial state shipped in `INITIAL_STATE` for joining mid-round.

Plumb through `net_server_broadcast_pickup_state(NetState*, int spawner_id, bool available)` and the client handler.

### Task 4 — Engineer deployable repair pack

Replace the M3 self-heal in `mech_step_drive` (search for `PASSIVE_ENGINEER_REPAIR` and `ENGINEER_HEAL`):

```c
if ((pressed & BTN_USE) && ch->passive == PASSIVE_ENGINEER_REPAIR
    && m->ability_cooldown <= 0.0f) {
    Vec2 pos = mech_pelvis_pos(w, mech_id);
    pos.y += 24.0f;
    pickup_spawn_transient(&w->pickups, (PickupSpawner){
        .pos = pos, .kind = PICKUP_REPAIR_PACK, .variant = 0,
        .respawn_ms = 0, .state = PICKUP_STATE_AVAILABLE,
        .available_at_tick = w->tick + 600,    // 10 s lifetime
        .flags = 0,
    });
    m->ability_cooldown = 30.0f;
}
```

Transient spawners are flagged so they're auto-removed on grab or at lifetime expiry, instead of transitioning to COOLDOWN.

### Task 5 — Burst SMG cadence fix

Add to `Mech`:

```c
uint8_t burst_pending_rounds;
float   burst_pending_timer;
```

In `mech_try_fire`, when WFIRE_BURST fires:

```c
case WFIRE_BURST:
    if (m->burst_pending_rounds == 0) {
        m->burst_pending_rounds = wpn->burst_rounds - 1;
        m->burst_pending_timer = wpn->burst_interval_sec;
        weapons_spawn_projectiles(w, mid, m->weapon_id);   // first round
    }
    break;
```

In `mech_step_drive`, top of fn:

```c
if (m->burst_pending_rounds > 0) {
    m->burst_pending_timer -= dt;
    if (m->burst_pending_timer <= 0.0f) {
        weapons_spawn_projectiles(w, mid, m->weapon_id);
        m->burst_pending_rounds--;
        m->burst_pending_timer = wpn->burst_interval_sec;
    }
}
```

Verify: `tests/shots/m3_grenade.shot` or write a new `tests/shots/m5_burst.shot` that holds Burst SMG and shows 3 visible rounds spaced 70 ms apart, not on the same tick.

### Task 6 — Practice dummy

Per `documents/m5/04-pickups.md` §"Practice dummy":

In `pickup_init_round`, when a spawner is `kind == PICKUP_PRACTICE_DUMMY`, spawn a dummy mech via `mech_create(w, CHASSIS_TROOPER, pos, 0, /*is_dummy*/true)` and immediately mark the spawner as consumed (state = COOLDOWN, available_at_tick = UINT64_MAX so it never respawns).

This means a map authored with a `PICKUP_PRACTICE_DUMMY` spawner spawns a static target dummy at level load. Useful for single-player testing.

## Done when

- `make` builds clean.
- A map with placed `LvlPickup` records (use a temp test map authored in the editor from P04, or hardcode in `build_foundry`) shows pickup sprites in-world.
- Touching a HEALTH pickup with damaged health refills HP and the spawner enters cooldown; respawns after the right delay.
- A POWERUP berserk pickup makes the next 15 s of fire deal 2× damage.
- POWERUP godmode makes the player invulnerable for 5 s.
- Engineer's BTN_USE drops a `PICKUP_REPAIR_PACK` at their feet that lasts 10 s; allies can grab it.
- Burst SMG fires 3 rounds at 70 ms cadence (verifiable in shot test).
- Single-player mode with a practice dummy in the map: player can shoot the dummy.
- Networking: 2 players in TDM with pickups; client sees pickup state transitions; bandwidth stays inside budget (verify with `tests/net/run_3p.sh`).

## Out of scope

- The actual *art* for pickup sprites — that's P13 (HUD/atlas) + P16 (asset gen). For this prompt use placeholder colored circles per `pickup_kind_color()` helper.
- The audio cues — that's P14 (audio module). Wire the call sites with `audio_play_at(SFX_PICKUP_*, pos)` stubs that are no-ops until P14 lands the audio module.
- CTF flag pickups — that's P07 (CTF mode). Pickups and flags are different systems.
- Map content with actual pickup placement — that's P17/P18.

## How to verify

```bash
make
make test-physics
./tests/net/run.sh
./tests/net/run_3p.sh
```

Smoke: write `tests/shots/m5_pickups.shot` that spawns a HEALTH pack, walks the player onto it, captures before/after HP. Verify HP increases.

## Close-out

1. Update `CURRENT_STATE.md`: pickups runtime in; Engineer deployable; Burst SMG cadence; practice dummy.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Engineer ability heals self instead of dropping a deployable".
   - **Delete** "Burst SMG fires all rounds on the same tick".
   - **Delete** "Single-player mode has no practice dummy".
   - **Add** "Pickup transient state isn't persisted across host restarts" (pre-disclosed).
3. Bump protocol id `S0LF → S0LG` in `version.h` if not already.
4. Don't commit unless explicitly asked.

## Common pitfalls

- **Touch detection radius is 24 px** (per the doc). Don't use the existing weapon range or projectile size — pickups have their own.
- **Apply rule "return false when the mech can't use the item"** is important — without it, walking over a HEALTH pack at full HP consumes it. The doc spells out per-kind rules.
- **Powerup state on snapshot apply (client-side)** — clients must mirror the timer ticks locally; the timer doesn't ride the wire as a float, only the bit.
- **Transient spawner cleanup**: when a transient spawner's lifetime expires, remove it from the pool. Don't leave dead entries.
- **`SOLDUT_PROTOCOL_ID` bump**: confirm the previous version. If P03 already bumped to S0LG, don't double-bump.
