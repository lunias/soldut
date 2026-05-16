#include "physics.h"

#include "level.h"
#include "log.h"

#include <math.h>
#include <string.h>

/* ---- Gravity ------------------------------------------------------- */

/* Forward decl — implemented further down (Task 8b). */
static void apply_ambient_zone_overrides(World *w, float dt,
                                         uint8_t *zero_g_mask);

void physics_apply_gravity(World *w, float dt) {
    ParticlePool *p = &w->particles;
    Vec2 g = w->level.gravity;

    /* P02: zero per-particle contact data at the start of each tick.
     * Whatever fresh contacts the relaxation pass produces overwrite
     * these; particles with no contact this tick read as (0, 0, 0). */
    if (p->contact_nx_q) memset(p->contact_nx_q, 0, (size_t)p->count);
    if (p->contact_ny_q) memset(p->contact_ny_q, 0, (size_t)p->count);
    if (p->contact_kind) memset(p->contact_kind, 0, (size_t)p->count);

    /* ZERO_G ambient zones: build a per-particle bitmask of "skip
     * gravity" so the gravity loop can branch cheaply. The mask lives
     * on the frame stack; capacity matches the pool. */
    uint8_t zero_g_local[PARTICLES_CAPACITY];
    uint8_t *zero_g = NULL;
    if (w->level.ambi_count > 0) {
        memset(zero_g_local, 0, (size_t)p->count);
        zero_g = zero_g_local;
    }

    /* Verlet absorbs accelerations as `+ a*dt^2` inside the integrate
     * step. We apply a velocity nudge here by displacing pos directly,
     * which accumulates into the (pos - prev) term on the next integrate.
     *
     * That's equivalent and lets us layer per-frame forces (jet, run,
     * recoil) the same way without double-bookkeeping a velocity field. */
    apply_ambient_zone_overrides(w, dt, zero_g);

    float fx = g.x * dt * dt;
    float fy = g.y * dt * dt;
    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        if (p->inv_mass[i] <= 0.0f) continue;
        if (zero_g && zero_g[i]) continue;
        p->pos_x[i] += fx;
        p->pos_y[i] += fy;
    }
}

/* ---- Verlet integrate --------------------------------------------- */

void physics_integrate(World *w, float dt) {
    (void)dt;   /* dt enters via the (pos - prev) implicit velocity */
    ParticlePool *p = &w->particles;
    const float damp = PHYSICS_VELOCITY_DAMP;
    /* M6 P09 — Per-particle damp override. A particle whose last-tick
     * contact_kind has the ICE bit set keeps almost all its momentum
     * (0.9995 vs the default 0.99). Combined with the friction = 1.0
     * tangential-preservation at contact_with_velocity, this makes
     * ICE feel genuinely slippery — once you build any horizontal
     * speed, you slide for several seconds before contact / drag
     * stops you. Combined with WIND's target-velocity push (see
     * apply_ambient_zone_overrides) the mech accelerates almost
     * unopposed under wind on ice, but barely budges under wind on
     * concrete — exactly the "wind + ice compound" the user wants. */
    const float damp_ice = 0.9995f;

    /* Swept collision against tiles. The discrete inside-tile escape
     * in collide_map_one_pass only fires if the post-integrate center
     * lands inside a solid tile. With sustained jet from below, a
     * particle can build up enough velocity to skip clear over a
     * 1-tile-thick platform in one tick (it's below the platform
     * before integrate, above the platform after — never inside).
     * Ray-cast from prev to the integrated pos and clamp the move at
     * the first solid we hit so the post-integrate pos always lies
     * just shy of the tile we'd otherwise have tunneled through. */
    const Level *L = &w->level;
    const float r  = PHYSICS_PARTICLE_RADIUS;

    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        if (p->inv_mass[i] <= 0.0f) continue;

        float px = p->pos_x[i],  py = p->pos_y[i];
        float qx = p->prev_x[i], qy = p->prev_y[i];

        /* M6 P09 — ICE damp override. Read last-tick contact_kind; if
         * the particle is touching an ICE surface, use the high-momentum
         * damp so velocity persists across the verlet integrate. */
        float d = damp;
        if (p->contact_kind && (p->contact_kind[i] & TILE_F_ICE)) {
            d = damp_ice;
        }

        /* x_{n+1} = x_n + (x_n - x_{n-1}) * damp  (forces already added) */
        float nx = px + (px - qx) * d;
        float ny = py + (py - qy) * d;

        /* Sweep from start-of-tick prev to the integrated pos. We
         * sweep from prev (not from px, the post-force pre-integrate
         * pos) because forces alone can also push a particle through
         * a tile in one tick. If the path hits a solid before the
         * end, pull back the move to land r pixels short of the
         * crossing on the side we came from. */
        float dx = nx - qx;
        float dy = ny - qy;
        float seg2 = dx * dx + dy * dy;
        if (seg2 > 1.0f) {
            float t = 1.0f;
            /* M6 P09 — kinematic-variant: a particle moving UPWARD
             * through a TILE_F_ONE_WAY tile is NOT pre-clamped, so it
             * can pass through and the runtime collide_map_one_pass
             * stays the sole gate for ONE_WAY semantics. Falling onto
             * an ONE_WAY platform still clamps correctly. */
            if (level_ray_hits_kinematic(L, (Vec2){qx, qy}, (Vec2){nx, ny}, &t)) {
                float seg_len = sqrtf(seg2);
                /* Stop r+epsilon px before the tile boundary. */
                float t_clamped = t - (r + 0.5f) / seg_len;
                if (t_clamped < 0.0f) t_clamped = 0.0f;
                nx = qx + dx * t_clamped;
                ny = qy + dy * t_clamped;
            }
        }

        p->prev_x[i] = px;
        p->prev_y[i] = py;
        p->pos_x[i] = nx;
        p->pos_y[i] = ny;
    }
    /* GROUNDED bits are cleared inside physics_constrain_and_collide. */
}

/* ---- Constraint relaxation ---------------------------------------- */

/* M6 audit — stiffness factor applied per iteration to the grappling
 * hook's constraints (CSTR_FIXED_ANCHOR for tile anchors,
 * CSTR_DISTANCE_LIMIT for mech-bone anchors). With 12 iters per tick
 * and k = 0.10, the per-tick correction is 1 - (1-0.10)^12 ≈ 72%, so
 * a freshly-attached rope visibly stretches and recoils over a couple
 * of frames instead of snapping to length on tick 1. Skeleton bones
 * use solve_distance (not gated by this factor) so the body stays at
 * rest length. */
#define GRAPPLE_ROPE_STIFFNESS  0.10f

static void solve_distance(ParticlePool *p, const Constraint *c) {
    float ax = p->pos_x[c->a], ay = p->pos_y[c->a];
    float bx = p->pos_x[c->b], by = p->pos_y[c->b];
    float dx = bx - ax, dy = by - ay;
    float d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    float diff = (d - c->rest) / d;

    float wa = p->inv_mass[c->a];
    float wb = p->inv_mass[c->b];
    float wsum = wa + wb;
    if (wsum < 1e-6f) return;

    float ka = (wa / wsum) * 0.5f;
    float kb = (wb / wsum) * 0.5f;

    p->pos_x[c->a] += dx * diff * ka;
    p->pos_y[c->a] += dy * diff * ka;
    p->pos_x[c->b] -= dx * diff * kb;
    p->pos_y[c->b] -= dy * diff * kb;
}

/* M6 audit: CSTR_DISTANCE_LIMIT is only used for the grappling hook's
 * mech-to-mech case (skeleton bones use CSTR_DISTANCE / solve_distance
 * with the 0.5 per-side factor, which is fine for rest-length
 * maintenance). Applying the same `GRAPPLE_ROPE_STIFFNESS` factor here
 * gives the mech-bone grapple the same stretchy feel as the
 * tile-anchor case. */
static void solve_distance_limit(ParticlePool *p, const Constraint *c) {
    float ax = p->pos_x[c->a], ay = p->pos_y[c->a];
    float bx = p->pos_x[c->b], by = p->pos_y[c->b];
    float dx = bx - ax, dy = by - ay;
    float d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    float target = d;
    if (d < c->min_len) target = c->min_len;
    else if (d > c->max_len) target = c->max_len;
    else return;

    float diff = (d - target) / d * GRAPPLE_ROPE_STIFFNESS;

    float wa = p->inv_mass[c->a];
    float wb = p->inv_mass[c->b];
    float wsum = wa + wb;
    if (wsum < 1e-6f) return;

    float ka = (wa / wsum) * 0.5f;
    float kb = (wb / wsum) * 0.5f;

    p->pos_x[c->a] += dx * diff * ka;
    p->pos_y[c->a] += dy * diff * ka;
    p->pos_x[c->b] -= dx * diff * kb;
    p->pos_y[c->b] -= dy * diff * kb;
}

/* Angle constraint: clamp the interior angle at joint B (between B→A
 * and B→C) into [min_ang, max_ang]. The previous implementation used
 * atan2-difference + modulo, which wraps catastrophically near the
 * π boundary (a "straight chain" can flip to -π and clamp to a giant
 * correction). Using acos(dot) gives a stable angle in [0, π]. */
static void solve_angle(ParticlePool *p, const Constraint *c) {
    int A = c->a, B = c->c, C = c->b;
    float bx = p->pos_x[B], by = p->pos_y[B];
    float ax = p->pos_x[A] - bx, ay = p->pos_y[A] - by;
    float cx = p->pos_x[C] - bx, cy = p->pos_y[C] - by;

    float la = sqrtf(ax * ax + ay * ay);
    float lc = sqrtf(cx * cx + cy * cy);
    if (la < 1e-6f || lc < 1e-6f) return;

    float dot = (ax * cx + ay * cy) / (la * lc);
    if (dot >  1.0f) dot =  1.0f;
    if (dot < -1.0f) dot = -1.0f;
    float angle = acosf(dot);          /* [0, π]; π = straight chain */

    float clamped = angle;
    if (clamped < c->min_ang)  clamped = c->min_ang;
    else if (clamped > c->max_ang) clamped = c->max_ang;
    if (fabsf(clamped - angle) < 1e-4f) return;

    /* Rotation sign: cross > 0 → C is CCW from A. Opening the angle
     * (clamped > angle) means rotating A clockwise and C counter-
     * clockwise (when cross > 0); flip when cross < 0. We apply half
     * the correction to each endpoint. */
    float cross = ax * cy - ay * cx;
    float sign  = (cross >= 0.0f) ? 1.0f : -1.0f;
    float corr  = (clamped - angle) * 0.5f * sign;

    float ca = cosf(-corr), sa = sinf(-corr);
    float cc = cosf( corr), sc = sinf( corr);

    float new_ax = ax * ca - ay * sa;
    float new_ay = ax * sa + ay * ca;
    float new_cx = cx * cc - cy * sc;
    float new_cy = cx * sc + cy * cc;

    if (p->inv_mass[A] > 0.0f) {
        p->pos_x[A] = bx + new_ax;
        p->pos_y[A] = by + new_ay;
    }
    if (p->inv_mass[C] > 0.0f) {
        p->pos_x[C] = bx + new_cx;
        p->pos_y[C] = by + new_cy;
    }
}

/* CSTR_FIXED_ANCHOR (P06): one-sided distance limit toward the
 * constraint's inline `fixed_pos`. When the particle is farther than
 * `rest`, pull it in. When closer, the rope is slack — no force.
 * End b has effective inv_mass = 0 (it's a fixed world point).
 *
 * Used by the grappling hook when anchored to a tile. The one-sided
 * behaviour gives the firer the "Tarzan swing" feel: the body hangs
 * at rope length, swings as a pendulum, and can drift closer to the
 * anchor (slack) without being shoved away.
 *
 * M6 audit: stretchy via GRAPPLE_ROPE_STIFFNESS (~10% per iter so the
 * rope visibly extends and recoils on attach instead of snapping to
 * length). ALSO swept-test the constraint move against solid tiles —
 * if the correction would pull the particle through a wall, clamp
 * the move so the particle stops at the wall surface. Without this,
 * holding-retract or grappling across a wall can drag the firer's
 * pelvis straight through solid tiles. */
static void solve_fixed_anchor(ParticlePool *p, const Level *L,
                               const Constraint *c) {
    int ai = c->a;
    if (p->inv_mass[ai] <= 0.0f) return;
    float dx = c->fixed_pos.x - p->pos_x[ai];
    float dy = c->fixed_pos.y - p->pos_y[ai];
    float d2 = dx * dx + dy * dy;
    if (d2 < 1e-6f) return;
    float d = sqrtf(d2);
    if (d <= c->rest) return;            /* slack — no force */
    float diff = (d - c->rest) / d * GRAPPLE_ROPE_STIFFNESS;

    float mx = dx * diff;
    float my = dy * diff;

    /* Swept-test against solid tiles. If the segment from current pos
     * to target crosses a solid tile, stop the move just shy of the
     * tile so the constraint never drags the particle into geometry.
     * level_ray_hits returns t ∈ [0, 1] for the first hit; we leave
     * the particle r+0.5 px shy of the hit point on the side it came
     * from (matches the integrate sweep's clamp). */
    float seg2 = mx * mx + my * my;
    if (L && seg2 > 1.0f) {
        float t = 1.0f;
        Vec2 from = { p->pos_x[ai], p->pos_y[ai] };
        Vec2 to   = { from.x + mx, from.y + my };
        if (level_ray_hits(L, from, to, &t)) {
            float seg_len = sqrtf(seg2);
            const float r = PHYSICS_PARTICLE_RADIUS;
            float t_clamped = t - (r + 0.5f) / seg_len;
            if (t_clamped < 0.0f) t_clamped = 0.0f;
            mx *= t_clamped;
            my *= t_clamped;
        }
    }
    p->pos_x[ai] += mx;
    p->pos_y[ai] += my;
}

static void solve_constraints_one_pass(World *w) {
    ConstraintPool *cp = &w->constraints;
    ParticlePool   *pp = &w->particles;
    const Level    *L  = &w->level;
    for (int i = 0; i < cp->count; ++i) {
        const Constraint *c = &cp->items[i];
        if (!c->active) continue;
        switch ((ConstraintKind)c->kind) {
            case CSTR_DISTANCE:        solve_distance(pp, c);       break;
            case CSTR_DISTANCE_LIMIT:  solve_distance_limit(pp, c); break;
            case CSTR_ANGLE:           solve_angle(pp, c);          break;
            case CSTR_FIXED_ANCHOR:    solve_fixed_anchor(pp, L, c); break;
        }
    }
}

/* ---- Map collision ------------------------------------------------- */

/* For each particle: sample the four tile neighborhoods around it; if
 * any are SOLID and the particle's circle overlaps the tile rectangle,
 * push the particle out along the shortest-axis separation.
 *
 * This is enough for M1's tile-only world. Slopes and free polygons get
 * a closest-point test in a later milestone. */
/* Two flavors of contact:
 *  - position-only (used inside the relaxation iters): pushes pos out of
 *    the surface, leaves prev alone. The body re-equilibrates over the
 *    next constraint iteration.
 *  - position + velocity (used on the final pass): also zeros the
 *    velocity component along the contact normal and applies tangential
 *    friction, so we don't bounce off the floor or coast forever. */
/* Pack (nx, ny) into the per-particle Q1.7 contact-normal slots. Done
 * by both contact flavors so the next tick's mech_step_drive sees the
 * latest contact regardless of which iteration produced it. */
static inline void persist_contact(ParticlePool *p, int i,
                                   float nx, float ny, uint8_t kind) {
    int qx = (int)(nx * 127.0f);
    int qy = (int)(ny * 127.0f);
    if (qx >  127) qx =  127;
    if (qx < -128) qx = -128;
    if (qy >  127) qy =  127;
    if (qy < -128) qy = -128;
    p->contact_nx_q[i] = (int8_t)qx;
    p->contact_ny_q[i] = (int8_t)qy;
    p->contact_kind[i] = kind;
    /* Floor (ny < -0.5) is the GROUNDED case (set elsewhere by the
     * collision caller). Ceiling (ny > 0.5) is the new flag — used by
     * apply_jet_force to slide along angled overhangs. */
    if (ny >  0.5f) p->flags[i] |= PARTICLE_FLAG_CEILING;
}

static inline void contact_position_only(ParticlePool *p, int i,
                                         float nx, float ny, float amount,
                                         uint8_t kind) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
    persist_contact(p, i, nx, ny, kind);
}

static inline void contact_with_velocity(ParticlePool *p, int i,
                                         float nx, float ny, float amount,
                                         uint8_t kind) {
    p->pos_x[i] += nx * amount;
    p->pos_y[i] += ny * amount;
    float vx = p->pos_x[i] - p->prev_x[i];
    float vy = p->pos_y[i] - p->prev_y[i];
    float vn = vx * nx + vy * ny;

    /* Slope-angle-aware friction (P02). Floor (ny ≈ -1): full friction
     * (~0.92). 45° slope (ny ≈ -0.7): less friction (~0.94 — slide a
     * bit). Steep slope (ny ≈ -0.4, 65°): very low friction (~0.97 —
     * slide freely). Combined with slope-tangent run velocity in
     * mech.c, this gives the Soldat-like uphill-slow / downhill-fast
     * feel. */
    float ny_abs = (ny < 0.0f) ? -ny : ny;
    float friction = 0.99f - 0.07f * ny_abs;     /* [0.92 .. 0.99] */
    if (friction > 0.998f) friction = 0.998f;
    /* M6 P09 — bumped ICE 0.998 → 1.0 (zero tangential friction). The
     * 0.998 friction (~12 % loss / sec) was being swamped by the global
     * PHYSICS_VELOCITY_DAMP = 0.99 (~45 % loss / sec) so the mech felt
     * effectively the same on ice as on concrete. With friction = 1.0,
     * contact preserves every drop of tangential momentum and the only
     * decay is the per-integrate damp — paired with the ICE damp
     * override at physics_integrate (~0.9985 vs the default 0.99), the
     * mech slides convincingly for ~2 s before stopping. */
    if (kind & TILE_F_ICE) friction = 1.0f;

    float vtx = (vx - vn * nx) * friction;
    float vty = (vy - vn * ny) * friction;
    p->prev_x[i] = p->pos_x[i] - vtx;
    p->prev_y[i] = p->pos_y[i] - vty;

    persist_contact(p, i, nx, ny, kind);
}

static void collide_map_one_pass(World *w, bool finalize_velocity) {
    ParticlePool *p  = &w->particles;
    const Level  *L  = &w->level;
    const float   ts = (float)L->tile_size;
    const float   r  = PHYSICS_PARTICLE_RADIUS;

    for (int i = 0; i < p->count; ++i) {
        if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        /* wan-fixes-3 — kinematic particles (inv_mass == 0) are
         * authoritatively positioned elsewhere (e.g. snapshot_interp_remotes
         * for non-local mechs on the client). Tile collision must NOT
         * displace them; otherwise the body shape drifts asymmetrically
         * (one foot pushed out of a wall, pelvis lerps smoothly, render
         * shows wobbling limbs). Mirrors the inv_mass == 0 early-return
         * in physics_apply_gravity / physics_integrate / solve_distance. */
        if (p->inv_mass[i] <= 0.0f) continue;

        float px = p->pos_x[i];
        float py = p->pos_y[i];
        int   tx = (int)(px / ts);
        int   ty = (int)(py / ts);

        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int cx = tx + dx, cy = ty + dy;
                uint16_t tflags = level_flags_at(L, cx, cy);
                if (!(tflags & TILE_F_SOLID)) continue;
                /* M6 P09 — BACKGROUND tiles are purely decorative. The
                 * polygon path already honors this (POLY_KIND_BACKGROUND
                 * is skipped at src/physics.c:633,916). Mirror it on the
                 * tile side so editor-authored BACKGROUND tiles stop
                 * lying. SOLID is implied by the gate above, so a
                 * BACKGROUND tile reads as "draws like a tile, doesn't
                 * collide" — exactly what the editor's checkbox UI
                 * promised. */
                if (tflags & TILE_F_BACKGROUND) continue;
                uint8_t  kind   = (uint8_t)(tflags & 0xff);

                float minx = (float)cx * ts;
                float miny = (float)cy * ts;
                float maxx = minx + ts;
                float maxy = miny + ts;

                float qx = (px < minx) ? minx : (px > maxx ? maxx : px);
                float qy = (py < miny) ? miny : (py > maxy ? maxy : py);
                float ddx = px - qx;
                float ddy = py - qy;
                float d2 = ddx * ddx + ddy * ddy;
                if (d2 >= r * r) continue;

                /* M6 P09 — ONE_WAY tile: standard floor-platform
                 * semantics. Block when the particle is moving DOWN
                 * onto the top surface; pass-through otherwise. Two
                 * checks combined:
                 *   1. Velocity sign — moving up (vy < 0) = passable;
                 *      moving down or at rest = blocking.
                 *   2. Center-position guard — the particle's center
                 *      must be ABOVE the tile-top for "land on top" to
                 *      apply. Particles whose centers are below the
                 *      tile top pass through (they're inside or below
                 *      the tile body, not landing on its surface).
                 * The center-position check prevents the body from
                 * sticking after the constraint relaxation pushes a
                 * foot a few pixels into the tile interior — the
                 * collide pass now only catches the genuine top-surface
                 * landing case.
                 *
                 * Mirrors the poly version at src/physics.c:717-721:
                 * "came from passable side" → skip contact. */
                if (tflags & TILE_F_ONE_WAY) {
                    float vy = py - p->prev_y[i];
                    /* Moving up (vy < 0): always passable. */
                    if (vy < -0.001f) continue;
                    /* Center below the top edge: inside / below tile;
                     * not a landing-on-top contact. */
                    if (py > miny + 1.0f) continue;
                }

                float nx, ny, amount;
                if (d2 > 1e-4f) {
                    float d = sqrtf(d2);
                    nx = ddx / d; ny = ddy / d;
                    amount = r - d;
                    /* If the particle ends up on the opposite side
                     * of the tile from where it came (e.g., constraint
                     * relaxation flicked a body part through a 1-tile
                     * platform), push it back toward the side it
                     * came from instead of further out the wrong side. */
                    float ppx = p->prev_x[i];
                    float ppy = p->prev_y[i];
                    if (ppy > maxy && py < miny) {
                        nx = 0; ny = 1; amount = (maxy - py) + r;
                    } else if (ppy < miny && py > maxy) {
                        nx = 0; ny = -1; amount = (py - miny) + r;
                    } else if (ppx > maxx && px < minx) {
                        nx = 1; ny = 0; amount = (maxx - px) + r;
                    } else if (ppx < minx && px > maxx) {
                        nx = -1; ny = 0; amount = (px - minx) + r;
                    }
                } else {
                    /* Center inside the tile. Pick the exit by where the
                     * particle came from (its prev position is stable
                     * across iteration pushes, since position-only
                     * contacts don't update prev), and only fall back to
                     * a neighbour-priority heuristic when prev was also
                     * inside the tile.
                     *
                     * Without the came-from check, a jet that drives a
                     * particle up into a 1-tile-thick platform from
                     * below gets pushed UP through to the top (open-up
                     * was first in priority), tunneling the body
                     * through the platform and adding to its upward
                     * speed in the process. */
                    bool open_up    = (level_tile_at(L, cx, cy - 1) != TILE_SOLID);
                    bool open_down  = (level_tile_at(L, cx, cy + 1) != TILE_SOLID);
                    bool open_left  = (level_tile_at(L, cx - 1, cy) != TILE_SOLID);
                    bool open_right = (level_tile_at(L, cx + 1, cy) != TILE_SOLID);

                    float ppx = p->prev_x[i];
                    float ppy = p->prev_y[i];
                    bool from_above = ppy < miny;   /* moving down into tile */
                    bool from_below = ppy > maxy;   /* moving up into tile */
                    bool from_left  = ppx < minx;
                    bool from_right = ppx > maxx;

                    float d_top = py - miny, d_bot = maxy - py;
                    float d_lft = px - minx, d_rgt = maxx - px;

                    if (from_below && open_down) {
                        nx = 0; ny = 1; amount = d_bot + r;
                    } else if (from_above && open_up) {
                        nx = 0; ny = -1; amount = d_top + r;
                    } else if (from_left && open_left) {
                        nx = -1; ny = 0; amount = d_lft + r;
                    } else if (from_right && open_right) {
                        nx = 1; ny = 0; amount = d_rgt + r;
                    } else {
                        /* No directional hint (prev was also inside or
                         * exactly on the boundary). Pick the SHORTEST
                         * exit among the open sides. Old code preferred
                         * open_up first which pushed a foot 32 px
                         * skyward whenever its center landed on a
                         * floor tile's right edge (the slope-tile
                         * transition case — d_rgt = 0 trumps any
                         * other distance). Shortest-open works for
                         * both edge-grazes (d_rgt = 0 → push right
                         * by r) and genuinely-inside cases. Fall
                         * back to shortest-of-all when no side is
                         * open. */
                        float best_d = 1e30f; int axis = -1;
                        if (open_up    && d_top < best_d) { best_d = d_top; axis = 2; }
                        if (open_down  && d_bot < best_d) { best_d = d_bot; axis = 3; }
                        if (open_left  && d_lft < best_d) { best_d = d_lft; axis = 0; }
                        if (open_right && d_rgt < best_d) { best_d = d_rgt; axis = 1; }
                        if (axis < 0) {
                            /* Surrounded by solids — shortest of all. */
                            best_d = d_top; axis = 2;
                            if (d_lft < best_d) { best_d = d_lft; axis = 0; }
                            if (d_rgt < best_d) { best_d = d_rgt; axis = 1; }
                            if (d_bot < best_d) { best_d = d_bot; axis = 3; }
                        }
                        switch (axis) {
                            case 0: nx = -1; ny =  0; amount = d_lft + r; break;
                            case 1: nx =  1; ny =  0; amount = d_rgt + r; break;
                            case 2: nx =  0; ny = -1; amount = d_top + r; break;
                            default: nx =  0; ny =  1; amount = d_bot + r; break;
                        }
                    }

                    SHOT_LOG("t=%llu inside_tile particle=%d tile=(%d,%d) "
                             "pos=(%.1f,%.1f) prev=(%.1f,%.1f) exit=(%.0f,%.0f)",
                             (unsigned long long)w->tick, i, cx, cy,
                             px, py, ppx, ppy, nx, ny);
                }

                if (finalize_velocity) contact_with_velocity(p, i, nx, ny, amount, kind);
                else                   contact_position_only(p, i, nx, ny, amount, kind);
                if (ny < -0.5f) p->flags[i] |= PARTICLE_FLAG_GROUNDED;

                /* Refresh local copy so subsequent neighbours see the push. */
                px = p->pos_x[i];
                py = p->pos_y[i];
            }
        }
    }
}

/* ---- Polygon collision (P02) -------------------------------------- */

/* Closest point on a triangle (a, b, c) to point p. Returns the world-
 * space closest point and writes the index of the touched edge (0/1/2)
 * via *out_edge — used so the caller can read the polygon's pre-baked
 * normal for the push-out direction. Vertex-closest cases (where the
 * closest point is exactly at one of the triangle's corners) set
 * *out_edge = -1; the caller then averages the two adjacent edge normals. */
static Vec2 closest_point_on_tri(Vec2 p,
                                 Vec2 a, Vec2 b, Vec2 c, int *out_edge)
{
    /* Christer Ericke / Eberly. Compute barycentric-region tests
     * against the three vertices (1/3) and three edges (3/6); inside
     * is the seventh region.
     *
     * For small polys (~1-2 tile span) the per-call cost is dominated
     * by the dot-products below; staying inline keeps the hot path
     * tight. */
    Vec2 ab = { b.x - a.x, b.y - a.y };
    Vec2 ac = { c.x - a.x, c.y - a.y };
    Vec2 ap = { p.x - a.x, p.y - a.y };
    float d1 = ab.x * ap.x + ab.y * ap.y;
    float d2 = ac.x * ap.x + ac.y * ap.y;
    if (d1 <= 0.0f && d2 <= 0.0f) { *out_edge = -1; return a; }   /* A vertex */

    Vec2 bp = { p.x - b.x, p.y - b.y };
    float d3 = ab.x * bp.x + ab.y * bp.y;
    float d4 = ac.x * bp.x + ac.y * bp.y;
    if (d3 >= 0.0f && d4 <= d3) { *out_edge = -1; return b; }     /* B vertex */

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        *out_edge = 0;                                            /* AB edge */
        return (Vec2){ a.x + v * ab.x, a.y + v * ab.y };
    }

    Vec2 cp = { p.x - c.x, p.y - c.y };
    float d5 = ab.x * cp.x + ab.y * cp.y;
    float d6 = ac.x * cp.x + ac.y * cp.y;
    if (d6 >= 0.0f && d5 <= d6) { *out_edge = -1; return c; }     /* C vertex */

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        *out_edge = 2;                                            /* CA edge */
        return (Vec2){ a.x + w * ac.x, a.y + w * ac.y };
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        *out_edge = 1;                                            /* BC edge */
        return (Vec2){ b.x + w * (c.x - b.x), b.y + w * (c.y - b.y) };
    }

    /* Inside the triangle. */
    float denom = 1.0f / (va + vb + vc);
    float vv = vb * denom;
    float ww = vc * denom;
    *out_edge = -2;                                               /* inside */
    return (Vec2){ a.x + ab.x * vv + ac.x * ww,
                   a.y + ab.y * vv + ac.y * ww };
}

/* Map a PolyKind to the matching TILE_F_* bit so contact_kind can be
 * read uniformly by the friction code. SOLID has no extra bits set. */
static inline uint8_t poly_kind_to_contact_kind(uint16_t kind) {
    uint8_t k = TILE_F_SOLID;
    switch ((PolyKind)kind) {
        case POLY_KIND_ICE:        k |= TILE_F_ICE;        break;
        case POLY_KIND_DEADLY:     k |= TILE_F_DEADLY;     break;
        case POLY_KIND_ONE_WAY:    k |= TILE_F_ONE_WAY;    break;
        case POLY_KIND_BACKGROUND: k  = TILE_F_BACKGROUND; break;
        case POLY_KIND_SOLID:                              break;
    }
    return k;
}

static void collide_polys_one_pass(World *w, bool finalize_velocity) {
    ParticlePool *pp = &w->particles;
    const Level  *L  = &w->level;
    if (L->poly_count == 0 || !L->poly_grid_off) return;

    const float ts = (float)L->tile_size;
    const float r  = PHYSICS_PARTICLE_RADIUS;
    const int   W  = L->width;
    const int   H  = L->height;

    for (int i = 0; i < pp->count; ++i) {
        if (!(pp->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
        /* wan-fixes-3 — same kinematic gate as collide_map_one_pass. */
        if (pp->inv_mass[i] <= 0.0f) continue;

        float px = pp->pos_x[i];
        float py = pp->pos_y[i];
        int   tx = (int)(px / ts);
        int   ty = (int)(py / ts);
        if (tx < 0 || tx >= W || ty < 0 || ty >= H) continue;

        int cell = ty * W + tx;
        int s    = L->poly_grid_off[cell];
        int e    = L->poly_grid_off[cell + 1];
        for (int k = s; k < e; ++k) {
            int pi = L->poly_grid[k];
            const LvlPoly *poly = &L->polys[pi];
            if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;

            Vec2 a = { (float)poly->v_x[0], (float)poly->v_y[0] };
            Vec2 b = { (float)poly->v_x[1], (float)poly->v_y[1] };
            Vec2 c = { (float)poly->v_x[2], (float)poly->v_y[2] };

            int edge = -2;
            Vec2 cpt = closest_point_on_tri((Vec2){px, py}, a, b, c, &edge);
            float ddx = px - cpt.x;
            float ddy = py - cpt.y;
            float d2  = ddx * ddx + ddy * ddy;

            if (edge != -2) {
                /* Outside the triangle: contact only if within radius. */
                if (d2 >= r * r) continue;
            }
            /* edge == -2 means the particle's center is INSIDE the
             * triangle — always push out. */

            float nx, ny, amount;
            if (edge == 0 || edge == 1 || edge == 2) {
                /* Use the polygon's pre-baked edge normal — Q1.15. */
                nx = poly->normal_x[edge] / 32767.0f;
                ny = poly->normal_y[edge] / 32767.0f;
                if (d2 > 1e-4f) amount = r - sqrtf(d2);
                else            amount = r;
            } else if (edge == -1) {
                /* Vertex-closest. Average the two adjacent edge
                 * normals (e and (e-1)); for our triangle that's
                 * vertices A→edges 0,2; B→0,1; C→1,2. */
                int e1, e2;
                if      (cpt.x == a.x && cpt.y == a.y) { e1 = 0; e2 = 2; }
                else if (cpt.x == b.x && cpt.y == b.y) { e1 = 0; e2 = 1; }
                else                                   { e1 = 1; e2 = 2; }
                nx = (poly->normal_x[e1] + poly->normal_x[e2]) / (2.0f * 32767.0f);
                ny = (poly->normal_y[e1] + poly->normal_y[e2]) / (2.0f * 32767.0f);
                float nlen = sqrtf(nx * nx + ny * ny);
                if (nlen > 1e-4f) { nx /= nlen; ny /= nlen; }
                else              { nx = 0.0f; ny = -1.0f; }
                if (d2 > 1e-4f) amount = r - sqrtf(d2);
                else            amount = r;
            } else {
                /* Inside the triangle: pick the nearest edge and push
                 * the particle out across it (plus r). closest_point_
                 * on_tri's "interior" return doesn't tell us which
                 * edge is nearest, so do a small per-edge distance
                 * scan here.  Three iters; only fires when a particle
                 * has actually penetrated the polygon. */
                Vec2 verts[3] = { a, b, c };
                float best_d2 = 1e30f;
                int   best_e  = 0;
                Vec2  best_q  = (Vec2){ px, py };
                for (int e = 0; e < 3; ++e) {
                    Vec2 va = verts[e];
                    Vec2 vb = verts[(e + 1) % 3];
                    float lex = vb.x - va.x, ley = vb.y - va.y;
                    float len2 = lex * lex + ley * ley;
                    if (len2 < 1e-6f) continue;
                    float tt = ((px - va.x) * lex + (py - va.y) * ley) / len2;
                    if (tt < 0.0f) tt = 0.0f;
                    if (tt > 1.0f) tt = 1.0f;
                    float qx = va.x + tt * lex;
                    float qy = va.y + tt * ley;
                    float ddx = px - qx, ddy = py - qy;
                    float d2_e = ddx * ddx + ddy * ddy;
                    if (d2_e < best_d2) {
                        best_d2 = d2_e;
                        best_e  = e;
                        best_q  = (Vec2){ qx, qy };
                    }
                }
                nx = poly->normal_x[best_e] / 32767.0f;
                ny = poly->normal_y[best_e] / 32767.0f;
                amount = sqrtf(best_d2) + r;
                /* d2 was 0 (interior); this exit is along the edge
                 * normal so the body lands on the surface, not the
                 * floor. */
                (void)best_q;
            }

            /* ONE_WAY: only block if the particle came from the side
             * the normal points toward. (For a floor-style ramp with
             * normal pointing up, particles coming from above are
             * blocked; particles rising from below pass through.) */
            if ((PolyKind)poly->kind == POLY_KIND_ONE_WAY) {
                float dprev = (pp->prev_x[i] - cpt.x) * nx +
                              (pp->prev_y[i] - cpt.y) * ny;
                if (dprev <= 0.0f) continue;       /* came from passable side */
            }

            uint8_t kind = poly_kind_to_contact_kind(poly->kind);
            if (finalize_velocity) contact_with_velocity(pp, i, nx, ny, amount, kind);
            else                   contact_position_only(pp, i, nx, ny, amount, kind);
            if (ny < -0.5f) pp->flags[i] |= PARTICLE_FLAG_GROUNDED;

            px = pp->pos_x[i];
            py = pp->pos_y[i];
        }
    }
}

void physics_constrain_and_collide(World *w) {
    /* Clear stale grounded + ceiling bits — collide passes will re-assert
     * whichever ones still apply this tick. */
    ParticlePool *p = &w->particles;
    for (int i = 0; i < p->count; ++i)
        p->flags[i] &= (uint8_t)~(PARTICLE_FLAG_GROUNDED | PARTICLE_FLAG_CEILING);

    int iters = PHYSICS_CONSTRAINT_ITERATIONS;
    for (int it = 0; it < iters; ++it) {
        bool last = (it == iters - 1);
        solve_constraints_one_pass(w);
        collide_map_one_pass(w, /*finalize_velocity*/ last);
        collide_polys_one_pass(w, /*finalize_velocity*/ last);
    }
}

/* ---- Ambient zones (P02 / M6 P09) -------------------------------- */
/*
 * apply_ambient_zone_overrides — called from physics_apply_gravity
 * before the gravity write.
 *
 * M6 P09 — WIND rewritten as a target-velocity push (was a tiny
 * per-tick prev nudge by `st * dt` ≈ 0.008 px that did nothing
 * visible). The new model treats `strength_q` as a fraction of a
 * MAX wind speed (250 px/s) and a MAX acceleration (600 px/s²) —
 * particles in the rect accelerate toward `strength * 250 px/s` in
 * the wind direction, capped per tick at `strength * 600 px/s²`.
 *
 * On flat concrete (contact friction 0.92 + integrate damp 0.99 →
 * per-tick decay ≈ 8.9 %) the wind can only push the mech to
 * ~50-60 px/s — feels like leaning into a stiff breeze.
 *
 * On ICE (contact friction 1.0 + per-particle integrate damp 0.9995
 * → per-tick decay ≈ 0.05 %) the wind cleanly drives the mech to
 * the full target. Once at target speed, no further push (the cap
 * absorbs friction loss but doesn't accelerate past target).
 *
 * ZERO_G zones flag particles for the gravity loop to skip. ACID
 * zones do nothing here (mech.c::mech_apply_environmental handles
 * the damage tick; they are fundamentally a per-mech concern).
 */
/* M6 P09 — WIND tuning. Reference: RUN_SPEED_PXS = 280 px/s (walking),
 * JET_THRUST_PXS2 = 2200 px/s² (jetpack). Max wind exceeds walking
 * speed by 2.5× so a strength=1.0 storm visibly out-drags any
 * ground-locomotion attempt to fight it; mid-strength (0.4-0.6) winds
 * feel like leaning into a stiff breeze. The acceleration cap is
 * ~half jet thrust so the buildup time matches a player jet-burst —
 * roughly 0.6 s to reach full wind speed from rest. Designers scale
 * down via LvlAmbi.strength_q (Q1.15, 0..32767 → 0..1.0). */
#define WIND_MAX_SPEED_PXS   700.0f   /* px/s at strength=1.0 (~2.5× walking) */
#define WIND_MAX_ACCEL_PXS2  1500.0f  /* per-tick acceleration cap (~0.6 s ramp) */

static void apply_ambient_zone_overrides(World *w, float dt,
                                         uint8_t *zero_g_mask)
{
    if (w->level.ambi_count == 0) return;
    ParticlePool *p = &w->particles;
    for (int z = 0; z < w->level.ambi_count; ++z) {
        const LvlAmbi *a = &w->level.ambis[z];
        if (a->kind != AMBI_WIND && a->kind != AMBI_ZERO_G) continue;
        float minx = (float)a->rect_x;
        float miny = (float)a->rect_y;
        float maxx = minx + (float)a->rect_w;
        float maxy = miny + (float)a->rect_h;
        if (a->kind == AMBI_WIND) {
            float sx = (float)a->dir_x_q / 32767.0f;
            float sy = (float)a->dir_y_q / 32767.0f;
            float st = (float)a->strength_q / 32767.0f;
            /* Normalize the direction so editor-authored (.85, .5) still
             * produces a unit-vector wind direction (was: the per-axis
             * Q15 → float conversion left non-unit vectors that scaled
             * the effective strength). */
            float dlen = sqrtf(sx * sx + sy * sy);
            if (dlen > 1e-4f) { sx /= dlen; sy /= dlen; }
            else              { sx = 1.0f; sy = 0.0f; }
            /* Target speed (px/tick) and per-tick acceleration cap. */
            float target_pxtick = st * WIND_MAX_SPEED_PXS  * dt;
            float accel_pxtick  = st * WIND_MAX_ACCEL_PXS2 * dt * dt;
            for (int i = 0; i < p->count; ++i) {
                if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
                if (p->inv_mass[i] <= 0.0f) continue;
                float px = p->pos_x[i], py = p->pos_y[i];
                if (px < minx || px > maxx || py < miny || py > maxy) continue;
                /* Project current per-tick velocity onto wind dir. */
                float vx = px - p->prev_x[i];
                float vy = py - p->prev_y[i];
                float vproj = vx * sx + vy * sy;
                float deficit = target_pxtick - vproj;
                if (deficit <= 0.0f) continue;          /* already at speed */
                float push = (deficit < accel_pxtick) ? deficit : accel_pxtick;
                /* Inject the per-tick velocity gain by displacing pos
                 * (Verlet absorbs it via the next integrate's pos-prev
                 * delta — matches the gravity convention at line 50). */
                p->pos_x[i] += push * sx;
                p->pos_y[i] += push * sy;
            }
        } else { /* AMBI_ZERO_G */
            if (!zero_g_mask) continue;
            for (int i = 0; i < p->count; ++i) {
                if (!(p->flags[i] & PARTICLE_FLAG_ACTIVE)) continue;
                float px = p->pos_x[i], py = p->pos_y[i];
                if (px < minx || px > maxx || py < miny || py > maxy) continue;
                zero_g_mask[i] = 1;
            }
        }
    }
}

void physics_translate_kinematic_swept(ParticlePool *p, const Level *L,
                                       int i, float dx, float dy) {
    float qx = p->pos_x[i];
    float qy = p->pos_y[i];
    float nx = qx + dx;
    float ny = qy + dy;
    float seg2 = dx * dx + dy * dy;
    if (seg2 > 1.0f) {
        float t = 1.0f;
        if (level_ray_hits(L, (Vec2){qx, qy}, (Vec2){nx, ny}, &t)) {
            float seg_len = sqrtf(seg2);
            float t_clamped = t - (PHYSICS_PARTICLE_RADIUS + 0.5f) / seg_len;
            if (t_clamped < 0.0f) t_clamped = 0.0f;
            SHOT_LOG("pose_sweep clamp particle=%d at=(%.1f,%.1f) "
                     "want=(%.2f,%.2f) t=%.3f -> got=(%.2f,%.2f)",
                     i, qx, qy, dx, dy, t_clamped,
                     dx * t_clamped, dy * t_clamped);
            dx *= t_clamped;
            dy *= t_clamped;
        }
    }
    p->pos_x[i]  += dx; p->pos_y[i]  += dy;
    p->prev_x[i] += dx; p->prev_y[i] += dy;
}

void physics_apply_impulse(ParticlePool *p, int idx, Vec2 imp) {
    if (idx < 0 || idx >= p->count) return;
    p->pos_x[idx] += imp.x;
    p->pos_y[idx] += imp.y;
}

/* ---- M6 post-pose terrain push-out -------------------------------- */

/* Push one particle out of any overlapping solid tile. Kinematic
 * translate (pos AND prev shifted by the same delta) so the move
 * doesn't inject velocity. Mirrors the tile-push logic in
 * collide_map_one_pass minus the per-particle inv_mass gate and the
 * prev-tracked "exit direction" heuristic — the latter assumes
 * particles are moving INTO the tile, which isn't always true here
 * (pose-derived positions can be statically inside). Instead we use
 * the simple shortest-axis escape. */
static void push_out_of_solid_tiles_kinematic(ParticlePool *p,
                                              const Level *L, int i)
{
    const float ts = (float)L->tile_size;
    const float r  = PHYSICS_PARTICLE_RADIUS;
    float px = p->pos_x[i];
    float py = p->pos_y[i];
    int   tx = (int)(px / ts);
    int   ty = (int)(py / ts);

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = tx + dx, cy = ty + dy;
            uint16_t tflags = level_flags_at(L, cx, cy);
            if (!(tflags & TILE_F_SOLID)) continue;

            float minx = (float)cx * ts, miny = (float)cy * ts;
            float maxx = minx + ts,      maxy = miny + ts;

            float qx = (px < minx) ? minx : (px > maxx ? maxx : px);
            float qy = (py < miny) ? miny : (py > maxy ? maxy : py);
            float ddx = px - qx, ddy = py - qy;
            float d2  = ddx * ddx + ddy * ddy;
            if (d2 >= r * r) continue;

            float nx, ny, amount;
            if (d2 > 1e-4f) {
                float d = sqrtf(d2);
                nx = ddx / d; ny = ddy / d;
                amount = r - d;
            } else {
                /* Deep inside the tile — pick the shortest-axis exit. */
                float d_top = py - miny, d_bot = maxy - py;
                float d_lft = px - minx, d_rgt = maxx - px;
                float min_d = d_top; int axis = 2;
                if (d_lft < min_d) { min_d = d_lft; axis = 0; }
                if (d_rgt < min_d) { min_d = d_rgt; axis = 1; }
                if (d_bot < min_d) { min_d = d_bot; axis = 3; }
                switch (axis) {
                    case 0: nx = -1; ny =  0; amount = d_lft + r; break;
                    case 1: nx =  1; ny =  0; amount = d_rgt + r; break;
                    case 2: nx =  0; ny = -1; amount = d_top + r; break;
                    default: nx = 0; ny =  1; amount = d_bot + r; break;
                }
            }
            float push_x = nx * amount, push_y = ny * amount;
            p->pos_x [i] += push_x; p->pos_y [i] += push_y;
            p->prev_x[i] += push_x; p->prev_y[i] += push_y;
            /* Refresh local cache so subsequent neighbours see the push. */
            px = p->pos_x[i]; py = p->pos_y[i];
        }
    }
}

/* Push one particle out of any overlapping solid polygon (slopes etc.).
 * Same kinematic-translate pattern as above. Mirrors the geometry in
 * collide_polys_one_pass — uses closest_point_on_tri to find the
 * pushout direction, falls back to pre-baked edge normals when the
 * point is exactly on an edge/vertex. */
static void push_out_of_solid_polys_kinematic(ParticlePool *p,
                                              const Level *L, int i)
{
    if (L->poly_count == 0 || !L->poly_grid_off) return;
    const float ts = (float)L->tile_size;
    const float r  = PHYSICS_PARTICLE_RADIUS;
    const int   W  = L->width;
    const int   H  = L->height;

    float px = p->pos_x[i];
    float py = p->pos_y[i];
    int   tx = (int)(px / ts);
    int   ty = (int)(py / ts);
    if (tx < 0 || tx >= W || ty < 0 || ty >= H) return;

    int cell = ty * W + tx;
    int s    = L->poly_grid_off[cell];
    int e    = L->poly_grid_off[cell + 1];
    for (int k = s; k < e; ++k) {
        int pi = L->poly_grid[k];
        const LvlPoly *poly = &L->polys[pi];
        if ((PolyKind)poly->kind == POLY_KIND_BACKGROUND) continue;

        Vec2 a = { (float)poly->v_x[0], (float)poly->v_y[0] };
        Vec2 b = { (float)poly->v_x[1], (float)poly->v_y[1] };
        Vec2 c = { (float)poly->v_x[2], (float)poly->v_y[2] };

        int edge = -2;
        Vec2 cpt = closest_point_on_tri((Vec2){px, py}, a, b, c, &edge);
        float ddx = px - cpt.x;
        float ddy = py - cpt.y;
        float d2  = ddx * ddx + ddy * ddy;

        /* edge != -2 means the particle's center is OUTSIDE the
         * triangle — only contact if within the radius. */
        if (edge != -2 && d2 >= r * r) continue;

        float nx, ny, amount;
        if (edge >= 0) {
            nx = poly->normal_x[edge] / 32767.0f;
            ny = poly->normal_y[edge] / 32767.0f;
            amount = (d2 > 1e-4f) ? (r - sqrtf(d2)) : r;
        } else if (edge == -1) {
            int e1, e2;
            if      (cpt.x == a.x && cpt.y == a.y) { e1 = 0; e2 = 2; }
            else if (cpt.x == b.x && cpt.y == b.y) { e1 = 0; e2 = 1; }
            else                                   { e1 = 1; e2 = 2; }
            nx = (poly->normal_x[e1] + poly->normal_x[e2]) / (2.0f * 32767.0f);
            ny = (poly->normal_y[e1] + poly->normal_y[e2]) / (2.0f * 32767.0f);
            float nlen = sqrtf(nx * nx + ny * ny);
            if (nlen > 1e-4f) { nx /= nlen; ny /= nlen; }
            else              { nx = 0.0f; ny = -1.0f; }
            amount = (d2 > 1e-4f) ? (r - sqrtf(d2)) : r;
        } else {
            /* Inside the triangle. Pick nearest edge. */
            Vec2 verts[3] = { a, b, c };
            float best_d2 = 1e30f;
            int   best_e  = 0;
            for (int eg = 0; eg < 3; ++eg) {
                Vec2 v0 = verts[eg];
                Vec2 v1 = verts[(eg + 1) % 3];
                float ex = v1.x - v0.x, ey = v1.y - v0.y;
                float ll = ex*ex + ey*ey;
                float t  = (ll > 1e-6f) ? ((px - v0.x)*ex + (py - v0.y)*ey) / ll : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float qx2 = v0.x + ex * t, qy2 = v0.y + ey * t;
                float ddx2 = px - qx2, ddy2 = py - qy2;
                float dd2  = ddx2*ddx2 + ddy2*ddy2;
                if (dd2 < best_d2) { best_d2 = dd2; best_e = eg; }
            }
            nx = poly->normal_x[best_e] / 32767.0f;
            ny = poly->normal_y[best_e] / 32767.0f;
            amount = r + sqrtf(best_d2);
        }
        float push_x = nx * amount, push_y = ny * amount;
        p->pos_x [i] += push_x; p->pos_y [i] += push_y;
        p->prev_x[i] += push_x; p->prev_y[i] += push_y;
        px = p->pos_x[i]; py = p->pos_y[i];
    }
}

/* Map PART_* → LIMB_* bit. Mirrors mech_ik.c's helper; we keep a
 * local copy so physics.c stays standalone from mech_ik. */
static uint8_t physics_part_to_limb_bit(int part) {
    switch (part) {
        case PART_HEAD:      return LIMB_HEAD;
        case PART_L_SHOULDER: case PART_L_ELBOW: case PART_L_HAND: return LIMB_L_ARM;
        case PART_R_SHOULDER: case PART_R_ELBOW: case PART_R_HAND: return LIMB_R_ARM;
        case PART_L_HIP: case PART_L_KNEE: case PART_L_FOOT:       return LIMB_L_LEG;
        case PART_R_HIP: case PART_R_KNEE: case PART_R_FOOT:       return LIMB_R_LEG;
        default: return 0;
    }
}

void physics_push_mech_out_of_terrain(World *w, int mech_id) {
    if (mech_id < 0 || mech_id >= w->mech_count) return;
    Mech *m = &w->mechs[mech_id];
    if (!m->alive) return;

    ParticlePool *p = &w->particles;
    const Level *L = &w->level;
    uint8_t mask = m->dismember_mask;

    for (int i = 0; i < PART_COUNT; ++i) {
        uint8_t bit = physics_part_to_limb_bit(i);
        if (bit && (mask & bit)) continue;     /* dismembered → free Verlet */
        int idx = m->particle_base + i;
        if (!(p->flags[idx] & PARTICLE_FLAG_ACTIVE)) continue;
        push_out_of_solid_tiles_kinematic(p, L, idx);
        push_out_of_solid_polys_kinematic(p, L, idx);
    }
}
