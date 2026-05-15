#include "ctf.h"

#include "audio.h"
#include "game.h"
#include "level.h"
#include "lobby.h"
#include "log.h"
#include "match.h"
#include "mech.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The carrier's render position (used for touch tests when the flag is
 * CARRIED — but ctf_step skips touches on CARRIED flags, so this is
 * only used when callers want to draw the flag at the chest). */
static Vec2 carrier_chest(const World *w, int mech_id) {
    if (mech_id < 0 || mech_id >= w->mech_count) return (Vec2){ 0.0f, 0.0f };
    return mech_chest_pos(w, mech_id);
}

/* Sort the level's LvlFlag records into world->flags[0]=RED,
 * world->flags[1]=BLUE. If the level doesn't carry exactly one of each
 * team, leave the world's flag_count at 0 — the rotation should have
 * already skipped it via the mode-mask validator in main.c. */
void ctf_init_round(World *w, MatchModeId mode) {
    memset(w->flags, 0, sizeof w->flags);
    w->flag_count = 0;
    if (mode != MATCH_MODE_CTF) return;

    const Level *L = &w->level;
    if (L->flag_count != 2 || !L->flags) {
        LOG_W("ctf_init_round: map has flag_count=%d (expected 2) — CTF disabled this round",
              L->flag_count);
        return;
    }

    int red_idx = -1, blue_idx = -1;
    for (int i = 0; i < L->flag_count; ++i) {
        if (L->flags[i].team == MATCH_TEAM_RED  && red_idx  < 0) red_idx  = i;
        if (L->flags[i].team == MATCH_TEAM_BLUE && blue_idx < 0) blue_idx = i;
    }
    if (red_idx < 0 || blue_idx < 0) {
        LOG_W("ctf_init_round: map missing RED (%d) or BLUE (%d) flag — CTF disabled",
              red_idx, blue_idx);
        return;
    }

    const LvlFlag *fr = &L->flags[red_idx];
    const LvlFlag *fb = &L->flags[blue_idx];
    w->flags[0] = (Flag){
        .home_pos       = (Vec2){ (float)fr->pos_x, (float)fr->pos_y },
        .team           = MATCH_TEAM_RED,
        .status         = FLAG_HOME,
        .carrier_mech   = -1,
        .dropped_pos    = (Vec2){ 0.0f, 0.0f },
        .return_at_tick = 0,
    };
    w->flags[1] = (Flag){
        .home_pos       = (Vec2){ (float)fb->pos_x, (float)fb->pos_y },
        .team           = MATCH_TEAM_BLUE,
        .status         = FLAG_HOME,
        .carrier_mech   = -1,
        .dropped_pos    = (Vec2){ 0.0f, 0.0f },
        .return_at_tick = 0,
    };
    w->flag_count = 2;
    LOG_I("ctf_init_round: flags at RED(%.0f,%.0f) BLUE(%.0f,%.0f)",
          (double)w->flags[0].home_pos.x, (double)w->flags[0].home_pos.y,
          (double)w->flags[1].home_pos.x, (double)w->flags[1].home_pos.y);
}

Vec2 ctf_flag_position(const World *w, int f) {
    if (f < 0 || f >= w->flag_count) return (Vec2){ 0.0f, 0.0f };
    const Flag *flag = &w->flags[f];
    switch ((FlagStatus)flag->status) {
        case FLAG_HOME:    return flag->home_pos;
        case FLAG_DROPPED: return flag->dropped_pos;
        case FLAG_CARRIED: return carrier_chest(w, flag->carrier_mech);
    }
    return flag->home_pos;
}

bool ctf_is_carrier(const World *w, int mech_id) {
    for (int i = 0; i < w->flag_count; ++i) {
        const Flag *fl = &w->flags[i];
        if (fl->status == FLAG_CARRIED && fl->carrier_mech == mech_id) {
            return true;
        }
    }
    return false;
}

/* Index helpers for the both-flags-home capture rule. flags[0] is RED,
 * flags[1] is BLUE. */
static int own_flag_idx(int team) {
    return (team == MATCH_TEAM_RED) ? 0 : 1;
}
static int enemy_flag_idx(int team) {
    return (team == MATCH_TEAM_RED) ? 1 : 0;
}

/* True if mech `mi` carries the flag that BELONGS to the opposite team
 * of `own_f`. This is the precondition for a capture: touching your own
 * home flag while you have the enemy flag in hand. */
static bool player_carries_enemy_flag(const World *w, int mi, int own_f) {
    int e = (own_f == 0) ? 1 : 0;
    return (w->flags[e].status == FLAG_CARRIED &&
            w->flags[e].carrier_mech == mi);
}

/* All ctf transitions just mark world.flag_state_dirty; main.c is the
 * single broadcaster. Keeps ctf.c independent of net.c and the Game
 * lifecycle, and consolidates wire chatter to one site. */
static void mark_dirty(World *w) {
    w->flag_state_dirty = true;
}

static void ctf_pickup(struct Game *g, int f, int mi) {
    Flag *flag = &g->world.flags[f];
    Vec2 sound_pos = ctf_flag_position(&g->world, f);
    flag->status         = FLAG_CARRIED;
    flag->carrier_mech   = (int8_t)mi;
    flag->return_at_tick = 0;
    int slot = lobby_find_slot_by_mech(&g->lobby, mi);
    LOG_I("ctf: pickup flag=%d (team=%u) by mech=%d slot=%d",
          f, (unsigned)flag->team, mi, slot);
    if (slot >= 0) {
        char msg[80];
        snprintf(msg, sizeof msg, "%s grabbed the %s flag",
                 g->lobby.slots[slot].name,
                 (flag->team == MATCH_TEAM_RED) ? "Red" : "Blue");
        lobby_chat_system(&g->lobby, msg);
    }
    audio_play_at(SFX_FLAG_PICKUP, sound_pos);
    mark_dirty(&g->world);
}

static void ctf_capture(struct Game *g, int mi) {
    Mech *m = &g->world.mechs[mi];
    int captured = enemy_flag_idx(m->team);
    Flag *flag = &g->world.flags[captured];

    /* Score: +1 to team_score per capture, +1 to the carrier's
     * personal score. team_score is the round-end threshold against
     * `match.score_limit` — so the host's configured score_limit
     * directly equals "captures needed to win" in CTF, matching FFA
     * (kills to win) and TDM (team kills to win). The original M5 P07
     * design had +5 per capture, which made a default score_limit of
     * 5 end the round on the FIRST capture; the user reported that as
     * the "round ends as soon as I bring the flag back" bug. */
    if (m->team >= 0 && m->team < MATCH_TEAM_COUNT) {
        g->match.team_score[m->team] += 1;
        g->match.score_dirty = true;
    }
    int slot = lobby_find_slot_by_mech(&g->lobby, mi);
    if (slot >= 0) g->lobby.slots[slot].score += 1;

    /* Send the captured flag home. The own flag was already HOME (that
     * was the precondition for capturing). */
    flag->status         = FLAG_HOME;
    flag->carrier_mech   = -1;
    flag->return_at_tick = 0;

    LOG_I("ctf: capture by mech=%d (team=%d) score R%d/B%d",
          mi, m->team,
          g->match.team_score[MATCH_TEAM_RED],
          g->match.team_score[MATCH_TEAM_BLUE]);
    if (slot >= 0) {
        char msg[120];
        snprintf(msg, sizeof msg, "%s scored for %s — %d-%d",
                 g->lobby.slots[slot].name,
                 (m->team == MATCH_TEAM_RED) ? "Red" : "Blue",
                 g->match.team_score[MATCH_TEAM_RED],
                 g->match.team_score[MATCH_TEAM_BLUE]);
        lobby_chat_system(&g->lobby, msg);
    }
    /* Capture is loud and team-shaping — every viewer hears it
     * regardless of distance. */
    audio_play_global(SFX_FLAG_CAPTURE);
    mark_dirty(&g->world);
    /* Lobby slot scores changed; reship the table on the next net_poll. */
    g->lobby.dirty = true;
}

static void ctf_return_flag(struct Game *g, int f, int by_mech) {
    Flag *flag = &g->world.flags[f];
    Vec2 sound_pos = flag->home_pos;
    flag->status         = FLAG_HOME;
    flag->carrier_mech   = -1;
    flag->return_at_tick = 0;
    audio_play_at(SFX_FLAG_RETURN, sound_pos);

    if (by_mech >= 0) {
        int slot = lobby_find_slot_by_mech(&g->lobby, by_mech);
        if (slot >= 0) {
            g->lobby.slots[slot].score += 1;
            char msg[80];
            snprintf(msg, sizeof msg, "%s returned the %s flag",
                     g->lobby.slots[slot].name,
                     (flag->team == MATCH_TEAM_RED) ? "Red" : "Blue");
            lobby_chat_system(&g->lobby, msg);
            g->lobby.dirty = true;
        }
        LOG_I("ctf: return flag=%d by mech=%d", f, by_mech);
    } else {
        char msg[80];
        snprintf(msg, sizeof msg, "%s flag auto-returned",
                 (flag->team == MATCH_TEAM_RED) ? "Red" : "Blue");
        lobby_chat_system(&g->lobby, msg);
        LOG_I("ctf: auto-return flag=%d", f);
    }
    mark_dirty(&g->world);
}

/* Resolve which transition fires for a (mech, flag) touch. Caller has
 * already gated `flag->status != FLAG_CARRIED`. */
static void ctf_touch(struct Game *g, int f, int mi) {
    Flag *flag = &g->world.flags[f];
    Mech *m = &g->world.mechs[mi];

    if ((int)m->team == (int)flag->team) {
        /* Same-team touch. */
        if (flag->status == FLAG_HOME) {
            if (player_carries_enemy_flag(&g->world, mi, f)) {
                ctf_capture(g, mi);
            }
            /* Otherwise: player walking past their own flag at base,
             * empty-handed. No-op. */
        } else if (flag->status == FLAG_DROPPED) {
            ctf_return_flag(g, f, mi);
        }
    } else {
        /* Enemy-team touch. HOME / DROPPED both transition to CARRIED.
         * Carrier already filtered out by the CARRIED gate in caller. */
        ctf_pickup(g, f, mi);
    }
}

void ctf_step(struct Game *g, float dt) {
    (void)dt;
    World *w = &g->world;
    MatchState *match = &g->match;
    if (match->mode  != MATCH_MODE_CTF)        return;
    if (match->phase != MATCH_PHASE_ACTIVE)    return;
    if (!w->authoritative)                     return;
    if (w->flag_count == 0)                    return;

    for (int f = 0; f < w->flag_count; ++f) {
        Flag *flag = &w->flags[f];

        /* Auto-return on timer. */
        if (flag->status == FLAG_DROPPED && w->tick >= flag->return_at_tick) {
            ctf_return_flag(g, f, /*by_mech*/ -1);
            continue;
        }

        if (flag->status == FLAG_CARRIED) continue;

        /* Touch detection — distance from the mech's chest to the
         * nearest point on the visible flag staff (a vertical line
         * segment from `home_pos` up by FLAG_STAFF_HEIGHT_PX, matching
         * render.c::draw_flag). Authoring the flag's `pos_y` at the
         * platform surface lets the staff/pennant lie at chest height
         * automatically — without segment math the chest-to-base
         * distance was the only test, and maps that placed the base
         * 50 px below chest height (Crossfire) never fired pickup.
         * (See FLAG_STAFF_HEIGHT_PX in world.h.) */
        Vec2 fp = ctf_flag_position(w, f);
        const float staff_top_y = fp.y - FLAG_STAFF_HEIGHT_PX;  /* smaller y = higher on screen */
        const float staff_bot_y = fp.y;
        const float r2 = FLAG_TOUCH_RADIUS_PX * FLAG_TOUCH_RADIUS_PX;
        for (int mi = 0; mi < w->mech_count; ++mi) {
            Mech *m = &w->mechs[mi];
            if (!m->alive)    continue;
            if (m->is_dummy)  continue;
            if (m->team == MATCH_TEAM_NONE) continue;
            Vec2 cp = mech_chest_pos(w, mi);
            float dx = cp.x - fp.x;
            /* Clamp chest's y onto the staff line segment, then take
             * Euclidean distance from chest to the clamp point. */
            float dy;
            if      (cp.y < staff_top_y) dy = staff_top_y - cp.y;
            else if (cp.y > staff_bot_y) dy = cp.y - staff_bot_y;
            else                          dy = 0.0f;
            if (dx * dx + dy * dy > r2) continue;
            ctf_touch(g, f, mi);
            /* If the touch transitioned this flag into CARRIED, stop —
             * subsequent mechs can't grab it on the same tick. The
             * HOME / DROPPED-after-return cases also stop gracefully
             * because the pickup branch of ctf_touch is the only one
             * that would fire from another mech in the same loop. */
            if (flag->status == FLAG_CARRIED) break;
        }
    }
}

void ctf_drop_on_death(World *w, MatchModeId mode, int mech_id, Vec2 death_pos) {
    if (mode != MATCH_MODE_CTF) return;
    if (!w->authoritative) return;
    if (w->flag_count <= 0) return;
    for (int f = 0; f < w->flag_count; ++f) {
        Flag *flag = &w->flags[f];
        if (flag->status != FLAG_CARRIED)          continue;
        if (flag->carrier_mech != (int8_t)mech_id) continue;
        flag->status         = FLAG_DROPPED;
        flag->carrier_mech   = -1;
        flag->dropped_pos    = death_pos;
        flag->return_at_tick = w->tick + FLAG_AUTO_RETURN_TICKS;
        audio_play_at(SFX_FLAG_DROP, death_pos);
        LOG_I("ctf: drop flag=%d carrier mech=%d at (%.0f,%.0f)",
              f, mech_id, (double)death_pos.x, (double)death_pos.y);
        mark_dirty(w);
    }
}
