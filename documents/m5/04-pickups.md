# M5 — Pickup system

The map-driven equipment-swap layer. Health packs, ammo crates, armor, weapon swaps, power-ups, jet fuel canisters — all spawned from designer-placed `LvlPickup` records, picked up on touch, and respawned after a timer.

The format that delivers spawner records is in [01-lvl-format.md](01-lvl-format.md). The audio cues this system fires are in [09-audio.md](09-audio.md). The Engineer's repair pack — currently a self-heal in M3 — graduates to a deployable pickup in this document.

## What the M3/M4 build has

There is **no pickup system at M4**. `assets/` is empty. The Engineer's `BTN_USE` calls `mech_apply_damage` with negative damage to heal the engineer themselves (50 HP, 30 s cooldown, `mech.c::ENGINEER_HEAL`). No spawners, no items, no audio cues, no network sync. M5 builds the entire layer.

## Two entities, one map record

The spec separates **spawner** (persistent, lives in the level) from **item** (transient, spawned/grabbed/respawning). The `LvlPickup` record on disk describes a spawner; the runtime creates a single transient `Pickup` per spawner that toggles between AVAILABLE and COOLDOWN states.

```c
// src/pickup.h — new module
typedef enum {
    PICKUP_HEALTH = 0,
    PICKUP_AMMO_PRIMARY,
    PICKUP_AMMO_SECONDARY,
    PICKUP_ARMOR,
    PICKUP_WEAPON,
    PICKUP_POWERUP,
    PICKUP_JET_FUEL,
    PICKUP_REPAIR_PACK,        // engineer-deployed
    PICKUP_PRACTICE_DUMMY,     // single-player practice mode
    PICKUP_KIND_COUNT
} PickupKind;

typedef enum {
    HEALTH_SMALL = 0,    // +25
    HEALTH_MEDIUM,        // +60
    HEALTH_LARGE,         // +full
} HealthVariant;

typedef enum {
    POWERUP_BERSERK = 0,  // 2× damage, 15 s
    POWERUP_INVISIBILITY, // alpha 0.2, 8 s
    POWERUP_GODMODE,      // no incoming damage, 5 s
} PowerupVariant;

typedef enum {
    PICKUP_STATE_AVAILABLE = 0,
    PICKUP_STATE_COOLDOWN,
} PickupState;

typedef struct PickupSpawner {
    Vec2          pos;
    PickupKind    kind;
    uint8_t       variant;            // HealthVariant / PowerupVariant / weapon_id / armor_id
    uint16_t      respawn_ms;         // 0 = use kind default
    PickupState   state;
    uint64_t      available_at_tick;  // when state will become AVAILABLE
    uint16_t      flags;              // CONTESTED / RARE / HOST_ONLY
} PickupSpawner;

#define PICKUP_CAPACITY 64

typedef struct {
    PickupSpawner items[PICKUP_CAPACITY];
    int            count;
} PickupPool;
```

Lives in `World` alongside the existing `ParticlePool` / `ProjectilePool` / `FxPool`:

```c
// src/world.h — added to World struct
PickupPool  pickups;
```

## Default respawn timers

Adopted from Q3 with tuning for Soldut's faster rounds:

| Kind / variant | Default respawn (s) | Notes |
|---|---|---|
| HEALTH small (+25) | 20 | Was 35 in Q3; we're tighter |
| HEALTH medium (+60) | 30 | |
| HEALTH large (+full) | 60 | The "mega" — heavily contested |
| AMMO_PRIMARY | 25 | |
| AMMO_SECONDARY | 25 | |
| ARMOR (light/heavy/reactive) | 30 | |
| WEAPON (specific) | 30 | |
| POWERUP berserk | 90 | 15 s effect |
| POWERUP invisibility | 90 | 8 s effect |
| POWERUP godmode | 180 | 5 s effect; rare |
| JET_FUEL | 15 | At the bottom of vertical drops |
| REPAIR_PACK (engineer) | n/a, lifetime 10 s | One-shot, no respawn |
| PRACTICE_DUMMY | n/a, lifetime ∞ | Single-player only |

These are starting numbers. The map authoring guide in [07-maps.md](07-maps.md) tells designers to override per-spawner via `LvlPickup.respawn_ms` for high-stakes pickups.

## State machine

```
            +--------------+
            | AVAILABLE    | <----+
            +------+-------+      |
                   | mech touches |
                   v              | tick = available_at_tick
            +--------------+      |
            | COOLDOWN     | -----+
            +--------------+
```

Server-side only. Clients render based on the most recent server state — they don't independently transition.

```c
// src/pickup.c
void pickup_step(World *w, float dt) {
    if (!w->authoritative) return;     // clients don't transition
    PickupPool *p = &w->pickups;
    for (int i = 0; i < p->count; ++i) {
        PickupSpawner *s = &p->items[i];
        if (s->state == PICKUP_STATE_COOLDOWN &&
            w->tick >= s->available_at_tick)
        {
            s->state = PICKUP_STATE_AVAILABLE;
            net_server_broadcast_pickup_state(&g->net, i, /*available*/true);
            audio_play_at(SFX_PICKUP_RESPAWN_for_kind(s->kind), s->pos);
        }
    }
    // Touch detection: each AVAILABLE spawner is a 24-px-radius
    // trigger zone on the pelvis. Cheap N×M loop, N = ~40 pickups, M
    // = ~32 mechs.
    for (int i = 0; i < p->count; ++i) {
        PickupSpawner *s = &p->items[i];
        if (s->state != PICKUP_STATE_AVAILABLE) continue;
        for (int mi = 0; mi < w->mech_count; ++mi) {
            Mech *m = &w->mechs[mi];
            if (!m->alive) continue;
            Vec2 mp = mech_chest_pos(w, mi);
            float dx = mp.x - s->pos.x, dy = mp.y - s->pos.y;
            if (dx*dx + dy*dy > 24.0f * 24.0f) continue;
            if (apply_pickup(w, mi, s)) {
                // Pickup consumed; transition to cooldown.
                s->state = PICKUP_STATE_COOLDOWN;
                int respawn_ms = s->respawn_ms ? s->respawn_ms
                                               : default_respawn_ms(s->kind, s->variant);
                s->available_at_tick = w->tick +
                    (uint64_t)((float)respawn_ms * 0.06f);  // 60 Hz → ms→tick
                net_server_broadcast_pickup_state(&g->net, i, /*available*/false);
                audio_play_at(SFX_PICKUP_GRAB_for_kind(s->kind), s->pos);
            }
        }
    }
}
```

`apply_pickup` returns false when the mech can't actually use the item (e.g. full health on a HEALTH pickup). False = don't consume. This avoids "I touched the pack but nothing happened" feeling — the pack stays and respawn timer doesn't tick.

### Apply rules per kind

```c
static bool apply_pickup(World *w, int mi, const PickupSpawner *s) {
    Mech *m = &w->mechs[mi];
    switch (s->kind) {
        case PICKUP_HEALTH: {
            float bonus = health_amount(s->variant);
            if (m->health >= m->health_max) return false;
            m->health = fminf(m->health + bonus, m->health_max);
            return true;
        }
        case PICKUP_AMMO_PRIMARY: {
            const Weapon *wpn = weapon_def(m->primary_id);
            if (m->ammo_primary >= wpn->mag_size) return false;
            m->ammo_primary = wpn->mag_size;
            return true;
        }
        case PICKUP_AMMO_SECONDARY: {
            const Weapon *wpn = weapon_def(m->secondary_id);
            if (m->ammo_secondary >= wpn->mag_size) return false;
            m->ammo_secondary = wpn->mag_size;
            return true;
        }
        case PICKUP_ARMOR: {
            const Armor *a = armor_def((int)s->variant);
            // Replace existing armor unconditionally (no "armor stack").
            m->armor_id = (int)s->variant;
            m->armor_hp = a->hp;
            m->armor_hp_max = a->hp;
            m->armor_charges = a->reactive_charges;
            return true;
        }
        case PICKUP_WEAPON: {
            const Weapon *wpn = weapon_def((int)s->variant);
            if (!wpn) return false;
            // Slot decision: replaces same-class slot (primary or secondary).
            if (wpn->klass == WEAPON_CLASS_PRIMARY) {
                m->primary_id = (int)s->variant;
                m->ammo_primary = wpn->mag_size;
            } else {
                m->secondary_id = (int)s->variant;
                m->ammo_secondary = wpn->mag_size;
            }
            return true;
        }
        case PICKUP_POWERUP:
            return apply_powerup(w, mi, (PowerupVariant)s->variant);
        case PICKUP_JET_FUEL: {
            if (m->fuel >= m->fuel_max) return false;
            m->fuel = m->fuel_max;
            return true;
        }
        case PICKUP_REPAIR_PACK: {
            // Allies + the engineer themselves can grab.
            if (m->health >= m->health_max) return false;
            m->health = fminf(m->health + 50.0f, m->health_max);
            return true;
        }
        case PICKUP_PRACTICE_DUMMY:
            // Not actually a pickup — handled separately. See "Practice
            // dummy" below.
            return false;
    }
    return false;
}
```

## Power-ups (timed buffs)

Powerups don't immediately grant their effect; they set a timer field on the mech:

```c
// src/world.h — added to Mech
float powerup_berserk_remaining;     // > 0 = active
float powerup_invis_remaining;
float powerup_godmode_remaining;
```

These tick down in `mech_step_drive`. Damage paths consult them:

- `mech_apply_damage`: skip damage if `powerup_godmode_remaining > 0`. Damage *outgoing* (in `weapons_fire_*`, `projectile_step`) is doubled if `powerup_berserk_remaining > 0` for the shooter.
- Render path: alpha-mod the mech's draw color by 0.2 if `powerup_invis_remaining > 0`. Local mech sees a stronger alpha (0.5) so the player can still play.

Network: powerup state rides on the EntitySnapshot via 3 reserved bits in `state_bits`. A 24-byte EntitySnapshot widens to 27 (we already widened in M3); the powerup bits go into the existing reserved space.

```c
enum {
    SNAP_STATE_BERSERK    = 1u << 8,   // widen state_bits to u16
    SNAP_STATE_INVIS      = 1u << 9,
    SNAP_STATE_GODMODE    = 1u << 10,
};
```

Bumping state_bits from u8 to u16 requires a snapshot wire-format change → bump `SOLDUT_PROTOCOL_ID` from `S0LF` (M4) to `S0LG` (M5). EntitySnapshot grows by 1 byte; total 28 bytes.

Per the bandwidth budget, 28 × 32 × 30 Hz = 26.4 KB/s uncompressed, still inside [10-performance-budget.md](../10-performance-budget.md)'s downstream target.

## Trigger zones, not bobbing items

The pickup is a 24-px-radius trigger zone centered on `LvlPickup.pos`. There is no physical entity, no collision shape, no constraint. Render-side, the pickup is drawn as:

- A 32×32 sprite of the pickup type (from a single `assets/sprites/pickups.png` atlas).
- Bobs (sin wave, 0.5 Hz, ±4 px Y) when `state == AVAILABLE`.
- Spins (1 turn / 4 s, only the visual sprite — we DrawTexturePro with rotation) when `state == AVAILABLE`.
- Hidden when `state == COOLDOWN`.
- Pulses (alpha + scale) for the last 1 s before respawn (a designer-readable "almost back" cue).

All bob/spin math is render-side. The trigger zone is a static 24 px radius regardless of bob position — the sprite is cosmetic, not collision.

## Wire protocol

Pickup state changes are **events**, not per-tick state. Event:

```c
// NET_MSG_PICKUP_STATE — new message, NET_CH_EVENT (reliable, ordered)
struct {
    uint8_t  msg_type;           // NET_MSG_PICKUP_STATE
    uint8_t  spawner_id;         // index into world.pickups.items
    uint8_t  state;              // PickupState
    uint8_t  reserved;
    uint64_t available_at_tick;  // for AVAILABLE: when next cooldown ends; for COOLDOWN: when next available
}; // 12 bytes
```

Server broadcasts on every state transition. Clients update their local `pickups` pool. Initial state is shipped in `INITIAL_STATE` for joining mid-round (each spawner's current state + cooldown time).

Pickup grabs that the server consumes go through this event; the client never *initiates* a grab — touching the pickup is a server-side observation. Predictive client-side render of "I just grabbed a thing" is a 50-ms perception window we live with: at LAN ping the snap is invisible; at WAN ping the player gets the item delivered ~50 ms after they touched it. Acceptable for v1.

Bandwidth: 12 bytes × ~10 events/min/player × 16 players = 32 B/s aggregate. Trivial.

## Audio cues

Two distinct categories:

1. **Pickup grab sounds**, played at the spawner position on grab. Listener-relative pan + attenuation per [09-audio.md](09-audio.md). Each kind has its own sample (so other players can hear "they just grabbed armor" from across the map).
2. **Respawn cues**, played at the spawner position on transition AVAILABLE. Only fires for high-tier pickups: HEALTH_LARGE, ARMOR (any), JET_FUEL, POWERUP (any), WEAPON. Skipped for small health, ammo, repair packs.

This matches the Q3/UT tradition: the megahealth/armor respawn cue is a load-bearing skill expression — players who track timers gain a real advantage.

Audio paths are listed in [09-audio.md](09-audio.md) §"SFX manifest".

## Engineer's deployable repair pack

The M3 build hooks `BTN_USE` on the Engineer chassis to a self-heal. M5 replaces this with a deployable pickup:

```c
// src/mech.c — modified
static void apply_engineer_use(World *w, int mech_id) {
    Mech *m = &w->mechs[mech_id];
    if (m->ability_cooldown > 0.0f) return;
    Vec2 pos = mech_pelvis_pos(w, mech_id);
    pos.y += 24.0f;   // drop at feet, slightly below pelvis

    // Spawn a transient PICKUP_REPAIR_PACK at pos with 10 s lifetime.
    // It uses the pickup pool's transient slots — separate from the
    // level-defined spawners.
    pickup_spawn_transient(&w->pickups, (PickupSpawner){
        .pos = pos,
        .kind = PICKUP_REPAIR_PACK,
        .variant = 0,
        .respawn_ms = 0,
        .state = PICKUP_STATE_AVAILABLE,
        .available_at_tick = w->tick + 600,    // 10 s lifetime ends in available_at_tick fashion
        .flags = 0,
    });
    m->ability_cooldown = 30.0f;
}
```

Transient spawners are flagged so they're auto-removed on grab or on lifetime expiry, instead of transitioning to COOLDOWN. They're also the only entities that can grow `pickups.count` past `LvlPickup` count from the level — capped at `PICKUP_CAPACITY = 64`.

The runtime mode becomes:

- **Level-defined spawners** (indices `0..level->pickup_count - 1`): persistent across the round, transition AVAILABLE↔COOLDOWN.
- **Transient spawners** (indices `level->pickup_count..pickups.count - 1`): one-shot, removed on grab or on `tick >= available_at_tick`.

## Practice dummy

A new pickup kind: `PICKUP_PRACTICE_DUMMY`. The pickup is *not* grabbed; instead, when the level first loads it spawns a dummy mech at the spawner's position. No grab logic, no respawn — the dummy lives until the round ends.

This addresses the M4 trade-off "single-player mode has no practice dummy." Maps that designate themselves as practice-friendly include a `PICKUP_PRACTICE_DUMMY` spawner at a sensible spot. The single-player offline-host path's lobby starts the round without opposition; the dummy is the opposition.

```c
// src/pickup.c — called once at level load
void pickup_init_round(World *w, const Level *level) {
    PickupPool *p = &w->pickups;
    p->count = 0;
    for (int i = 0; i < level->pickup_count; ++i) {
        const LvlPickup *lp = &level->pickups[i];
        p->items[p->count++] = (PickupSpawner){
            .pos = (Vec2){lp->pos_x, lp->pos_y},
            .kind = (PickupKind)lp->category,
            .variant = lp->variant,
            .respawn_ms = lp->respawn_ms,
            .state = PICKUP_STATE_AVAILABLE,
            .available_at_tick = 0,
            .flags = lp->flags,
        };
        // Special: PRACTICE_DUMMY immediately spawns a dummy mech and
        // marks the spawner consumed (so it doesn't show as a pickup).
        if ((PickupKind)lp->category == PICKUP_PRACTICE_DUMMY) {
            mech_create(w, CHASSIS_TROOPER,
                        (Vec2){lp->pos_x, lp->pos_y}, 0, /*is_dummy*/true);
            p->items[p->count - 1].state = PICKUP_STATE_COOLDOWN;
            p->items[p->count - 1].available_at_tick = UINT64_MAX;
        }
    }
}
```

## Burst SMG cadence (residual fix)

Burst SMG (`g_weapons[WEAPON_BURST_SMG]`) carries `burst_rounds=3, burst_interval_sec=0.070`. M3 fires all 3 rounds on the same tick (TRADE_OFFS entry). M5 lands the proper queued cadence:

```c
// src/world.h — added to Mech
uint8_t  burst_pending_rounds;   // remaining rounds in current burst (0 = none)
float    burst_pending_timer;    // seconds until next round

// src/mech.c::mech_try_fire — when WFIRE_BURST fires
case WFIRE_BURST:
    if (m->burst_pending_rounds == 0) {
        m->burst_pending_rounds = wpn->burst_rounds - 1;
        m->burst_pending_timer = wpn->burst_interval_sec;
        weapons_spawn_projectiles(w, mid, m->weapon_id);   // first round
    }
    break;

// src/mech.c::mech_step_drive — top of fn
if (m->burst_pending_rounds > 0) {
    m->burst_pending_timer -= dt;
    if (m->burst_pending_timer <= 0.0f) {
        weapons_spawn_projectiles(w, mid, m->weapon_id);
        m->burst_pending_rounds--;
        m->burst_pending_timer = wpn->burst_interval_sec;
    }
}
```

Self-bink accumulates over the 3 rounds, producing the expected wider spread on the trailing 2 rounds — matches the design intent in [04-combat.md](../04-combat.md).

Documented as resolving the M3 trade-off "Burst SMG fires all rounds on the same tick."

## Pickup placement guidelines for designers

Per [07-level-design.md](../07-level-design.md) §"Pickup placement heuristics":

1. Health packs along routes between spawn and combat.
2. Power weapons (Rail Cannon, Mass Driver) at the most contested points.
3. Armor in transitional spots — between flow channels.
4. Power-ups at ends of risky paths, slow respawn.
5. Jet fuel at bottoms of vertical drops.

The map-level archetype additions in [07-maps.md](07-maps.md) §"Caves, alcoves, and hidden nooks" further specify *where the path* runs:

- **Edge alcoves** (floor-level nooks against outer walls) — pair with AMMO and HEALTH small/medium. Cheap walk-in.
- **Jetpack alcoves** (high on side walls, jet-only entry) — pair with ARMOR, POWERUP, JET_FUEL. The risk-reward shape demands a *high-stakes* prize.
- **Cave systems** (linked alcove networks) — pair with the rarest pickups: WEAPON Mass Driver, POWERUP godmode. The commitment to enter a cave network is high; the reward must match.
- **Slope-roof nooks** (formed by 45° angled ceilings) — pair with JET_FUEL or ARMOR reactive. The angled-ceiling jet-slide entry is the map's signature movement — the pickup *rewards* learning that movement.

The editor's "validate" pass warns if:

- A power weapon (RAIL, MASS, MICRO) spawns within 200 px of a HEALTH_LARGE.
- Total pickup count exceeds `MAX_PICKUPS_PER_MAP` (= 40, per [07-level-design.md](../07-level-design.md)).
- Two spawners are within 64 px of each other.
- A small-tier pickup (HEALTH small, AMMO_SECONDARY) sits in a jetpack alcove — the reward doesn't justify the commitment. (Anti-pattern flagged in [07-maps.md](07-maps.md) §"Balance and contestability".)

These are warnings, not errors — designers can override.

## Done when

- `src/pickup.{c,h}` exists with the API above.
- `pickup_init_round` runs at level load, populating from `LvlPickup` records.
- `pickup_step` runs each authoritative tick; transitions are broadcast as `NET_MSG_PICKUP_STATE`.
- Touching an AVAILABLE pickup heals/refills/equips per the table above.
- Powerups time out correctly; bandwidth is within budget; berserk doubles damage (verified via shot test).
- Engineer's `BTN_USE` drops a deployable repair pack instead of self-healing; the M3 trade-off entry is deleted.
- Burst SMG fires 3 rounds at 70 ms cadence; the M3 trade-off entry is deleted.
- `PICKUP_PRACTICE_DUMMY` works in single-player offline-host mode; the M4 trade-off entry is deleted.
- All 8 ship maps have at least 12 pickup spawners (per the size table in [07-level-design.md](../07-level-design.md)); the editor validates this on save.

## Trade-offs to log

- **Pickup transient state isn't persisted across host restarts.** A host who crashes and restarts mid-round resets all spawner cooldowns. Acceptable.
- **Practice dummy AI is dummy-static.** It doesn't move or shoot. M5 stretch: simple AI for the practice dummy. Stretch-for-stretch.
- **No "you just got armor" UI flash.** The ammo/health bars update; that's the entire feedback. If players can't tell what they just grabbed, we add a HUD popup in M6.
