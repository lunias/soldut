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

/* Per-mech record.
 *   M3 widened to 27 bytes to carry the loadout (chassis + armor +
 *     jetpack + secondary).
 *   P03 widens state_bits from u8 → u16 to make room for
 *     SNAP_STATE_IS_DUMMY at bit 11 (bits 0..7 are the M2 set; bits
 *     8..15 are P03+). Wire size grows by 1 → 28 bytes.
 *
 * The loadout is technically static for the life of a mech, but we
 * ship it every snapshot for simplicity — the bandwidth cost is small
 * (~5 bytes × 32 mechs × 30 Hz = 4.8 KB/s) and it lets a mid-stream
 * client see correct stats without waiting on a separate reliable
 * message.
 *
 * Quantization:
 *   pos_x_q, pos_y_q   → 1/4 px      (16-bit signed covers ±8190 px)
 *   vel_x_q, vel_y_q   → 1/16 px/tick
 *   aim_q              → uint16 fraction of 2π
 *   torso_q            → uint16 fraction of 2π
 *   health             → uint8 (0..255 fraction of health_max)
 *   armor              → uint8 (0..255 fraction of armor_max)
 *   weapon_id          → uint8 (active slot's weapon)
 *   ammo               → uint8 (active slot's ammo)
 *   state_bits         → uint16 (see SNAP_STATE_* below)
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
    uint16_t state_bits;
    uint8_t  team;
    uint16_t limb_bits;
    uint8_t  chassis_id;
    uint8_t  armor_id;
    uint8_t  jetpack_id;
    uint8_t  primary_id;        /* primary slot's weapon — distinct from `weapon_id`
                                 * (which is the active slot's). Pre-S0LH this was
                                 * inferred client-side from the first snapshot's
                                 * weapon_id, which broke on mid-round join when
                                 * the host had its secondary equipped at connect
                                 * time, AND on mid-round PICKUP_WEAPON swaps. */
    uint8_t  secondary_id;
    uint8_t  ammo_secondary;
    /* M6 — Gait cycle position in [0, 1). 0 = stride start. Quantized
     * to u16 with quant_phase / dequant_phase (1/65536 px resolution).
     * The procedural pose function in mech_ik.c reads this directly to
     * drive the RUN gait so every client renders the same foot frame
     * at the same tick. Outside ANIM_RUN, the field is 0 (so non-RUN
     * mechs don't accidentally trigger gait motion via stale data). */
    uint16_t gait_phase_q;
    /* P06 — Grapple suffix (only present on the wire when state_bits
     * has SNAP_STATE_GRAPPLING set). When the bit is clear, these
     * fields are zero. Idle (state == GRAPPLE_IDLE) keeps the suffix
     * absent so idle bandwidth stays flat. */
    uint8_t  grapple_state;
    int8_t   grapple_anchor_mech;
    uint8_t  grapple_anchor_part;
    int16_t  grapple_anchor_x_q;
    int16_t  grapple_anchor_y_q;
} EntitySnapshot;

/* On-wire size of one EntitySnapshot.
 *   M3 = 22 (M2 size) + 5 = 27 bytes.
 *   P03 widens state_bits u8 → u16 = 28 bytes.
 *   P06 adds an OPTIONAL 8-byte grapple suffix gated by
 *     SNAP_STATE_GRAPPLING; the fixed-width minimum stays 28.
 *   P10-followup adds primary_id u8 = 29 bytes; protocol id bumps
 *     S0LG → S0LH (see version.h).
 *   M6 adds gait_phase_q u16 = 31 bytes; protocol id bumps S0LI →
 *     S0LJ. Phase rides every entity record (not just RUN-anim
 *     mechs) so the decoder layout stays fixed-width. */
#define ENTITY_SNAPSHOT_WIRE_BYTES         31
#define ENTITY_SNAPSHOT_GRAPPLE_BYTES       8

/* ---- ProjectileSnapshot (M6 P12) ---------------------------------- *
 *
 * One entry per replicated projectile in the snapshot stream, appended
 * after the EntitySnapshot array as `u16 proj_count + N × 14 bytes`.
 *
 * Today only bouncy projectiles ride here (PROJ_FRAG_GRENADE — see
 * spec doc 12-projectile-snapshot-replication.md for why). Non-bouncy
 * AOE projectiles (rockets, plasma orbs) follow deterministic
 * gravity+drag trajectories with no surface reflections, so client/
 * server divergence stays within a pixel or two and the existing
 * FIRE_EVENT + NET_MSG_EXPLOSION dedup is sufficient. The wire format
 * is generic on `kind` so adding more kinds later is a server-side
 * filter change.
 *
 * Quantization mirrors EntitySnapshot — pos: 1/4 px, vel: 1/16 px/tick
 * — so the dequant helpers in snapshot.c are reused.
 *
 * Wire layout (14 bytes, little-endian):
 *   u16 net_id        — stable per-projectile id (0 reserved for
 *                       "unassigned"; server's monotonic counter
 *                       skips 0 on wrap).
 *   u8  kind          — ProjectileKind enum.
 *   u8  owner_mech    — mech_id (0..MAX_MECHS-1).
 *   i16 pos_x_q       — 1/4 px.
 *   i16 pos_y_q
 *   i16 vel_x_q       — 1/16 px/tick.
 *   i16 vel_y_q
 *   u8  flags         — bit 0: bouncy, bit 1: exploded
 *                       (forward-compat hook — server populates the
 *                       bouncy bit so a future client that decides
 *                       on bouncy at receive time can route without
 *                       re-reading the weapon table).
 *   u8  fuse_ticks    — remaining `life * 60` clamped to 255. Used
 *                       by the client to fade the sprite as the fuse
 *                       runs out (purely cosmetic — server still owns
 *                       the kill).
 */
typedef struct {
    uint16_t net_id;
    uint8_t  kind;
    uint8_t  owner_mech;
    int16_t  pos_x_q;
    int16_t  pos_y_q;
    int16_t  vel_x_q;
    int16_t  vel_y_q;
    uint8_t  flags;
    uint8_t  fuse_ticks;
} ProjectileSnapshot;

#define PROJECTILE_SNAPSHOT_WIRE_BYTES 14

enum {
    PROJ_SNAP_F_BOUNCY   = 1u << 0,
    PROJ_SNAP_F_EXPLODED = 1u << 1,
};

/* Cap on projectiles per snapshot. Worst-case `64 × 14 = 896 bytes` of
 * projectile payload plus the existing mech entries. At realistic
 * combat density (4-8 mechs, ≤10 grenades airborne) we ship ~300 bytes
 * total — comfortably under typical MTU. The cap is a safety bound for
 * adversarial bot-spam scenarios; over it the server drops the oldest
 * alive projectiles silently. */
#define SNAPSHOT_PROJECTILE_CAP 64

enum {
    /* Original 8-bit set (M2). */
    SNAP_STATE_ALIVE       = 1u << 0,
    SNAP_STATE_JET         = 1u << 1,
    SNAP_STATE_CROUCH      = 1u << 2,
    SNAP_STATE_PRONE       = 1u << 3,
    SNAP_STATE_FIRE        = 1u << 4,
    SNAP_STATE_RELOAD      = 1u << 5,
    SNAP_STATE_GROUNDED    = 1u << 6,
    SNAP_STATE_FACING_LEFT = 1u << 7,
    /* Upper byte (P03+). P05 spends bits 8/9/10 on powerup state — the
     * timer ticks server-side; clients set their local timer to a
     * sentinel value while the bit is observed so render can read
     * "alpha-mod for invis", etc. without floats riding the wire. */
    SNAP_STATE_BERSERK     = 1u <<  8,    /* powerup: 2× outgoing damage */
    SNAP_STATE_INVIS       = 1u <<  9,    /* powerup: render alpha-mod */
    SNAP_STATE_GODMODE     = 1u << 10,    /* powerup: ignore incoming damage */
    SNAP_STATE_IS_DUMMY    = 1u << 11,    /* practice dummy — skips arm-aim drive */
    SNAP_STATE_GRAPPLING   = 1u << 12,    /* P06: trailing 8-byte grapple suffix follows entity record */
    /* wan-fixes-3 followup — server-authoritative "is running" flag.
     * Set when the server's anim_id == ANIM_RUN (i.e., the player is
     * intentionally walking, not just briefly mid-gait-lift). Pre-
     * existing client derivation from velocity flickered between RUN
     * and STAND during stride transitions because vx briefly dipped
     * below the threshold; reading this bit makes the client's
     * anim_id mirror the server's deterministically. Additive bit;
     * old clients ignore it and fall back to the velocity heuristic. */
    SNAP_STATE_RUNNING     = 1u << 13,
    /* M6 P02 — Burst-jet boost active. Set when m->boost_timer > 0.0f
     * at snapshot-record time. Drives:
     *  (a) the 8× exhaust-particle spike on the plume FX, and
     *  (b) the leading-edge SFX_JET_BOOST trigger on remote mechs in
     *      snapshot_apply (the SFX for owner-side mechs fires from
     *      mech_step_drive directly).
     * Boost timer decays locally on each side so we only need the
     * bit, not a timer field. */
    SNAP_STATE_BOOSTING    = 1u << 14,
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
    SnapshotHeader     header;
    EntitySnapshot     ents[MAX_MECHS];
    int                ent_count;
    /* M6 P12 — Replicated projectile array. Same lifetime as `ents`. */
    ProjectileSnapshot projs[SNAPSHOT_PROJECTILE_CAP];
    int                proj_count;
    bool               valid;
    double             recv_time;     /* client-side: real seconds when received */
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
 * the frame, mark them dead.
 *
 * The LOCAL mech is fully snapped (reconcile.c replays unacked inputs
 * after to bring it forward to "now"). REMOTE mechs do NOT snap their
 * positions here — instead, the snapshot's pelvis pos+vel is pushed to
 * the per-mech ring (Mech.remote_snap_ring). Position is written each
 * tick by snapshot_interp_remotes, lerping between the two bracketing
 * ring entries at `render_time_ms - INTERP_DELAY_MS`. Health, state
 * bits, ammo, etc. apply to all mechs unconditionally.
 *
 * Large remote-mech corrections (>200 px — likely a respawn) clear
 * the ring and snap fully so we don't slowly slide across the level. */
void snapshot_apply(World *w, const SnapshotFrame *frame);

/* wan-fixes-20 — push pelvis pos/vel from `frame` into each remote
 * mech's per-mech interp ring WITHOUT touching local-mech reconcile
 * state, health/weapon/team/aim fields, ensure_mech_slot spawn paths,
 * or the stale-sweep that kills unseen mechs. Designed for snapshots
 * that arrived out of order (their server_time is older than the
 * latest seen): we still want them in the ring so pick_bracket has
 * a richer set of samples for forward interpolation, but we MUST
 * NOT regress health/weapon/team back to the stale state nor confuse
 * reconcile by replaying inputs from a stale ack point.
 *
 * Caller is responsible for the staleness gate — typically "older
 * than latest but within REMOTE_SNAP_STALE_MAX_MS" (see net.c
 * client_handle_snapshot). */
void snapshot_apply_remote_ring_only(World *w, const SnapshotFrame *frame);

/* Max staleness (ms behind the latest-seen server_time) we'll accept
 * a reordered snapshot for. Past this, the ring's oldest entry is
 * newer than the incoming one and there's no useful insertion to
 * make. At 60 Hz snapshots × 8-entry ring that's 133 ms of span;
 * 250 ms gives comfortable headroom for bursty WAN reorder
 * (the worst observed in the 2026-05-15 MN ↔ AZ playtest was
 * ~50 ms windows). */
#define REMOTE_SNAP_STALE_MAX_MS 250u

/* P03 — interpolate remote mechs between bracketing ring entries.
 * `render_time_ms` is the client's local render clock — typically
 * `latest_server_time_ms - INTERP_DELAY_MS`, advanced each tick by
 * dt_ms to keep motion smooth between snapshot arrivals. Skips the
 * local mech and any mech whose ring is empty. */
void snapshot_interp_remotes(World *w, uint32_t render_time_ms);

/* M6 P12 — interpolate snapshot-replicated projectiles. Walks
 * `world.projectiles` for alive slots with `net_id != 0` (= server-
 * replicated) and writes pos_x/pos_y + vel_x/vel_y from the
 * per-projectile (snap_a, snap_b) pair at render_time_ms. Slots with
 * net_id == 0 (locally-spawned, non-replicated projectiles — non-
 * bouncy AOE / pellets / hitscan tracers in the future) are skipped
 * untouched. Should be called each client sim tick AFTER
 * snapshot_interp_remotes and BEFORE the next simulate_step's
 * projectile_step. */
void snapshot_interp_projectiles(World *w, uint32_t render_time_ms);

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
