/*
 * Headless physics tester. Runs simulate() against a real World without
 * opening a raylib window, and dumps particle positions across phases of
 * synthetic input so we can see what the body actually does.
 *
 * Build:    make test-physics
 * Run:      ./build/headless_sim
 *
 * The test exercises four scenarios:
 *   1. Spawn position
 *   2. Idle for 2 seconds — does the body fold under just gravity + pose?
 *   3. Hold right for 1 second — does the mech actually move?
 *   4. Release input — does it stop snappily?
 *   5. Hold jet — how fast does it climb?
 */

#define _POSIX_C_SOURCE 200809L

#include "../src/game.h"
#include "../src/input.h"
#include "../src/level.h"
#include "../src/log.h"
#include "../src/mech.h"
#include "../src/simulate.h"
#include "../src/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float seg_len(const ParticlePool *p, int a, int b) {
    float dx = p->pos_x[a] - p->pos_x[b];
    float dy = p->pos_y[a] - p->pos_y[b];
    return sqrtf(dx * dx + dy * dy);
}

static void dump(const World *w, int mid, const char *tag) {
    const Mech *m = &w->mechs[mid];
    int b = m->particle_base;
    const ParticlePool *p = &w->particles;
    printf("--- %-28s mech=%d alive=%d grounded=%d hp=%.0f anim=%d ---\n",
           tag, mid, m->alive, m->grounded, m->health, m->anim_id);
    printf("  PELVIS  %7.2f, %7.2f      vel (%5.2f, %5.2f)\n",
           p->pos_x[b + PART_PELVIS], p->pos_y[b + PART_PELVIS],
           p->pos_x[b + PART_PELVIS] - p->prev_x[b + PART_PELVIS],
           p->pos_y[b + PART_PELVIS] - p->prev_y[b + PART_PELVIS]);
    printf("  CHEST   %7.2f, %7.2f\n",
           p->pos_x[b + PART_CHEST], p->pos_y[b + PART_CHEST]);
    printf("  HEAD    %7.2f, %7.2f\n",
           p->pos_x[b + PART_HEAD], p->pos_y[b + PART_HEAD]);
    printf("  L_KNEE  %7.2f, %7.2f      L_FOOT %7.2f, %7.2f\n",
           p->pos_x[b + PART_L_KNEE], p->pos_y[b + PART_L_KNEE],
           p->pos_x[b + PART_L_FOOT], p->pos_y[b + PART_L_FOOT]);
    printf("  R_KNEE  %7.2f, %7.2f      R_FOOT %7.2f, %7.2f\n",
           p->pos_x[b + PART_R_KNEE], p->pos_y[b + PART_R_KNEE],
           p->pos_x[b + PART_R_FOOT], p->pos_y[b + PART_R_FOOT]);
    printf("  R_SHO   %7.2f, %7.2f      R_HAND %7.2f, %7.2f\n",
           p->pos_x[b + PART_R_SHOULDER], p->pos_y[b + PART_R_SHOULDER],
           p->pos_x[b + PART_R_HAND], p->pos_y[b + PART_R_HAND]);
    /* Bone-length sanity. */
    printf("  spine=%.2f (rest 30)   l_thigh=%.2f l_shin=%.2f (rest ~18)\n",
           seg_len(p, b + PART_CHEST, b + PART_PELVIS),
           seg_len(p, b + PART_L_HIP, b + PART_L_KNEE),
           seg_len(p, b + PART_L_KNEE, b + PART_L_FOOT));
    printf("  upright_check: chest_y < pelvis_y? %s   head_y < chest_y? %s\n",
           p->pos_y[b + PART_CHEST] < p->pos_y[b + PART_PELVIS] ? "yes" : "NO (folded!)",
           p->pos_y[b + PART_HEAD]  < p->pos_y[b + PART_CHEST]  ? "yes" : "NO (folded!)");
}

int main(void) {
    log_init(NULL);
    Game g;
    if (!game_init(&g)) {
        fprintf(stderr, "game_init failed\n");
        return 1;
    }

    level_build_tutorial(&g.world.level, &g.level_arena);

    g.world.local_mech_id = mech_create(&g.world, CHASSIS_TROOPER,
                                        (Vec2){520.0f, 920.0f}, 1, false);
    g.world.dummy_mech_id = mech_create(&g.world, CHASSIS_TROOPER,
                                        (Vec2){2400.0f, 984.0f}, 2, true);

    int player = g.world.local_mech_id;
    int dummy  = g.world.dummy_mech_id;

    g.world.mechs[player].aim_world = (Vec2){620.0f, 900.0f};
    /* Place the dummy's aim point far enough away that small body
     * movements don't flip the aim direction (which would yank the
     * R arm pose target around and pump lateral force into the body). */
    g.world.mechs[dummy ].aim_world = (Vec2){12400.0f, 1000.0f};

    printf("=========================================\n");
    printf("  PHASE 1 — Spawn\n");
    printf("=========================================\n");
    dump(&g.world, player, "spawn (player)");
    dump(&g.world, dummy,  "spawn (dummy)");

    ClientInput in = (ClientInput){0};
    in.dt = 1.0f / 60.0f;

    printf("\n=========================================\n");
    printf("  PHASE 2 — Idle 2s (gravity + pose only)\n");
    printf("=========================================\n");
    for (int t = 1; t <= 120; ++t) {
        simulate(&g.world, in, in.dt);
        if (t == 1 || t == 5 || t == 10 || t == 15 || t == 20 || t == 30 || t == 60 || t == 120) {
            char tag[48];
            snprintf(tag, sizeof(tag), "DUMMY t=%d", t);
            dump(&g.world, dummy, tag);
        }
    }

    printf("\n=========================================\n");
    printf("  PHASE 3 — Hold RIGHT 1s\n");
    printf("=========================================\n");
    in.buttons = BTN_RIGHT;
    /* Keep aim relative to the chest, otherwise it lags behind and the
     * mech turns to face left as the chest passes the original aim. */
    for (int t = 1; t <= 60; ++t) {
        const Mech *m = &g.world.mechs[player];
        int b = m->particle_base;
        g.world.mechs[player].aim_world = (Vec2){
            g.world.particles.pos_x[b + PART_CHEST] + 100.0f,
            g.world.particles.pos_y[b + PART_CHEST]
        };
        simulate(&g.world, in, in.dt);
        if (t == 1 || t == 5 || t == 30 || t == 60) {
            char tag[48];
            snprintf(tag, sizeof(tag), "running right t=%d", t);
            dump(&g.world, player, tag);
        }
    }

    printf("\n=========================================\n");
    printf("  PHASE 4 — Release input (stop snap?)\n");
    printf("=========================================\n");
    in.buttons = 0;
    for (int t = 1; t <= 60; ++t) {
        simulate(&g.world, in, in.dt);
        if (t == 1 || t == 5 || t == 15 || t == 60) {
            char tag[48];
            snprintf(tag, sizeof(tag), "released t=%d", t);
            dump(&g.world, player, tag);
        }
    }

    printf("\n=========================================\n");
    printf("  PHASE 5 — Hold JET 0.5s\n");
    printf("=========================================\n");
    in.buttons = BTN_JET;
    for (int t = 1; t <= 30; ++t) {
        simulate(&g.world, in, in.dt);
        if (t == 1 || t == 10 || t == 30) {
            char tag[48];
            snprintf(tag, sizeof(tag), "jet t=%d", t);
            dump(&g.world, player, tag);
        }
    }

    return 0;
}
