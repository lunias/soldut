#include "match.h"

#include "lobby.h"
#include "log.h"

#include <stdint.h>
#include <string.h>
#include <strings.h>

void match_init(MatchState *m, MatchModeId mode, int score_limit,
                float time_limit, bool friendly_fire)
{
    memset(m, 0, sizeof *m);
    m->mode             = (mode < MATCH_MODE_COUNT) ? mode : MATCH_MODE_FFA;
    m->phase            = MATCH_PHASE_LOBBY;
    m->map_id           = 0;
    m->score_limit      = (score_limit > 0) ? score_limit : 25;
    m->time_limit       = (time_limit > 0.0f) ? time_limit : 600.0f;
    m->time_remaining   = m->time_limit;
    m->countdown_default= 5.0f;
    m->summary_default  = 15.0f;
    m->friendly_fire    = friendly_fire;
    m->winner_team      = MATCH_TEAM_NONE;
    m->mvp_slot         = -1;
    m->solo_warning_remaining = -1.0f;
    m->rounds_per_match = 3;        /* config-driven, overridden in bootstrap */
    m->rounds_played    = 0;
    m->inter_round_countdown_default = 3.0f;
}

void match_begin_countdown(MatchState *m, float countdown_seconds) {
    m->phase               = MATCH_PHASE_COUNTDOWN;
    m->countdown_remaining = (countdown_seconds > 0.0f)
                              ? countdown_seconds
                              : m->countdown_default;
    LOG_I("match: countdown %.1fs (mode=%s, limit=%d, time=%.0fs)",
          (double)m->countdown_remaining, match_mode_name(m->mode),
          m->score_limit, (double)m->time_limit);
}

void match_begin_round(MatchState *m) {
    m->phase           = MATCH_PHASE_ACTIVE;
    m->time_remaining  = m->time_limit;
    m->winner_team     = MATCH_TEAM_NONE;
    m->mvp_slot        = -1;
    m->solo_warning_remaining = -1.0f;
    for (int t = 0; t < MATCH_TEAM_COUNT; ++t) m->team_score[t] = 0;
    LOG_I("match: round begin (mode=%s, map=%d, limit=%d)",
          match_mode_name(m->mode), m->map_id, m->score_limit);
}

void match_end_round(MatchState *m, const LobbyState *lobby) {
    m->phase             = MATCH_PHASE_SUMMARY;
    m->summary_remaining = m->summary_default;

    /* MVP — slot with the highest score among in-use slots. Ties broken
     * by lower deaths, then lower id. */
    int best = -1;
    int best_score = -2147483647;
    int best_deaths = 2147483647;
    for (int i = 0; lobby && i < MAX_LOBBY_SLOTS; ++i) {
        const LobbySlot *s = &lobby->slots[i];
        if (!s->in_use) continue;
        if (s->score > best_score ||
            (s->score == best_score && s->deaths < best_deaths))
        {
            best        = i;
            best_score  = s->score;
            best_deaths = s->deaths;
        }
    }
    m->mvp_slot = best;

    /* Winner. */
    if (m->mode == MATCH_MODE_FFA) {
        m->winner_team = MATCH_TEAM_NONE;   /* no team winner in FFA */
    } else {
        if (m->team_score[MATCH_TEAM_RED] > m->team_score[MATCH_TEAM_BLUE])
            m->winner_team = MATCH_TEAM_RED;
        else if (m->team_score[MATCH_TEAM_BLUE] > m->team_score[MATCH_TEAM_RED])
            m->winner_team = MATCH_TEAM_BLUE;
        else
            m->winner_team = MATCH_TEAM_NONE;   /* draw */
    }

    LOG_I("match: round end (mvp_slot=%d, winner_team=%d, R%d/B%d)",
          m->mvp_slot, m->winner_team,
          m->team_score[MATCH_TEAM_RED], m->team_score[MATCH_TEAM_BLUE]);
}

bool match_tick(MatchState *m, float dt) {
    switch (m->phase) {
        case MATCH_PHASE_LOBBY:
            return false;
        case MATCH_PHASE_COUNTDOWN:
            m->countdown_remaining -= dt;
            if (m->countdown_remaining <= 0.0f) {
                m->countdown_remaining = 0.0f;
                return true;
            }
            return false;
        case MATCH_PHASE_ACTIVE:
            if (m->time_limit > 0.0f) {
                m->time_remaining -= dt;
                if (m->time_remaining <= 0.0f) {
                    m->time_remaining = 0.0f;
                    return true;
                }
            }
            return false;
        case MATCH_PHASE_SUMMARY:
            m->summary_remaining -= dt;
            if (m->summary_remaining <= 0.0f) {
                m->summary_remaining = 0.0f;
                return true;
            }
            return false;
    }
    return false;
}

bool match_apply_kill(MatchState *m, LobbyState *lobby,
                      int killer_slot, int victim_slot, uint32_t kill_flags)
{
    (void)kill_flags;
    if (m->phase != MATCH_PHASE_ACTIVE) return false;
    if (!lobby) return false;

    /* Suicide / environmental kills don't credit a killer. */
    bool is_suicide = (killer_slot < 0) || (killer_slot == victim_slot);

    if (victim_slot >= 0 && victim_slot < MAX_LOBBY_SLOTS &&
        lobby->slots[victim_slot].in_use)
    {
        lobby->slots[victim_slot].deaths++;
    }

    if (!is_suicide && killer_slot >= 0 && killer_slot < MAX_LOBBY_SLOTS &&
        lobby->slots[killer_slot].in_use)
    {
        LobbySlot *ks = &lobby->slots[killer_slot];
        LobbySlot *vs = (victim_slot >= 0 && victim_slot < MAX_LOBBY_SLOTS)
                        ? &lobby->slots[victim_slot] : NULL;
        bool team_kill = vs && (m->mode == MATCH_MODE_TDM || m->mode == MATCH_MODE_CTF) &&
                         ks->team == vs->team && ks->team != MATCH_TEAM_NONE;
        if (team_kill) {
            ks->score--;
            ks->team_kills++;
        } else {
            ks->kills++;
            ks->score++;
            if (m->mode == MATCH_MODE_TDM || m->mode == MATCH_MODE_CTF) {
                if (ks->team >= 0 && ks->team < MATCH_TEAM_COUNT) {
                    m->team_score[ks->team]++;
                }
            }
        }
    } else if (is_suicide && victim_slot >= 0 && victim_slot < MAX_LOBBY_SLOTS &&
               lobby->slots[victim_slot].in_use)
    {
        LobbySlot *vs = &lobby->slots[victim_slot];
        vs->score--;        /* documents/02-game-design.md §"Scoring" */
    }

    return match_round_should_end(m);
}

bool match_round_should_end(const MatchState *m) {
    if (m->phase != MATCH_PHASE_ACTIVE) return false;
    if (m->time_limit > 0.0f && m->time_remaining <= 0.0f) return true;
    if (m->mode == MATCH_MODE_TDM || m->mode == MATCH_MODE_CTF) {
        for (int t = 1; t < MATCH_TEAM_COUNT; ++t)
            if (m->team_score[t] >= m->score_limit) return true;
    }
    /* FFA: a per-player kill cap — the caller checks lobby slot scores
     * because the score lives there, not in MatchState. */
    return false;
}

bool match_step_solo_warning(MatchState *m, const struct World *w, float dt) {
    if (!m || !w) return false;
    if (m->phase != MATCH_PHASE_ACTIVE) {
        m->solo_warning_remaining = -1.0f;
        return false;
    }
    int mech_count = 0, alive_count = 0;
    for (int i = 0; i < w->mech_count; ++i) {
        const Mech *mm = &w->mechs[i];
        if (mm->is_dummy) continue;
        mech_count++;
        if (mm->alive) alive_count++;
    }
    /* Single-player / pre-spawn (mech_count <= 1 from the start) is
     * exempt — there's no "remaining" without a baseline. */
    bool rule_active = (mech_count >= 2 && alive_count <= 1);
    if (!rule_active) {
        m->solo_warning_remaining = -1.0f;
        return false;
    }
    if (m->solo_warning_remaining < 0.0f) {
        /* First tick the rule matched — arm the 3-second timer. */
        m->solo_warning_remaining = 3.0f;
        LOG_I("match: only %d alive of %d — round ends in 3s",
              alive_count, mech_count);
        return false;
    }
    m->solo_warning_remaining -= dt;
    if (m->solo_warning_remaining <= 0.0f) {
        m->solo_warning_remaining = 0.0f;
        return true;
    }
    return false;
}

const char *match_mode_name(MatchModeId mode) {
    switch (mode) {
        case MATCH_MODE_FFA: return "FFA";
        case MATCH_MODE_TDM: return "TDM";
        case MATCH_MODE_CTF: return "CTF";
        default:             return "?";
    }
}

MatchModeId match_mode_from_name(const char *name) {
    if (!name) return MATCH_MODE_FFA;
    if (strcasecmp(name, "ffa") == 0 ||
        strcasecmp(name, "deathmatch") == 0 ||
        strcasecmp(name, "dm") == 0)        return MATCH_MODE_FFA;
    if (strcasecmp(name, "tdm") == 0 ||
        strcasecmp(name, "team") == 0)      return MATCH_MODE_TDM;
    if (strcasecmp(name, "ctf") == 0 ||
        strcasecmp(name, "flag") == 0)      return MATCH_MODE_CTF;
    LOG_W("match_mode_from_name: unknown '%s' — defaulting to FFA", name);
    return MATCH_MODE_FFA;
}

const char *match_phase_name(MatchPhase phase) {
    switch (phase) {
        case MATCH_PHASE_LOBBY:     return "LOBBY";
        case MATCH_PHASE_COUNTDOWN: return "COUNTDOWN";
        case MATCH_PHASE_ACTIVE:    return "ACTIVE";
        case MATCH_PHASE_SUMMARY:   return "SUMMARY";
    }
    return "?";
}

/* ---- Wire encode/decode ------------------------------------------- */
/* Layout (20 bytes):
 *   u8  mode
 *   u8  phase
 *   u8  map_id
 *   u8  friendly_fire
 *   u16 score_limit
 *   u16 reserved
 *   f32 time_remaining
 *   i16 team_score[3]   (6 bytes — index 0 unused but kept for symmetry)
 */

static void w_u8(uint8_t **p, uint8_t v)  { (*p)[0] = v; *p += 1; }
static void w_u16(uint8_t **p, uint16_t v) { (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); *p += 2; }
static void w_i16(uint8_t **p, int16_t v)  { w_u16(p, (uint16_t)v); }
static void w_f32(uint8_t **p, float v) {
    uint32_t u; memcpy(&u, &v, 4);
    (*p)[0] = (uint8_t)u; (*p)[1] = (uint8_t)(u >> 8);
    (*p)[2] = (uint8_t)(u >> 16); (*p)[3] = (uint8_t)(u >> 24);
    *p += 4;
}
static uint8_t  r_u8 (const uint8_t **p) { uint8_t v=(*p)[0]; *p+=1; return v; }
static uint16_t r_u16(const uint8_t **p) { uint16_t v=(uint16_t)((*p)[0]|((*p)[1]<<8)); *p+=2; return v; }
static int16_t  r_i16(const uint8_t **p) { return (int16_t)r_u16(p); }
static float r_f32(const uint8_t **p) {
    uint32_t u = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) |
                 ((uint32_t)(*p)[2] << 16) | ((uint32_t)(*p)[3] << 24);
    *p += 4; float v; memcpy(&v, &u, 4); return v;
}

void match_encode(const MatchState *m, uint8_t *out) {
    uint8_t *p = out;
    w_u8 (&p, (uint8_t)m->mode);
    w_u8 (&p, (uint8_t)m->phase);
    w_u8 (&p, (uint8_t)m->map_id);
    w_u8 (&p, m->friendly_fire ? 1u : 0u);
    w_u16(&p, (uint16_t)m->score_limit);
    w_u16(&p, 0);
    w_f32(&p, m->time_remaining);
    for (int t = 0; t < MATCH_TEAM_COUNT; ++t)
        w_i16(&p, (int16_t)m->team_score[t]);
}

void match_decode(MatchState *m, const uint8_t *in) {
    const uint8_t *p = in;
    m->mode             = (MatchModeId)r_u8(&p);
    m->phase            = (MatchPhase) r_u8(&p);
    m->map_id           = (int)r_u8(&p);
    m->friendly_fire    = r_u8(&p) ? true : false;
    m->score_limit      = (int)r_u16(&p);
    (void)r_u16(&p);
    m->time_remaining   = r_f32(&p);
    for (int t = 0; t < MATCH_TEAM_COUNT; ++t)
        m->team_score[t] = (int)r_i16(&p);
}
