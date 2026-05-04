# M5 — Capture the Flag mode

The third game mode. Currently `MATCH_MODE_CTF` is in the enum at `src/match.h:24`, accepted by the lobby UI, accepted by the config parser, and treated as TDM at runtime (it has no flag entities, no capture rule, no carrier penalties).

This document specifies what M5 lights up. It depends on the level format's FLAG section ([01-lvl-format.md](01-lvl-format.md)) and integrates with the pickup system's audio cue style ([04-pickups.md](04-pickups.md)).

## What CTF should feel like

Two teams, two flags. To score, you grab the **enemy** flag, carry it back to **your own** flag base — but only if your own flag is at home. (If the other team has yours, you have to defend / retrieve before you can capture.)

This is the **both-flags-home** rule, the Tribes / Quake / Threewave standard. Touch-capture is the alternative (popular in Soldat Junior); the both-home rule rewards coordinated defense and avoids "everyone runs flags simultaneously and nothing matters" stalemates. Per the research in [00-overview.md](00-overview.md): right rule for our 4v4–8v8 player counts.

## Map prerequisites

A CTF-capable map has, in its `.lvl` file:

- A **FLAG section** with exactly 2 records: one with `team = MATCH_TEAM_RED`, one with `team = MATCH_TEAM_BLUE`.
- A `META.mode_mask` with `MATCH_MODE_CTF` (= 4) bit set.
- At least 8 `LvlSpawn` records per team — CTF rounds last longer than FFA, so respawn variety matters.

The editor validates this on save. A map without a complete FLAG pair is silently demoted out of any rotation that includes `mode=ctf`.

The 8 ship maps in [07-maps.md](07-maps.md) — only **Crossfire** and **Citadel** are CTF-capable. The other 6 are FFA/TDM. CTF maps are bigger and more symmetric; not every map is built for them.

## Runtime data

```c
// src/world.h — added to MatchState (in match.h, but the runtime live
// state is in World)
typedef enum {
    FLAG_HOME = 0,
    FLAG_CARRIED,
    FLAG_DROPPED,
} FlagStatus;

typedef struct {
    Vec2         home_pos;             // base position from LvlFlag
    uint8_t      team;                 // MATCH_TEAM_RED or _BLUE
    FlagStatus   status;
    int          carrier_mech;         // valid iff CARRIED; else -1
    Vec2         dropped_pos;          // valid iff DROPPED
    uint64_t     return_at_tick;       // valid iff DROPPED; auto-return at this tick
} Flag;

// World struct adds:
Flag flags[2];   // [0]=Red, [1]=Blue; only populated when match.mode == CTF
int  flag_count;
```

The flag isn't a full mech or projectile — it's a small persistent record with a position, a status, and an optional carrier. The visual representation is in [08-rendering.md](08-rendering.md).

## State transitions

```
                        +--------------------+
                        | HOME (at base)     |
                        +-+----------+-------+
   enemy carrier touches | |^         |
   own flag drops (via   | || own    |
   dispatcher rule)      | || team   |
                         v |  touches v
                        +-+----------+-------+
                        | CARRIED            |
                        +-+----------+-------+
   carrier dies            |
                           v
                        +--------------------+
                        | DROPPED            |
                        +-+----------+-+-----+
   own team touches dropped |^         | enemy team
   (return to home)         ||         | touches (pickup)
                            |v         v
                        (HOME)        (CARRIED)
   30 s elapsed without
   touch → auto-return
                            v
                        (HOME)
```

Per-tick check on the server (matches the pickup-touch shape):

```c
// src/ctf.c — new module
void ctf_step(World *w, MatchState *match, float dt) {
    if (match->mode != MATCH_MODE_CTF) return;
    if (match->phase != MATCH_PHASE_ACTIVE) return;
    if (!w->authoritative) return;

    for (int f = 0; f < w->flag_count; ++f) {
        Flag *flag = &w->flags[f];

        // Auto-return on timer.
        if (flag->status == FLAG_DROPPED && w->tick >= flag->return_at_tick) {
            ctf_return_flag(w, match, f, /*by_mech*/-1);
            continue;
        }

        // Touch detection: any mech within 36 px of the flag's current
        // position, walking through the appropriate "trigger zone" rule.
        Vec2 flag_pos = ctf_flag_position(w, f);
        for (int mi = 0; mi < w->mech_count; ++mi) {
            Mech *m = &w->mechs[mi];
            if (!m->alive) continue;
            Vec2 mp = mech_chest_pos(w, mi);
            float dx = mp.x - flag_pos.x, dy = mp.y - flag_pos.y;
            if (dx*dx + dy*dy > 36.0f * 36.0f) continue;

            ctf_touch(w, match, f, mi);
        }
    }
}
```

`ctf_touch` resolves which transition fires:

```c
static void ctf_touch(World *w, MatchState *match, int f, int mi) {
    Flag *flag = &w->flags[f];
    Mech *m = &w->mechs[mi];

    // Same-team touch.
    if (m->team == flag->team) {
        switch (flag->status) {
            case FLAG_HOME:
                // Same-team mech touching their own flag while it's
                // home + carrying the enemy flag: SCORE.
                if (ctf_player_carries_enemy_flag(w, mi, f)) {
                    ctf_capture(w, match, mi);
                }
                break;
            case FLAG_DROPPED:
                // Friendly return.
                ctf_return_flag(w, match, f, mi);
                break;
            case FLAG_CARRIED:
                // shouldn't happen — the carrier is already an enemy
                break;
        }
    } else {
        // Enemy-team touch.
        switch (flag->status) {
            case FLAG_HOME:
            case FLAG_DROPPED:
                ctf_pickup(w, match, f, mi);
                break;
            case FLAG_CARRIED:
                // already carried; can't double-carry
                break;
        }
    }
}
```

## ctf_pickup

```c
static void ctf_pickup(World *w, MatchState *match, int f, int mi) {
    Flag *flag = &w->flags[f];
    flag->status = FLAG_CARRIED;
    flag->carrier_mech = mi;
    audio_play_at(SFX_FLAG_PICKUP, mech_pelvis_pos(w, mi));
    net_server_broadcast_flag_state(&g->net, w);
    lobby_chat_system_for_flag(&g->lobby, "%s grabbed the %s flag!",
                               g->lobby.slots[mi /* via slot lookup */].name,
                               team_name(flag->team));
}
```

The carrier is now flagged. Two penalties apply (per Soldat tradition + research recommendation):

1. **Half jet thrust.** `Mech.fuel_max * 0.5` effective; jet pulse in `apply_jet_force` checks `mech_carrying_flag(mid)` and halves the impulse. Fuel drain is unchanged.
2. **Secondary disabled.** `mech_try_fire` checks: if the active slot is secondary AND the firer is a flag carrier, return without firing. The secondary slot's UI shows a "DISABLED" overlay during carry.

These match Soldat. The "speed penalty" pattern from Quake is intentionally not adopted — Soldut's movement should still feel responsive, and the jet+secondary combo is enough drag.

## ctf_capture

```c
static void ctf_capture(World *w, MatchState *match, int mi) {
    Mech *m = &w->mechs[mi];
    int captured_flag = (m->team == MATCH_TEAM_RED) ? 1 /*Blue*/ : 0 /*Red*/;
    Flag *flag = &w->flags[captured_flag];

    // Score: +5 to the team, +1 to the player.
    match->team_score[m->team] += 5;
    int slot = lobby_find_slot_by_mech(&g->lobby, mi);
    if (slot >= 0) g->lobby.slots[slot].score += 1;

    // Send the flag home.
    flag->status = FLAG_HOME;
    flag->carrier_mech = -1;
    audio_play_global(SFX_FLAG_CAPTURE);          // played for everyone
    net_server_broadcast_flag_state(&g->net, w);
    lobby_chat_system(&g->lobby, "%s scored for %s! (%d-%d)",
                      g->lobby.slots[slot].name,
                      team_name(m->team),
                      match->team_score[MATCH_TEAM_RED],
                      match->team_score[MATCH_TEAM_BLUE]);

    // Also: +1 personal score on win (already incremented above).
}
```

Capture limit is 5 (per `match.score_limit` for CTF — different default from FFA's 25). Configurable via `soldut.cfg`.

## ctf_return_flag

Both manual (friendly touch) and auto (30 s) returns:

```c
static void ctf_return_flag(World *w, MatchState *match, int f, int mi) {
    Flag *flag = &w->flags[f];
    flag->status = FLAG_HOME;
    flag->carrier_mech = -1;
    audio_play_at(SFX_FLAG_RETURN, flag->home_pos);
    net_server_broadcast_flag_state(&g->net, w);

    if (mi >= 0) {
        // +1 personal score for the returner.
        int slot = lobby_find_slot_by_mech(&g->lobby, mi);
        if (slot >= 0) g->lobby.slots[slot].score += 1;
        lobby_chat_system(&g->lobby, "%s returned the %s flag",
                          g->lobby.slots[slot].name, team_name(flag->team));
    } else {
        lobby_chat_system(&g->lobby, "%s flag auto-returned",
                          team_name(flag->team));
    }
}
```

## Carrier dies → flag drops

The death path checks: if the dying mech is a flag carrier, drop the flag at the death position with a 30 s auto-return timer:

```c
// src/mech.c::mech_kill — added
void mech_kill(World *w, int mech_id, int part, Vec2 dir, float impulse,
               int killer, int weapon)
{
    // ... existing kill logic ...
    if (g->match.mode == MATCH_MODE_CTF) {
        for (int f = 0; f < w->flag_count; ++f) {
            if (w->flags[f].status == FLAG_CARRIED &&
                w->flags[f].carrier_mech == mech_id)
            {
                w->flags[f].status = FLAG_DROPPED;
                w->flags[f].carrier_mech = -1;
                w->flags[f].dropped_pos = mech_pelvis_pos(w, mech_id);
                w->flags[f].return_at_tick = w->tick + 30 * 60;  // 30 s @ 60 Hz
                audio_play_at(SFX_FLAG_DROP, w->flags[f].dropped_pos);
                net_server_broadcast_flag_state(&g->net, w);
            }
        }
    }
}
```

## Flag rendering

Two visual modes:

1. **At base or dropped on the ground**: the flag stands vertically at the position. `home_pos` for HOME state; `dropped_pos` for DROPPED. Rendered as a vertical staff (line) + triangular pennant (small triangle), team-colored.

2. **Carried**: drawn at the carrier's pelvis position, slightly offset (chest height + ~12 px to one side). Renders behind the carrier's torso so the carrier's body still reads cleanly.

```c
// src/render.c — add
static void draw_flag(const World *w, int f) {
    const Flag *flag = &w->flags[f];
    Color tc = (flag->team == MATCH_TEAM_RED)
              ? (Color){220, 80, 80, 255}
              : (Color){ 80, 140, 220, 255};
    Vec2 base;
    bool wave = false;
    switch (flag->status) {
        case FLAG_HOME:
            base = flag->home_pos;
            break;
        case FLAG_DROPPED:
            base = flag->dropped_pos;
            break;
        case FLAG_CARRIED: {
            const Mech *m = &w->mechs[flag->carrier_mech];
            Vec2 chest = mech_chest_pos(w, flag->carrier_mech);
            base = (Vec2){chest.x + (m->facing_left ? 8.0f : -8.0f),
                          chest.y - 18.0f};
            wave = true;
            break;
        }
    }
    Vec2 staff_top = (Vec2){base.x, base.y - 28.0f};
    DrawLineEx(base, staff_top, 2.0f, (Color){80, 60, 40, 255});
    Vec2 pen_tip = (Vec2){staff_top.x + 16.0f + (wave ? sinf(w->tick * 0.2f) * 2.0f : 0.0f),
                          staff_top.y + 6.0f};
    Vec2 pen_bot = (Vec2){staff_top.x, staff_top.y + 12.0f};
    Vec2 verts[3] = { staff_top, pen_tip, pen_bot };
    DrawTriangle(verts[0], verts[1], verts[2], tc);
}
```

The pennant waves while carried (a small sin offset on the tip vertex). Audio in [09-audio.md](09-audio.md) handles a continuous "carrier wind" loop.

## HUD

The HUD (`src/hud.c`) gains a CTF-mode panel:

- Top-center: score banner shows `RED 2 — 1 BLUE` and remaining time. Already present from M4's `match_overlay_draw`; CTF mode just changes the format.
- Top-left and top-right corners: small flag icons indicating each flag's status — at home (solid), carried (animated pulse), or dropped (cross-hatched). Clicking the icon (M6 stretch) does nothing; v1 is just visual.
- **Off-screen compass arrow**: when a flag is on the map but off-screen (carried by a teammate, or dropped out of view), a small triangular arrow at the screen edge points toward it. Color = team. The arrow's distance from the screen edge is proportional to flag distance (hint at how far). Implementation: 50 LOC in `hud.c`.

```c
// src/hud.c — new helper for CTF
static void draw_flag_compass(const World *w, int sw, int sh, Camera2D cam) {
    if (w->flag_count == 0) return;
    for (int f = 0; f < w->flag_count; ++f) {
        Vec2 fp = ctf_flag_position(w, f);
        Vector2 sp = GetWorldToScreen2D((Vector2){fp.x, fp.y}, cam);
        // Inside the screen?
        if (sp.x >= 0 && sp.x < sw && sp.y >= 0 && sp.y < sh) continue;
        // Project to the screen edge.
        // ... compute angle from screen center to flag, place arrow on edge ...
    }
}
```

## Wire protocol

Two flags = 20 bytes max state, well within bandwidth budget. The encoding is event-driven (broadcast on transitions):

```c
// NET_MSG_FLAG_STATE — new message, NET_CH_EVENT (reliable, ordered)
struct {
    uint8_t  msg_type;
    uint8_t  flag_count;        // 0 = no CTF; 2 = both flags follow
    struct {
        uint8_t  team;
        uint8_t  status;        // FlagStatus
        uint8_t  carrier_mech;  // 0xFF = none
        uint8_t  reserved;
        int16_t  pos_x;         // dropped_pos for DROPPED, home_pos for HOME, ignored for CARRIED
        int16_t  pos_y;
        uint16_t return_in_ticks; // valid iff DROPPED, else 0
    } flags[2];
};                            // 2 + 2*12 = 26 bytes
```

Broadcast on every state transition. Bandwidth: ~6 events/min/round × 26 B / 16 players = trivial.

`INITIAL_STATE` (the join handshake) carries the same FlagState message so a joining client sees correct flag positions immediately.

## Lag compensation

We do **not** lag-compensate flag pickup or capture. Both use server-current positions. Per the research, all major CTF games (Q3, UT, Source) do the same. Reasoning:

- The flag doesn't move during a pickup decision (you're touching a static thing); rollback offers nothing.
- Rollback creates ghost-grab artifacts where the flag teleports out of someone's hand because client A grabbed it 80 ms ago in the past.
- Players accept the small RTT-window unfairness for this specific event.

Hitscan / projectile damage *to the carrier* is still lag-compensated normally; that's already in the M2 code path.

## Wire format payment

Adding `Flag flags[2]` to the snapshot would inflate every snapshot. Instead, flags ride only on transitions (event channel) and on join. The runtime state is locally cached on each client.

This means a client that misses an event (packet drop on the reliable channel) is briefly out of sync. But: ENet's reliable+ordered delivery guarantees the event arrives eventually; the client is guaranteed to recover within ~1 RTT. We accept the brief ghost.

## Carrier penalty enforcement on the wire

The carrier penalties (half jet, no secondary) are server-authoritative — applied in the simulate step on the server, just like any other gameplay rule. Clients run the *same* simulate path with `world.authoritative=false`, so they see the right behavior locally too. The flag status from the most recent FlagState event is the source of truth.

If a client's flag-status cache is stale (race during the brief window between server transition and event arrival), the client briefly applies/unapplies the penalty incorrectly — corrected on next snapshot. Same shape as any other late-applying gameplay state; not a real concern.

## Spawn rules during CTF

Per [02-game-design.md](../02-game-design.md) §"Respawn":

- 3 s respawn after death.
- Team-side spawn (CTF map's spawn lanes are TEAM_RED or TEAM_BLUE; the FFA fallback never fires).
- ≥800 px from nearest enemy.
- Not currently occupied.

If the carrier respawns at a spawn point near the dropped flag, that's intentional — it lets you contest your own flag back.

## Configuration

`soldut.cfg` accepts mode rotations including ctf:

```ini
mode_rotation=ffa,tdm,ctf,tdm
map_rotation=foundry,reactor,crossfire,catwalk
```

When the rotation lands on `mode=ctf` and the map's mode_mask doesn't allow CTF, the host logs a warning and skips to the next rotation entry.

```c
// src/main.c::start_round — modify map+mode validation
if (g->match.mode == MATCH_MODE_CTF) {
    const MapMeta *m = map_meta_for(g->match.map_id);
    if (!(m->mode_mask & (1u << MATCH_MODE_CTF))) {
        LOG_W("start_round: map %d doesn't support CTF; advancing to next rotation entry",
              g->match.map_id);
        g->round_counter++;
        g->match.map_id = config_pick_map(&g->config, g->round_counter);
        g->match.mode = config_pick_mode(&g->config, g->round_counter);
    }
}
```

## Done when

- Two CTF maps (Crossfire + Citadel) play end-to-end: pick up enemy flag, carry to your base while your flag is home, score, both flags reset to home.
- A carrier dying drops the flag at the death position; auto-returns after 30 s.
- A friendly mech touching a dropped flag returns it instantly.
- The HUD shows score banner with team scores; off-screen compass arrows point at flag positions outside the viewport.
- A 4v4 LAN playtest run shows: capture rate ~1/min, no desync, no "flag is in two places" bugs.
- The M4 trade-off "MATCH_MODE_CTF accepted at config but plays as TDM at M4" is removed from match.h's enum comment and from any deferred-features doc.

## Trade-offs to log

- **No "flag held / time" stat** in the round summary at v1. The kill feed already shows captures and returns; a per-player carry-time stat is a polish item.
- **Carrier secondary is fully disabled, not partially.** Soldat had partial restrictions (the carrier could fire some secondaries). We simplify: carrier = no secondary fire. Re-evaluate in playtest.
- **Auto-return is 30 s flat.** Threewave and UT have variations (countdown that *hides* the flag's pulse for the last 5 s). We don't ship the visual fade.
- **No flag dropped indicator on the minimap.** Compass arrows do the same job for the entire map; if minimap lands later (M6 stretch), it gets flag pings.
