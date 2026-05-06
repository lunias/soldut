#include "lobby.h"

#include "log.h"
#include "maps.h"
#include "match.h"
#include "mech.h"
#include "world.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void lobby_init(LobbyState *L, float auto_start_seconds) {
    memset(L, 0, sizeof *L);
    L->auto_start_default = (auto_start_seconds > 0.0f) ? auto_start_seconds : 60.0f;
    L->vote_default       = 15.0f;
    L->vote_map_a = L->vote_map_b = L->vote_map_c = -1;
    L->dirty = true;
}

/* ---- Slot management ---------------------------------------------- */

int lobby_add_slot(LobbyState *L, int peer_id, const char *name, bool is_host) {
    /* If a slot already exists for this peer, return it (idempotent on
     * reconnect within the same lobby). */
    int found = lobby_find_slot_by_peer(L, peer_id);
    if (found >= 0) return found;

    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        if (L->slots[i].in_use) continue;
        LobbySlot *s = &L->slots[i];
        memset(s, 0, sizeof *s);
        s->in_use   = true;
        s->peer_id  = peer_id;
        s->mech_id  = -1;
        s->team     = MATCH_TEAM_FFA;     /* default; FFA mode treats as team 1 */
        s->is_host  = is_host;
        s->loadout  = mech_default_loadout();
        snprintf(s->name, sizeof s->name, "%s", name && *name ? name : "player");
        s->name[sizeof s->name - 1] = '\0';
        L->slot_count++;
        L->dirty = true;
        return i;
    }
    return -1;
}

void lobby_remove_slot(LobbyState *L, int slot) {
    if (slot < 0 || slot >= MAX_LOBBY_SLOTS) return;
    LobbySlot *s = &L->slots[slot];
    if (!s->in_use) return;
    memset(s, 0, sizeof *s);
    s->peer_id = -1;
    s->mech_id = -1;
    if (L->slot_count > 0) L->slot_count--;
    L->dirty = true;
}

int lobby_find_slot_by_peer(const LobbyState *L, int peer_id) {
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        if (L->slots[i].in_use && L->slots[i].peer_id == peer_id) return i;
    }
    return -1;
}

int lobby_find_slot_by_name(const LobbyState *L, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        if (!L->slots[i].in_use) continue;
        if (strcasecmp(L->slots[i].name, name) == 0) return i;
    }
    return -1;
}

int lobby_find_slot_by_mech(const LobbyState *L, int mech_id) {
    if (mech_id < 0) return -1;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        if (L->slots[i].in_use && L->slots[i].mech_id == mech_id) return i;
    }
    return -1;
}

LobbySlot *lobby_slot(LobbyState *L, int slot) {
    if (slot < 0 || slot >= MAX_LOBBY_SLOTS) return NULL;
    return L->slots[slot].in_use ? &L->slots[slot] : NULL;
}

/* ---- Setters ------------------------------------------------------- */

void lobby_set_loadout(LobbyState *L, int slot, MechLoadout lo) {
    LobbySlot *s = lobby_slot(L, slot);
    if (!s) return;
    s->loadout = lo;
    L->dirty = true;
}

void lobby_set_ready(LobbyState *L, int slot, bool ready) {
    LobbySlot *s = lobby_slot(L, slot);
    if (!s) return;
    if (s->ready == ready) return;
    s->ready = ready;
    L->dirty = true;
}

void lobby_set_team(LobbyState *L, int slot, int team) {
    LobbySlot *s = lobby_slot(L, slot);
    if (!s) return;
    if (team < 0 || team >= MATCH_TEAM_COUNT) return;
    if (s->team == team) return;
    s->team = team;
    L->dirty = true;
}

void lobby_set_mech(LobbyState *L, int slot, int mech_id) {
    LobbySlot *s = lobby_slot(L, slot);
    if (!s) return;
    if (s->mech_id == mech_id) return;
    s->mech_id = mech_id;
    /* mech_id rides the slot wire — clients need it to resolve their
     * own mech after ROUND_START. Mark dirty so the next net_poll
     * broadcasts the updated table. */
    L->dirty = true;
}

/* ---- Chat ---------------------------------------------------------- */

static void scrub_ascii(char *out, int out_cap, const char *in) {
    if (out_cap <= 0) return;
    int n = 0;
    for (const char *p = in; *p && n + 1 < out_cap; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\t') c = ' ';
        if (c < 32 || c > 126) continue;
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

bool lobby_chat_post(LobbyState *L, int sender_slot, const char *text,
                     float server_time)
{
    if (!text || !*text) return false;
    /* Rate-limit per slot (system messages bypass). */
    if (sender_slot >= 0) {
        LobbySlot *s = lobby_slot(L, sender_slot);
        if (!s) return false;
        if (server_time - s->last_chat_time < LOBBY_CHAT_RATE) return false;
        s->last_chat_time = server_time;
    }

    /* Scrub into a buffer small enough that "<name>: <body>" still
     * fits in line->text without truncation warnings. Reserve ~32 chars
     * for the sender prefix. */
    char body[LOBBY_CHAT_BYTES - 32];
    scrub_ascii(body, sizeof body, text);
    if (body[0] == '\0') return false;

    int idx = L->chat_count % LOBBY_CHAT_LINES;
    LobbyChatLine *line = &L->chat[idx];
    memset(line, 0, sizeof *line);
    line->sender_slot = sender_slot;
    line->age         = 0.0f;

    if (sender_slot < 0) {
        snprintf(line->text, sizeof line->text, "* %s", body);
        line->sender_team = 0;
    } else {
        const LobbySlot *s = &L->slots[sender_slot];
        line->sender_team = (uint8_t)s->team;
        snprintf(line->text, sizeof line->text, "%s: %s", s->name, body);
    }
    L->chat_count++;
    return true;
}

void lobby_chat_system(LobbyState *L, const char *text) {
    lobby_chat_post(L, -1, text, 0.0f);
}

void lobby_chat_age(LobbyState *L, float dt) {
    int n = (L->chat_count < LOBBY_CHAT_LINES) ? L->chat_count : LOBBY_CHAT_LINES;
    for (int i = 0; i < n; ++i) L->chat[i].age += dt;
}

/* ---- Map vote ----------------------------------------------------- */

void lobby_vote_start(LobbyState *L, int a, int b, int c, float duration) {
    L->vote_map_a = a; L->vote_map_b = b; L->vote_map_c = c;
    L->vote_mask_a = L->vote_mask_b = L->vote_mask_c = 0;
    L->vote_active = true;
    L->vote_remaining = (duration > 0.0f) ? duration : L->vote_default;
    L->dirty = true;
}

void lobby_vote_cast(LobbyState *L, int slot, int choice) {
    if (!L->vote_active) return;
    if (slot < 0 || slot >= MAX_LOBBY_SLOTS) return;
    if (!L->slots[slot].in_use) return;
    uint32_t bit = 1u << slot;
    L->vote_mask_a &= ~bit;
    L->vote_mask_b &= ~bit;
    L->vote_mask_c &= ~bit;
    if (choice == 0) L->vote_mask_a |= bit;
    if (choice == 1) L->vote_mask_b |= bit;
    if (choice == 2) L->vote_mask_c |= bit;
    L->dirty = true;
}

static int popcount32(uint32_t x) {
    int n = 0;
    while (x) { n += (int)(x & 1u); x >>= 1; }
    return n;
}

int lobby_vote_winner(const LobbyState *L) {
    int va = popcount32(L->vote_mask_a);
    int vb = popcount32(L->vote_mask_b);
    int vc = popcount32(L->vote_mask_c);
    if (va == 0 && vb == 0 && vc == 0) {
        /* No votes — fall back to whichever slot is set. */
        return (L->vote_map_a >= 0) ? L->vote_map_a :
               (L->vote_map_b >= 0) ? L->vote_map_b :
                                      L->vote_map_c;
    }
    if (va >= vb && va >= vc) return L->vote_map_a;
    if (vb >= va && vb >= vc) return L->vote_map_b;
    return L->vote_map_c;
}

void lobby_vote_clear(LobbyState *L) {
    L->vote_active = false;
    L->vote_remaining = 0.0f;
    L->vote_mask_a = L->vote_mask_b = L->vote_mask_c = 0;
}

/* ---- Auto-start --------------------------------------------------- */

void lobby_auto_start_arm(LobbyState *L, float seconds) {
    if (L->auto_start_active) return;
    L->auto_start_active    = true;
    L->auto_start_remaining = (seconds > 0.0f) ? seconds : L->auto_start_default;
    L->dirty = true;
}

void lobby_auto_start_cancel(LobbyState *L) {
    if (!L->auto_start_active) return;
    L->auto_start_active    = false;
    L->auto_start_remaining = 0.0f;
    L->dirty = true;
}

bool lobby_tick(LobbyState *L, float dt) {
    bool fired = false;
    if (L->auto_start_active) {
        L->auto_start_remaining -= dt;
        if (L->auto_start_remaining <= 0.0f) {
            L->auto_start_remaining = 0.0f;
            L->auto_start_active    = false;
            fired = true;
        }
    }
    if (L->vote_active) {
        L->vote_remaining -= dt;
        if (L->vote_remaining <= 0.0f) L->vote_remaining = 0.0f;
    }
    return fired;
}

bool lobby_all_ready(const LobbyState *L) {
    int active = 0, ready = 0;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        const LobbySlot *s = &L->slots[i];
        if (!s->in_use) continue;
        if (s->team == MATCH_TEAM_NONE) continue;   /* spectators don't count */
        active++;
        if (s->ready) ready++;
    }
    return active > 0 && ready == active;
}

void lobby_reset_round_stats(LobbyState *L) {
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        if (!L->slots[i].in_use) continue;
        L->slots[i].score          = 0;
        L->slots[i].kills          = 0;
        L->slots[i].deaths         = 0;
        L->slots[i].team_kills     = 0;
        L->slots[i].longest_streak = 0;
        L->slots[i].current_streak = 0;
        L->slots[i].ready          = false;
    }
    L->dirty = true;
}

/* ---- Bans --------------------------------------------------------- */

bool lobby_is_banned(const LobbyState *L, uint32_t addr, const char *name) {
    for (int i = 0; i < L->ban_count; ++i) {
        const LobbyBan *b = &L->bans[i];
        if (!b->in_use) continue;
        if (b->addr_host == addr && addr != 0) return true;
        if (name && b->name[0] && strcasecmp(b->name, name) == 0) return true;
    }
    return false;
}

void lobby_ban_addr(LobbyState *L, uint32_t addr, const char *name) {
    if (lobby_is_banned(L, addr, name)) return;
    if (L->ban_count >= LOBBY_BANS_MAX) {
        LOG_W("lobby_ban: ban list full (%d), dropping oldest", LOBBY_BANS_MAX);
        memmove(&L->bans[0], &L->bans[1], sizeof L->bans[0] * (LOBBY_BANS_MAX - 1));
        L->ban_count--;
    }
    LobbyBan *b = &L->bans[L->ban_count++];
    memset(b, 0, sizeof *b);
    b->in_use    = true;
    b->addr_host = addr;
    if (name) {
        snprintf(b->name, sizeof b->name, "%s", name);
        b->name[sizeof b->name - 1] = '\0';
    }
    /* Auto-persist when a path was registered via lobby_load_bans. */
    if (L->ban_path[0]) lobby_save_bans(L, L->ban_path);
}

/* ---- Bans: persistence (P09) -------------------------------------- *
 *
 * Format: one ban per line, "<addr_hex> <name>" — addr_hex is an 8-char
 * hex u32 ("00000000" for name-only bans, since the current kick/ban
 * path passes addr=0). Trailing whitespace and blank lines are
 * tolerated. Lines starting with '#' are comments. The file is
 * rewritten in full on every change (the ban list caps at 32 entries,
 * so this is trivial). Atomic-write isn't worth the complexity here:
 * losing one ban entry on a host-crash mid-write is acceptable; the
 * trade-off doc was up-front about that.
 */

void lobby_load_bans(LobbyState *L, const char *path) {
    if (!L || !path) return;
    /* Record the path so subsequent lobby_ban_addr calls auto-save. */
    snprintf(L->ban_path, sizeof L->ban_path, "%s", path);
    L->ban_path[sizeof L->ban_path - 1] = '\0';

    FILE *f = fopen(path, "r");
    if (!f) {
        /* First-time host: no file yet. Not an error. */
        return;
    }
    char line[256];
    int loaded = 0;
    while (fgets(line, sizeof line, f) && L->ban_count < LOBBY_BANS_MAX) {
        /* Skip leading whitespace + blank/comment lines. */
        char *s = line;
        while (*s && (*s == ' ' || *s == '\t')) s++;
        if (*s == '\0' || *s == '\n' || *s == '#') continue;

        unsigned addr = 0;
        char nbuf[LOBBY_NAME_BYTES] = {0};
        /* "%23s" caps name to LOBBY_NAME_BYTES-1 = 23 chars. */
        int n = sscanf(s, "%x %23s", &addr, nbuf);
        if (n < 1) continue;
        LobbyBan *b = &L->bans[L->ban_count++];
        memset(b, 0, sizeof *b);
        b->in_use    = true;
        b->addr_host = (uint32_t)addr;
        if (n >= 2) snprintf(b->name, sizeof b->name, "%s", nbuf);
        loaded++;
    }
    fclose(f);
    if (loaded > 0) LOG_I("lobby: loaded %d ban(s) from %s", loaded, path);
}

void lobby_save_bans(const LobbyState *L, const char *path) {
    if (!L || !path) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_W("lobby: bans save failed: cannot write %s", path);
        return;
    }
    fprintf(f, "# soldut bans — addr_hex name (one per line)\n");
    for (int i = 0; i < L->ban_count; ++i) {
        const LobbyBan *b = &L->bans[i];
        if (!b->in_use) continue;
        const char *name = b->name[0] ? b->name : "-";
        fprintf(f, "%08x %s\n", (unsigned)b->addr_host, name);
    }
    fclose(f);
}

/* ---- Wire codec --------------------------------------------------- */

static void w_u8 (uint8_t **p, uint8_t v)  { (*p)[0] = v; *p += 1; }
static void w_u16(uint8_t **p, uint16_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static void w_i16(uint8_t **p, int16_t v)  { w_u16(p, (uint16_t)v); }
static void w_bytes(uint8_t **p, const void *src, int n) {
    memcpy(*p, src, (size_t)n); *p += n;
}
static uint8_t  r_u8 (const uint8_t **p) { uint8_t v=(*p)[0]; *p+=1; return v; }
static uint16_t r_u16(const uint8_t **p) {
    uint16_t v = (uint16_t)((*p)[0] | ((*p)[1] << 8)); *p += 2; return v;
}
static int16_t  r_i16(const uint8_t **p) { return (int16_t)r_u16(p); }
static void r_bytes(const uint8_t **p, void *dst, int n) {
    memcpy(dst, *p, (size_t)n); *p += n;
}

static void encode_one_slot(const LobbySlot *s, uint8_t **p) {
    w_u8 (p, s->in_use ? 1u : 0u);
    w_u8 (p, (uint8_t)s->team);
    w_u8 (p, s->ready ? 1u : 0u);
    w_u8 (p, s->is_host ? 1u : 0u);
    w_u8 (p, (uint8_t)s->loadout.chassis_id);
    w_u8 (p, (uint8_t)s->loadout.primary_id);
    w_u8 (p, (uint8_t)s->loadout.secondary_id);
    w_u8 (p, (uint8_t)s->loadout.armor_id);
    w_u8 (p, (uint8_t)s->loadout.jetpack_id);
    w_i16(p, (int16_t)s->score);
    w_i16(p, (int16_t)s->kills);
    w_i16(p, (int16_t)s->deaths);
    char nm[LOBBY_NAME_BYTES] = {0};
    snprintf(nm, sizeof nm, "%s", s->name);
    w_bytes(p, nm, LOBBY_NAME_BYTES);
    /* Ships -1 (0xFF as int8) when no mech is assigned (lobby phase). */
    int8_t mid = (s->mech_id >= 0 && s->mech_id < 127) ? (int8_t)s->mech_id : -1;
    w_u8(p, (uint8_t)mid);
}

static void decode_one_slot(LobbySlot *s, const uint8_t **p) {
    s->in_use            = r_u8(p) ? true : false;
    s->team              = (int)r_u8(p);
    s->ready             = r_u8(p) ? true : false;
    s->is_host           = r_u8(p) ? true : false;
    s->loadout.chassis_id   = (int)r_u8(p);
    s->loadout.primary_id   = (int)r_u8(p);
    s->loadout.secondary_id = (int)r_u8(p);
    s->loadout.armor_id     = (int)r_u8(p);
    s->loadout.jetpack_id   = (int)r_u8(p);
    s->score             = (int)r_i16(p);
    s->kills             = (int)r_i16(p);
    s->deaths            = (int)r_i16(p);
    char nm[LOBBY_NAME_BYTES];
    r_bytes(p, nm, LOBBY_NAME_BYTES);
    nm[LOBBY_NAME_BYTES - 1] = '\0';
    snprintf(s->name, sizeof s->name, "%s", nm);
    int8_t mid = (int8_t)r_u8(p);
    s->mech_id = (mid < 0) ? -1 : (int)mid;
}

void lobby_encode_list(const LobbyState *L, uint8_t *out) {
    uint8_t *p = out;
    w_u16(&p, (uint16_t)MAX_LOBBY_SLOTS);
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        encode_one_slot(&L->slots[i], &p);
    }
}

void lobby_decode_list(LobbyState *L, const uint8_t *in) {
    const uint8_t *p = in;
    int slots = (int)r_u16(&p);
    if (slots > MAX_LOBBY_SLOTS) slots = MAX_LOBBY_SLOTS;
    /* Wipe before decode so dropped peers vanish. */
    int new_count = 0;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        memset(&L->slots[i], 0, sizeof L->slots[i]);
        L->slots[i].peer_id = -1;
        L->slots[i].mech_id = -1;
    }
    for (int i = 0; i < slots; ++i) {
        decode_one_slot(&L->slots[i], &p);
        /* peer_id is server-side runtime — clients don't track it.
         * mech_id IS on the wire and must NOT be reset here; this used
         * to clobber the value the client needs to resolve its own
         * mech (and produced a black-screen client). */
        L->slots[i].peer_id = -1;
        if (L->slots[i].in_use) new_count++;
    }
    L->slot_count = new_count;
}

void lobby_encode_slot(const LobbyState *L, int slot, uint8_t *out) {
    uint8_t *p = out;
    w_u8(&p, (uint8_t)slot);
    if (slot < 0 || slot >= MAX_LOBBY_SLOTS) {
        LobbySlot empty = {0};
        encode_one_slot(&empty, &p);
        return;
    }
    encode_one_slot(&L->slots[slot], &p);
}

void lobby_decode_slot(LobbyState *L, const uint8_t *in) {
    const uint8_t *p = in;
    int slot = (int)r_u8(&p);
    if (slot < 0 || slot >= MAX_LOBBY_SLOTS) return;
    LobbySlot s = (LobbySlot){0};
    decode_one_slot(&s, &p);
    /* Preserve fields the wire format doesn't carry. */
    s.peer_id = L->slots[slot].peer_id;
    s.mech_id = L->slots[slot].mech_id;
    s.last_chat_time = L->slots[slot].last_chat_time;
    int was_in_use = L->slots[slot].in_use;
    L->slots[slot] = s;
    if (s.in_use && !was_in_use) L->slot_count++;
    else if (!s.in_use && was_in_use && L->slot_count > 0) L->slot_count--;
}

void lobby_encode_chat_line(const LobbyChatLine *line, uint8_t *out) {
    uint8_t *p = out;
    w_u8(&p, (uint8_t)(line->sender_slot < 0 ? 0xFF : line->sender_slot));
    w_u8(&p, line->sender_team);
    char body[LOBBY_CHAT_BYTES] = {0};
    snprintf(body, sizeof body, "%s", line->text);
    w_bytes(&p, body, LOBBY_CHAT_BYTES);
}

void lobby_decode_chat_line(LobbyChatLine *line, const uint8_t *in) {
    const uint8_t *p = in;
    uint8_t s = r_u8(&p);
    line->sender_slot = (s == 0xFFu) ? -1 : (int)s;
    line->sender_team = r_u8(&p);
    char body[LOBBY_CHAT_BYTES];
    r_bytes(&p, body, LOBBY_CHAT_BYTES);
    body[LOBBY_CHAT_BYTES - 1] = '\0';
    snprintf(line->text, sizeof line->text, "%s", body);
    line->age = 0.0f;
}

/* ---- Round mech spawn / teardown --------------------------------- */

void lobby_spawn_round_mechs(LobbyState *L, World *world,
                             int map_id, int local_slot, MatchModeId mode)
{
    if (!world) return;

    /* Clear pools so successive rounds don't grow indefinitely. */
    world->particles.count    = 0;
    world->constraints.count  = 0;
    world->projectiles.count  = 0;
    world->mech_count         = 0;
    world->local_mech_id      = -1;
    world->dummy_mech_id      = -1;

    int spawned = 0;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        LobbySlot *s = &L->slots[i];
        if (!s->in_use) { s->mech_id = -1; continue; }
        if (s->team == MATCH_TEAM_NONE) {
            /* Spectator — no mech. */
            s->mech_id = -1;
            continue;
        }
        Vec2 spawn = map_spawn_point((MapId)map_id, &world->level,
                                     i, s->team, mode);
        /* Per-slot spawn coordinates — diagnostic only (e2e tests use
         * this to verify F5 test-play picked the .lvl spawn point).
         * SHOT_LOG is a no-op in production play. */
        SHOT_LOG("lobby: slot %d team %d -> spawn (%.1f, %.1f)",
                 i, (int)s->team, (double)spawn.x, (double)spawn.y);
        int mid = mech_create_loadout(world, s->loadout, spawn,
                                      s->team, /*is_dummy*/false);
        if (mid < 0) {
            LOG_W("lobby_spawn_round_mechs: slot %d failed to spawn", i);
            s->mech_id = -1;
            continue;
        }
        s->mech_id = mid;
        if (i == local_slot) world->local_mech_id = mid;
        spawned++;
    }
    LOG_I("lobby_spawn_round_mechs: %d mech(s) spawned (local_slot=%d)",
          spawned, local_slot);
}

void lobby_clear_round_mechs(LobbyState *L, World *world) {
    if (!world) return;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        L->slots[i].mech_id = -1;
    }
    world->particles.count   = 0;
    world->constraints.count = 0;
    world->projectiles.count = 0;
    world->mech_count        = 0;
    world->local_mech_id     = -1;
    world->dummy_mech_id     = -1;
    if (world->fx.items) world->fx.count = 0;
    for (int i = 0; i < KILLFEED_CAPACITY; ++i) {
        world->killfeed[i] = (KillFeedEntry){0};
    }
    world->killfeed_count    = 0;
    world->last_event[0]     = '\0';
    world->last_event_time   = 0.0f;
}
