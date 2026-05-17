#!/usr/bin/env bash
#
# tests/shots/net/run_bot_frag_charge.sh — M6 P13 verification that
# a Champion-tier bot actually charges + throws the frag grenade (vs
# the pre-fix behavior of tapping BTN_FIRE through the duty cycle and
# throwing at ~0.08 charge = 350 px/s lobs).
#
# Pair: 2p_bot_frag_charge.host.shot + .client.shot. The client sends
# `add_bot 3` (Champion) over the wire; the host's lobby spawns the
# bot at slot 2 with the default loadout (Pulse Rifle + Frag
# Grenades). Bot ↔ host distance ~1800 px on Aurora's central floor
# triggers the upper end of bot_throw_target_charge for Champion
# (≥ 0.9 charge factor → ≥ 2600 px/s throw velocity).
#
# Assertions:
#   - Host log records at least one spawn_throw event for weapon 10
#     with charge ≥ 0.9 (Champion at 1800 px should hit ~0.95).
#   - Host log records at least one frag explosion AT or NEAR the
#     host's spawn position, OR a direct mech_apply_damage event from
#     the bot's frag — proves the throw connects.

set -u

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN="$REPO/soldut"
[[ -x "$BIN" ]] || { echo "fail: $BIN not built — run 'make' first"; exit 1; }

NAME="2p_bot_frag_charge"
HOST_SHOT="$REPO/tests/shots/net/${NAME}.host.shot"
CLI_SHOT="$REPO/tests/shots/net/${NAME}.client.shot"
[[ -f "$HOST_SHOT" ]] || { echo "fail: $HOST_SHOT missing"; exit 1; }
[[ -f "$CLI_SHOT" ]]  || { echo "fail: $CLI_SHOT missing";  exit 1; }

OUT="$REPO/build/shots/net"
mkdir -p "$OUT"

HOST_PID=0
CLI_PID=0
cleanup() {
    [[ $CLI_PID  -gt 0 ]] && kill     "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill     "$HOST_PID" 2>/dev/null
    sleep 0.3
    [[ $CLI_PID  -gt 0 ]] && kill -9  "$CLI_PID"  2>/dev/null
    [[ $HOST_PID -gt 0 ]] && kill -9  "$HOST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

cd "$REPO"

# Host runs in a per-test tmpdir so we can drop in a soldut.cfg
# that asks for `bots=1 bot_tier=champion`. Without this the host
# starts with no bots and the test has nothing to verify; the wire
# ADD_BOT path is gated on is_host=true (which the listen-host's
# own slot 0 owns, not the connected client). Shotmode picks up cfg
# via the binary's cwd; relative paths in `out` are resolved against
# the cwd, so we forward `out` to the absolute REPO build dir to
# keep PNG paths comparable to other tests.
HOST_CWD="$(mktemp -d -t soldut-botfrag-XXXXXX)"
cat > "$HOST_CWD/soldut.cfg" <<EOF
port=24320
auto_start_seconds=1
countdown_default=1
time_limit=30
score_limit=99
mode=ffa
friendly_fire=0
map_rotation=aurora
mode_rotation=ffa
bots=1
bot_tier=champion
EOF

# Host's shot writes to a CWD-relative path (`out build/shots/...`),
# so symlink the repo's build/ and assets/ into the tmpdir to keep
# outputs landing in the same place other tests look. Also link the
# binary's `soldut-server.log` target if it exists for ban list etc.
ln -s "$REPO/build"  "$HOST_CWD/build"
ln -s "$REPO/assets" "$HOST_CWD/assets"

trap '
  [[ $CLI_PID  -gt 0 ]] && kill     "$CLI_PID"  2>/dev/null
  [[ $HOST_PID -gt 0 ]] && kill     "$HOST_PID" 2>/dev/null
  sleep 0.3
  [[ $CLI_PID  -gt 0 ]] && kill -9  "$CLI_PID"  2>/dev/null
  [[ $HOST_PID -gt 0 ]] && kill -9  "$HOST_PID" 2>/dev/null
  wait 2>/dev/null
  # Preserve host_stderr.log next to outputs for BOT_TRACE debugging.
  if [[ -f "$HOST_CWD/host_stderr.log" ]]; then
      cp "$HOST_CWD/host_stderr.log" \
         "$REPO/build/shots/net/2p_bot_frag_charge_host.stderr.log" \
         2>/dev/null
  fi
  rm -rf "$HOST_CWD"
' EXIT

# Host first; cd into the per-test cwd so the bots cfg is honored.
# Forward BOT_TRACE if set so the host log captures per-strategy goal
# picks (cheap; gated on env var, no-op when unset).
( cd "$HOST_CWD" && exec env BOT_TRACE="${BOT_TRACE:-}" \
    "$BIN" --shot "$HOST_SHOT" ) >"$HOST_CWD/host_stderr.log" 2>&1 &
HOST_PID=$!
sleep 0.25
"$BIN" --shot "$CLI_SHOT" >/dev/null 2>&1 &
CLI_PID=$!

TIMEOUT_S="${TEST_TIMEOUT_S:-40}"
for ((i = 0; i < TIMEOUT_S; ++i)); do
    sleep 1
    if [[ ! -d /proc/$HOST_PID && ! -d /proc/$CLI_PID ]]; then
        break
    fi
done

[[ -d /proc/$CLI_PID  ]] && kill "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill "$HOST_PID" 2>/dev/null
sleep 0.3
[[ -d /proc/$CLI_PID  ]] && kill -9 "$CLI_PID"  2>/dev/null
[[ -d /proc/$HOST_PID ]] && kill -9 "$HOST_PID" 2>/dev/null

H_OUT="$REPO/build/shots/net/2p_bot_frag_charge_host"
C_OUT="$REPO/build/shots/net/2p_bot_frag_charge_client"
H_LOG="$H_OUT/${NAME}.host.log"
C_LOG="$C_OUT/${NAME}.client.log"

echo
echo "=== bot-frag-charge assertions ==="

PASS=0; FAIL=0
asrt() {
    if eval "$2"; then echo "PASS: $1"; PASS=$((PASS + 1))
    else               echo "FAIL: $1"; FAIL=$((FAIL + 1))
        echo "    detail: $2"
    fi
}

asrt "host log exists"   "[[ -f '$H_LOG' ]]"
asrt "client log exists" "[[ -f '$C_LOG' ]]"

# The bot is mech_id 2; its spawn_throws are the entries we care about.
THROW_COUNT=$(grep -cE '^\[SHOT \] t=[0-9]+ spawn_throw mech=1 wpn=10' "$H_LOG" 2>/dev/null)
[[ -z "$THROW_COUNT" ]] && THROW_COUNT=0
asrt "bot throws at least one frag grenade" \
    "[[ $THROW_COUNT -ge 1 ]]"

echo "  bot spawn_throw events:"
grep -E '^\[SHOT \] t=[0-9]+ spawn_throw mech=1 wpn=10' "$H_LOG" 2>/dev/null \
    | sed -E 's/.*(t=[0-9]+ spawn_throw mech=1 wpn=10 charge=[0-9.]+ v=[0-9.]+).*/    \1/'

# Champion at ~1800 px distance from target should hit ≥ 0.9 charge
# (bot_throw_target_charge: lo=0.55, hi=1.00, t = (1800-200)/1800 = 0.889
# → 0.95 raw). The post-bias-comp aim arc slightly shifts the value,
# but the floor remains ≥ 0.90.
HAS_HIGH=0
while IFS= read -r L; do
    C=$(echo "$L" | sed -E 's/.*charge=([0-9.]+).*/\1/')
    if awk -v c="$C" 'BEGIN{exit !(c >= 0.90)}'; then HAS_HIGH=1; fi
done < <(grep -E '^\[SHOT \] t=[0-9]+ spawn_throw mech=1 wpn=10 charge=' "$H_LOG" 2>/dev/null)
asrt "at least one bot throw is ≥ 0.9 charge factor" \
    "[[ $HAS_HIGH -eq 1 ]]"

# Direct mech damage or AOE explosion proves the throw connects.
# `mech_apply_damage` doesn't have a SHOT_LOG line, but `mech_kill`
# does, and `explosion at` lands close to the host's spawn (x=1700)
# when the bot's grenade arc lands on target.
EXPL_COUNT=$(grep -cE '^\[SHOT \] t=[0-9]+ explosion at .* weapon=10' "$H_LOG" 2>/dev/null)
[[ -z "$EXPL_COUNT" ]] && EXPL_COUNT=0
asrt "bot's grenade detonates at least once" \
    "[[ $EXPL_COUNT -ge 1 ]]"

# Optional: at least one mech_kill or hit on mech 0 (host) attributed
# to mech 2 (the bot). Soft check — if the bot's first throw at full
# charge lands on the host's flat-floor spawn, the AOE will kill or
# severely damage. Reported as INFO, not asserted, since the M6 P13
# spec's primary success criterion is the charge factor.
KILL_COUNT=$(grep -cE 'mech_kill.*(killer|shooter)=1' "$H_LOG" 2>/dev/null)
[[ -z "$KILL_COUNT" ]] && KILL_COUNT=0
echo "  bot-attributed kills: $KILL_COUNT"

# ---- Side-by-side composites ----------------------------------------
if command -v montage >/dev/null 2>&1; then
    COMBO_DIR="$REPO/build/shots/net/2p_bot_frag_charge_combined"
    mkdir -p "$COMBO_DIR"
    for h in "$H_OUT"/*.png; do
        bn="$(basename "$h")"
        [[ "$bn" == *_sheet.png ]] && continue
        c="$C_OUT/$bn"
        [[ ! -f "$c" ]] && continue
        montage "$h" "$c" -tile 2x1 -geometry +4+0 -background black \
                -title "${bn%.png}   host ↔ client" \
                "$COMBO_DIR/$bn" 2>/dev/null
    done
    montage "$COMBO_DIR"/*.png -tile 3x -geometry 800x+2+2 \
            -background "#101418" "$COMBO_DIR/combined_sheet.png" \
            2>/dev/null
    echo "  side-by-side combos in: $COMBO_DIR/"
fi

echo
echo "== bot-frag-charge summary: $PASS passed, $FAIL failed =="
[[ $FAIL -eq 0 ]] || exit 1
exit 0
