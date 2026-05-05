#pragma once

#include "world.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * match — round state, scoring, win-condition evaluation.
 *
 * Lives on the server; clients receive a small MatchSnapshot through
 * NET_MSG_LOBBY_ROUND_START + per-frame STATE updates piggy-backed on
 * the existing world snapshot path.
 *
 * documents/02-game-design.md §"Modes" pins the contract:
 *   FFA  — first to N kills, or highest at timer end.
 *   TDM  — two teams; first team to N team-kills, or higher at end.
 *   CTF  — first team to N captures (M5 P07; flags + carry + capture +
 *          carrier penalties + auto-return). See documents/m5/06-ctf.md.
 */

typedef enum {
    MATCH_MODE_FFA = 0,
    MATCH_MODE_TDM,
    MATCH_MODE_CTF,
    MATCH_MODE_COUNT
} MatchModeId;

typedef enum {
    MATCH_PHASE_LOBBY = 0,        /* world idle; players in waiting room */
    MATCH_PHASE_COUNTDOWN,        /* pre-round briefing (5 s) */
    MATCH_PHASE_ACTIVE,           /* round running */
    MATCH_PHASE_SUMMARY,          /* scoreboard (15 s) */
} MatchPhase;

#define MATCH_TEAM_NONE      0    /* spectator / unassigned */
#define MATCH_TEAM_RED       1
#define MATCH_TEAM_BLUE      2
#define MATCH_TEAM_FFA       1    /* in FFA we put everyone on team 1 */
#define MATCH_TEAM_COUNT     3

typedef struct MatchState {
    MatchModeId  mode;
    MatchPhase   phase;
    int          map_id;                   /* MapId — current playing map */

    /* Limits and clocks. */
    int          score_limit;              /* kill cap (FFA) / team kill cap (TDM) */
    float        time_limit;               /* seconds; 0 = unlimited */
    float        time_remaining;
    float        countdown_remaining;      /* MATCH_PHASE_COUNTDOWN */
    float        summary_remaining;        /* MATCH_PHASE_SUMMARY */
    float        countdown_default;        /* set when entering countdown */
    float        summary_default;
    bool         friendly_fire;            /* mirrors world.friendly_fire */

    /* Team scores (TDM). FFA leaves these untouched and reads per-player
     * scores from the lobby slots. */
    int          team_score[MATCH_TEAM_COUNT];
    int          team_player_count[MATCH_TEAM_COUNT];

    /* Outcome — set when phase transitions to SUMMARY. */
    int          winner_team;              /* MATCH_TEAM_NONE = draw */
    int          mvp_slot;                 /* lobby slot id of the round MVP, -1 if none */
} MatchState;

/* Initialize defaults from a config snapshot. Called once at server
 * start; mode/limit/time may be overridden per round. */
void match_init(MatchState *m, MatchModeId mode, int score_limit,
                float time_limit, bool friendly_fire);

/* Begin the pre-round countdown. Snaps into MATCH_PHASE_COUNTDOWN. */
void match_begin_countdown(MatchState *m, float countdown_seconds);

/* Begin the actual round. Resets timers, clears team scores, leaves
 * win/MVP unset. Caller is responsible for spawning mechs. */
void match_begin_round(MatchState *m);

/* End the active round. Computes winner + MVP from the supplied lobby
 * slot scores (FFA winner = highest score; TDM winner = team with
 * higher team_score). Snaps into MATCH_PHASE_SUMMARY. */
struct LobbyState;
void match_end_round(MatchState *m, const struct LobbyState *lobby);

/* Per-tick advance. Decrements the active timer for the current phase.
 * Returns true when the phase should transition (caller acts). */
bool match_tick(MatchState *m, float dt);

/* Apply a kill to the score. Called by the server's kill-feed path
 * AFTER the kill has been recorded in lobby slots. Updates team scores
 * for TDM. Returns true if the score limit was hit by this kill. */
struct LobbyState;
bool match_apply_kill(MatchState *m, struct LobbyState *lobby,
                      int killer_slot, int victim_slot, uint32_t kill_flags);

/* True if the active round's win condition is met (score limit hit or
 * timer expired). Read by the simulation loop to know when to end. */
bool match_round_should_end(const MatchState *m);

/* Mode helpers. */
const char *match_mode_name(MatchModeId mode);
MatchModeId match_mode_from_name(const char *name);
const char *match_phase_name(MatchPhase phase);

/* Snapshot encoding for the LOBBY_ROUND_START message body. The encoded
 * size is constant. */
#define MATCH_SNAPSHOT_WIRE_BYTES  20
void match_encode(const MatchState *m, uint8_t *out);
void match_decode(MatchState *m, const uint8_t *in);
