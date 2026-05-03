#include "decal.h"
#include "game.h"
#include "level.h"
#include "log.h"
#include "mech.h"
#include "platform.h"
#include "render.h"
#include "simulate.h"
#include "version.h"
#include "weapons.h"

#include "../third_party/raylib/src/raylib.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * M1 entry point.
 *
 *   - Build the tutorial level.
 *   - Spawn a player mech and a target dummy.
 *   - Run a fixed-step 60 Hz simulate inside an accumulator loop.
 *   - Render at the display's vsync rate.
 */

#define SIM_HZ        60
static const double TICK_DT = 1.0 / (double)SIM_HZ;
#define MAX_FRAME_DT  0.25

static void seed_world(Game *g) {
    World *w = &g->world;

    /* Build the tutorial map (its arena memory must outlive the World). */
    level_build_tutorial(&w->level, &g->level_arena);

    /* Splat layer covers the entire level. */
    decal_init((int)level_width_px(&w->level),
               (int)level_height_px(&w->level));

    /* Spawn the player so its feet rest 4 px (= particle radius) above
     * the platform top. With the foot's particle center at floor−r the
     * collision distance equals r, so there's no first-tick overlap and
     * the body doesn't have to settle through the constraint solver
     * before play feels coherent. */
    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;
    Vec2 player_spawn = { 16.0f * 32.0f + 8.0f,
                          30.0f * 32.0f - feet_below_pelvis - foot_clearance };
    w->local_mech_id = mech_create(w, CHASSIS_TROOPER, player_spawn,
                                   /*team*/ 1, /*is_dummy*/ false);

    /* Dummy on the right platform (row 32 → y=1024). */
    Vec2 dummy_spawn = { 75.0f * 32.0f,
                         32.0f * 32.0f - feet_below_pelvis - foot_clearance };
    w->dummy_mech_id = mech_create(w, CHASSIS_TROOPER, dummy_spawn,
                                   /*team*/ 2, /*is_dummy*/ true);

    /* Camera initial focus. */
    w->camera_target = player_spawn;
    w->camera_smooth = player_spawn;
    w->camera_zoom   = 1.4f;
}

static void draw_diag(void *user, int sw, int sh) {
    (void)sw;
    const Game *g = (const Game *)user;
    int fps = GetFPS();
    Color st = (fps >= 55) ? GREEN : (fps >= 30) ? YELLOW : RED;
    DrawText("soldut " SOLDUT_VERSION_STRING, 12, 10, 18, RAYWHITE);
    DrawText(TextFormat("FPS %d", fps), 12, 32, 18, st);
    DrawText(TextFormat("tick %llu  mechs %d  particles %d",
                        (unsigned long long)g->world.tick,
                        g->world.mech_count,
                        g->world.particles.count),
             12, 52, 14, LIGHTGRAY);
    DrawText("WASD: move/jet  SPACE: jump  LMB: fire",
             12, sh - 22, 14, GRAY);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    log_init("soldut.log");
    LOG_I("soldut " SOLDUT_VERSION_STRING " (M1) starting");

    Game game;
    if (!game_init(&game)) { log_shutdown(); return EXIT_FAILURE; }

    PlatformConfig pcfg = {
        .window_w = 1280, .window_h = 720,
        .vsync = true, .fullscreen = false,
        .title = "Soldut " SOLDUT_VERSION_STRING " — M1: one mech",
    };
    if (!platform_init(&pcfg)) {
        game_shutdown(&game); log_shutdown(); return EXIT_FAILURE;
    }

    seed_world(&game);

    Renderer rd;
    renderer_init(&rd,
        GetScreenWidth(), GetScreenHeight(),
        mech_chest_pos(&game.world, game.world.local_mech_id));

    PlatformFrame pf = {0};
    double accum = 0.0;
    double last  = GetTime();

    while (!WindowShouldClose()) {
        double now = GetTime();
        double dt  = now - last;
        last = now;
        if (dt > MAX_FRAME_DT) dt = MAX_FRAME_DT;
        accum += dt;

        platform_begin_frame(&pf);

        /* Fixed-step simulation. We sample input each tick and feed it
         * through simulate(); the platform layer keeps the most recent
         * cursor pos for renderer use. */
        while (accum >= TICK_DT) {
            ClientInput in;
            platform_sample_input(&in);
            in.dt = (float)TICK_DT;
            in.seq = (uint16_t)(game.world.tick + 1);

            /* Convert cursor screen-space to world-space using the *current*
             * camera. Then write it onto the local mech as aim_world. */
            Vec2 cursor_world = renderer_screen_to_world(&rd,
                (Vec2){ in.aim_x, in.aim_y });
            if (game.world.local_mech_id >= 0) {
                game.world.mechs[game.world.local_mech_id].aim_world = cursor_world;
            }

            simulate(&game.world, in, (float)TICK_DT);
            game.input = in;
            accum -= TICK_DT;
        }

        renderer_draw_frame(&rd, &game.world,
                            pf.render_w, pf.render_h,
                            (float)(accum / TICK_DT),
                            (Vec2){ (float)GetMouseX(), (float)GetMouseY() },
                            draw_diag, &game);
    }

    LOG_I("soldut shutting down (ran %llu sim ticks)",
          (unsigned long long)game.world.tick);
    decal_shutdown();
    platform_shutdown();
    game_shutdown(&game);
    log_shutdown();
    return EXIT_SUCCESS;
}
