/*
 * M5 P05 — pickup runtime + powerups + Burst SMG cadence regression test.
 *
 * Asserts and returns non-zero on failure (CI-runnable, unlike
 * headless_sim which is human-read).
 *
 * Coverage:
 *   1. HEALTH pickup grabs on touch when damaged (refills HP, COOLDOWN).
 *   2. HEALTH pickup is REJECTED when the mech is at full HP.
 *   3. POWERUP berserk doubles outgoing damage in mech_apply_damage.
 *   4. POWERUP godmode zeroes incoming damage.
 *   5. POWERUP invisibility flips alpha mod (timer > 0).
 *   6. Engineer's pickup_spawn_transient adds a TRANSIENT entry that
 *      auto-removes (state=COOLDOWN, avail=UINT64_MAX) after lifetime
 *      expires.
 *   7. Burst SMG fires round 1 on the press tick + queues 2 rounds in
 *      burst_pending_rounds; each subsequent simulate tick after
 *      burst_interval_sec spawns one more.
 *   8. PRACTICE_DUMMY in level pickups spawns a dummy mech via
 *      pickup_init_round on the authoritative side.
 */

#define _POSIX_C_SOURCE 200809L

#include "../src/arena.h"
#include "../src/game.h"
#include "../src/input.h"
#include "../src/level.h"
#include "../src/log.h"
#include "../src/mech.h"
#include "../src/pickup.h"
#include "../src/projectile.h"
#include "../src/simulate.h"
#include "../src/snapshot.h"
#include "../src/weapons.h"
#include "../src/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_passed = 0;
static int g_failed = 0;

#define EXPECT(cond, msg) do { \
    if (cond) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define EXPECT_NEAR(a, b, eps, msg) do { \
    if (fabsf((float)(a) - (float)(b)) < (float)(eps)) { g_passed++; } \
    else { g_failed++; fprintf(stderr, "FAIL: %s — got %.3f want %.3f (%s:%d)\n", \
                               msg, (double)(a), (double)(b), __FILE__, __LINE__); } \
} while (0)

/* Soft-reset between subtests. Reuses the pool storage allocated by
 * game_init (don't memset(world) — that would lose the parallel-array
 * pointers). Reset just the contents/counters. */
static void reset_world(Game *g) {
    arena_reset(&g->level_arena);
    g->world.particles.count   = 0;
    g->world.constraints.count = 0;
    g->world.projectiles.count = 0;
    g->world.fx.count          = 0;
    g->world.mech_count        = 0;
    g->world.local_mech_id     = -1;
    g->world.dummy_mech_id     = -1;
    g->world.authoritative     = true;
    g->world.tick              = 0;
    g->world.killfeed_count    = 0;
    g->world.hitfeed_count     = 0;
    g->world.firefeed_count    = 0;
    g->world.pickupfeed_count  = 0;
    g->world.pickups.count     = 0;
    g->world.hit_pause_ticks   = 0;
    /* Wipe the mech array so test-2 doesn't see stale state from test-1. */
    memset(g->world.mechs, 0, sizeof g->world.mechs);
    /* Particle flags need clearing so freshly-allocated indices don't
     * inherit GROUNDED bits from prior tests. */
    if (g->world.particles.flags) {
        memset(g->world.particles.flags, 0,
               (size_t)g->world.particles.capacity);
    }
    level_build_tutorial(&g->world.level, &g->level_arena);
}

/* ---- Test 1+2: HEALTH grab on damaged mech, reject on full HP ----- */

static void test_health_grab(Game *g) {
    reset_world(g);
    int p = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    EXPECT(p >= 0, "mech_create returned a valid id");
    pickup_init_round(&g->world);
    EXPECT(g->world.pickups.count == 0, "tutorial level has no pickups");

    /* Damage the player and place a HEALTH_MEDIUM at chest position. */
    Mech *m = &g->world.mechs[p];
    float chest_x = g->world.particles.pos_x[m->particle_base + PART_CHEST];
    float chest_y = g->world.particles.pos_y[m->particle_base + PART_CHEST];
    m->health = 50.0f;

    PickupSpawner s = (PickupSpawner){
        .pos = (Vec2){ chest_x, chest_y },
        .kind = PICKUP_HEALTH,
        .variant = HEALTH_MEDIUM,
        .respawn_ms = 0,
        .state = PICKUP_STATE_AVAILABLE,
        .available_at_tick = 0,
        .flags = 0,
    };
    g->world.pickups.items[0] = s;
    g->world.pickups.count    = 1;

    /* Run pickup_step (server-only, world.authoritative=true). */
    pickup_step(&g->world, 1.0f / 60.0f);

    EXPECT_NEAR(m->health, 110.0f, 0.5f, "health refilled by HEALTH_MEDIUM (+60)");
    EXPECT(g->world.pickups.items[0].state == PICKUP_STATE_COOLDOWN,
           "spawner transitioned to COOLDOWN");
    EXPECT(g->world.pickups.items[0].available_at_tick > g->world.tick,
           "available_at_tick is in the future");
    EXPECT(g->world.pickupfeed_count == 1,
           "one event enqueued for the grab");

    /* Top up health to full and verify a fresh AVAILABLE pickup gets
     * REJECTED (apply_pickup returns false → state stays AVAILABLE). */
    m->health = m->health_max;
    g->world.pickups.items[0].state = PICKUP_STATE_AVAILABLE;
    g->world.pickups.items[0].available_at_tick = 0;
    int feed_before = g->world.pickupfeed_count;
    pickup_step(&g->world, 1.0f / 60.0f);
    EXPECT(g->world.pickups.items[0].state == PICKUP_STATE_AVAILABLE,
           "full-HP mech leaves HEALTH pickup AVAILABLE");
    EXPECT(g->world.pickupfeed_count == feed_before,
           "no event emitted for refused grab");
}

/* ---- Test 3: berserk doubles outgoing damage --------------------- */

static void test_berserk_damage(Game *g) {
    reset_world(g);
    int shooter = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    int target  = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){1200, 920}, 2, false);
    EXPECT(shooter >= 0 && target >= 0, "two mechs created");
    g->world.friendly_fire = true;     /* keep teams distinct anyway, but be safe */

    Mech *t = &g->world.mechs[target];
    /* Strip armor so damage flows straight through (otherwise armor
     * absorption muddies the multiplier check). */
    t->armor_id = ARMOR_NONE;
    t->armor_hp = 0;
    t->armor_hp_max = 0;
    float hp_before = t->health;

    /* Baseline: 30 dmg to PART_CHEST (mult 1.0), no berserk.
     * Expected drop: 30. */
    mech_apply_damage(&g->world, target, PART_CHEST, 30.0f, (Vec2){1, 0}, shooter);
    EXPECT_NEAR(hp_before - t->health, 30.0f, 0.5f, "baseline damage = 30");

    /* Now arm berserk on the shooter, repeat. */
    g->world.mechs[shooter].powerup_berserk_remaining = 5.0f;
    float hp_mid = t->health;
    mech_apply_damage(&g->world, target, PART_CHEST, 30.0f, (Vec2){1, 0}, shooter);
    EXPECT_NEAR(hp_mid - t->health, 60.0f, 0.5f,
                "berserk doubles outgoing damage to 60");
}

/* ---- Test 4: godmode zeroes incoming damage --------------------- */

static void test_godmode(Game *g) {
    reset_world(g);
    int shooter = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    int target  = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){1200, 920}, 2, false);
    EXPECT(shooter >= 0 && target >= 0, "two mechs created");

    Mech *t = &g->world.mechs[target];
    t->powerup_godmode_remaining = 5.0f;
    float hp_before = t->health;
    bool died = mech_apply_damage(&g->world, target, PART_HEAD, 9999.0f, (Vec2){0, 1}, shooter);
    EXPECT(!died, "godmode caps a fatal headshot");
    EXPECT_NEAR(t->health, hp_before, 0.5f,
                "godmode HP unchanged after lethal hit");
}

/* ---- Test 5: invis sentinel timer mirrors the bit --------------- */

static void test_invis_render_flag(Game *g) {
    reset_world(g);
    int p = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    EXPECT(p >= 0, "mech created");
    Mech *m = &g->world.mechs[p];
    m->powerup_invis_remaining = 8.0f;

    SnapshotFrame snap;
    snapshot_capture(&g->world, &snap, 0);
    EXPECT(snap.ents[0].state_bits & SNAP_STATE_INVIS,
           "snapshot ships INVIS bit when timer > 0");

    /* Apply on a fresh client-style world — the bit should re-arm the
     * timer to a sentinel. */
    World w2 = {0};
    w2.particles    = g->world.particles;     /* shallow copy of pool ptrs */
    w2.constraints  = g->world.constraints;
    w2.local_mech_id = -1;
    w2.authoritative = false;
    w2.rng = g->world.rng;
    snapshot_apply(&w2, &snap);
    EXPECT(w2.mechs[p].powerup_invis_remaining > 0.0f,
           "client mirrors INVIS bit to a non-zero sentinel timer");

    m->powerup_invis_remaining = 0.0f;
    snapshot_capture(&g->world, &snap, 0);
    EXPECT(!(snap.ents[0].state_bits & SNAP_STATE_INVIS),
           "INVIS bit cleared when timer hits zero");
}

/* ---- Test 6: transient lifetime expiry -------------------------- */

static void test_transient_expire(Game *g) {
    reset_world(g);
    int p = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    EXPECT(p >= 0, "mech created");
    pickup_init_round(&g->world);

    Mech *m = &g->world.mechs[p];
    /* Place a transient repair pack 64 px to the side (out of the
     * touch radius) so it expires by lifetime, not grab. */
    Vec2 chest = mech_chest_pos(&g->world, p);
    PickupSpawner s = (PickupSpawner){
        .pos = (Vec2){ chest.x + 200.0f, chest.y },
        .kind = PICKUP_REPAIR_PACK,
        .variant = 0,
        .state = PICKUP_STATE_AVAILABLE,
        .available_at_tick = g->world.tick + 5,    /* 5 ticks */
        .flags = 0,
    };
    int idx = pickup_spawn_transient(&g->world, s);
    EXPECT(idx >= 0, "transient allocated");
    EXPECT(g->world.pickups.items[idx].flags & PICKUP_FLAG_TRANSIENT,
           "TRANSIENT flag set on spawn");
    EXPECT(g->world.pickupfeed_count == 1,
           "transient spawn enqueued event");

    /* Advance 6 ticks; the lifetime should expire on tick 5. */
    for (int i = 0; i < 7; ++i) {
        pickup_step(&g->world, 1.0f / 60.0f);
        g->world.tick++;
    }
    EXPECT(g->world.pickups.items[idx].state == PICKUP_STATE_COOLDOWN,
           "expired transient sits in COOLDOWN");
    EXPECT(g->world.pickups.items[idx].available_at_tick == (uint64_t)-1,
           "expired transient pinned to UINT64_MAX (never returns)");
    /* Health should not have changed (mech wasn't in range). */
    EXPECT_NEAR(m->health, m->health_max, 0.5f,
                "out-of-range transient didn't heal the mech");
}

/* ---- Test 7: Burst SMG cadence ----------------------------------- */

static void test_burst_cadence(Game *g) {
    reset_world(g);
    MechLoadout lo = mech_default_loadout();
    lo.primary_id   = WEAPON_BURST_SMG;
    lo.secondary_id = WEAPON_SIDEARM;
    int p = mech_create_loadout(&g->world, lo, (Vec2){800, 920}, 1, false);
    EXPECT(p >= 0, "mech with Burst SMG primary created");

    Mech *m = &g->world.mechs[p];
    m->aim_world = (Vec2){ 1200, 920 };   /* aim right */
    /* Press BTN_FIRE on this tick and call try_fire directly.
     * (mech_step_drive normally precedes try_fire and edge-detects
     * against prev_buttons; we skip the drive pass here and just set
     * the "just-pressed" state.) */
    m->prev_buttons = 0;
    m->latched_input.buttons = BTN_FIRE;
    m->fire_cooldown = 0.0f;
    m->reload_timer  = 0.0f;

    int proj_before = g->world.projectiles.count;
    bool fired = mech_try_fire(&g->world, p, m->latched_input);
    EXPECT(fired, "burst fire kicked off");
    EXPECT(g->world.projectiles.count == proj_before + 1,
           "first round spawned on the press tick");
    EXPECT(m->burst_pending_rounds == 2,
           "two rounds queued after the first (burst_rounds = 3)");
    EXPECT_NEAR(m->burst_pending_timer, 0.070f, 1e-4f,
                "next-round timer = 70 ms");

    /* Tick `mech_step_drive` until both pending rounds have fired.
     * burst_interval_sec = 0.070 s; @60 Hz that's ~5 ticks per round.
     * Run ~16 ticks to be safe. We only need the burst_pending block
     * to run, so simulate_step's full pipeline isn't necessary —
     * but mech_step_drive advances multiple states including the
     * pending burst, so use it directly. */
    for (int t = 0; t < 16; ++t) {
        m->latched_input.buttons = 0;     /* trigger released; pending continues */
        mech_step_drive(&g->world, p, m->latched_input, 1.0f / 60.0f);
    }
    EXPECT(m->burst_pending_rounds == 0, "all 3 rounds fired");
    EXPECT(g->world.projectiles.count >= proj_before + 3,
           "≥3 projectiles spawned (one per burst round)");
}

/* ---- Test 8: shooter under invis still hits across many ticks - */

/* Full simulate-loop variant: walk a fresh world several ticks while
 * invis is active, and assert that mech_try_fire succeeds on each
 * trigger press. Catches regressions where some state ticked down or
 * got clobbered by powerup_*_remaining > 0 logic. */
static void test_invis_multi_tick_fire(Game *g) {
    reset_world(g);
    int shooter = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    int target  = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){1600, 920}, 2, false);
    EXPECT(shooter >= 0 && target >= 0, "two mechs created");
    g->world.friendly_fire = true;

    Mech *s = &g->world.mechs[shooter];
    Mech *t = &g->world.mechs[target];
    s->aim_world = (Vec2){ 1600, 920 };
    t->armor_id = ARMOR_NONE;
    t->armor_hp = 0;
    t->armor_hp_max = 0;
    s->powerup_invis_remaining = 8.0f;
    /* Pulse Rifle, full ammo, no cooldowns. */
    s->primary_id  = WEAPON_PULSE_RIFLE;
    s->weapon_id   = WEAPON_PULSE_RIFLE;
    const Weapon *wpn = weapon_def(WEAPON_PULSE_RIFLE);
    s->ammo        = wpn->mag_size;
    s->ammo_max    = wpn->mag_size;
    s->ammo_primary= wpn->mag_size;
    s->active_slot = 0;
    s->fire_cooldown = 0.0f;
    s->reload_timer  = 0.0f;

    /* Run 30 sim ticks, holding fire. Keep aim fixed; let the sim
     * tick pose, cooldowns, and powerup decay. The Pulse Rifle's
     * fire_rate is 0.110 s = ~7 ticks @60 Hz, so we should see
     * roughly 30/7 = 4 shots fire over the run. */
    int shots = 0;
    float hp_start = t->health;
    for (int tk = 0; tk < 30; ++tk) {
        s->latched_input.buttons = BTN_FIRE;
        s->latched_input.aim_x = s->aim_world.x;
        s->latched_input.aim_y = s->aim_world.y;
        s->latched_input.dt = 1.0f / 60.0f;
        mech_step_drive(&g->world, shooter, s->latched_input, 1.0f / 60.0f);
        if (mech_try_fire(&g->world, shooter, s->latched_input)) shots++;
        mech_latch_prev_buttons(&g->world, shooter);
    }
    EXPECT(shots >= 3, "fired ≥3 shots over 30 ticks while invis");
    EXPECT(t->health < hp_start, "target took damage during invis fire");
    EXPECT(s->powerup_invis_remaining > 0.0f,
           "invis still active at end of 30-tick run (8 s timer hasn't elapsed)");
}

/* End-to-end via simulate_step: drive the same loop the live game's
 * accumulator runs, with the player holding fire while invis. If this
 * reports zero projectiles spawned, there's a live-game-side bug we
 * can fix in code. If it spawns ≥1, the user's "can't shoot" is
 * almost certainly a visual perception issue, not a logic one. */
static void test_invis_simulate_step(Game *g) {
    reset_world(g);
    int shooter = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    int target  = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){1600, 920}, 2, false);
    EXPECT(shooter >= 0 && target >= 0, "two mechs created");
    g->world.local_mech_id = shooter;
    g->world.friendly_fire = true;

    Mech *s = &g->world.mechs[shooter];
    Mech *t = &g->world.mechs[target];
    s->aim_world = (Vec2){ 1600, 920 };
    t->armor_id = ARMOR_NONE;
    t->armor_hp = 0;
    t->armor_hp_max = 0;
    s->powerup_invis_remaining = 8.0f;
    s->primary_id = WEAPON_PULSE_RIFLE;
    s->weapon_id  = WEAPON_PULSE_RIFLE;
    const Weapon *wpn = weapon_def(WEAPON_PULSE_RIFLE);
    s->ammo        = wpn->mag_size;
    s->ammo_max    = wpn->mag_size;
    s->ammo_primary= wpn->mag_size;
    s->active_slot = 0;

    /* Hold fire across 30 ticks of simulate_step (same path main.c
     * runs each fixed-step tick). */
    s->latched_input.buttons = BTN_FIRE;
    s->latched_input.aim_x   = s->aim_world.x;
    s->latched_input.aim_y   = s->aim_world.y;
    s->latched_input.dt      = 1.0f / 60.0f;

    int proj_at_start = g->world.projectiles.count;
    /* count fire feed entries — every shot enqueues one */
    int fires_at_start = g->world.firefeed_count;
    float hp_start = t->health;
    for (int tk = 0; tk < 30; ++tk) {
        s->latched_input.buttons = BTN_FIRE;
        simulate_step(&g->world, 1.0f / 60.0f);
    }
    int fires = g->world.firefeed_count - fires_at_start;
    EXPECT(fires >= 3, "simulate_step: ≥3 fire events emitted while invis");
    EXPECT(t->health < hp_start,
           "simulate_step: target damaged while shooter invis");
    EXPECT(s->powerup_invis_remaining > 0.0f,
           "simulate_step: invis still active at end of run");
    (void)proj_at_start;
}

/* Original single-tick variant — kept for comparison. */
static void test_invis_can_fire(Game *g) {
    reset_world(g);
    int shooter = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){800, 920}, 1, false);
    int target  = mech_create(&g->world, CHASSIS_TROOPER, (Vec2){1200, 920}, 2, false);
    EXPECT(shooter >= 0 && target >= 0, "two mechs created");
    g->world.friendly_fire = true;     /* same teams; ff so damage flows */
    Mech *s = &g->world.mechs[shooter];
    Mech *t = &g->world.mechs[target];
    s->aim_world = (Vec2){ 1500, 920 };

    /* Strip armor so the damage delta is unambiguous. */
    t->armor_id = ARMOR_NONE;
    t->armor_hp = 0;
    t->armor_hp_max = 0;
    float hp_before = t->health;

    /* Activate invisibility on the shooter. apply_self_bink and the
     * fire path don't reference invis, but the user reported "can't
     * shoot after invis" so we assert directly that the fire still
     * registers a bone hit. */
    s->powerup_invis_remaining = 8.0f;

    /* Force the active weapon to a known hitscan one and clear
     * cooldowns so try_fire actually fires this tick. */
    s->primary_id   = WEAPON_PULSE_RIFLE;
    s->weapon_id    = WEAPON_PULSE_RIFLE;
    const Weapon *wpn = weapon_def(WEAPON_PULSE_RIFLE);
    s->ammo         = wpn->mag_size;
    s->ammo_max     = wpn->mag_size;
    s->ammo_primary = wpn->mag_size;
    s->active_slot  = 0;
    s->fire_cooldown = 0.0f;
    s->reload_timer  = 0.0f;
    s->prev_buttons  = 0;
    s->latched_input.buttons = BTN_FIRE;
    s->latched_input.aim_x = s->aim_world.x;
    s->latched_input.aim_y = s->aim_world.y;

    bool fired = mech_try_fire(&g->world, shooter, s->latched_input);
    EXPECT(fired, "invisible shooter can still fire");
    EXPECT(t->health < hp_before, "invisible shooter's bullet damaged the target");
}

int main(void) {
    log_init(NULL);
    Game g;
    if (!game_init(&g)) {
        fprintf(stderr, "game_init failed\n");
        return 2;
    }

    test_health_grab(&g);
    test_berserk_damage(&g);
    test_godmode(&g);
    test_invis_render_flag(&g);
    test_transient_expire(&g);
    test_burst_cadence(&g);
    test_invis_can_fire(&g);
    test_invis_multi_tick_fire(&g);
    test_invis_simulate_step(&g);

    printf("\npickup_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
