# P09 — `BTN_FIRE_SECONDARY` (RMB) + UI controls panel + bans.txt persistence + vote picker UI

## What this prompt does

Adds `BTN_FIRE_SECONDARY` (bit 12) bound to RMB so the secondary slot fires one-shot without `BTN_SWAP`. Implements `fire_other_slot_one_shot` that swaps weapon stats temporarily for the dispatch then restores. Adds the host-controls panel (kick/ban buttons in the player list with confirmation modals). Implements `bans.txt` persistence (load on start, append on update). Implements the three-card map vote picker on the summary screen.

Small grab-bag prompt; ~2 hours of work. Resolves four trade-offs.

## Required reading

1. `CLAUDE.md`
2. `documents/02-game-design.md` §"Lobby flow"
3. **`documents/m5/13-controls-and-residuals.md`** — the spec for BTN_FIRE_SECONDARY
4. `documents/m5/07-maps.md` §"Vote picker UI", §"Host controls"
5. `TRADE_OFFS.md` — "Map vote picker UI is partial", "Kick / ban UI not exposed", "bans.txt not persisted"
6. `src/input.h` — bitmask
7. `src/platform.c` — input sampling
8. `src/mech.{c,h}` — `mech_try_fire`, `swap_weapon`
9. `src/weapons.{c,h}` — fire dispatch
10. `src/lobby.{c,h}` — `lobby_ban_addr`, `LobbyBan`
11. `src/lobby_ui.c` — player list rendering, summary screen
12. `src/net.{c,h}` — `NET_MSG_LOBBY_KICK`, `LOBBY_VOTE_STATE`

## Concrete tasks

### Task 1 — `BTN_FIRE_SECONDARY` in input bitmask

In `src/input.h`:

```c
BTN_FIRE_SECONDARY = 1u << 12,    // NEW — fires secondary slot one-shot
```

In `src/platform.c::platform_sample_input`:

```c
if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) buttons |= BTN_FIRE_SECONDARY;
```

### Task 2 — `fire_other_slot_one_shot` in `mech.c`

Per `documents/m5/13-controls-and-residuals.md` §"Implementation in `mech_try_fire`":

```c
static void fire_other_slot_one_shot(World *w, int mid) {
    Mech *m = &w->mechs[mid];
    int other_slot = m->active_slot ^ 1;
    int weapon_id  = (other_slot == 0) ? m->primary_id : m->secondary_id;
    int *ammo_ptr  = (other_slot == 0) ? &m->ammo_primary : &m->ammo_secondary;
    const Weapon *wpn = weapon_def(weapon_id);
    if (!wpn) return;
    if (m->fire_cooldown > 0.0f) return;
    if (m->reload_timer > 0.0f) return;
    if (wpn->mag_size > 0 && *ammo_ptr <= 0) return;

    // Save active state.
    int saved_w = m->weapon_id;
    int saved_a = m->ammo;
    int saved_am = m->ammo_max;

    m->weapon_id = weapon_id;
    m->ammo      = *ammo_ptr;
    m->ammo_max  = wpn->mag_size;

    // Dispatch by fire kind — same switch as fire_active_slot.
    weapons_fire_dispatch(w, mid, wpn);

    // Decrement OTHER slot's ammo.
    *ammo_ptr = m->ammo;

    // Restore.
    m->weapon_id = saved_w;
    m->ammo      = saved_a;
    m->ammo_max  = saved_am;
}
```

Factor out `weapons_fire_dispatch(World*, int mid, const Weapon*)` from the existing fire path so both `fire_active_slot` and `fire_other_slot_one_shot` can call it.

In `mech_try_fire`, add:

```c
uint16_t pressed = (uint16_t)((~m->prev_buttons) & in.buttons);
if (in.buttons & BTN_FIRE) fire_active_slot(w, mid);
if (pressed & BTN_FIRE_SECONDARY) fire_other_slot_one_shot(w, mid);
```

(Note: BTN_FIRE_SECONDARY is edge-triggered (`pressed`) to avoid auto-fire on hold; BTN_FIRE may stay level-triggered for full-auto weapons. Match existing convention.)

### Task 3 — Carrier penalty interaction

If the player carries a CTF flag (P07), `fire_other_slot_one_shot` checks: if the *other* slot is the secondary slot AND the player is a flag carrier, return without firing. (Carriers can't fire secondary; the existing rule applies whichever path triggers it.)

### Task 4 — Host-controls panel: kick/ban buttons

In `src/lobby_ui.c::player_row`, when local user is host AND the row is not the host's own slot, show two small buttons inside the row: `[Kick]` `[Ban]`.

Click → modal confirmation:

```c
"Kick PlayerName?"
[Cancel]   [Kick]
```

For Ban: `"Ban PlayerName? (persists across host restarts)"`.

Confirmed → `net_client_send_kick(slot)` / `net_client_send_ban(slot)` (already wired in M4).

Use raygui's `GuiMessageBox` for the modal.

### Task 5 — `bans.txt` persistence

Per `documents/m5/07-maps.md` §"Host controls":

In `src/lobby.c`:

```c
void lobby_load_bans(LobbyState *L, const char *path);   // called at server start
void lobby_save_bans(const LobbyState *L, const char *path); // called on every ban update
```

Format: one ban per line, `<addr_hex> <name>`. ~50 LOC.

`addr_hex` is the `addr_host` u32 in hex (8 chars). Read via `fscanf(f, "%x %s\n", ...)`. Write via `fprintf`.

Hook `lobby_load_bans(&g->lobby, "bans.txt")` into `bootstrap_host`. Hook `lobby_save_bans(&g->lobby, "bans.txt")` at the end of `lobby_ban_addr` so every ban gets written.

The path is the binary-relative `"bans.txt"` (next to the executable, like `soldut.cfg`).

### Task 6 — Three-card map vote picker

In `src/lobby_ui.c::summary_screen_run`, after the scoreboard, add a 3-card panel.

When entering MATCH_PHASE_SUMMARY, the server calls `lobby_vote_start(L, a, b, c, 15.0f)` with three randomly-chosen maps from the rotation (excluding the just-played one). The vote_state ships via existing `LOBBY_VOTE_STATE` message.

UI: three cards stacked or side-by-side, each showing:
- Map thumbnail (`assets/maps/<short>_thumb.png` — placeholder if missing)
- Map display name + blurb
- Live vote tally (popcount of the candidate's vote_mask)
- "Vote" button (disabled if you've already voted or vote not active)

Click → `net_client_send_map_vote(0/1/2)` (already wired).

Vote timer counts down; when 0, server-side `lobby_vote_winner` picks; `begin_next_lobby` uses it instead of the rotation default.

Server-side: in `host_match_flow_step`'s SUMMARY phase, when `lobby_vote_start` triggered, store winner; when timer expires, override `g->match.map_id` with the winner.

### Task 7 — Updated keybinds in About screen / pause menu

Add a small keybinds list, viewable from the title screen's "About" button (if it exists; if not, add one). Per `documents/m5/13-controls-and-residuals.md` §"Updated keybind table".

## Done when

- `make` builds clean.
- LMB fires active slot; RMB fires the inactive slot one-shot.
- Trooper-with-Pulse-Rifle-and-Frags: LMB rapid-fires; RMB throws a grenade; primary stays active.
- Shared `fire_cooldown` prevents LMB+RMB double DPS.
- A flag carrier can't fire secondary via either path.
- Host can hover a non-host player row, click Kick or Ban; confirmation modal appears; clicking Confirm executes; banned IP is persisted in `bans.txt`.
- Restarting the host: `bans.txt` reloads; the previously-banned address can't re-connect.
- Round end: 3-card vote picker appears on summary; players vote; winner becomes next round's map.
- Test scaffold `tests/shots/m5_secondary_fire.shot` shows a Trooper firing primary + secondary in sequence without losing primary.

## Out of scope

- Keybind remapping UI: M6 polish.
- Controller / gamepad: out of scope per design canon.
- BTN_FIRE_SECONDARY charge weapon support (Rail Cannon's charge_sec): documented to require the active-slot path. Don't implement.
- Map thumbnails (real art): P13/P16 generates them. Use placeholder gray rectangles for now.

## How to verify

```bash
make
./tests/net/run.sh
./tests/net/run_3p.sh
```

Manual: launch host, host's lobby has `[Kick] [Ban]` buttons on join (after at least one client connects). Click → confirm → client disconnects.

## Close-out

1. Update `CURRENT_STATE.md`: BTN_FIRE_SECONDARY, host controls, bans.txt, vote picker.
2. Update `TRADE_OFFS.md`:
   - **Delete** "Map vote picker UI is partial".
   - **Delete** "Kick / ban UI not exposed".
   - **Delete** "bans.txt not persisted".
3. Don't commit unless explicitly asked.

## Common pitfalls

- **`BTN_FIRE_SECONDARY` edge-triggered, `BTN_FIRE` level-triggered**: this is intentional. Don't change BTN_FIRE to edge-triggered; full-auto weapons need the level-trigger.
- **Ammo decrement order**: save → swap → fire → restore. The fire path reads `m->ammo`; if you forget to swap before, you'll decrement the active slot's ammo instead.
- **`bans.txt` race condition**: if the host crashes between two simultaneous bans, one might be lost. Accept; add atomic-rename if it bites.
- **Vote picker shows three different maps**: use the existing rotation table, randomly pick 3 distinct entries excluding the current map. If rotation has <3 maps, fewer cards.
- **Modal click consumes the underlying click**: get raygui's modal dismissal right or you'll get double-clicks.
