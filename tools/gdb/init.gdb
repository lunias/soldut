# tools/gdb/init.gdb — auto-loaded by `make gdb*` targets.
#
# Project conventions for soldut:
#   - One Game struct (g) is the spine. World state under g->world.
#   - 16 particles per mech, indexed by m->particle_base + PART_*.
#   - PART_PELVIS = 3 (most useful — body position).
#
# This file gives gdb a few project-specific helpers so common
# debugging tasks ("where is mech 0's pelvis right now?") are one
# command instead of ten.

set print pretty on
set pagination off
set confirm off
set history save on
set history filename ~/.gdb_history_soldut
set history size 4096

# Catch SIGSEGV / SIGABRT cleanly. ENet uses SIGPIPE (ignored).
handle SIGPIPE nostop noprint pass
handle SIGSEGV stop print
handle SIGABRT stop print
handle SIGINT  stop print

# ---- Project-specific helpers -------------------------------------

# `pelv MECHID` — print pelvis position + velocity for a mech.
# Usage:  (gdb) pelv 0
#         (gdb) pelv 1
define pelv
    set $m = &game.world.mechs[$arg0]
    set $pp = &game.world.particles
    set $idx = $m->particle_base + 3
    printf "mech %d  pelv=(%.1f, %.1f)  vel=(%.2f, %.2f)  alive=%d  hp=%.0f\n", \
        $arg0, \
        $pp->pos_x[$idx], $pp->pos_y[$idx], \
        $pp->pos_x[$idx] - $pp->prev_x[$idx], \
        $pp->pos_y[$idx] - $pp->prev_y[$idx], \
        (int)$m->alive, $m->health
end
document pelv
Print pelvis position + velocity for mech ID. Usage: pelv 0
end

# `mechs` — print pelvis pos for every alive mech.
define mechs
    set $i = 0
    while $i < game.world.mech_count
        if game.world.mechs[$i].alive
            pelv $i
        end
        set $i = $i + 1
    end
end
document mechs
Print pelvis pos for every alive mech.
end

# `lobby` — print lobby slot table.
define lobby
    set $i = 0
    printf "slot in_use team ready host  name             mech_id\n"
    while $i < 32
        if game.lobby.slots[$i].in_use
            printf "%4d %5d %4d %5d %4d  %-16s %d\n", \
                $i, \
                (int)game.lobby.slots[$i].in_use, \
                game.lobby.slots[$i].team, \
                (int)game.lobby.slots[$i].ready, \
                (int)game.lobby.slots[$i].is_host, \
                game.lobby.slots[$i].name, \
                game.lobby.slots[$i].mech_id
        end
        set $i = $i + 1
    end
end
document lobby
Print the lobby slot table (in-use slots only).
end

# `match` — print match phase + score state.
define match
    set $phs = game.match.phase
    printf "mode=%d phase=%d map=%d  countdown=%.2f  time_remaining=%.2f  ", \
        game.match.mode, $phs, game.match.map_id, \
        game.match.countdown_remaining, game.match.time_remaining
    printf "scores=[R%d, B%d]  mvp_slot=%d\n", \
        game.match.team_score[1], game.match.team_score[2], \
        game.match.mvp_slot
end
document match
Print match state: mode, phase, timers, scores, mvp.
end

# `net` — print network state summary.
define net
    set $r = game.net.role
    printf "role=%d  bind_port=%u  peers=%d  bytes_sent=%uKB  bytes_recv=%uKB\n", \
        $r, game.net.bind_port, game.net.peer_count, \
        game.net.bytes_sent / 1024, game.net.bytes_recv / 1024
end
document net
Print net state: role (0=offline 1=server 2=client), port, peer count, bytes.
end

# `bp_snap` — break in snapshot_apply, useful for tracing snap-back jitter.
define bp_snap
    break snapshot_apply
    commands
        silent
        printf "snapshot_apply: ent_count=%d local_mech_id=%d\n", \
            frame->ent_count, w->local_mech_id
        continue
    end
end
document bp_snap
Set a silent breakpoint in snapshot_apply that prints ent_count + local_mech_id and continues.
end

# Helpful aliases.
alias -a c   = continue
alias -a s   = step
alias -a n   = next
alias -a fin = finish
alias -a bt  = backtrace
alias -a l   = list

printf "soldut gdb init loaded.\n"
printf "  helpers:  pelv N, mechs, lobby, match, net, bp_snap\n"
printf "  see tools/gdb/init.gdb\n"
