#pragma once

#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Snapshot encode/decode and history.
 *
 * A snapshot is the world state at server tick T, expressed as the
 * union of every alive mech's compact 22-byte EntitySnapshot record
 * plus a small header. We delta-encode each mech against the most
 * recent baseline the receiving client has acked (32-bit dirty mask
 * up front, only-changed fields after).
 *
 * On the server we cache the latest broadcast snapshot per peer; when
 * the peer acks it, that becomes the baseline against which the next
 * snapshot is delta'd. On the client we keep a small ring of received
 * snapshots so render can interpolate remote mechs ~100 ms in the past.
 */

/* ---- Wire format (raw, fixed-width, little-endian) --------------- */

/* Header that precedes the entity stream. (NET_MSG_SNAPSHOT tag
 * byte is *outside* this — written by net.c.)
 *
 *   server_tick      8B   monotonic
 *   server_time_ms   4B   ms since server_start (for client smoothing)
 *   baseline_tick    8B   tick we delta'd against, or 0 for full snapshot
 *   ack_input_seq    2B   last input seq the server has processed for THIS peer
 *   entity_count     2B   how many EntitySnapshot records follow
 *
 * Total: 24 bytes header. */
/* Note: this is the in-memory representation. The wire format is
 * written field-by-field by snapshot_encode (so we don't depend on
 * compiler struct layout). On the wire it's 24 bytes. */
typedef struct {
    uint64_t server_tick;
    uint32_t server_time_ms;
    uint64_t baseline_tick;
    uint16_t ack_input_seq;
    uint16_t entity_count;
} SnapshotHeader;

#define SNAPSHOT_HEADER_WIRE_BYTES 24

/* Per-mech record. M3 widens this from M2's 22 bytes to 27 bytes to
 * carry the loadout (chassis + armor + jetpack + secondary). The
 * loadout is technically static for the life of a mech, but we ship it
 * every snapshot for simplicity — the bandwidth cost is small (5 bytes
 * × 32 mechs × 30 Hz = 4.8 KB/s) and it lets a mid-stream client see
 * correct stats without waiting on a separate reliable message.
 *
 * Quantization:
 *   pos_x_q, pos_y_q   → 1/8 px      (16-bit signed covers ±4096 px)
 *   vel_x_q, vel_y_q   → 1/16 px/tick
 *   aim_q              → uint16 fraction of 2π
 *   torso_q            → uint16 fraction of 2π
 *   health             → uint8 (0..255 fraction of health_max)
 *   armor              → uint8 (0..255 fraction of armor_max)
 *   weapon_id          → uint8 (active slot's weapon)
 *   ammo               → uint8 (active slot's ammo)
 *   state_bits         → 1 alive | 2 jet | 4 crouch | 8 prone | 16 fire
 *                        | 32 reload | 64 grounded | 128 facing_left
 *   team               → uint8
 *   limb_bits          → uint16 (LIMB_* flags)
 *   chassis_id         → uint8
 *   armor_id           → uint8
 *   jetpack_id         → uint8
 *   secondary_id       → uint8
 *   ammo_secondary     → uint8
 */
typedef struct {
    uint16_t mech_id;
    int16_t  pos_x_q, pos_y_q;
    int16_t  vel_x_q, vel_y_q;
    uint16_t aim_q;
    uint16_t torso_q;
    uint8_t  health;
    uint8_t  armor;
    uint8_t  weapon_id;
    uint8_t  ammo;
    uint8_t  state_bits;
    uint8_t  team;
    uint16_t limb_bits;
    uint8_t  chassis_id;
    uint8_t  armor_id;
    uint8_t  jetpack_id;
    uint8_t  secondary_id;
    uint8_t  ammo_secondary;
} EntitySnapshot;

/* On-wire size of one EntitySnapshot. M3 = 22 (M2 size) + 5 = 27 bytes. */
#define ENTITY_SNAPSHOT_WIRE_BYTES 27

enum {
    SNAP_STATE_ALIVE       = 1u << 0,
    SNAP_STATE_JET         = 1u << 1,
    SNAP_STATE_CROUCH      = 1u << 2,
    SNAP_STATE_PRONE       = 1u << 3,
    SNAP_STATE_FIRE        = 1u << 4,
    SNAP_STATE_RELOAD      = 1u << 5,
    SNAP_STATE_GROUNDED    = 1u << 6,
    SNAP_STATE_FACING_LEFT = 1u << 7,
};

/* Bits used in the per-entity dirty mask when delta-encoding. (We
 * write the mask as a single uint16_t before each entity in delta
 * mode.) When baseline_tick == 0 the snapshot is a full snapshot —
 * no mask, all fields present. */
enum {
    SNAP_DIRTY_POS    = 1u << 0,
    SNAP_DIRTY_VEL    = 1u << 1,
    SNAP_DIRTY_AIM    = 1u << 2,
    SNAP_DIRTY_TORSO  = 1u << 3,
    SNAP_DIRTY_HEALTH = 1u << 4,
    SNAP_DIRTY_ARMOR  = 1u << 5,
    SNAP_DIRTY_WEAPON = 1u << 6,
    SNAP_DIRTY_AMMO   = 1u << 7,
    SNAP_DIRTY_STATE  = 1u << 8,
    SNAP_DIRTY_TEAM   = 1u << 9,
    SNAP_DIRTY_LIMB   = 1u << 10,
};

/* In-memory copy of one snapshot — the server caches one of these per
 * peer for delta encoding; the client keeps a ring of these for
 * interpolation. */
typedef struct {
    SnapshotHeader  header;
    EntitySnapshot  ents[MAX_MECHS];
    int             ent_count;
    bool            valid;
    double          recv_time;     /* client-side: real seconds when received */
} SnapshotFrame;

/* ---- Sample a SnapshotFrame from the live World (server-side) ----- */

void snapshot_capture(const World *w, SnapshotFrame *out, uint16_t ack_input_seq);

/* ---- Encode/decode the wire bytes ------------------------------- */

/* Encode `cur` into `buf`, optionally delta'd against `baseline` (NULL
 * = full snapshot). Returns the number of bytes written, or 0 on
 * overflow. The buffer should be sized for the worst case
 * (sizeof(SnapshotHeader) + MAX_MECHS * (sizeof(EntitySnapshot) + 2)). */
int  snapshot_encode(const SnapshotFrame *cur,
                     const SnapshotFrame *baseline,
                     uint8_t *buf, int buf_cap);

/* Decode wire bytes into `out`. If the snapshot is a delta,
 * `baseline` must be supplied; otherwise pass NULL. Returns true on
 * success. */
bool snapshot_decode(const uint8_t *buf, int len,
                     const SnapshotFrame *baseline,
                     SnapshotFrame *out);

/* ---- Apply a snapshot to a client's World ------------------------- */

/* Overwrite the world's mech states from `frame`. For mechs we don't
 * have yet locally, spawn them. For mechs in the world that aren't in
 * the frame, mark them dead. The local mech (id == local_mech_id) is
 * also written, but reconcile.c will replay subsequent inputs after
 * this call to bring it up to "now."
 *
 * `is_local_predict` should be true for replay calls (do not record
 * lag history); false on a freshly received snapshot. */
void snapshot_apply(World *w, const SnapshotFrame *frame);

/* ---- Lag-comp lookup (server-side) ------------------------------- */

/* Returns true if `target_mech_id` has bone history covering
 * `target_tick`; writes the historical positions for each part into
 * `out_x` / `out_y` (length PART_COUNT). Used by the server hitscan
 * path to rewind to the shooter's perceived state. */
bool snapshot_lag_lookup(const World *w, int target_mech_id,
                         uint64_t target_tick,
                         float *out_x, float *out_y);

/* Record bone history for one mech at the current world tick. Called
 * on the server at end-of-tick. */
void snapshot_record_lag_hist(World *w, int mech_id);
