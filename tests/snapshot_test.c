/*
 * tests/snapshot_test.c — unit test for the snapshot wire codec,
 * specifically the P10-followup `primary_id` field.
 *
 * Pre-fix: EntitySnapshot didn't carry primary_id; the client
 * inferred it from the first snapshot's weapon_id (active slot's
 * weapon). When the host's mech had its secondary equipped at
 * first-snapshot time (mid-round PICKUP_WEAPON or a mid-round-join
 * client whose first snapshot saw active_slot==1), the client ended
 * up with primary_id == secondary's weapon, then the active_slot
 * derivation
 *   `(weapon_id == primary_id) ? 0 : 1`
 * returned 0 (wrong) for the entire session.
 *
 * Post-fix: primary_id rides the wire as u8; the client reads it
 * directly. ENTITY_SNAPSHOT_WIRE_BYTES grew 28→29; protocol id
 * bumped S0LG → S0LH (0x53304C47 → 0x53304C48).
 *
 * Net Phase 3: protocol id bumped S0LH → S0LI (0x53304C48 → 0x53304C49)
 * because NET_MSG_INPUT widened from a single record to a count-prefixed
 * batch of up to NET_INPUT_REDUNDANCY=4 inputs per datagram (server
 * dedupes by seq so packet loss doesn't desync prediction).
 *
 * M6 P01: protocol id bumped S0LI → S0LJ (0x53304C49 → 0x53304C4A)
 * because EntitySnapshot gained a `gait_phase_q` u16 between
 * `ammo_secondary` and the optional grapple suffix.
 * ENTITY_SNAPSHOT_WIRE_BYTES 29 → 31. The new field carries gait cycle
 * position so the M6 procedural pose function renders the same foot
 * frame on every client.
 *
 * M6 P02: protocol id bumped S0LJ → S0LK (0x53304C4A → 0x53304C4B)
 * for SNAP_STATE_BOOSTING at bit 14 of state_bits — drives the
 * Burst-jet plume FX spike + leading-edge SFX_JET_BOOST cue on
 * remote mechs. No wire-size change; bit lives in slots previously
 * unused at S0LJ.
 */

#include "../src/log.h"
#include "../src/snapshot.h"
#include "../src/version.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int g_failed = 0;

#define ASSERT_EQ(label, actual, expected) do {                          \
    long long __a = (long long)(actual);                                 \
    long long __e = (long long)(expected);                               \
    if (__a != __e) {                                                    \
        fprintf(stderr, "FAIL: %s: actual=%lld expected=%lld\n",         \
                label, __a, __e); ++g_failed;                            \
    } else {                                                             \
        fprintf(stdout, "ok:   %s: %lld\n", label, __a);                 \
    }                                                                    \
} while (0)

int main(void) {
    log_init("/tmp/snapshot_test.log");

    /* ---- Test 1: protocol id bumped to S0LK (M6 P02) ------------- */
    ASSERT_EQ("SOLDUT_PROTOCOL_ID == 'S0LK'", SOLDUT_PROTOCOL_ID,
              0x53304C4Bu);

    /* ---- Test 2: ENTITY_SNAPSHOT_WIRE_BYTES grew to 31 ----------- */
    ASSERT_EQ("ENTITY_SNAPSHOT_WIRE_BYTES == 31",
              ENTITY_SNAPSHOT_WIRE_BYTES, 31);

    /* ---- Test 3: round-trip with primary_id != weapon_id --------- *
     * This is the bug scenario: the host's mech has its secondary
     * slot active (e.g. swapped to Sidearm) so weapon_id (Sidearm)
     * differs from primary_id (Pulse Rifle). The wire codec must
     * carry both fields independently. */
    {
        SnapshotFrame in = {0};
        in.valid                   = true;
        in.header.server_tick      = 12345;
        in.header.server_time_ms   = 67890;
        in.header.entity_count     = 1;
        in.ent_count               = 1;
        in.ents[0].mech_id         = 0;
        in.ents[0].pos_x_q         = 100;
        in.ents[0].pos_y_q         = 200;
        in.ents[0].weapon_id       = 9;   /* Sidearm — currently active   */
        in.ents[0].primary_id      = 1;   /* Pulse Rifle — primary slot   */
        in.ents[0].secondary_id    = 9;   /* Sidearm — secondary slot     */
        in.ents[0].chassis_id      = 0;   /* Trooper                       */
        in.ents[0].armor_id        = 1;   /* Light                         */
        in.ents[0].jetpack_id      = 1;   /* Standard                      */
        in.ents[0].state_bits      = SNAP_STATE_ALIVE;
        in.ents[0].team            = 1;
        in.ents[0].health          = 200;
        in.ents[0].armor           = 100;
        in.ents[0].ammo            = 12;
        in.ents[0].ammo_secondary  = 30;
        in.ents[0].limb_bits       = 0;

        uint8_t buf[256];
        int n = snapshot_encode(&in, NULL, buf, sizeof buf);
        ASSERT_EQ("encoded byte count == header + 1 entity", n,
                  SNAPSHOT_HEADER_WIRE_BYTES + ENTITY_SNAPSHOT_WIRE_BYTES);

        SnapshotFrame out = {0};
        bool ok = snapshot_decode(buf, n, NULL, &out);
        ASSERT_EQ("snapshot_decode returned true", (int)ok, 1);
        ASSERT_EQ("decoded ent_count == 1", out.ent_count, 1);
        ASSERT_EQ("decoded primary_id == 1 (Pulse Rifle)",
                  out.ents[0].primary_id, 1);
        ASSERT_EQ("decoded secondary_id == 9 (Sidearm)",
                  out.ents[0].secondary_id, 9);
        ASSERT_EQ("decoded weapon_id == 9 (Sidearm — active)",
                  out.ents[0].weapon_id, 9);

        /* The `active_slot` derivation in snapshot_apply:
         *     active_slot = (weapon_id == primary_id) ? 0 : 1
         * With primary_id correctly round-tripped, this yields 1
         * (secondary slot is active). Pre-fix the client would have
         * inferred primary_id = weapon_id at spawn time (= 9), and the
         * same expression would yield 0 — claiming the primary slot is
         * active when in fact it's the secondary. */
        int derived_active_slot =
            (out.ents[0].weapon_id == out.ents[0].primary_id) ? 0 : 1;
        ASSERT_EQ("derived active_slot == 1 (host on secondary)",
                  derived_active_slot, 1);
    }

    /* ---- Test 4: round-trip with primary_id == weapon_id --------- *
     * The common case: mech is on its primary slot. weapon_id and
     * primary_id are both the primary's weapon. Derived active_slot
     * is 0. */
    {
        SnapshotFrame in = {0};
        in.valid               = true;
        in.ent_count           = 1;
        in.header.entity_count = 1;
        in.ents[0].weapon_id    = 1;   /* Pulse Rifle — active */
        in.ents[0].primary_id   = 1;   /* Pulse Rifle — primary */
        in.ents[0].secondary_id = 9;   /* Sidearm     — secondary */
        in.ents[0].state_bits   = SNAP_STATE_ALIVE;

        uint8_t buf[256];
        int n = snapshot_encode(&in, NULL, buf, sizeof buf);
        SnapshotFrame out = {0};
        bool ok = snapshot_decode(buf, n, NULL, &out);
        ASSERT_EQ("primary==active: decode ok", (int)ok, 1);
        ASSERT_EQ("primary==active: decoded primary_id == 1",
                  out.ents[0].primary_id, 1);
        int derived =
            (out.ents[0].weapon_id == out.ents[0].primary_id) ? 0 : 1;
        ASSERT_EQ("primary==active: derived active_slot == 0",
                  derived, 0);
    }

    /* ---- Test 5: primary_id changing across snapshots ----------- *
     * Simulates the PICKUP_WEAPON case: snapshot 1 has primary=PulseRifle,
     * snapshot 2 has primary=MassDriver. The codec must decode each
     * primary_id independently — there's no "first wins" memory in the
     * wire. */
    {
        uint8_t buf[256];
        SnapshotFrame in = {0};
        in.valid               = true;
        in.ent_count           = 1;
        in.header.entity_count = 1;
        in.ents[0].weapon_id   = 1;   /* Pulse Rifle */
        in.ents[0].primary_id  = 1;
        in.ents[0].state_bits  = SNAP_STATE_ALIVE;
        int n1 = snapshot_encode(&in, NULL, buf, sizeof buf);
        SnapshotFrame out1 = {0};
        snapshot_decode(buf, n1, NULL, &out1);
        ASSERT_EQ("snapshot 1: primary_id == 1 (Pulse Rifle)",
                  out1.ents[0].primary_id, 1);

        in.ents[0].weapon_id  = 6;    /* Mass Driver */
        in.ents[0].primary_id = 6;
        int n2 = snapshot_encode(&in, NULL, buf, sizeof buf);
        SnapshotFrame out2 = {0};
        snapshot_decode(buf, n2, NULL, &out2);
        ASSERT_EQ("snapshot 2: primary_id == 6 (Mass Driver)",
                  out2.ents[0].primary_id, 6);
    }

    /* ---- Test 6 (M6): gait_phase_q round-trips with ~16-bit precision.
     * The wire ships gait position as a u16 (1/65536 resolution). After
     * round-tripping a few representative phase values, the decoded
     * field's float value should match within 1/65536 = ~1.5e-5. */
    {
        const float phases[] = {0.0f, 0.25f, 0.5f, 0.749f, 0.999f};
        for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
            SnapshotFrame in = {0};
            in.valid               = true;
            in.ent_count           = 1;
            in.header.entity_count = 1;
            in.ents[0].state_bits  = SNAP_STATE_ALIVE;
            in.ents[0].weapon_id   = 1;
            in.ents[0].primary_id  = 1;
            /* Encode phase via the public helper would be cleaner but
             * the helper's static; use the same arithmetic the encoder
             * applies. */
            float p = phases[i];
            int v = (int)(p * 65536.0f);
            if (v < 0) v = 0;
            if (v > 65535) v = 65535;
            in.ents[0].gait_phase_q = (uint16_t)v;

            uint8_t buf[256];
            int n = snapshot_encode(&in, NULL, buf, sizeof buf);
            SnapshotFrame out = {0};
            bool ok = snapshot_decode(buf, n, NULL, &out);
            char label[96];
            snprintf(label, sizeof label,
                     "gait_phase_q round-trip at %.4f", (double)p);
            int passed = ok && out.ents[0].gait_phase_q == in.ents[0].gait_phase_q;
            if (!passed) {
                fprintf(stderr,
                        "FAIL: %s: encoded=%u decoded=%u ok=%d\n", label,
                        (unsigned)in.ents[0].gait_phase_q,
                        (unsigned)out.ents[0].gait_phase_q, (int)ok);
                ++g_failed;
            } else {
                fprintf(stdout, "ok:   %s: %u\n", label,
                        (unsigned)in.ents[0].gait_phase_q);
            }
        }
    }

    if (g_failed > 0) {
        fprintf(stderr, "\nsnapshot_test: %d failures\n", g_failed);
        return 1;
    }
    fprintf(stdout, "\nsnapshot_test: all passed\n");
    return 0;
}
