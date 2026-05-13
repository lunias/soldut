#pragma once

#include "match.h"
#include "mech.h"
#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * lobby — server-authoritative waiting-room state.
 *
 * Each connected peer occupies one LobbySlot. The host's own player
 * lives in slot 0. The server owns all of this; clients receive the
 * full slot table via NET_MSG_LOBBY_PLAYER_LIST whenever it changes
 * (small enough at 32 players that we don't bother delta-ing).
 *
 * Per documents/02-game-design.md §"Lobby flow" and §"Chat" + §"Map
 * vote". Chat is rate-limited to 1 msg/0.5 s/player; the server scrubs
 * to ASCII printable (UTF-8 deferred — see TRADE_OFFS.md if needed).
 */

/* Slots and mechs are a 1:1 mapping at M4 — every connected peer (or
 * the host's own player) gets one mech slot. world.h defines MAX_MECHS;
 * we reuse that cap so lobby.h doesn't need to include net.h. */
#define MAX_LOBBY_SLOTS    MAX_MECHS
#define LOBBY_NAME_BYTES   24
#define LOBBY_CHAT_LINES   32
#define LOBBY_CHAT_BYTES   192      /* sender prefix + body */
#define LOBBY_CHAT_RATE    0.5f     /* min seconds between messages per slot */
#define LOBBY_BANS_MAX     32

typedef struct LobbySlot {
    bool         in_use;
    int          peer_id;             /* index into NetState.peers; -1 for the host's own slot */
    int          mech_id;             /* world.mechs[] slot, or -1 outside MATCH_PHASE_ACTIVE */

    char         name[LOBBY_NAME_BYTES];
    int          team;                /* MATCH_TEAM_* */
    bool         ready;
    bool         is_host;
    MechLoadout  loadout;

    /* Round stats, reset by lobby_reset_round_stats. */
    int          score;               /* kills - team_kills - suicides */
    int          kills;
    int          deaths;
    int          team_kills;
    int          longest_streak;
    int          current_streak;

    /* Chat rate-limit. */
    float        last_chat_time;

    /* Bot fill (M6+) — server-only fields, never on the wire. Clients
     * see bots as ordinary lobby slots (the encoder ignores these).
     * `is_bot` flags slots whose `latched_input` is filled each tick
     * by bot_step instead of by a network peer; `bot_tier` selects the
     * BotPersonality preset to attach to the spawned mech. */
    bool         is_bot;
    uint8_t      bot_tier;
} LobbySlot;

typedef struct LobbyChatLine {
    char     text[LOBBY_CHAT_BYTES];
    float    age;                     /* seconds since posted; HUD uses for fade */
    int      sender_slot;             /* -1 for system */
    uint8_t  sender_team;
} LobbyChatLine;

typedef struct LobbyBan {
    bool      in_use;
    uint32_t  addr_host;              /* IPv4, network byte order */
    char      name[LOBBY_NAME_BYTES];
} LobbyBan;

/* Bans persist across host restarts via a flat `bans.txt` file next to
 * the binary (one entry per line — see lobby.c). The path is captured
 * by `lobby_load_bans` so subsequent `lobby_ban_addr` calls auto-save. */
#define LOBBY_BAN_PATH_BYTES 96

typedef struct LobbyState {
    LobbySlot      slots[MAX_LOBBY_SLOTS];
    int            slot_count;        /* number of in_use slots */

    /* Chat ring. count is monotonic; head index = count % LINES. */
    LobbyChatLine  chat[LOBBY_CHAT_LINES];
    int            chat_count;

    /* Auto-start: starts when a server has at least 50% of its expected
     * slots filled OR a host clicks "Start Now". The countdown timer
     * runs when auto_start_active is true; expires → match_begin_countdown. */
    bool           auto_start_active;
    float          auto_start_remaining;
    float          auto_start_default;

    /* Map vote. Three candidates picked by server when entering vote
     * phase; each peer votes once for vote_a / vote_b / vote_c via a
     * bit in the masks. The mask bit index = lobby slot id. */
    int            vote_map_a, vote_map_b, vote_map_c;
    uint32_t       vote_mask_a, vote_mask_b, vote_mask_c;
    bool           vote_active;
    float          vote_remaining;
    float          vote_default;

    /* Bans persist across rounds. */
    LobbyBan       bans[LOBBY_BANS_MAX];
    int            ban_count;

    /* Where to write the ban list when it changes. Set by
     * lobby_load_bans; empty until then so unit tests can use
     * lobby_ban_addr without any disk I/O. */
    char           ban_path[LOBBY_BAN_PATH_BYTES];

    /* Has the lobby state changed in a way clients need to know about
     * since the last broadcast? Set true on every mutation; cleared by
     * lobby_broadcast_if_dirty(). */
    bool           dirty;
} LobbyState;

void lobby_init(LobbyState *L, float auto_start_seconds);

/* Slot management — all return slot index on success or -1. */
int  lobby_add_slot(LobbyState *L, int peer_id, const char *name, bool is_host);
void lobby_remove_slot(LobbyState *L, int slot);
int  lobby_find_slot_by_peer(const LobbyState *L, int peer_id);
int  lobby_find_slot_by_name(const LobbyState *L, const char *name);
int  lobby_find_slot_by_mech(const LobbyState *L, int mech_id);
LobbySlot *lobby_slot(LobbyState *L, int slot);

/* Bot-fill helpers (M6+). Slots created by lobby_add_bot_slot have
 * is_bot=true and peer_id=-1 (same as the host's own slot — the
 * is_bot field distinguishes them). They count against
 * MAX_LOBBY_SLOTS and ride the wire to clients just like real peers,
 * but never have a NetState peer behind them. */
int  lobby_add_bot_slot(LobbyState *L, int bot_index, uint8_t tier);
int  lobby_bot_count(const LobbyState *L);
void lobby_clear_bot_slots(LobbyState *L);

/* Idempotent — makes the bot population match (want, tier). Adds
 * missing bot slots, removes excess, and rewrites the tier on
 * existing ones if `tier` changed. Bots are always assigned in
 * ascending bot-index order so names stay stable across calls.
 * `team_balance` true alternates RED/BLUE for TDM/CTF; false
 * leaves the FFA default. Caller marks `dirty` if anything changed
 * via the standard mutation paths. Safe to call from the lobby UI
 * tick, bootstrap_host, dedicated_run, and start_round. */
void lobby_apply_bot_fill(LobbyState *L, int want, uint8_t tier,
                          bool team_balance);

/* Mutators — these set `dirty` so the change ships on the next broadcast. */
void lobby_set_loadout(LobbyState *L, int slot, MechLoadout lo);
void lobby_set_ready  (LobbyState *L, int slot, bool ready);
void lobby_set_team   (LobbyState *L, int slot, int team);
void lobby_set_mech   (LobbyState *L, int slot, int mech_id);

/* Returns true if `text` was accepted (rate-limit / length / sanitize ok)
 * AND added to the ring. The text is server-side scrubbed (printable
 * ASCII only). System messages can be added with sender_slot = -1. */
bool lobby_chat_post(LobbyState *L, int sender_slot, const char *text,
                     float server_time);
void lobby_chat_system(LobbyState *L, const char *text);

/* Map vote workflow. */
void lobby_vote_start(LobbyState *L, int a, int b, int c, float duration);
void lobby_vote_cast (LobbyState *L, int slot, int choice /*0/1/2*/);
int  lobby_vote_winner(const LobbyState *L);    /* the map_id with the most votes */
void lobby_vote_clear(LobbyState *L);

/* Auto-start. Caller decides when to enter (e.g. when N>=2 slots filled).
 * Tick advances the countdown; returns true when it expires. */
void lobby_auto_start_arm   (LobbyState *L, float seconds);
void lobby_auto_start_cancel(LobbyState *L);
bool lobby_tick(LobbyState *L, float dt);

/* Returns true if every in-use, non-spectator slot is ready. */
bool lobby_all_ready(const LobbyState *L);

/* Reset per-round stats (kills/deaths/score) on every slot. */
void lobby_reset_round_stats(LobbyState *L);

/* Bans. address = network byte order IPv4. Name match is case-insensitive
 * substring; address match is exact. */
bool lobby_is_banned    (const LobbyState *L, uint32_t addr, const char *name);
void lobby_ban_addr     (LobbyState *L, uint32_t addr, const char *name);

/* Persistence: read `path` into L->bans (one entry per line:
 *   "<addr_hex> <name>"
 * with addr_hex an 8-char hex u32, "00000000" for name-only bans).
 * Records the path on L for subsequent auto-save from lobby_ban_addr.
 * Missing file is OK — first-time hosts just have no bans on file. */
void lobby_load_bans    (LobbyState *L, const char *path);
void lobby_save_bans    (const LobbyState *L, const char *path);

/* Wire encoding for NET_MSG_LOBBY_PLAYER_LIST.
 *
 * Layout per slot (always sent for the full table, fixed-width):
 *   u8  in_use
 *   u8  team
 *   u8  ready
 *   u8  is_host
 *   u8  chassis_id u8 primary_id u8 secondary_id u8 armor_id u8 jetpack_id
 *   i16 score   i16 kills   i16 deaths
 *   bytes[24] name
 *   i8  mech_id     (-1 = not in a round)
 *
 * Per slot = 9 bytes (u8 fields) + 6 bytes (3× i16) + 24 (name) + 1
 * (mech_id) = 40 bytes. 32 slots = 1280 bytes. Adds a 2-byte header
 * (slot_count). The total list payload is 1282 bytes.
 *
 * mech_id rides the wire so the client can resolve its slot to a
 * world.mechs[] index after ROUND_START. Without it the client never
 * knows which snapshot mech is "theirs", and the camera never follows
 * anyone. (Fixed an early-M4 black-screen bug — see CURRENT_STATE.) */
#define LOBBY_SLOT_WIRE_BYTES   40
#define LOBBY_LIST_WIRE_BYTES   (2 + LOBBY_SLOT_WIRE_BYTES * MAX_LOBBY_SLOTS)

void lobby_encode_list(const LobbyState *L, uint8_t *out);
void lobby_decode_list(LobbyState *L, const uint8_t *in);

/* Compact encoding for an individual slot — used for JOINED / UPDATE
 * deltas. */
#define LOBBY_SLOT_DELTA_WIRE_BYTES  (1 + LOBBY_SLOT_WIRE_BYTES)
void lobby_encode_slot(const LobbyState *L, int slot, uint8_t *out);
void lobby_decode_slot(LobbyState *L, const uint8_t *in);

/* Encode/decode one chat line. */
#define LOBBY_CHAT_WIRE_BYTES  (1 + 1 + LOBBY_CHAT_BYTES)
void lobby_encode_chat_line(const LobbyChatLine *line, uint8_t *out);
void lobby_decode_chat_line(LobbyChatLine *line, const uint8_t *in);

/* Per-tick chat aging (HUD reads `age` to fade lines out). */
void lobby_chat_age(LobbyState *L, float dt);

/* Spawn one mech for each active slot on the (server-side) world.
 * Updates each slot's `mech_id` so the server can route inputs and
 * kill-feed events back to the right slot. Caller must have built the
 * level for `map_id` already. Sets world->local_mech_id from the
 * supplied `local_slot` (or -1 for pure-server use).
 *
 * After this, mech_count == number of in-use slots.
 */
struct World;
/* `mode` selects the spawn-lane bias: in FFA we spread players across
 * the map; in TDM/CTF we honor each slot's team for red/blue sides. */
void lobby_spawn_round_mechs(LobbyState *L, struct World *world,
                             int map_id, int local_slot, MatchModeId mode);

/* Inverse: tear down all mechs spawned for the round, clear each
 * slot's mech_id, and reset the world's mech/particle/constraint
 * counts back to zero. Called when transitioning out of an active
 * round so the next round starts with a clean pool. */
void lobby_clear_round_mechs(LobbyState *L, struct World *world);
