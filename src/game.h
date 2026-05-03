#pragma once

#include "arena.h"
#include "hash.h"
#include "input.h"
#include "net.h"
#include "reconcile.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * `Game` is the spine. One process, one Game. Pointers to subsystems
 * we'll add at later milestones live here; at M1 it owns the allocators,
 * the RNG, and the World.
 *
 * See [09-codebase-architecture.md].
 */

typedef enum {
    MODE_BOOT = 0,    /* before init has completed */
    MODE_LOBBY,       /* lobby UI; no match running */
    MODE_MATCH,       /* in-round simulation */
    MODE_SUMMARY,     /* round summary screen */
    MODE_QUIT,        /* shutdown requested */
} GameMode;

typedef struct Game {
    Arena permanent;
    Arena level_arena;
    Arena frame_arena;

    /* The world's RNG. Single owner so simulate() stays a pure function. */
    pcg32_t rng;

    GameMode mode;
    uint64_t tick;          /* monotonic simulation tick counter */
    double   time_seconds;  /* wall clock since process start */

    /* Most recent input sampled by the platform layer. */
    ClientInput input;

    /* The simulation. */
    World world;

    /* Networking. M2 lives here. NET_ROLE_OFFLINE for single-player; set
     * by main.c after parsing CLI flags. */
    NetState net;

    /* Client-side prediction + reconciliation buffer. Only used when
     * net.role == NET_ROLE_CLIENT. The host doesn't predict (server is
     * authoritative on the same process). */
    Reconcile reconcile;
} Game;

bool game_init(Game *g);
void game_shutdown(Game *g);

/* Called once per render frame, after the platform has sampled input.
 * At M0 this just bumps tick and clears the frame arena. */
void game_step(Game *g, double dt);
