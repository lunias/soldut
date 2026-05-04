# P07 — CTF mode

## What this prompt does

Lights up `MATCH_MODE_CTF` from "treats as TDM" to a real CTF round: flag entities, base zones from `LvlFlag` records, capture rule (both flags home), drop / friendly-touch return, carrier penalties (half jet + secondary disabled), HUD compass arrows for off-screen flags, network protocol on `NET_CH_EVENT`.

Depends on P01 (`.lvl` FLAG section), P05 (audio cue stubs), P09 (carrier penalty rule for fire-secondary path).

## Required reading

1. `CLAUDE.md`
2. `documents/02-game-design.md` §"Capture the Flag"
3. **`documents/m5/06-ctf.md`** — the spec
4. `documents/m5/01-lvl-format.md` §"FLAG — CTF flag bases"
5. `src/match.{c,h}` — phase + scoring
6. `src/world.h` — Mech (you may add `is_carrier_flag_id` field) + Flag struct
7. `src/lobby.{c,h}` — slots, scores
8. `src/net.{c,h}` — channels, message tags
9. `src/snapshot.{c,h}` — INITIAL_STATE
10. `src/hud.c` — for the compass arrow render
11. `src/render.c` — for flag rendering

## Concrete tasks

### Task 1 — Flag runtime data

Per `documents/m5/06-ctf.md` §"Runtime data":

```c
typedef enum { FLAG_HOME = 0, FLAG_CARRIED, FLAG_DROPPED } FlagStatus;
typedef struct {
    Vec2         home_pos;
    uint8_t      team;
    FlagStatus   status;
    int          carrier_mech;    // -1 if not carried
    Vec2         dropped_pos;
    uint64_t     return_at_tick;
} Flag;

// World struct:
Flag flags[2];   // [0]=Red, [1]=Blue; populated when match.mode == CTF
int  flag_count;
```

Populate `flags[]` from `world->level.flags` (LvlFlag records) at round start in `start_round` when `match.mode == MATCH_MODE_CTF`.

### Task 2 — `src/ctf.{c,h}`

```c
void ctf_step(World *w, MatchState *match, float dt);   // server-only, called per tick
void ctf_pickup(World*, MatchState*, int flag, int mech_id);
void ctf_capture(World*, MatchState*, int mech_id);
void ctf_return_flag(World*, MatchState*, int flag, int by_mech_id);
Vec2 ctf_flag_position(const World*, int flag_idx);     // home/dropped/carrier
bool ctf_player_carries_enemy_flag(const World*, int mech_id, int flag_idx);
```

`ctf_step` per `documents/m5/06-ctf.md` §"State transitions":
- 36 px touch radius.
- Auto-return after 30 s (1800 ticks at 60 Hz).
- Per-mech touch detection.
- Resolve transitions per the same-team / enemy-team / status table.

### Task 3 — Carrier penalties

In `mech.c::apply_jet_force`, halve thrust if the mech is a flag carrier:

```c
if (mech_is_flag_carrier(world, mech_id)) thrust_pxs2 *= 0.5f;
```

In `mech_try_fire`, gate secondary fire (BTN_FIRE_SECONDARY from P09 + active-slot=secondary):

```c
if (mech_is_flag_carrier(...) && (active_slot == SECONDARY || pressed & BTN_FIRE_SECONDARY)) return;
```

### Task 4 — Death drops the flag

In `mech_kill`, if the dying mech is carrying a flag (any of `world->flags[i].carrier_mech == mech_id`), drop the flag at the death position with 30 s auto-return timer. Spawn audio cue (P05's stub).

### Task 5 — Score on capture / return

Per `documents/m5/06-ctf.md` §"ctf_capture", §"ctf_return_flag":

- Capture: +5 to team, +1 to player.
- Return: +1 to player.
- TK rule: friendly carriers can't capture without their own flag home. Already handled by the capture logic.

### Task 6 — Wire protocol: `NET_MSG_FLAG_STATE`

Per `documents/m5/06-ctf.md` §"Wire protocol":

```c
NET_MSG_FLAG_STATE = 43,    // server → all (NET_CH_EVENT)
```

26-byte payload: msg_type + flag_count + 2 × FlagWire (12 bytes each).

Server broadcasts on every state transition. Client mirrors locally. INITIAL_STATE also carries flag state for joining clients.

### Task 7 — Flag render

In `src/render.c::draw_mech` (or new `draw_flags`), per `documents/m5/06-ctf.md` §"Flag rendering":

Render flags as a vertical staff + triangular pennant. Team-coloured. At `flag.home_pos` for HOME, `flag.dropped_pos` for DROPPED, at `mech.chest + offset` for CARRIED. Pennant waves on carrier (sin offset on tip vertex).

### Task 8 — HUD compass arrow

In `src/hud.c`, add a `draw_flag_compass` per `documents/m5/06-ctf.md` §"HUD" + §"Off-screen compass arrow":

For each flag, project its world position to screen. If off-screen, draw a small triangular arrow at the closest screen edge pointing toward the flag, colored by team. Distance proportional to off-screen distance (subtle hint).

### Task 9 — Round-end + capture limit

In `match_round_should_end`, the existing TDM rule reuses team_score; CTF score limit defaults to 5 (per the doc).

### Task 10 — Mode-mask validation in `start_round`

Per `documents/m5/06-ctf.md` §"Configuration":

When the rotation lands on `mode=ctf` and the map's `META.mode_mask & MATCH_MODE_CTF` is unset, log warning and skip to next rotation entry.

## Done when

- `make` builds clean.
- A CTF map (Crossfire from P18 or a hardcoded one for now): pick up enemy flag, carry to your base while your flag is home, score; both flags reset.
- Carrier dying drops flag at death position; auto-returns after 30 s.
- Friendly mech touching dropped flag returns it instantly.
- HUD score banner shows team scores; off-screen flag arrows render.
- A 4v4 LAN playtest run shows: capture rate ~1/min, no desync.
- The previous "treats as TDM" behavior is gone — CTF is now CTF.
- TRADE_OFFS entry "MATCH_MODE_CTF accepted at config but plays as TDM at M4" can be removed (it's not a discrete entry but mentioned in match.h's enum comment — clean it up).

## Out of scope

- Per-player carrier-time stat in summary: M6 polish.
- Map-specific CTF layouts: P17/P18.
- Lag-comp on flag pickup: explicitly not done (server-current).
- Visual fade of dropped-flag-about-to-auto-return: M6 polish.

## How to verify

```bash
make
./tests/net/run_3p.sh
```

Write `tests/shots/m5_ctf.shot`: 2 players, one grabs the enemy flag, runs to base, captures.

## Close-out

1. Update `CURRENT_STATE.md`: CTF in.
2. Update `TRADE_OFFS.md`:
   - **Add** "Carrier secondary is fully disabled, not partially" (pre-disclosed).
   - **Add** "Auto-return is 30 s flat (no fading visual countdown)" (pre-disclosed).
3. Update `src/match.h`'s comment on `MATCH_MODE_CTF` — no longer "plays as TDM at M4".
4. Don't commit unless explicitly asked.

## Common pitfalls

- **Flag is not a mech, not a particle, not a projectile**: it's a dedicated entity in `World.flags[2]`. Don't try to repurpose existing pools.
- **Touch radius 36 px** vs pickup's 24 px — flag radius is bigger because the flag staff is bigger than a pickup sprite.
- **Both flags must be home for capture**: this is the both-home rule. Touching your home flag with the enemy flag while your flag is in the dropped/carried state does NOT score.
- **Team comparison**: `MATCH_TEAM_FFA` aliases `MATCH_TEAM_RED` (both = 1). The CTF code must check `match.mode == MATCH_MODE_CTF` first; CTF only ever has RED or BLUE.
- **Carrier at death-on-spawn** (e.g., suicided into a deadly tile): drop flag at death position even when respawning immediately.
