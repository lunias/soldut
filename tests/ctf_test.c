/*
 * M5 P07 — CTF runtime regression test.
 *
 * Asserts and returns non-zero on failure (CI-runnable, unlike
 * headless_sim which is human-read).
 *
 * Coverage:
 *   1.  ctf_init_round with mode=CTF and a Red/Blue flag pair populates
 *       world.flags[]; flags[0]=RED, flags[1]=BLUE, both HOME, carrier=-1.
 *   2.  ctf_init_round with mode!=CTF zeroes flag_count.
 *   3.  ctf_init_round with mode=CTF but a single flag fails out
 *       (flag_count stays 0; warning logged).
 *   4.  ctf_flag_position returns home_pos for HOME, dropped_pos for
 *       DROPPED, carrier chest for CARRIED.
 *   5.  ctf_step pickup: enemy-team mech inside touch radius takes a
 *       HOME flag. Status → CARRIED, carrier_mech set, dirty bit set.
 *   6.  ctf_step doesn't pick up own-team flag (no-op).
 *   7.  ctf_drop_on_death drops the carried flag at the death position
 *       with FLAG_AUTO_RETURN_TICKS pending. Sets dirty bit.
 *   8.  ctf_step return-by-friend: same-team mech touching DROPPED flag
 *       returns it instantly (status → HOME, +1 to slot score).
 *   9.  ctf_step auto-return: tick advances past return_at_tick →
 *       status → HOME without a returner; no score awarded.
 *  10.  ctf_step capture: enemy carrier touches own home flag while
 *       carrying. team_score += 5; slot.score += 1; carried flag
 *       sent home; both flags HOME.
 *  11.  ctf_is_carrier returns true for the carrier, false otherwise.
 *  12.  Carrier flag is RESPECTED: when both flags are home and the
 *       same-team mech touches their own home flag WITHOUT carrying
 *       the enemy flag, no capture (no scoring).
 */

#define _POSIX_C_SOURCE 200809L

#include "../src/arena.h"
#include "../src/ctf.h"
#include "../src/game.h"
#include "../src/level.h"
#include "../src/lobby.h"
#include "../src/log.h"
#include "../src/match.h"
#include "../src/mech.h"
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

/* Reset world + lobby + level, then author a level with a Red/Blue flag
 * pair at known positions on a flat floor. The mech_chest_pos call in
 * ctf_step needs particle_base populated, so we also build out the
 * skeleton via mech_create. */
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
    g->world.flag_count        = 0;
    g->world.flag_state_dirty  = false;
    g->world.match_mode_cached = 0;
    g->world.hit_pause_ticks   = 0;
    memset(g->world.flags, 0, sizeof g->world.flags);
    memset(g->world.mechs, 0, sizeof g->world.mechs);
    if (g->world.particles.flags) {
        memset(g->world.particles.flags, 0, (size_t)g->world.particles.capacity);
    }
    level_build_tutorial(&g->world.level, &g->level_arena);
    /* Attach two LvlFlag records to the level. Position chosen so the
     * pelvis spawn lane (~960 floor y, varied x) leaves the flags well
     * inside the play area but apart from each other. */
    g->world.level.flags = (LvlFlag *)arena_alloc(&g->level_arena, sizeof(LvlFlag) * 2);
    g->world.level.flags[0] = (LvlFlag){
        .pos_x = 600, .pos_y = 980,
        .team  = MATCH_TEAM_RED,
    };
    g->world.level.flags[1] = (LvlFlag){
        .pos_x = 2400, .pos_y = 980,
        .team  = MATCH_TEAM_BLUE,
    };
    g->world.level.flag_count = 2;
    /* Lobby scaffolding for slot scoring + chat hooks. Reuse the seat
     * helpers from lobby.c. */
    memset(&g->lobby, 0, sizeof g->lobby);
    lobby_init(&g->lobby, 60.0f);
}

/* Helper: place mech `mid`'s chest at `(x, y)` so ctf_step's
 * mech_chest_pos sees it inside touch radius. We move the entire body
 * by deltaing every particle from the current chest position. */
static void teleport_mech_chest(Game *g, int mid, float x, float y) {
    Mech *m = &g->world.mechs[mid];
    Vec2 chest = mech_chest_pos(&g->world, mid);
    float dx = x - chest.x;
    float dy = y - chest.y;
    int b = m->particle_base;
    for (int part = 0; part < PART_COUNT; ++part) {
        g->world.particles.pos_x [b + part] += dx;
        g->world.particles.pos_y [b + part] += dy;
        g->world.particles.prev_x[b + part] += dx;
        g->world.particles.prev_y[b + part] += dy;
    }
}

/* Helper: seat a mech as a lobby slot too so lobby_find_slot_by_mech
 * resolves correctly during scoring. */
static int seat_mech(Game *g, int chassis, Vec2 pos, int team) {
    int mid = mech_create(&g->world, chassis, pos, team, false);
    if (mid < 0) return -1;
    int slot = lobby_add_slot(&g->lobby, /*peer_id*/-1, "p", false);
    if (slot < 0) return -1;
    g->lobby.slots[slot].team    = team;
    g->lobby.slots[slot].mech_id = mid;
    return mid;
}

/* ---- Test 1+2+3: ctf_init_round modes ---------------------------- */

static void test_init_round(Game *g) {
    reset_world(g);
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    EXPECT(g->world.flag_count == 2, "init_round CTF: flag_count=2");
    EXPECT(g->world.flags[0].team == MATCH_TEAM_RED,  "init_round: flags[0]=RED");
    EXPECT(g->world.flags[1].team == MATCH_TEAM_BLUE, "init_round: flags[1]=BLUE");
    EXPECT(g->world.flags[0].status == FLAG_HOME, "init_round: RED status HOME");
    EXPECT(g->world.flags[1].status == FLAG_HOME, "init_round: BLUE status HOME");
    EXPECT(g->world.flags[0].carrier_mech == -1, "init_round: RED carrier -1");
    EXPECT(g->world.flags[1].carrier_mech == -1, "init_round: BLUE carrier -1");
    EXPECT_NEAR(g->world.flags[0].home_pos.x, 600.0f, 0.5f, "init_round: RED home_x");
    EXPECT_NEAR(g->world.flags[0].home_pos.y, 980.0f, 0.5f, "init_round: RED home_y");
    EXPECT_NEAR(g->world.flags[1].home_pos.x, 2400.0f, 0.5f, "init_round: BLUE home_x");

    /* TDM mode → no flags */
    reset_world(g);
    ctf_init_round(&g->world, MATCH_MODE_TDM);
    EXPECT(g->world.flag_count == 0, "init_round TDM: flag_count=0");

    /* CTF mode but only one flag → bail */
    reset_world(g);
    g->world.level.flag_count = 1;
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    EXPECT(g->world.flag_count == 0, "init_round CTF w/ 1 flag: flag_count=0");
}

/* ---- Test 4: ctf_flag_position ----------------------------------- */

static void test_flag_position(Game *g) {
    reset_world(g);
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    Vec2 p0 = ctf_flag_position(&g->world, 0);
    EXPECT_NEAR(p0.x, 600.0f, 0.5f, "flag_position HOME: x = home_x");
    EXPECT_NEAR(p0.y, 980.0f, 0.5f, "flag_position HOME: y = home_y");

    g->world.flags[0].status      = FLAG_DROPPED;
    g->world.flags[0].dropped_pos = (Vec2){123.0f, 456.0f};
    Vec2 p1 = ctf_flag_position(&g->world, 0);
    EXPECT_NEAR(p1.x, 123.0f, 0.5f, "flag_position DROPPED: x = dropped_x");
    EXPECT_NEAR(p1.y, 456.0f, 0.5f, "flag_position DROPPED: y = dropped_y");
}

/* ---- Test 5: enemy touches HOME flag → pickup -------------------- */

static void test_enemy_pickup(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    /* Blue mech (team 2) goes for the Red flag at (600, 980). */
    int blue = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_BLUE);
    EXPECT(blue >= 0, "seat_mech: BLUE id valid");
    teleport_mech_chest(g, blue, 600.0f, 980.0f);
    g->world.flag_state_dirty = false;

    ctf_step(g, 1.0f/60.0f);

    EXPECT(g->world.flags[0].status == FLAG_CARRIED,
           "enemy touch HOME → CARRIED");
    EXPECT((int)g->world.flags[0].carrier_mech == blue,
           "carrier_mech == blue mech id");
    EXPECT(g->world.flag_state_dirty, "flag_state_dirty set on pickup");
}

/* ---- Test 6: same-team mech touching HOME flag → no-op ----------- */

static void test_friendly_no_pickup(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    int red = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_RED);
    EXPECT(red >= 0, "seat_mech: RED id valid");
    teleport_mech_chest(g, red, 600.0f, 980.0f);
    g->world.flag_state_dirty = false;

    ctf_step(g, 1.0f/60.0f);

    EXPECT(g->world.flags[0].status == FLAG_HOME,
           "friendly touch HOME (no enemy flag) → no-op");
    EXPECT(g->world.flags[0].carrier_mech == -1, "carrier still -1");
    EXPECT(!g->world.flag_state_dirty, "no dirty bit set on no-op");
}

/* ---- Test 7: carrier dies → flag drops --------------------------- */

static void test_carrier_dies_drop(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    g->world.match_mode_cached = (int)MATCH_MODE_CTF;
    ctf_init_round(&g->world, MATCH_MODE_CTF);

    /* Pre-place a Blue carrier holding the Red flag. */
    int blue = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_BLUE);
    g->world.flags[0].status        = FLAG_CARRIED;
    g->world.flags[0].carrier_mech  = (int8_t)blue;
    g->world.flags[0].return_at_tick= 0;
    g->world.flag_state_dirty       = false;

    Vec2 death_pos = (Vec2){ 1234.0f, 567.0f };
    ctf_drop_on_death(&g->world, MATCH_MODE_CTF, blue, death_pos);

    EXPECT(g->world.flags[0].status == FLAG_DROPPED,
           "carrier-dies: status → DROPPED");
    EXPECT(g->world.flags[0].carrier_mech == -1,
           "carrier-dies: carrier_mech reset to -1");
    EXPECT_NEAR(g->world.flags[0].dropped_pos.x, 1234.0f, 0.5f,
                "carrier-dies: dropped_pos.x");
    EXPECT_NEAR(g->world.flags[0].dropped_pos.y,  567.0f, 0.5f,
                "carrier-dies: dropped_pos.y");
    EXPECT(g->world.flags[0].return_at_tick ==
           (uint64_t)FLAG_AUTO_RETURN_TICKS,
           "carrier-dies: return_at_tick = tick + 1800");
    EXPECT(g->world.flag_state_dirty, "carrier-dies: dirty bit set");

    /* Non-CTF mode → no-op */
    g->world.flags[0].status      = FLAG_CARRIED;
    g->world.flags[0].carrier_mech= (int8_t)blue;
    g->world.flag_state_dirty     = false;
    ctf_drop_on_death(&g->world, MATCH_MODE_TDM, blue, death_pos);
    EXPECT(g->world.flags[0].status == FLAG_CARRIED,
           "non-CTF mode → drop no-op");
}

/* ---- Test 8: friendly returns DROPPED flag instantly ------------- */

static void test_friendly_return(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    int red = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_RED);
    int red_slot = lobby_find_slot_by_mech(&g->lobby, red);
    EXPECT(red_slot >= 0, "red slot resolved");
    int score_before = g->lobby.slots[red_slot].score;

    /* Drop the Red flag somewhere; teleport the Red mech onto it. */
    g->world.flags[0].status        = FLAG_DROPPED;
    g->world.flags[0].dropped_pos   = (Vec2){ 1500.0f, 980.0f };
    g->world.flags[0].carrier_mech  = -1;
    g->world.flags[0].return_at_tick= g->world.tick + 600;
    teleport_mech_chest(g, red, 1500.0f, 980.0f);
    g->world.flag_state_dirty       = false;

    ctf_step(g, 1.0f/60.0f);

    EXPECT(g->world.flags[0].status == FLAG_HOME,
           "friendly-return: DROPPED → HOME");
    EXPECT(g->world.flags[0].carrier_mech == -1,
           "friendly-return: carrier still -1");
    EXPECT(g->lobby.slots[red_slot].score == score_before + 1,
           "friendly-return: returner +1 score");
    EXPECT(g->world.flag_state_dirty, "friendly-return: dirty bit set");
}

/* ---- Test 9: auto-return on timer -------------------------------- */

static void test_auto_return(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    ctf_init_round(&g->world, MATCH_MODE_CTF);

    /* Place the dropped flag far from any mech so touch never fires. */
    g->world.flags[1].status        = FLAG_DROPPED;
    g->world.flags[1].dropped_pos   = (Vec2){ 3000.0f, 980.0f };
    g->world.flags[1].carrier_mech  = -1;
    g->world.flags[1].return_at_tick= 100;
    g->world.tick = 50;
    g->world.flag_state_dirty = false;

    ctf_step(g, 1.0f/60.0f);
    EXPECT(g->world.flags[1].status == FLAG_DROPPED,
           "auto-return: still DROPPED before timer");

    g->world.tick = 100;
    ctf_step(g, 1.0f/60.0f);
    EXPECT(g->world.flags[1].status == FLAG_HOME,
           "auto-return: tick == return_at_tick → HOME");
    EXPECT(g->world.flag_state_dirty, "auto-return: dirty bit set");
}

/* ---- Test 10: capture (both-flags-home rule) --------------------- */

static void test_capture(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    g->world.match_mode_cached = (int)MATCH_MODE_CTF;
    ctf_init_round(&g->world, MATCH_MODE_CTF);

    /* Blue mech carrying the Red flag, walking into the Blue home. */
    int blue = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_BLUE);
    int blue_slot = lobby_find_slot_by_mech(&g->lobby, blue);
    EXPECT(blue_slot >= 0, "blue slot resolved");

    g->world.flags[0].status      = FLAG_CARRIED;
    g->world.flags[0].carrier_mech= (int8_t)blue;
    /* flags[1] (Blue home) stays HOME (precondition for capture). */

    /* Park the Blue mech on the Blue home flag. */
    teleport_mech_chest(g, blue, 2400.0f, 980.0f);
    g->world.flag_state_dirty = false;
    int score_before     = g->lobby.slots[blue_slot].score;
    int team_score_before= g->match.team_score[MATCH_TEAM_BLUE];

    ctf_step(g, 1.0f/60.0f);

    EXPECT(g->world.flags[0].status == FLAG_HOME,
           "capture: Red flag returned HOME");
    EXPECT(g->world.flags[0].carrier_mech == -1,
           "capture: Red carrier reset to -1");
    EXPECT(g->world.flags[1].status == FLAG_HOME,
           "capture: Blue flag still HOME");
    EXPECT(g->match.team_score[MATCH_TEAM_BLUE] == team_score_before + 5,
           "capture: team_score += 5");
    EXPECT(g->lobby.slots[blue_slot].score == score_before + 1,
           "capture: scorer slot.score += 1");
    EXPECT(g->world.flag_state_dirty, "capture: dirty bit set");
}

/* ---- Test 11: ctf_is_carrier ------------------------------------- */

static void test_is_carrier(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    int blue = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_BLUE);
    int red  = seat_mech(g, CHASSIS_TROOPER, (Vec2){810, 920}, MATCH_TEAM_RED);
    EXPECT(!ctf_is_carrier(&g->world, blue), "is_carrier false initially (blue)");
    EXPECT(!ctf_is_carrier(&g->world, red),  "is_carrier false initially (red)");
    g->world.flags[0].status      = FLAG_CARRIED;
    g->world.flags[0].carrier_mech= (int8_t)blue;
    EXPECT( ctf_is_carrier(&g->world, blue), "is_carrier true after CARRIED");
    EXPECT(!ctf_is_carrier(&g->world, red),  "is_carrier false for non-carrier");
}

/* ---- Test 12: capture requires carrying enemy flag --------------- */

static void test_capture_requires_carry(Game *g) {
    reset_world(g);
    g->match.mode  = MATCH_MODE_CTF;
    g->match.phase = MATCH_PHASE_ACTIVE;
    g->world.match_mode_cached = (int)MATCH_MODE_CTF;
    ctf_init_round(&g->world, MATCH_MODE_CTF);
    int blue = seat_mech(g, CHASSIS_TROOPER, (Vec2){800, 920}, MATCH_TEAM_BLUE);
    int blue_slot = lobby_find_slot_by_mech(&g->lobby, blue);
    teleport_mech_chest(g, blue, 2400.0f, 980.0f);   /* on Blue home */
    g->world.flag_state_dirty = false;
    int team_score_before = g->match.team_score[MATCH_TEAM_BLUE];

    ctf_step(g, 1.0f/60.0f);

    EXPECT(g->match.team_score[MATCH_TEAM_BLUE] == team_score_before,
           "no-carry touch: team_score unchanged");
    EXPECT(!g->world.flag_state_dirty,
           "no-carry touch: no dirty bit");
    (void)blue_slot;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    log_init("");

    Game game;
    if (!game_init(&game)) { fprintf(stderr, "game_init failed\n"); return 1; }

    test_init_round         (&game);
    test_flag_position      (&game);
    test_enemy_pickup       (&game);
    test_friendly_no_pickup (&game);
    test_carrier_dies_drop  (&game);
    test_friendly_return    (&game);
    test_auto_return        (&game);
    test_capture            (&game);
    test_is_carrier         (&game);
    test_capture_requires_carry(&game);

    fprintf(stdout, "\nctf_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
