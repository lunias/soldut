#include "pickup.h"

#include "log.h"
#include "mech.h"
#include "particle.h"
#include "weapons.h"

#include <math.h>
#include <string.h>

/* Server-side: queue a state-change event onto World.pickupfeed for
 * main.c to drain and broadcast via NET_MSG_PICKUP_STATE. Mirrors the
 * HitFeed / FireFeed monotonic-counter pattern. Silently drops events
 * if we somehow overflow (worst case: pool of 64 toggling every tick;
 * we'd cycle back into ring entries 64 ticks later, which still arrives
 * on the wire — just not in original order, which is fine for a
 * full-state event). */
static void enqueue_pickup_event(World *w, int spawner_id) {
    if (!w->authoritative) return;
    int slot = w->pickupfeed_count % PICKUPFEED_CAPACITY;
    w->pickupfeed[slot] = spawner_id;
    w->pickupfeed_count++;
}

/* Defaults table per `documents/m5/04-pickups.md` §"Default respawn
 * timers". Engineer pack and practice dummy don't respawn — they're
 * one-shots managed via PICKUP_FLAG_TRANSIENT or COOLDOWN+UINT64_MAX. */
int pickup_default_respawn_ms(uint8_t kind, uint8_t variant) {
    switch (kind) {
        case PICKUP_HEALTH:
            switch (variant) {
                case HEALTH_SMALL:  return 20000;
                case HEALTH_MEDIUM: return 30000;
                case HEALTH_LARGE:  return 60000;
                default:            return 30000;
            }
        case PICKUP_AMMO_PRIMARY:
        case PICKUP_AMMO_SECONDARY:    return 25000;
        case PICKUP_ARMOR:             return 30000;
        case PICKUP_WEAPON:            return 30000;
        case PICKUP_POWERUP:
            switch (variant) {
                case POWERUP_BERSERK:      return 90000;
                case POWERUP_INVISIBILITY: return 90000;
                case POWERUP_GODMODE:      return 180000;
                default:                   return 90000;
            }
        case PICKUP_JET_FUEL:          return 15000;
        case PICKUP_REPAIR_PACK:       return 0;     /* transient lifetime */
        case PICKUP_PRACTICE_DUMMY:    return 0;     /* one-shot */
        default:                       return 30000;
    }
}

/* RGBA8 color for the placeholder pickup sprite. P13 replaces this with
 * a sprite atlas lookup. Layout matches raylib's Color (r,g,b,a). */
static uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r) | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

uint32_t pickup_kind_color(uint8_t kind) {
    switch (kind) {
        case PICKUP_HEALTH:         return rgba(220,  60,  60, 255);  /* red cross */
        case PICKUP_AMMO_PRIMARY:   return rgba(220, 180,  60, 255);  /* amber */
        case PICKUP_AMMO_SECONDARY: return rgba(180, 220,  60, 255);  /* lime */
        case PICKUP_ARMOR:          return rgba( 80, 120, 220, 255);  /* steel blue */
        case PICKUP_WEAPON:         return rgba(220, 220, 220, 255);  /* white */
        case PICKUP_POWERUP:        return rgba(220, 100, 220, 255);  /* magenta */
        case PICKUP_JET_FUEL:       return rgba(120, 220, 220, 255);  /* cyan */
        case PICKUP_REPAIR_PACK:    return rgba(120, 220, 120, 255);  /* engineer green */
        case PICKUP_PRACTICE_DUMMY: return rgba(160, 160, 160, 255);  /* (not drawn) */
        default:                    return rgba(200, 200, 200, 255);
    }
}

/* ---- pickup_init_round ------------------------------------------- */

void pickup_init_round(World *w) {
    PickupPool *p = &w->pickups;
    memset(p, 0, sizeof *p);
    const Level *L = &w->level;
    int n = L->pickup_count;
    if (n > PICKUP_CAPACITY) {
        LOG_W("pickup_init_round: level has %d spawners, capacity %d — "
              "truncating", n, PICKUP_CAPACITY);
        n = PICKUP_CAPACITY;
    }
    for (int i = 0; i < n; ++i) {
        const LvlPickup *lp = &L->pickups[i];
        PickupSpawner *s = &p->items[p->count++];
        s->pos        = (Vec2){ (float)lp->pos_x, (float)lp->pos_y };
        s->kind       = lp->category;
        s->variant    = lp->variant;
        s->respawn_ms = lp->respawn_ms;
        s->state      = PICKUP_STATE_AVAILABLE;
        s->available_at_tick = 0;
        s->flags      = lp->flags;

        /* PRACTICE_DUMMY isn't a real pickup — it's a marker that says
         * "spawn a dummy mech here at level load." Server-only; pure
         * clients learn about the dummy via the snapshot stream and the
         * SNAP_STATE_IS_DUMMY bit. */
        if (s->kind == PICKUP_PRACTICE_DUMMY) {
            if (w->authoritative) {
                int dummy_id = mech_create(w, CHASSIS_TROOPER, s->pos,
                                           /*team*/0, /*is_dummy*/true);
                if (dummy_id >= 0) {
                    if (w->dummy_mech_id < 0) w->dummy_mech_id = dummy_id;
                    LOG_I("pickup_init_round: practice dummy at (%.0f,%.0f) "
                          "→ mech %d", s->pos.x, s->pos.y, dummy_id);
                }
            }
            /* Mark consumed regardless of side so it never shows up as a
             * pickup. UINT64_MAX means "available_at_tick will never be
             * reached" — i.e., the entry sits in COOLDOWN forever. */
            s->state             = PICKUP_STATE_COOLDOWN;
            s->available_at_tick = (uint64_t)-1;
        }
    }
    LOG_I("pickup_init_round: %d spawner(s) populated (level had %d, "
          "authoritative=%d)",
          p->count, L->pickup_count, (int)w->authoritative);
}

/* ---- pickup_spawn_transient -------------------------------------- */

int pickup_spawn_transient(World *w, PickupSpawner s) {
    PickupPool *p = &w->pickups;
    if (p->count >= PICKUP_CAPACITY) {
        LOG_W("pickup_spawn_transient: pool full (%d)", PICKUP_CAPACITY);
        return -1;
    }
    s.flags |= PICKUP_FLAG_TRANSIENT;
    int idx = p->count++;
    p->items[idx] = s;
    /* Tell clients about the new transient — the wire encoding includes
     * full spawner data (pos/kind/variant/flags) so the client can
     * extend its pool and render the placeholder sprite. */
    enqueue_pickup_event(w, idx);
    return idx;
}

/* ---- apply_pickup ------------------------------------------------- */

static float pickup_health_amount(uint8_t variant) {
    switch (variant) {
        case HEALTH_SMALL:  return 25.0f;
        case HEALTH_MEDIUM: return 60.0f;
        case HEALTH_LARGE:  return 1e9f;     /* clamped to health_max below */
        default:            return 25.0f;
    }
}

static bool apply_powerup(World *w, int mid, uint8_t variant) {
    Mech *m = &w->mechs[mid];
    switch (variant) {
        case POWERUP_BERSERK:
            m->powerup_berserk_remaining = 15.0f;
            return true;
        case POWERUP_INVISIBILITY:
            m->powerup_invis_remaining = 8.0f;
            return true;
        case POWERUP_GODMODE:
            m->powerup_godmode_remaining = 5.0f;
            return true;
        default:
            return false;
    }
}

static bool apply_pickup(World *w, int mid, const PickupSpawner *s) {
    Mech *m = &w->mechs[mid];
    switch (s->kind) {
        case PICKUP_HEALTH: {
            if (m->health >= m->health_max) return false;
            float amt = pickup_health_amount(s->variant);
            m->health = fminf(m->health + amt, m->health_max);
            return true;
        }
        case PICKUP_AMMO_PRIMARY: {
            const Weapon *wpn = weapon_def(m->primary_id);
            if (!wpn || wpn->mag_size <= 0) return false;
            if (m->ammo_primary >= wpn->mag_size) return false;
            m->ammo_primary = wpn->mag_size;
            if (m->active_slot == 0) m->ammo = m->ammo_primary;
            return true;
        }
        case PICKUP_AMMO_SECONDARY: {
            const Weapon *wpn = weapon_def(m->secondary_id);
            if (!wpn || wpn->mag_size <= 0) return false;
            if (m->ammo_secondary >= wpn->mag_size) return false;
            m->ammo_secondary = wpn->mag_size;
            if (m->active_slot == 1) m->ammo = m->ammo_secondary;
            return true;
        }
        case PICKUP_ARMOR: {
            const Armor *a = armor_def((int)s->variant);
            if (!a) return false;
            m->armor_id      = (int)s->variant;
            m->armor_hp      = a->hp;
            m->armor_hp_max  = a->hp;
            m->armor_charges = a->reactive_charges;
            return true;
        }
        case PICKUP_WEAPON: {
            const Weapon *wpn = weapon_def((int)s->variant);
            if (!wpn) return false;
            if (wpn->klass == WEAPON_CLASS_PRIMARY) {
                m->primary_id = (int)s->variant;
                m->ammo_primary = wpn->mag_size;
                if (m->active_slot == 0) {
                    m->weapon_id = m->primary_id;
                    m->ammo      = m->ammo_primary;
                    m->ammo_max  = wpn->mag_size;
                }
            } else {
                m->secondary_id   = (int)s->variant;
                m->ammo_secondary = wpn->mag_size;
                if (m->active_slot == 1) {
                    m->weapon_id = m->secondary_id;
                    m->ammo      = m->ammo_secondary;
                    m->ammo_max  = wpn->mag_size;
                }
            }
            return true;
        }
        case PICKUP_POWERUP:
            return apply_powerup(w, mid, s->variant);
        case PICKUP_JET_FUEL: {
            if (m->fuel >= m->fuel_max) return false;
            m->fuel = m->fuel_max;
            return true;
        }
        case PICKUP_REPAIR_PACK: {
            if (m->health >= m->health_max) return false;
            m->health = fminf(m->health + 50.0f, m->health_max);
            return true;
        }
        case PICKUP_PRACTICE_DUMMY:
        default:
            return false;
    }
}

/* ---- pickup_step -------------------------------------------------- */

#define PICKUP_TOUCH_RADIUS_PX 24.0f

/* Mech body covers ~96 px head-to-foot — a single 24 px circle on the
 * chest leaves the legs and head outside the trigger zone. Test the
 * pickup against four body anchors (chest / pelvis / each foot); any
 * one within `PICKUP_TOUCH_RADIUS_PX` is a touch. Cheap (4 squared
 * distances per pickup × mech), and matches Soldat-style "any body
 * part walks over the pack" feel. */
static bool pickup_mech_touches(const World *w, const Mech *m, Vec2 pos) {
    static const int parts[] = { PART_CHEST, PART_PELVIS, PART_L_FOOT, PART_R_FOOT };
    const ParticlePool *p = &w->particles;
    int b = m->particle_base;
    float r2 = PICKUP_TOUCH_RADIUS_PX * PICKUP_TOUCH_RADIUS_PX;
    for (int i = 0; i < (int)(sizeof parts / sizeof parts[0]); ++i) {
        int idx = b + parts[i];
        float dx = p->pos_x[idx] - pos.x;
        float dy = p->pos_y[idx] - pos.y;
        if (dx*dx + dy*dy <= r2) return true;
    }
    return false;
}

void pickup_step(World *w, float dt) {
    (void)dt;
    if (!w->authoritative) return;
    PickupPool *p = &w->pickups;

    /* Pass 1: COOLDOWN → AVAILABLE rollover. */
    for (int i = 0; i < p->count; ++i) {
        PickupSpawner *s = &p->items[i];
        if (s->state != PICKUP_STATE_COOLDOWN) continue;
        if (s->flags & PICKUP_FLAG_TRANSIENT) {
            /* Transient that's already been consumed/expired (state set
             * to COOLDOWN with available_at_tick=UINT64_MAX). Leave it.
             * Compaction would invalidate the spawner_id used over the
             * wire, so we leak entries within the round; round end
             * resets the pool.) */
            continue;
        }
        if (s->available_at_tick == (uint64_t)-1) continue;   /* permanently consumed */
        if (w->tick >= s->available_at_tick) {
            s->state = PICKUP_STATE_AVAILABLE;
            enqueue_pickup_event(w, i);
            SHOT_LOG("t=%llu pickup %d respawn (kind=%d)",
                     (unsigned long long)w->tick, i, (int)s->kind);
        }
    }

    /* Pass 2: AVAILABLE → COOLDOWN on touch. Cheap N×M (≤64×32) loop. */
    for (int i = 0; i < p->count; ++i) {
        PickupSpawner *s = &p->items[i];
        if (s->state != PICKUP_STATE_AVAILABLE) continue;
        /* Transient lifetime expiry — auto-consume without a grab. */
        if ((s->flags & PICKUP_FLAG_TRANSIENT)
            && s->available_at_tick != 0
            && w->tick >= s->available_at_tick)
        {
            s->state = PICKUP_STATE_COOLDOWN;
            s->available_at_tick = (uint64_t)-1;
            enqueue_pickup_event(w, i);
            SHOT_LOG("t=%llu pickup %d transient_expired (kind=%d)",
                     (unsigned long long)w->tick, i, (int)s->kind);
            continue;
        }
        for (int mi = 0; mi < w->mech_count; ++mi) {
            Mech *m = &w->mechs[mi];
            if (!m->alive || m->is_dummy) continue;
            if (!pickup_mech_touches(w, m, s->pos)) continue;
            if (!apply_pickup(w, mi, s)) continue;

            if (s->flags & PICKUP_FLAG_TRANSIENT) {
                s->state = PICKUP_STATE_COOLDOWN;
                s->available_at_tick = (uint64_t)-1;
            } else {
                s->state = PICKUP_STATE_COOLDOWN;
                int respawn_ms = s->respawn_ms ? s->respawn_ms
                                               : pickup_default_respawn_ms(s->kind, s->variant);
                /* 60 Hz sim → ms × 0.06 = ticks. Round to nearest. */
                s->available_at_tick = w->tick +
                    (uint64_t)((respawn_ms * 60 + 500) / 1000);
            }
            enqueue_pickup_event(w, i);
            /* Tactile feedback: a small spark burst at the grab point.
             * Per `documents/m5/04-pickups.md`, there's no HUD flash at
             * v1 — the bar moves and the sprite vanishes. The sparks
             * give the player a confirmation that's locality-correct
             * (it pops where they grabbed it, not on the HUD). */
            for (int k = 0; k < 12; ++k) {
                fx_spawn_spark(&w->fx, s->pos,
                               (Vec2){0.0f, -40.0f}, w->rng);
            }
            SHOT_LOG("t=%llu pickup %d grabbed by mech=%d (kind=%d)",
                     (unsigned long long)w->tick, i, mi, (int)s->kind);
            break;     /* one grab per spawner per tick */
        }
    }
}
