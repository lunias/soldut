#pragma once

#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Pickups (M5 P05).
 *
 * Map-driven equipment-swap layer per `documents/m5/04-pickups.md`. The
 * `LvlPickup` records loaded from the .lvl describe spawners; the runtime
 * keeps one `PickupSpawner` per record in `World.pickups`, toggling each
 * between AVAILABLE and COOLDOWN as mechs touch them. Engineer-deployed
 * repair packs and any future dynamic spawns reuse the same pool with
 * `PICKUP_FLAG_TRANSIENT` set so they auto-remove on grab or lifetime
 * expiry instead of cycling through COOLDOWN.
 *
 * Server-authoritative: only `world.authoritative=true` paths transition
 * state. The client mirrors transitions via NET_MSG_PICKUP_STATE.
 */

/* Populate the spawner pool from `world->level.pickups`. On the
 * authoritative side, also spawns a practice-dummy mech for any
 * PICKUP_PRACTICE_DUMMY spawner (and immediately marks the spawner
 * consumed so it never appears as a pickup). Pure clients rely on the
 * snapshot stream + SNAP_STATE_IS_DUMMY for dummy mechs, so they only
 * populate the spawner pool here.
 *
 * Resets `world.pickups.count = 0` first; safe to call once per round. */
void pickup_init_round(World *w);

/* Allocate a new transient spawner. Returns the pool index, or -1 if
 * the pool is full. Sets `PICKUP_FLAG_TRANSIENT` so step() removes the
 * entry on grab or when `tick >= available_at_tick` (used as lifetime
 * expiry rather than respawn timer). */
int  pickup_spawn_transient(World *w, PickupSpawner s);

/* Per-tick state machine + touch detection. Server-only. Drains the
 * AVAILABLE→COOLDOWN transition (on mech contact) and COOLDOWN→AVAILABLE
 * transition (on tick rollover). Broadcasts each transition via
 * NET_MSG_PICKUP_STATE so clients mirror state. Transients self-remove
 * on grab or lifetime expiry. */
void pickup_step(World *w, float dt);

/* Default respawn time in milliseconds for a (kind, variant) pair. The
 * spawner's `respawn_ms` overrides this when non-zero. Returns 0 for
 * kinds that don't respawn (REPAIR_PACK, PRACTICE_DUMMY). */
int  pickup_default_respawn_ms(uint8_t kind, uint8_t variant);

/* Render-side helper — placeholder color until the P13 sprite atlas
 * lands. Returns RGBA8 (raylib Color packing). */
uint32_t pickup_kind_color(uint8_t kind);
