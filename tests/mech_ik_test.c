/*
 * tests/mech_ik_test.c — unit tests for the 2-bone analytic IK.
 *
 * Covers the 7 cases laid out in documents/m6/01-ik-and-pose-sync.md
 * §10.1. Tolerance: 0.01 px (floating-point noise should be well
 * under that for these inputs).
 *
 * Sign convention recap (screen coords, y-down):
 *   Right-perpendicular = rotate forward by +90° → (-fy, fx).
 *   bend_sign = +1 places the joint on that side of the s→t line;
 *   bend_sign = -1 places it on the opposite side.
 */

#include "../src/log.h"
#include "../src/mech_ik.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static int g_failed = 0;

static bool nearly_equal(float a, float b, float tol) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d <= tol;
}

#define ASSERT_VEC2(label, got, ex, ey, tol) do {                        \
    if (!nearly_equal((got).x, (ex), (tol)) ||                           \
        !nearly_equal((got).y, (ey), (tol))) {                           \
        fprintf(stderr, "FAIL: %s: got=(%.4f,%.4f) want=(%.4f,%.4f)\n",  \
                label, (got).x, (got).y, (float)(ex), (float)(ey));      \
        ++g_failed;                                                      \
    } else {                                                             \
        fprintf(stderr, "PASS: %s\n", label);                            \
    }                                                                    \
} while (0)

#define ASSERT_BOOL(label, got, want) do {                               \
    if ((got) != (want)) {                                               \
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n",                    \
                label, (int)(got), (int)(want));                         \
        ++g_failed;                                                      \
    } else {                                                             \
        fprintf(stderr, "PASS: %s\n", label);                            \
    }                                                                    \
} while (0)

int main(void) {
    log_init(NULL);
    const float tol = 0.01f;

    /* ----- Case 1: Reach right, bend +1.
     * s=(0,0), t=(50,0), L1=L2=30. Distance 50, a = 25, h = sqrt(900-625) ≈ 16.583.
     * Right-perp of forward (1,0) is (0,1) → bend+1 places joint at (25, +16.583).
     */
    {
        Vec2 j = {0, 0};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){50, 0},
                                       30.0f, 30.0f, +1.0f, &j);
        ASSERT_VEC2("case1 reach right bend+1 joint", j,
                    25.0f, sqrtf(900.0f - 625.0f), tol);
        ASSERT_BOOL("case1 reachable", reachable, true);
    }

    /* ----- Case 2: Reach right, bend -1.
     * Same distance but joint flipped to (25, -16.583).
     */
    {
        Vec2 j = {0, 0};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){50, 0},
                                       30.0f, 30.0f, -1.0f, &j);
        ASSERT_VEC2("case2 reach right bend-1 joint", j,
                    25.0f, -sqrtf(900.0f - 625.0f), tol);
        ASSERT_BOOL("case2 reachable", reachable, true);
    }

    /* ----- Case 3: Aim straight up.
     * s=(0,0), t=(0,-50). Forward (0,-1), right-perp (1, 0).
     * a = 25 along (0,-1) = (0, -25). bend+1 → (-1, 0) * h = wait,
     * right-perp here: rotate forward by +90° in screen coords
     * (y-down). Forward = (0,-1). Right-perp = (-fy, fx) = (1, 0).
     * bend+1 → joint at (0,0) + 25*(0,-1) + h*(1,0) = (h, -25) ≈ (16.58, -25).
     *
     * Note: doc §10.1 wrote (-16.58, -25). That's wrong for the
     * convention picked here (right-perp = (-fy, fx)). Our math is
     * consistent with §6's right-perpendicular formula; the doc's
     * expected value uses the opposite convention.
     */
    {
        Vec2 j = {0, 0};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){0, -50},
                                       30.0f, 30.0f, +1.0f, &j);
        ASSERT_VEC2("case3 aim up bend+1 joint", j,
                    sqrtf(900.0f - 625.0f), -25.0f, tol);
        ASSERT_BOOL("case3 reachable", reachable, true);
    }

    /* ----- Case 4: Out of reach.
     * s=(0,0), t=(100,0), L1=L2=30. Reach 60 < distance 100.
     * Joint sits at s + L1 * unit(t-s) = (30, 0). Not reachable.
     */
    {
        Vec2 j = {0, 0};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){100, 0},
                                       30.0f, 30.0f, +1.0f, &j);
        ASSERT_VEC2("case4 out of reach joint", j, 30.0f, 0.0f, tol);
        ASSERT_BOOL("case4 not reachable", reachable, false);
    }

    /* ----- Case 5: Collinear collapsed.
     * s=(0,0), t=(5,0), L1=L2=30. min_d = |L1-L2| = 0 ≤ 5 — actually
     * the case is "target closer than |L1-L2|"; |L1-L2| = 0 here so
     * d=5 > 0 → not collapsed. We need L1 ≠ L2 to test the collapsed
     * branch properly. Use L1=30, L2=10 → min_d = 20, d = 5 → collapsed.
     * Joint placed at s + 30 * unit(t-s) = (30, 0). Not reachable.
     */
    {
        Vec2 j = {0, 0};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){5, 0},
                                       30.0f, 10.0f, +1.0f, &j);
        ASSERT_VEC2("case5 collapsed joint", j, 30.0f, 0.0f, tol);
        ASSERT_BOOL("case5 not reachable", reachable, false);
    }

    /* ----- Case 6: Asymmetric reachable.
     * s=(0,0), t=(40,30), L1=25, L2=35. d = sqrt(1600+900) = 50.
     * d in (|L1-L2|=10, L1+L2=60) → reachable.
     * a = (625 - 1225 + 2500) / 100 = 19.
     * h2 = 625 - 361 = 264 → h ≈ 16.248.
     * fx=0.8, fy=0.6; px=-0.6, py=0.8.
     * joint = (0 + 19*0.8 + 1*16.248*(-0.6),
     *          0 + 19*0.6 + 1*16.248*  0.8 )
     *       = (15.2 - 9.749, 11.4 + 12.999)
     *       = (5.451, 24.399).
     * Then |joint| ≈ 25 = L1 ✓, |t - joint| ≈ 35 = L2 ✓.
     */
    {
        Vec2 j = {0, 0};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){40, 30},
                                       25.0f, 35.0f, +1.0f, &j);
        ASSERT_BOOL("case6 reachable", reachable, true);
        /* Compute expected analytically rather than hard-coding so the
         * test stays robust to rounding. */
        float d  = 50.0f;
        float a  = (25.0f*25.0f - 35.0f*35.0f + d*d) / (2.0f*d);
        float h  = sqrtf(25.0f*25.0f - a*a);
        float fx = 40.0f / d, fy = 30.0f / d;
        float ex = a*fx + 1.0f * h * (-fy);
        float ey = a*fy + 1.0f * h *   fx;
        ASSERT_VEC2("case6 joint", j, ex, ey, tol);
        /* Verify chain lengths come out right. */
        float arm1 = sqrtf(j.x*j.x + j.y*j.y);
        float arm2 = sqrtf((40.0f-j.x)*(40.0f-j.x) + (30.0f-j.y)*(30.0f-j.y));
        if (!nearly_equal(arm1, 25.0f, 0.05f) ||
            !nearly_equal(arm2, 35.0f, 0.05f)) {
            fprintf(stderr,
                    "FAIL: case6 chain lengths arm1=%.3f want 25, arm2=%.3f want 35\n",
                    arm1, arm2); ++g_failed;
        } else {
            fprintf(stderr, "PASS: case6 chain lengths\n");
        }
    }

    /* ----- Case 7: Zero distance.
     * s=t=(0,0). d=0. Falls into out-of-reach branch (d >= L1+L2-EPS
     * is false but d <= |L1-L2|+EPS is true). Joint placed at s (the
     * inv=0 path), which is degenerate but stable.
     */
    {
        Vec2 j = {99, 99};
        bool reachable = mech_ik_2bone((Vec2){0, 0}, (Vec2){0, 0},
                                       30.0f, 30.0f, +1.0f, &j);
        ASSERT_VEC2("case7 zero distance joint", j, 0.0f, 0.0f, tol);
        ASSERT_BOOL("case7 not reachable", reachable, false);
    }

    /* ----- Sanity (bonus): reflexive bend sign.
     * Flipping bend_sign mirrors the joint across the s→t line. */
    {
        Vec2 j_pos = {0, 0}, j_neg = {0, 0};
        mech_ik_2bone((Vec2){10, 20}, (Vec2){80, 60},
                      30.0f, 30.0f, +1.0f, &j_pos);
        mech_ik_2bone((Vec2){10, 20}, (Vec2){80, 60},
                      30.0f, 30.0f, -1.0f, &j_neg);
        /* Midpoint between j_pos and j_neg lands on the s→t line. */
        Vec2 mid = (Vec2){ (j_pos.x + j_neg.x) * 0.5f,
                           (j_pos.y + j_neg.y) * 0.5f };
        /* Project mid onto s→t and verify (mid - projected) ≈ 0. */
        float sx = 10.0f, sy = 20.0f;
        float dx = 80.0f - sx, dy = 60.0f - sy;
        float d2 = dx*dx + dy*dy;
        float t = ((mid.x - sx)*dx + (mid.y - sy)*dy) / d2;
        float px = sx + dx*t, py = sy + dy*t;
        if (!nearly_equal(mid.x, px, 0.01f) ||
            !nearly_equal(mid.y, py, 0.01f)) {
            fprintf(stderr,
                    "FAIL: bend-sign symmetry: mid=(%.4f,%.4f) proj=(%.4f,%.4f)\n",
                    mid.x, mid.y, px, py); ++g_failed;
        } else {
            fprintf(stderr, "PASS: bend-sign symmetry\n");
        }
    }

    if (g_failed) {
        fprintf(stderr, "\nmech_ik_test: %d FAILED\n", g_failed);
        return 1;
    }
    fprintf(stderr, "\nmech_ik_test: ALL PASS\n");
    return 0;
}
