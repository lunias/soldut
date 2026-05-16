#define _POSIX_C_SOURCE 200809L

/* Windows cross-build: ENet's win32.h pulls in windows.h via
 * winsock2.h. windows.h then drags in wingdi.h (which declares
 * `Rectangle` as a function) and winuser.h (which declares
 * `CloseWindow` and `ShowCursor`). Our own headers transitively
 * include raylib via math.h — raylib defines `Rectangle` as a
 * struct typedef and `CloseWindow` / `ShowCursor` as functions
 * with different signatures. Same TU = compile errors.
 *
 * NOGDI + NOUSER tell windows.h to skip wingdi.h + winuser.h, which
 * we don't need from this file (net code touches sockets, not GDI
 * or user input). WIN32_LEAN_AND_MEAN trims the rest of the
 * less-used windows.h surface for faster compilation.
 *
 * On non-Windows builds these macros are no-ops (windows.h isn't
 * included at all). */
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER

#include "net.h"

#include "audio.h"
#include "bot.h"
#include "ctf.h"
#include "decal.h"
#include "game.h"
#include "hash.h"
#include "level.h"
#include "level_io.h"
#include "lobby.h"
#include "log.h"
#include "map_cache.h"
#include "map_download.h"
#include "match.h"
#include "maps.h"
#include "mech.h"
#include "particle.h"
#include "pickup.h"
#include "projectile.h"
#include "reconcile.h"
#include "snapshot.h"
#include "weapons.h"
#include "world.h"

#include <stdio.h>

#include "../third_party/enet/include/enet/enet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Lifecycle (process global) ----------------------------------- */

static int g_net_init_count = 0;

bool net_init(void) {
    if (g_net_init_count++ > 0) return true;
    if (enet_initialize() != 0) {
        LOG_E("enet_initialize() failed");
        g_net_init_count = 0;
        return false;
    }
    LOG_I("net_init: enet %d.%d.%d ready",
          ENET_VERSION_GET_MAJOR(ENET_VERSION),
          ENET_VERSION_GET_MINOR(ENET_VERSION),
          ENET_VERSION_GET_PATCH(ENET_VERSION));
    return true;
}

void net_shutdown(void) {
    if (--g_net_init_count > 0) return;
    if (g_net_init_count < 0) g_net_init_count = 0;
    enet_deinitialize();
    LOG_I("net_shutdown: enet released");
}

/* ---- HMAC-ish keyed hash ------------------------------------------ */
/* See [TRADE_OFFS.md] — at M2 we ship a non-cryptographic keyed FNV1a
 * for handshake token signing. Threat model is "casual on-LAN spoofer";
 * a real HMAC-SHA256 upgrade is one source file away when we need it. */

static uint32_t mint_token(const uint32_t secret[4],
                           uint32_t nonce, uint32_t addr_host)
{
    uint8_t buf[16 + 4 + 4];
    memcpy(buf,      secret,      16);
    memcpy(buf + 16, &nonce,       4);
    memcpy(buf + 20, &addr_host,   4);
    return (uint32_t)(fnv1a_64(buf, sizeof buf) >> 32);
}

static void mint_secret(uint32_t out[4]) {
    /* Mix wall-clock + a few enet host_random calls. Not cryptographic;
     * the goal is a value an off-LAN attacker can't predict, which
     * blocking on the ENet RNG also gives us. */
    uint64_t t = (uint64_t)time(NULL);
    pcg32_t r;
    pcg32_seed(&r, t ^ 0xa5a5a5a5deadbeefULL,
                  enet_host_random_seed());
    for (int i = 0; i < 4; ++i) out[i] = pcg32_next(&r);
}

/* ---- Wire encoding helpers (little-endian, packed) ---------------- */

static void w_u8 (uint8_t **p, uint8_t v)  { (*p)[0]=v; *p+=1; }
static void w_u16(uint8_t **p, uint16_t v) { (*p)[0]=(uint8_t)v; (*p)[1]=(uint8_t)(v>>8); *p+=2; }
static void w_u32(uint8_t **p, uint32_t v) {
    (*p)[0]=(uint8_t)v; (*p)[1]=(uint8_t)(v>>8); (*p)[2]=(uint8_t)(v>>16); (*p)[3]=(uint8_t)(v>>24); *p+=4;
}
static void w_u64(uint8_t **p, uint64_t v) {
    for (int i = 0; i < 8; ++i) { (*p)[i]=(uint8_t)(v>>(i*8)); }
    *p += 8;
}
static void w_f32(uint8_t **p, float v) {
    uint32_t u; memcpy(&u, &v, 4); w_u32(p, u);
}
static void w_bytes(uint8_t **p, const void *src, int n) {
    memcpy(*p, src, (size_t)n); *p += n;
}

static uint8_t  r_u8 (const uint8_t **p) { uint8_t v=(*p)[0]; *p+=1; return v; }
static uint16_t r_u16(const uint8_t **p) {
    uint16_t v = (uint16_t)((*p)[0] | ((*p)[1]<<8)); *p+=2; return v;
}
static uint32_t r_u32(const uint8_t **p) {
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1]<<8) |
                 ((uint32_t)(*p)[2]<<16) | ((uint32_t)(*p)[3]<<24);
    *p += 4; return v;
}
static uint64_t r_u64(const uint8_t **p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)(*p)[i] << (i*8);
    *p += 8; return v;
}
static float r_f32(const uint8_t **p) {
    uint32_t u = r_u32(p); float v; memcpy(&v, &u, 4); return v;
}
static void r_bytes(const uint8_t **p, void *dst, int n) {
    memcpy(dst, *p, (size_t)n); *p += n;
}

/* ---- Address helpers ---------------------------------------------- */

bool net_parse_addr(const char *s, uint32_t *out_host, uint16_t *out_port,
                    uint16_t default_port)
{
    if (!s || !*s) return false;
    char host[64] = {0};
    uint16_t port = default_port;

    /* Look for ':' from the right (so IPv6 inside [] would also work
     * eventually; for M2 we only need IPv4 a.b.c.d:port). */
    const char *colon = strrchr(s, ':');
    if (colon) {
        size_t hl = (size_t)(colon - s);
        if (hl == 0 || hl >= sizeof host) return false;
        memcpy(host, s, hl);
        host[hl] = '\0';
        port = (uint16_t)atoi(colon + 1);
        if (port == 0) return false;
    } else {
        size_t hl = strlen(s);
        if (hl == 0 || hl >= sizeof host) return false;
        memcpy(host, s, hl + 1);
    }

    ENetAddress a;
    a.host = 0; a.port = port;
    if (enet_address_set_host(&a, host) != 0) {
        /* Fall back to set_host_ip (no DNS). */
        if (enet_address_set_host_ip(&a, host) != 0) return false;
    }
    *out_host = a.host;
    *out_port = port;
    return true;
}

void net_format_addr(uint32_t host, uint16_t port, char *buf, size_t buf_len) {
    ENetAddress a; a.host = host; a.port = port;
    char hostname[64];
    if (enet_address_get_host_ip(&a, hostname, sizeof hostname) == 0) {
        snprintf(buf, buf_len, "%s:%u", hostname, (unsigned)port);
    } else {
        snprintf(buf, buf_len, "?:%u", (unsigned)port);
    }
}

/* ---- Server / Client lifecycle ------------------------------------ */

static NetPeer *server_alloc_peer(NetState *ns, void *enet_peer);
static void server_free_peer(NetState *ns, NetPeer *p);
static void client_dispatch_event(NetState *ns, Game *g, ENetEvent *ev);

bool net_server_start(NetState *ns, uint16_t port, Game *g) {
    memset(ns, 0, sizeof *ns);
    ns->role = NET_ROLE_SERVER;
    ns->bind_port = port ? port : SOLDUT_DEFAULT_PORT;
    ns->discovery_socket = -1;
    /* Phase 2 — bumped from 30 to 60 Hz default. bootstrap_host will
     * override from cfg.snapshot_hz before any peer connects. */
    ns->snapshot_hz       = 60;
    ns->snapshot_interval = 1.0 / (double)ns->snapshot_hz;
    ns->interp_delay_ms   = net_interp_delay_for(ns->snapshot_hz);

    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = ns->bind_port;

    /* maxClients=NET_MAX_PEERS, channels=NET_CH_COUNT, no bandwidth caps. */
    ENetHost *h = enet_host_create(&addr, NET_MAX_PEERS, NET_CH_COUNT, 0, 0);
    if (!h) {
        LOG_E("net_server_start: enet_host_create failed on port %u", port);
        memset(ns, 0, sizeof *ns);
        return false;
    }
    ns->enet_host = h;

    /* Phase 2 — ENet's built-in range coder. Free 30–50% bandwidth
     * reduction on snapshot streams (lots of zero / repeated bytes in
     * the SoA EntitySnapshot layout). Safe with no peer caps: the
     * coder is fully symmetric on both ends. */
    enet_host_compress_with_range_coder(h);

    mint_secret(ns->secret);
    g->world.authoritative = true;

    LOG_I("net_server_start: listening on port %u (max %d peers, snapshot=%u Hz, "
          "interp=%u ms, range_coder=on)",
          (unsigned)ns->bind_port, NET_MAX_PEERS,
          (unsigned)ns->snapshot_hz, (unsigned)ns->interp_delay_ms);
    return true;
}

void net_server_set_snapshot_hz(NetState *ns, int hz) {
    if (!ns || ns->role != NET_ROLE_SERVER) return;
    if (hz < 10) hz = 10;
    if (hz > 60) hz = 60;
    ns->snapshot_hz       = (uint16_t)hz;
    ns->snapshot_interval = 1.0 / (double)hz;
    /* If the cfg installed an override (wan-fixes-2), it wins over
     * the rate-derived value. Otherwise fall back to the formula. */
    ns->interp_delay_ms   = (ns->interp_delay_override_ms > 0)
                              ? ns->interp_delay_override_ms
                              : net_interp_delay_for((uint32_t)hz);
    LOG_I("net: snapshot_hz=%d interp=%u ms%s",
          hz, (unsigned)ns->interp_delay_ms,
          ns->interp_delay_override_ms > 0 ? " (cfg override)" : "");
}

void net_set_interp_delay_override(NetState *ns, uint32_t ms) {
    if (!ns) return;
    if (ms > 0) {
        if (ms < 40u)  ms = 40u;
        if (ms > 200u) ms = 200u;
    }
    ns->interp_delay_override_ms = ms;
    /* Apply immediately if we know our snapshot rate (i.e., the rate
     * has already been set on the server, or the client has already
     * received an ACCEPT). Otherwise the next *_set_snapshot_hz /
     * client_handle_accept call picks it up. */
    if (ns->snapshot_hz > 0) {
        ns->interp_delay_ms = (ms > 0)
            ? ms
            : net_interp_delay_for(ns->snapshot_hz);
    }
}

void net_set_stats_log_interval(NetState *ns, uint32_t ms) {
    if (!ns) return;
    ns->stats_log_interval_s   = (double)ms / 1000.0;
    ns->stats_log_accum_s      = 0.0;
    ns->prev_packets_sent      = ns->packets_sent;
    ns->prev_packets_recv      = ns->packets_recv;
    ns->prev_bytes_sent_global = ns->bytes_sent;
    ns->prev_bytes_recv_global = ns->bytes_recv;
    ns->prev_snapshots_applied = ns->snapshots_applied;
    for (int i = 0; i < NET_MAX_PEERS; ++i) {
        ns->peers[i].prev_bytes_sent = ns->peers[i].bytes_sent;
        ns->peers[i].prev_bytes_recv = ns->peers[i].bytes_recv;
    }
    ns->server.prev_bytes_sent = ns->server.bytes_sent;
    ns->server.prev_bytes_recv = ns->server.bytes_recv;
    if (ms > 0) {
        LOG_I("net: NET_STATS dump enabled, interval=%u ms", (unsigned)ms);
    }
}

bool net_client_connect(NetState *ns, const char *host, uint16_t port,
                        const char *display_name, Game *g)
{
    memset(ns, 0, sizeof *ns);
    ns->role = NET_ROLE_CLIENT;
    ns->discovery_socket = -1;
    /* Defaults; the server's ACCEPT will overwrite snapshot_hz +
     * interp_delay_ms based on the host's actual configured rate. We
     * start at 30 Hz / 100 ms because that's the M2 wire-default and
     * what a pre-Phase-2 host (no ACCEPT field) implies. */
    ns->snapshot_hz       = 30;
    ns->snapshot_interval = 1.0 / 30.0;
    ns->interp_delay_ms   = NET_INTERP_DELAY_MS;
    ns->local_mech_id_assigned = -1;
    /* wan-fixes-2 — install the cfg interp delay override BEFORE the
     * ACCEPT round-trip so client_handle_accept sees it. The memset
     * just above wiped any pre-call setter; reading from g->config
     * here is the cleanest path. */
    if (g && g->config.interp_delay_ms > 0) {
        ns->interp_delay_override_ms = (uint32_t)g->config.interp_delay_ms;
        if (ns->interp_delay_override_ms < 40u)  ns->interp_delay_override_ms = 40u;
        if (ns->interp_delay_override_ms > 200u) ns->interp_delay_override_ms = 200u;
        ns->interp_delay_ms = ns->interp_delay_override_ms;
    }

    /* Outgoing-only ENet host: 1 peer, NET_CH_COUNT channels. */
    ENetHost *eh = enet_host_create(NULL, 1, NET_CH_COUNT, 0, 0);
    if (!eh) {
        LOG_E("net_client_connect: enet_host_create (client) failed");
        memset(ns, 0, sizeof *ns);
        return false;
    }
    ns->enet_host = eh;
    /* Phase 2 — symmetric with the server; ENet drops the compression
     * silently if the peer doesn't have a matching coder, but our
     * server enables the same one in net_server_start. */
    enet_host_compress_with_range_coder(eh);

    ENetAddress addr;
    addr.port = port ? port : SOLDUT_DEFAULT_PORT;
    if (enet_address_set_host(&addr, host) != 0 &&
        enet_address_set_host_ip(&addr, host) != 0)
    {
        LOG_E("net_client_connect: cannot resolve host '%s'", host);
        enet_host_destroy(eh);
        memset(ns, 0, sizeof *ns);
        return false;
    }
    char abuf[64]; net_format_addr(addr.host, addr.port, abuf, sizeof abuf);
    LOG_I("net_client_connect: connecting to %s ...", abuf);

    ENetPeer *peer = enet_host_connect(eh, &addr, NET_CH_COUNT, /*data*/0);
    if (!peer) {
        LOG_E("net_client_connect: enet_host_connect returned NULL");
        enet_host_destroy(eh);
        memset(ns, 0, sizeof *ns);
        return false;
    }

    ns->server.enet_peer = peer;
    ns->server.state     = NET_PEER_CONNECTING;
    ns->server.remote_addr_host = addr.host;
    ns->server.remote_port      = addr.port;

    /* Block waiting for the ENet low-level CONNECT to complete (or
     * disconnect/timeout). 5 seconds is generous on a LAN. */
    ENetEvent ev;
    int got_connect = 0;
    enet_uint32 deadline = 5000;
    int events_seen = 0;
    int service_rc = 0;
    while ((service_rc = enet_host_service(eh, &ev, deadline)) > 0) {
        ++events_seen;
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            got_connect = 1;
            break;
        }
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            LOG_E("net_client_connect: enet refused / timed out");
            break;
        }
        deadline = 1000;
    }
    if (!got_connect) {
        /* Diagnose what happened. service_rc 0 = clean timeout (no
         * events received during the wait — usually packets are being
         * dropped between client and server; on Windows this is most
         * commonly Windows Defender Firewall blocking the dedicated
         * child's UDP port). service_rc < 0 = ENet internal error
         * (rare; bad socket state). */
        if (service_rc == 0 && events_seen == 0) {
            LOG_E("net_client_connect: no ENet events received during "
                  "5 s wait — client never got a CONNECT or DISCONNECT "
                  "from %s. ENet sent CONNECT command(s) but the server "
                  "never responded. Either packets aren't reaching the "
                  "server (firewall / NAT / OS routing) OR the server "
                  "isn't seeing them (check the dedicated child's log "
                  "for `server: peer connected` — its absence confirms "
                  "the packets never arrived).", abuf);
        } else if (service_rc < 0) {
            LOG_E("net_client_connect: enet_host_service returned %d "
                  "(internal error) after %d event(s)", service_rc, events_seen);
        } else {
            LOG_E("net_client_connect: enet handshake gave up after %d "
                  "event(s) without a CONNECT to %s", events_seen, abuf);
        }
        enet_peer_reset(peer);
        enet_host_destroy(eh);
        memset(ns, 0, sizeof *ns);
        return false;
    }

    /* Phase 3 — apply throttle + timeout AFTER the ENet connect ack
     * lands. Configuring on a pending CONNECTING peer queues commands
     * before the protocol handshake completes, which (in 1.3.18) can
     * blunt the initial round-trip and silently kill the connect. */
    enet_peer_throttle_configure(peer,
        /*interval_ms*/ 1000u,
        /*accel*/       ENET_PEER_PACKET_THROTTLE_ACCELERATION,
        /*decel*/       ENET_PEER_PACKET_THROTTLE_DECELERATION);
    enet_peer_timeout(peer, /*limit*/32u,
                      /*timeout_min_ms*/5000u,
                      /*timeout_max_ms*/15000u);
    LOG_I("net_client_connect: enet handshake complete; sending CONNECT_REQUEST");

    /* Send CONNECT_REQUEST(version, name) on LOBBY reliable. */
    uint8_t buf[64]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_CONNECT_REQUEST);
    w_u32(&p, SOLDUT_PROTOCOL_ID);
    w_u32(&p, (uint32_t)((SOLDUT_VERSION_MAJOR << 16) |
                         (SOLDUT_VERSION_MINOR << 8) |
                          SOLDUT_VERSION_PATCH));
    char name[24] = {0};
    if (display_name) {
        size_t n = strlen(display_name);
        if (n > 23) n = 23;
        memcpy(name, display_name, n);
    } else {
        snprintf(name, sizeof name, "player");
    }
    w_bytes(&p, name, 24);

    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, NET_CH_LOBBY, pkt);
    enet_host_flush(eh);

    /* Wait up to 5 s for ACCEPT or REJECT. enet_host_service blocks
     * up to its timeout; we route each event through the same
     * dispatcher the per-frame pump uses. */
    int total_ms = 0;
    while (!ns->connected && ns->server.state != NET_PEER_FREE && total_ms < 5000) {
        int rc = enet_host_service(eh, &ev, 250);
        if (rc < 0) break;
        if (rc > 0) {
            client_dispatch_event(ns, g, &ev);
        } else {
            total_ms += 250;
        }
    }
    if (!ns->connected) {
        LOG_E("net_client_connect: handshake timed out / rejected");
        if (ns->server.enet_peer) enet_peer_reset((ENetPeer *)ns->server.enet_peer);
        if (ns->enet_host) enet_host_destroy(eh);
        memset(ns, 0, sizeof *ns);
        return false;
    }

    LOG_I("net_client_connect: ACCEPTed as client_id=%u, mech=%d",
          (unsigned)ns->local_client_id, ns->local_mech_id_assigned);
    return true;
}

void net_close(NetState *ns) {
    if (!ns->enet_host) { memset(ns, 0, sizeof *ns); return; }
    if (ns->role == NET_ROLE_CLIENT && ns->server.enet_peer) {
        enet_peer_disconnect((ENetPeer *)ns->server.enet_peer, 0);
        /* Brief drain so DISCONNECT actually leaves. */
        ENetEvent ev;
        for (int i = 0; i < 30; ++i) {
            if (enet_host_service((ENetHost *)ns->enet_host, &ev, 100) <= 0) break;
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        }
    } else if (ns->role == NET_ROLE_SERVER) {
        for (int i = 0; i < NET_MAX_PEERS; ++i) {
            if (ns->peers[i].state != NET_PEER_FREE && ns->peers[i].enet_peer) {
                enet_peer_disconnect((ENetPeer *)ns->peers[i].enet_peer, 0);
            }
        }
        enet_host_flush((ENetHost *)ns->enet_host);
    }
    enet_host_destroy((ENetHost *)ns->enet_host);
    if (ns->discovery_socket >= 0) {
        enet_socket_destroy((ENetSocket)ns->discovery_socket);
    }
    memset(ns, 0, sizeof *ns);
}


/* ---- Server: peer table ------------------------------------------- */

static NetPeer *server_alloc_peer(NetState *ns, void *enet_peer) {
    for (int i = 0; i < NET_MAX_PEERS; ++i) {
        NetPeer *p = &ns->peers[i];
        if (p->state == NET_PEER_FREE) {
            memset(p, 0, sizeof *p);
            p->enet_peer = enet_peer;
            p->client_id = (uint32_t)i;
            p->mech_id   = -1;
            p->state     = NET_PEER_CONNECTING;
            ns->peer_count++;
            return p;
        }
    }
    return NULL;
}

static NetPeer *server_find_peer(NetState *ns, void *enet_peer) {
    for (int i = 0; i < NET_MAX_PEERS; ++i) {
        if (ns->peers[i].state != NET_PEER_FREE &&
            ns->peers[i].enet_peer == enet_peer) {
            return &ns->peers[i];
        }
    }
    return NULL;
}

static void server_free_peer(NetState *ns, NetPeer *p) {
    if (p->state == NET_PEER_FREE) return;
    p->state = NET_PEER_FREE;
    p->enet_peer = NULL;
    if (ns->peer_count > 0) ns->peer_count--;
}

/* ---- Outgoing helpers --------------------------------------------- */

static void enet_send_to(void *peer, int channel, uint32_t flags,
                         const void *data, int len)
{
    if (!peer || len <= 0) return;
    ENetPacket *pkt = enet_packet_create(data, (size_t)len, flags);
    enet_peer_send((ENetPeer *)peer, (uint8_t)channel, pkt);
}

static void send_reject(void *peer, uint8_t reason) {
    uint8_t buf[8]; uint8_t *p = buf;
    w_u8(&p, NET_MSG_REJECT);
    w_u8(&p, reason);
    enet_send_to(peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

static void send_challenge(void *peer, uint32_t nonce, uint32_t token) {
    uint8_t buf[16]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_CHALLENGE);
    w_u32(&p, nonce);
    w_u32(&p, token);
    enet_send_to(peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

static void send_accept(void *peer, uint32_t client_id, int mech_id,
                        uint32_t server_time_ms, uint64_t server_tick,
                        uint16_t snapshot_hz)
{
    /* Phase 2: appended snapshot_hz (u16) so the client can derive its
     * interp delay from the actual host rate. Back-compat: an old
     * client reads the first 19 bytes and ignores trailing — keeps its
     * own default rate. A new client connecting to an old host gets
     * blen=19 and falls back to default snapshot_hz=30. */
    uint8_t buf[24]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_ACCEPT);
    w_u32(&p, client_id);
    w_u16(&p, (uint16_t)mech_id);
    w_u32(&p, server_time_ms);
    w_u64(&p, server_tick);
    w_u16(&p, snapshot_hz);
    enet_send_to(peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

/* ---- Flag state wire (P07) -------------------------------------------
 *
 * Used both by NET_MSG_FLAG_STATE broadcasts (event channel, on every
 * state transition) AND embedded in NET_MSG_INITIAL_STATE so a joining
 * client sees the correct flag positions immediately.
 *
 * Wire layout (variable size):
 *   u8  flag_count          (0 = no CTF; 2 = both flags follow)
 *   per-flag (12 bytes × flag_count):
 *     u8  team              (MATCH_TEAM_RED or _BLUE)
 *     u8  status            (FlagStatus)
 *     i8  carrier_mech      (-1 = none)
 *     u8  reserved
 *     i16 pos_x_q           (1/4 px — home_pos for HOME, dropped_pos for
 *                            DROPPED, ignored for CARRIED)
 *     i16 pos_y_q
 *     u16 return_in_ticks   (DROPPED: ticks-from-now until auto-return)
 *     u16 reserved
 *
 * Total broadcast: 1 (tag) + 1 + 24 = 26 bytes when both flags present.
 * Bandwidth: ~6 events/min × 26 B × 16 peers ≈ 42 B/s aggregate. */
#define FLAG_STATE_REC_BYTES   12
#define FLAG_STATE_MAX_BYTES   (1 + 2 * FLAG_STATE_REC_BYTES)

static int encode_flag_state(const World *w, uint8_t *p_out) {
    uint8_t *p = p_out;
    int n = (w->flag_count > 2) ? 2 : w->flag_count;
    if (n < 0) n = 0;
    w_u8(&p, (uint8_t)n);
    for (int f = 0; f < n; ++f) {
        const Flag *fl = &w->flags[f];
        Vec2 pos;
        switch ((FlagStatus)fl->status) {
            case FLAG_HOME:    pos = fl->home_pos;    break;
            case FLAG_DROPPED: pos = fl->dropped_pos; break;
            default:           pos = (Vec2){ 0.0f, 0.0f }; break;
        }
        float qx = pos.x * 4.0f, qy = pos.y * 4.0f;
        if (qx >  32760.0f) qx =  32760.0f;
        if (qx < -32760.0f) qx = -32760.0f;
        if (qy >  32760.0f) qy =  32760.0f;
        if (qy < -32760.0f) qy = -32760.0f;
        int16_t pxq = (int16_t)(qx < 0 ? qx - 0.5f : qx + 0.5f);
        int16_t pyq = (int16_t)(qy < 0 ? qy - 0.5f : qy + 0.5f);
        uint16_t return_in = 0;
        if (fl->status == FLAG_DROPPED && fl->return_at_tick > w->tick) {
            uint64_t diff = fl->return_at_tick - w->tick;
            return_in = (diff > 0xFFFFu) ? 0xFFFFu : (uint16_t)diff;
        }
        w_u8 (&p, fl->team);
        w_u8 (&p, fl->status);
        w_u8 (&p, (uint8_t)fl->carrier_mech);
        w_u8 (&p, 0);
        w_u16(&p, (uint16_t)pxq);
        w_u16(&p, (uint16_t)pyq);
        w_u16(&p, return_in);
        w_u16(&p, 0);
    }
    return (int)(p - p_out);
}

/* Decode flag-state bytes into `w`. Returns bytes consumed, or 0 on
 * malformed input. Sets w->flag_count and w->flags[] for the records
 * actually present. */
static int decode_flag_state(const uint8_t *in, int in_len, World *w) {
    if (in_len < 1) return 0;
    const uint8_t *r = in;
    uint8_t n = r_u8(&r);
    if (n > 2) return 0;
    int need = 1 + (int)n * FLAG_STATE_REC_BYTES;
    if (in_len < need) return 0;
    w->flag_count = (int)n;
    for (int f = 0; f < (int)n; ++f) {
        Flag *fl = &w->flags[f];
        fl->team           = r_u8(&r);
        fl->status         = r_u8(&r);
        fl->carrier_mech   = (int8_t)r_u8(&r);
        (void)r_u8(&r);
        int16_t pxq        = (int16_t)r_u16(&r);
        int16_t pyq        = (int16_t)r_u16(&r);
        uint16_t return_in = r_u16(&r);
        (void)r_u16(&r);
        Vec2 pos = { (float)pxq / 4.0f, (float)pyq / 4.0f };
        if (fl->status == FLAG_HOME) {
            fl->home_pos = pos;
        } else if (fl->status == FLAG_DROPPED) {
            fl->dropped_pos    = pos;
            fl->return_at_tick = w->tick + (uint64_t)return_in;
        }
        /* CARRIED: position is derived from the carrier mech's chest at
         * draw time; nothing to update here. */
    }
    return need;
}

void net_server_broadcast_flag_state(NetState *ns, const struct World *w) {
    if (!ns || !w || ns->role != NET_ROLE_SERVER) return;
    if (w->flag_count <= 0) return;
    uint8_t buf[1 + FLAG_STATE_MAX_BYTES];
    uint8_t *p = buf;
    w_u8(&p, NET_MSG_FLAG_STATE);
    int n = encode_flag_state(w, p);
    p += n;
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
    ns->bytes_sent += (uint32_t)(p - buf);
    ns->packets_sent++;
}

static void client_handle_flag_state(NetState *ns, const uint8_t *body,
                                     int blen, Game *g)
{
    (void)ns;
    /* P14 — snapshot pre-decode status + positions per flag so we can
     * play the right SFX on transitions. ctf.c plays the same cues on
     * the host's authoritative side; this mirror lets pure clients
     * hear flag pickup / drop / return / capture. The wire is sparse
     * (event-driven), so each FLAG_STATE message is by definition a
     * transition for at least one flag. */
    Flag prev_flags[2];
    int  prev_count = g->world.flag_count;
    if (prev_count < 0) prev_count = 0;
    if (prev_count > 2) prev_count = 2;
    for (int i = 0; i < prev_count; ++i) prev_flags[i] = g->world.flags[i];

    int n = decode_flag_state(body, blen, &g->world);
    if (n <= 0) {
        LOG_W("client: malformed FLAG_STATE (%d bytes)", blen);
        return;
    }

    int new_count = g->world.flag_count;
    if (new_count > 2) new_count = 2;
    int common = (prev_count < new_count) ? prev_count : new_count;
    for (int f = 0; f < common; ++f) {
        const Flag *was = &prev_flags[f];
        const Flag *is_ = &g->world.flags[f];
        if (was->status == is_->status) continue;     /* no transition */
        /* Diagnostic line — paired CTF shot tests grep this on the
         * CLIENT log to prove the host's flag-state broadcast made the
         * wire round trip. Free at production runtime (SHOT_LOG
         * compiles to a no-op outside shot mode). */
        SHOT_LOG("client_handle_flag_state flag=%d %d->%d carrier=%d",
                 f, (int)was->status, (int)is_->status,
                 (int)is_->carrier_mech);

        /* Capture: CARRIED → HOME. Globally heard regardless of
         * distance to the scoring base. */
        if (was->status == FLAG_CARRIED && is_->status == FLAG_HOME) {
            audio_play_global(SFX_FLAG_CAPTURE);
            continue;
        }
        /* Auto-return / friendly return: DROPPED → HOME. Cue at the
         * home position so it spatializes correctly. */
        if (was->status == FLAG_DROPPED && is_->status == FLAG_HOME) {
            audio_play_at(SFX_FLAG_RETURN, is_->home_pos);
            continue;
        }
        /* Drop: CARRIED → DROPPED. Cue at the new dropped position. */
        if (was->status == FLAG_CARRIED && is_->status == FLAG_DROPPED) {
            audio_play_at(SFX_FLAG_DROP, is_->dropped_pos);
            continue;
        }
        /* Pickup: HOME / DROPPED → CARRIED. Cue at the previous
         * position (where the flag was when it got grabbed). */
        if (is_->status == FLAG_CARRIED) {
            Vec2 grab_pos = (was->status == FLAG_DROPPED) ? was->dropped_pos
                                                          : was->home_pos;
            audio_play_at(SFX_FLAG_PICKUP, grab_pos);
            continue;
        }
    }
}

/* ---- Map descriptor wire (P08) ----------------------------------
 *
 * 32 bytes: u32 crc32, u32 size_bytes, u8 short_name_len, char[24]
 * short_name, u8[3] reserved. Embedded in INITIAL_STATE. crc==0 +
 * size==0 means the host has no .lvl on disk and clients should use
 * their own MapId-rotation fallback. */
static void encode_map_descriptor(uint8_t **p, const MapDescriptor *d) {
    w_u32  (p, d->crc32);
    w_u32  (p, d->size_bytes);
    w_u8   (p, d->short_name_len);
    w_bytes(p, d->short_name, 24);
    w_u8   (p, 0);
    w_u8   (p, 0);
    w_u8   (p, 0);
}

static int decode_map_descriptor(const uint8_t *in, int in_len, MapDescriptor *out) {
    if (in_len < NET_MAP_DESCRIPTOR_BYTES) return 0;
    const uint8_t *r = in;
    out->crc32          = r_u32(&r);
    out->size_bytes     = r_u32(&r);
    out->short_name_len = r_u8 (&r);
    r_bytes(&r, out->short_name, 24);
    /* Defensive null-terminate. */
    if (out->short_name_len > 23) out->short_name_len = 23;
    out->short_name[out->short_name_len] = '\0';
    (void)r_u8(&r);
    (void)r_u8(&r);
    (void)r_u8(&r);
    return NET_MAP_DESCRIPTOR_BYTES;
}

/* ---- P08 — map sharing client outbound ---------------------------- */

void net_client_send_map_request(NetState *ns, uint32_t crc32, uint32_t resume_offset) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->server.enet_peer) return;
    uint8_t buf[NET_MAP_REQUEST_BYTES];
    uint8_t *p = buf;
    w_u8 (&p, NET_MSG_MAP_REQUEST);
    w_u32(&p, crc32);
    w_u32(&p, resume_offset);
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
    LOG_I("client: MAP_REQUEST crc=%08x offset=%u", (unsigned)crc32, (unsigned)resume_offset);
}

void net_client_send_map_ready(NetState *ns, uint32_t crc32, uint8_t status) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->server.enet_peer) return;
    uint8_t buf[NET_MAP_READY_BYTES];
    uint8_t *p = buf;
    w_u8 (&p, NET_MSG_MAP_READY);
    w_u32(&p, crc32);
    w_u8 (&p, status);
    w_u8 (&p, 0);
    w_u8 (&p, 0);
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
    LOG_I("client: MAP_READY crc=%08x status=%u", (unsigned)crc32, (unsigned)status);
}

/* Returns true if the client has the .lvl locally and can be marked
 * ready immediately. Returns false if a download must be initiated.
 *
 * Resolution order:
 *   1. assets/maps/<short_name>.lvl with matching CRC
 *   2. <download_cache>/<crc32_hex>.lvl
 *   3. otherwise: begin download
 *
 * For a code-built descriptor (crc==0 size==0) we send MAP_READY
 * immediately — the client uses its own MapId fallback. */
static bool client_resolve_or_download(NetState *ns, Game *g, const MapDescriptor *desc) {
    if (desc->crc32 == 0 && desc->size_bytes == 0) {
        /* Code-built fallback signal — no download. */
        net_client_send_map_ready(ns, 0u, NET_MAP_READY_OK);
        return true;
    }
    if (desc->size_bytes > NET_MAP_MAX_FILE_BYTES) {
        LOG_E("client: descriptor size %u > cap %u — refusing",
              (unsigned)desc->size_bytes, (unsigned)NET_MAP_MAX_FILE_BYTES);
        net_client_send_map_ready(ns, desc->crc32, NET_MAP_READY_TOO_LARGE);
        return false;
    }

    /* Probe the shipped path. */
    char shipped[256];
    if (map_cache_assets_path(desc->short_name, shipped, sizeof(shipped))) {
        if (map_cache_file_crc(shipped) == desc->crc32 &&
            map_cache_file_size(shipped) == desc->size_bytes) {
            LOG_I("client: map crc=%08x found at %s", (unsigned)desc->crc32, shipped);
            net_client_send_map_ready(ns, desc->crc32, NET_MAP_READY_OK);
            return true;
        }
    }

    /* Probe the cache. */
    if (map_cache_has(desc->crc32)) {
        const char *cached = map_cache_path(desc->crc32);
        if (map_cache_file_crc(cached) == desc->crc32 &&
            map_cache_file_size(cached) == desc->size_bytes) {
            LOG_I("client: map crc=%08x found in cache at %s",
                  (unsigned)desc->crc32, cached);
            net_client_send_map_ready(ns, desc->crc32, NET_MAP_READY_OK);
            return true;
        }
    }

    /* Begin download. */
    if (!map_download_begin(&g->map_download, desc, ns->server_time)) {
        LOG_E("client: map_download_begin failed for crc=%08x", (unsigned)desc->crc32);
        return false;
    }
    net_client_send_map_request(ns, desc->crc32, 0u);
    LOG_I("client: downloading map crc=%08x (%u bytes)",
          (unsigned)desc->crc32, (unsigned)desc->size_bytes);
    return false;
}

/* ACCEPT is followed by INITIAL_STATE which carries the lobby slot
 * table + match state so the client can render the lobby UI. The
 * world snapshot stream begins only at ROUND_START — clients don't
 * need world geometry until then.
 *
 * P07: appends an optional flag-state suffix so a client joining
 * mid-CTF-round (only possible in future flows; M4 parks joiners in
 * the lobby) sees correct flag positions immediately. The suffix is
 * variable-length: 1 byte flag_count + flag_count*12. flag_count = 0
 * outside CTF rounds; the trailing byte still ships.
 *
 * P08: appends a fixed-size 32-byte MapDescriptor so the joining
 * client can decide to download the .lvl before round start. */
static void send_initial_state(void *peer, const Game *g) {
    enum { CAP = 1 + 4
              + LOBBY_LIST_WIRE_BYTES
              + MATCH_SNAPSHOT_WIRE_BYTES
              + FLAG_STATE_MAX_BYTES
              + NET_MAP_DESCRIPTOR_BYTES };
    uint8_t buf[CAP];
    uint8_t *p = buf;
    w_u8 (&p, NET_MSG_INITIAL_STATE);
    w_u32(&p, SOLDUT_PROTOCOL_ID);            /* echo so version errors show twice */
    lobby_encode_list(&g->lobby, p);          p += LOBBY_LIST_WIRE_BYTES;
    match_encode     (&g->match, p);          p += MATCH_SNAPSHOT_WIRE_BYTES;
    int fs_n = encode_flag_state(&g->world, p);
    p += fs_n;
    encode_map_descriptor(&p, &g->server_map_desc);
    enet_send_to(peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

/* ---- Server: handle inbound LOBBY messages ----------------------- */

static void server_handle_connect_request(NetState *ns, NetPeer *p,
                                          const uint8_t *body, int blen,
                                          Game *g)
{
    (void)g;
    if (blen < 4 + 4 + 24) {
        LOG_W("server: short CONNECT_REQUEST from %u", (unsigned)p->client_id);
        send_reject(p->enet_peer, NET_REJECT_VERSION_MISMATCH);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }
    const uint8_t *r = body;
    uint32_t proto = r_u32(&r);
    uint32_t version = r_u32(&r);
    char name[24]; r_bytes(&r, name, 24); name[23] = '\0';

    if (proto != SOLDUT_PROTOCOL_ID) {
        LOG_W("server: protocol mismatch from peer %u (got 0x%08x, expect 0x%08x)",
              (unsigned)p->client_id, proto, SOLDUT_PROTOCOL_ID);
        send_reject(p->enet_peer, NET_REJECT_VERSION_MISMATCH);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }
    (void)version;

    /* Mint nonce and token; remember them on the peer for the
     * upcoming response check. */
    pcg32_t r2;
    pcg32_seed(&r2, enet_host_random_seed() ^ (uint64_t)(uintptr_t)p,
                  enet_host_random_seed());
    p->nonce = pcg32_next(&r2);
    p->token = mint_token(ns->secret, p->nonce, p->remote_addr_host);
    p->state = NET_PEER_CHALLENGED;
    memcpy(p->name, name, sizeof p->name);
    p->name[sizeof p->name - 1] = '\0';

    char abuf[48]; net_format_addr(p->remote_addr_host, p->remote_port, abuf, sizeof abuf);
    LOG_I("server: CONNECT_REQUEST from %s name='%s' (challenge nonce=0x%08x)",
          abuf, name, p->nonce);

    send_challenge(p->enet_peer, p->nonce, p->token);
}

static void server_handle_challenge_response(NetState *ns, NetPeer *p,
                                             const uint8_t *body, int blen,
                                             Game *g)
{
    if (blen < 8) return;
    const uint8_t *r = body;
    uint32_t nonce = r_u32(&r);
    uint32_t token = r_u32(&r);

    if (p->state != NET_PEER_CHALLENGED ||
        nonce != p->nonce || token != p->token)
    {
        LOG_W("server: bad CHALLENGE_RESPONSE from peer %u — disconnecting",
              (unsigned)p->client_id);
        send_reject(p->enet_peer, NET_REJECT_BAD_NONCE);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }

    /* Bans persist; reject early so we don't churn slots. */
    if (lobby_is_banned(&g->lobby, p->remote_addr_host, p->name)) {
        LOG_I("server: peer %u banned — rejecting", (unsigned)p->client_id);
        send_reject(p->enet_peer, NET_REJECT_BAD_NONCE);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }

    /* M4 join flow: place the peer in a lobby slot. The mech is
     * spawned only at ROUND_START. wan-fixes-6 — on a dedicated
     * server (no in-process host player), the first peer to ACCEPT
     * is treated as the host so the lobby UI's mode/map controls
     * stay editable. Subsequent joiners are non-host clients. The
     * listen-server path doesn't reach here for slot 0 (the host
     * was pre-added by bootstrap_host), so this gate only kicks in
     * under dedicated. */
    bool any_filled = false;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        if (g->lobby.slots[i].in_use) { any_filled = true; break; }
    }
    int slot = lobby_add_slot(&g->lobby, (int)p->client_id, p->name,
                              /*is_host*/!any_filled);
    if (slot < 0) {
        send_reject(p->enet_peer, NET_REJECT_SERVER_FULL);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }
    p->mech_id = -1;             /* assigned at round start */
    p->state   = NET_PEER_ACTIVE;

    LOG_I("server: ACCEPT peer %u → lobby slot %d (name='%s')",
          (unsigned)p->client_id, slot, p->name);

    /* Auto-arm the start countdown once we have at least 2 slots
     * (host + 1 joiner) — gives players an upper bound on lobby
     * idling. The host can still hit Ready to start sooner. */
    int active = 0;
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i)
        if (g->lobby.slots[i].in_use && g->lobby.slots[i].team != MATCH_TEAM_NONE) active++;
    if (active >= 2 && !g->lobby.auto_start_active &&
        g->match.phase == MATCH_PHASE_LOBBY)
    {
        lobby_auto_start_arm(&g->lobby, g->lobby.auto_start_default);
    }

    /* Friendly system message in chat. */
    char welcome[64];
    snprintf(welcome, sizeof welcome, "%s joined the lobby", p->name);
    lobby_chat_system(&g->lobby, welcome);

    uint32_t srv_ms = (uint32_t)(ns->server_time * 1000.0);
    send_accept(p->enet_peer, p->client_id, slot, srv_ms, g->world.tick,
                ns->snapshot_hz);
    send_initial_state(p->enet_peer, g);

    /* The next net_poll iteration will run the dirty-broadcast pass
     * (lobby state changed); that ships the joined slot to existing
     * peers. */
    g->lobby.dirty = true;
}

/* ---- Server LOBBY-channel handlers -------------------------------- */

static void server_handle_lobby_loadout(NetPeer *p, const uint8_t *body,
                                        int blen, Game *g)
{
    if (blen < 5) return;
    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (slot < 0) {
        LOG_W("server_handle_lobby_loadout: no slot for peer client_id=%u",
              (unsigned)p->client_id);
        return;
    }
    /* Valid in lobby/countdown/summary; ignored once a round is active. */
    if (g->match.phase == MATCH_PHASE_ACTIVE) {
        LOG_W("server_handle_lobby_loadout: rejecting (match active) slot=%d", slot);
        return;
    }
    const uint8_t *r = body;
    MechLoadout lo = {0};
    lo.chassis_id   = (int)r_u8(&r);
    lo.primary_id   = (int)r_u8(&r);
    lo.secondary_id = (int)r_u8(&r);
    lo.armor_id     = (int)r_u8(&r);
    lo.jetpack_id   = (int)r_u8(&r);
    LOG_I("DIAG-sync: server_handle_lobby_loadout slot=%d "
          "loadout{chassis=%d primary=%d secondary=%d armor=%d jet=%d} phase=%d",
          slot, lo.chassis_id, lo.primary_id, lo.secondary_id, lo.armor_id,
          lo.jetpack_id, (int)g->match.phase);
    /* Clamp ids so a malicious or stale client can't crash us. */
    if (lo.chassis_id < 0 || lo.chassis_id >= CHASSIS_COUNT) lo.chassis_id = CHASSIS_TROOPER;
    if (lo.primary_id < 0 || lo.primary_id >= WEAPON_COUNT)  lo.primary_id = WEAPON_PULSE_RIFLE;
    if (lo.secondary_id < 0 || lo.secondary_id >= WEAPON_COUNT) lo.secondary_id = WEAPON_SIDEARM;
    if (lo.armor_id < 0 || lo.armor_id >= ARMOR_COUNT)       lo.armor_id    = ARMOR_LIGHT;
    if (lo.jetpack_id < 0 || lo.jetpack_id >= JET_COUNT)     lo.jetpack_id  = JET_STANDARD;
    lobby_set_loadout(&g->lobby, slot, lo);
}

static void server_handle_lobby_ready(NetPeer *p, const uint8_t *body,
                                      int blen, Game *g)
{
    if (blen < 1) return;
    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (slot < 0) return;
    if (g->match.phase != MATCH_PHASE_LOBBY &&
        g->match.phase != MATCH_PHASE_SUMMARY) return;
    bool ready = body[0] ? true : false;
    lobby_set_ready(&g->lobby, slot, ready);
}

static void server_handle_lobby_team(NetPeer *p, const uint8_t *body,
                                     int blen, Game *g)
{
    if (blen < 1) return;
    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (slot < 0) return;
    if (g->match.phase != MATCH_PHASE_LOBBY &&
        g->match.phase != MATCH_PHASE_SUMMARY) return;
    int team = (int)body[0];
    if (team < 0 || team >= MATCH_TEAM_COUNT) return;
    /* In FFA mode, force everyone to team 1. */
    if (g->match.mode == MATCH_MODE_FFA && team != MATCH_TEAM_NONE) team = MATCH_TEAM_FFA;
    LOG_I("server: lobby team change peer=%u slot=%d team=%d",
          (unsigned)p->client_id, slot, team);
    lobby_set_team(&g->lobby, slot, team);
}

static void server_handle_lobby_chat(NetState *ns, NetPeer *p,
                                     const uint8_t *body, int blen, Game *g)
{
    if (blen < 1) return;
    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (slot < 0) return;
    char text[LOBBY_CHAT_BYTES] = {0};
    int n = blen < LOBBY_CHAT_BYTES - 1 ? blen : LOBBY_CHAT_BYTES - 1;
    memcpy(text, body, (size_t)n);
    text[n] = '\0';
    if (lobby_chat_post(&g->lobby, slot, text, ns->server_time)) {
        /* Fan out to all peers. */
        const LobbyChatLine *line = &g->lobby.chat[(g->lobby.chat_count - 1) % LOBBY_CHAT_LINES];
        net_server_broadcast_chat(ns, line->sender_slot, line->sender_team,
                                  line->text);
    }
}

static void server_handle_lobby_vote(NetState *ns, NetPeer *p,
                                     const uint8_t *body, int blen, Game *g)
{
    if (blen < 1) return;
    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (slot < 0) return;
    int choice = (int)body[0];
    if (choice < 0 || choice > 2) return;
    lobby_vote_cast(&g->lobby, slot, choice);
    net_server_broadcast_vote_state(ns, &g->lobby);
}

/* Public: enact a kick/ban on a target slot. Used by the wire handler
 * (after host validation) and by the host's own UI (which has already
 * validated host-ness by being in this code path at all). Bans by name
 * for now — peer IP banning is captured via the addr field but the
 * existing wire path passes 0; expansion is tracked separately. */
void net_server_kick_or_ban_slot(NetState *ns, Game *g, int target_slot, bool ban) {
    (void)ns;
    if (!g) return;
    LobbySlot *ts = lobby_slot(&g->lobby, target_slot);
    if (!ts || !ts->in_use || ts->is_host) return;       /* can't kick the host */
    /* Bots have no peer; remove the slot directly + broadcast. Ban
     * doesn't apply (no IP to remember). */
    if (ts->is_bot) {
        if (ban) return;
        char msg[64];
        snprintf(msg, sizeof msg, "%s removed by host", ts->name);
        lobby_remove_slot(&g->lobby, target_slot);
        net_server_broadcast_lobby_list(&g->net, &g->lobby);
        g->lobby.dirty = false;
        lobby_chat_system(&g->lobby, msg);
        return;
    }
    if (ban) lobby_ban_addr(&g->lobby, 0, ts->name);
    /* Disconnect the corresponding peer. */
    if (ts->peer_id >= 0) {
        for (int i = 0; i < NET_MAX_PEERS; ++i) {
            if (g->net.peers[i].state == NET_PEER_FREE) continue;
            if ((int)g->net.peers[i].client_id == ts->peer_id) {
                enet_peer_disconnect_later((ENetPeer *)g->net.peers[i].enet_peer, 0);
                break;
            }
        }
    }
    /* Slot will be freed by the DISCONNECT event; chat the news. */
    char msg[80];
    snprintf(msg, sizeof msg, "%s was %s by host", ts->name, ban ? "banned" : "kicked");
    lobby_chat_system(&g->lobby, msg);
}

/* M6 P04+ — host (over the wire) requests a new bot slot. */
static void server_handle_lobby_add_bot(NetState *ns, NetPeer *p,
                                        const uint8_t *body, int blen, Game *g)
{
    if (blen < 1) return;
    int requester = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (requester < 0) return;
    if (!g->lobby.slots[requester].is_host) {
        LOG_W("server: non-host slot %d tried ADD_BOT", requester);
        return;
    }
    uint8_t tier = body[0];
    if (tier >= BOT_TIER_COUNT) tier = BOT_TIER_VETERAN;
    int bot_idx = lobby_bot_count(&g->lobby);
    int slot = lobby_add_bot_slot(&g->lobby, bot_idx, tier);
    if (slot < 0) {
        LOG_W("server: ADD_BOT — lobby full");
        return;
    }
    /* Auto-balance teams for TDM / CTF. FFA keeps the default. */
    if (g->match.mode == MATCH_MODE_TDM || g->match.mode == MATCH_MODE_CTF) {
        int red = 0, blue = 0;
        for (int j = 0; j < MAX_LOBBY_SLOTS; ++j) {
            if (!g->lobby.slots[j].in_use) continue;
            if (j == slot) continue;
            if (g->lobby.slots[j].team == MATCH_TEAM_RED)  red++;
            if (g->lobby.slots[j].team == MATCH_TEAM_BLUE) blue++;
        }
        g->lobby.slots[slot].team = (red <= blue) ? MATCH_TEAM_RED
                                                  : MATCH_TEAM_BLUE;
    }
    net_server_broadcast_lobby_list(ns, &g->lobby);
    g->lobby.dirty = false;
    LOG_I("server: ADD_BOT slot=%d tier=%s", slot, bot_tier_name((BotTier)tier));
}

static void server_handle_lobby_bot_team(NetState *ns, NetPeer *p,
                                         const uint8_t *body, int blen,
                                         Game *g)
{
    if (blen < 2) return;
    int requester = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (requester < 0) return;
    if (!g->lobby.slots[requester].is_host) {
        LOG_W("server: non-host slot %d tried BOT_TEAM", requester);
        return;
    }
    int target = (int)body[0];
    int team   = (int)body[1];
    if (target < 0 || target >= MAX_LOBBY_SLOTS) return;
    LobbySlot *ts = &g->lobby.slots[target];
    if (!ts->in_use) return;
    if (!ts->is_bot) {
        /* Humans pick their own team via LOBBY_TEAM_CHANGE; the host
         * doesn't get to flip them. Silently drop. */
        LOG_W("server: BOT_TEAM target slot=%d isn't a bot", target);
        return;
    }
    if (team != MATCH_TEAM_RED && team != MATCH_TEAM_BLUE) {
        LOG_W("server: BOT_TEAM bad team=%d (must be RED=1 or BLUE=2)", team);
        return;
    }
    lobby_set_team(&g->lobby, target, team);
    /* Reship the table immediately so every peer (including the host
     * UI as a client) sees the change before the next round_start
     * runs auto-balance. */
    net_server_broadcast_lobby_list(ns, &g->lobby);
    g->lobby.dirty = false;
    LOG_I("server: BOT_TEAM slot=%d team=%d (by host slot=%d)",
          target, team, requester);
}

static void server_handle_lobby_kick_or_ban(NetState *ns, NetPeer *p,
                                            const uint8_t *body, int blen,
                                            Game *g, bool ban)
{
    if (blen < 1) return;
    int requester = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (requester < 0) return;
    if (!g->lobby.slots[requester].is_host) {
        LOG_W("server: non-host slot %d tried to %s", requester, ban ? "ban" : "kick");
        return;
    }
    int target = (int)body[0];
    net_server_kick_or_ban_slot(ns, g, target, ban);
}

/* wan-fixes-6 — host pushed new lobby settings. Only `slot.is_host`
 * may send; we trust the senders not to flood (the lobby UI throttles
 * naturally — one update per click). Applies to match + config, then
 * re-broadcasts the canonical MATCH_STATE + MAP_DESCRIPTOR so every
 * client (including the sender) renders the canonical post-change
 * state. */
static void server_handle_lobby_host_setup(NetState *ns, NetPeer *p,
                                           const uint8_t *body, int blen,
                                           Game *g)
{
    /* 9-byte body: mode(1) + map(2) + score(2) + time(2) + rounds(1)
     * + ff(1). Pre-rounds clients sent 8 bytes — we reject those here
     * since wire-compat across this M6 revision isn't a goal. */
    if (blen < 9) return;
    int requester = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
    if (requester < 0) return;
    if (!g->lobby.slots[requester].is_host) {
        LOG_W("server: non-host slot %d tried HOST_SETUP", requester);
        return;
    }
    /* Only accept during LOBBY / SUMMARY (between rounds). Mid-match
     * setting changes would break the active simulation. */
    if (g->match.phase != MATCH_PHASE_LOBBY &&
        g->match.phase != MATCH_PHASE_SUMMARY)
    {
        LOG_W("server: HOST_SETUP rejected mid-round (phase=%d)", (int)g->match.phase);
        return;
    }

    const uint8_t *r = body;
    uint8_t  mode_byte        = r_u8 (&r);
    uint16_t map_id           = r_u16(&r);
    uint16_t score_limit      = r_u16(&r);
    uint16_t time_limit_s     = r_u16(&r);
    uint8_t  rounds_per_match = r_u8 (&r);
    uint8_t  ff               = r_u8 (&r);

    MatchModeId mode = (MatchModeId)mode_byte;
    if (mode != MATCH_MODE_FFA && mode != MATCH_MODE_TDM && mode != MATCH_MODE_CTF) {
        LOG_W("server: HOST_SETUP bad mode=%u", (unsigned)mode_byte);
        return;
    }
    /* Map-id validation against the active registry. */
    if (map_id >= (uint16_t)g_map_registry.count) {
        LOG_W("server: HOST_SETUP bad map_id=%u (count=%d)",
              (unsigned)map_id, g_map_registry.count);
        return;
    }

    g->match.mode          = mode;
    g->match.map_id        = (int)map_id;
    if (score_limit > 0)      g->match.score_limit     = (int)score_limit;
    if (time_limit_s > 0)     g->match.time_limit      = (float)time_limit_s;
    if (rounds_per_match > 0 && rounds_per_match <= 32) {
        g->match.rounds_per_match = (int)rounds_per_match;
    }
    g->match.friendly_fire = (ff != 0) || (mode == MATCH_MODE_FFA);
    g->world.friendly_fire = g->match.friendly_fire;
    /* Mirror into config so the rotation pickers and subsequent
     * start_round derive from the new state. */
    g->config.mode               = mode;
    g->config.score_limit        = g->match.score_limit;
    g->config.time_limit         = g->match.time_limit;
    g->config.rounds_per_match   = g->match.rounds_per_match;
    g->config.friendly_fire      = (ff != 0);
    g->config.map_rotation[0]    = (int)map_id;
    g->config.map_rotation_count = 1;
    g->config.mode_rotation[0]   = mode;
    g->config.mode_rotation_count= 1;

    /* Rebuild the lobby map so INITIAL_STATE / serve descriptor /
     * map_kit follow. */
    arena_reset(&g->level_arena);
    map_build((MapId)g->match.map_id, &g->world, &g->level_arena);
    const MapDef *md = map_def(g->match.map_id);
    maps_refresh_serve_info(md ? md->short_name : NULL,
                            NULL, &g->server_map_desc,
                            g->server_map_serve_path,
                            sizeof(g->server_map_serve_path));

    LOG_I("server: HOST_SETUP applied: mode=%s map=%s score=%d time=%d rounds=%d ff=%d",
          match_mode_name(mode), md ? md->short_name : "?",
          g->match.score_limit, (int)g->match.time_limit,
          g->match.rounds_per_match, (int)g->match.friendly_fire);

    net_server_broadcast_match_state(ns, &g->match);
    net_server_broadcast_map_descriptor(ns, &g->server_map_desc);
}

/* ---- M5 P08 — server-side map sharing ---------------------------- */

void net_server_broadcast_map_descriptor(NetState *ns, const MapDescriptor *desc) {
    if (!ns || ns->role != NET_ROLE_SERVER || !ns->enet_host || !desc) return;
    uint8_t buf[1 + NET_MAP_DESCRIPTOR_BYTES];
    uint8_t *p = buf;
    w_u8(&p, NET_MSG_MAP_DESCRIPTOR);
    encode_map_descriptor(&p, desc);
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_LOBBY, pkt);
    ns->bytes_sent += (uint32_t)(p - buf);
    ns->packets_sent++;
    LOG_I("server: broadcast MAP_DESCRIPTOR crc=%08x size=%u short=%s",
          (unsigned)desc->crc32, (unsigned)desc->size_bytes, desc->short_name);
}

bool net_server_all_peers_map_ready(const NetState *ns, uint32_t current_map_crc) {
    if (!ns) return true;
    if (ns->role != NET_ROLE_SERVER) return true;
    /* If the current map is code-built (crc==0), there's nothing to
     * gate on — no peer needs to download. */
    if (current_map_crc == 0) return true;
    for (int i = 0; i < NET_MAX_PEERS; ++i) {
        const NetPeer *p = &ns->peers[i];
        if (p->state != NET_PEER_ACTIVE) continue;
        if (p->map_ready_crc != current_map_crc) return false;
    }
    return true;
}

/* Stream the host's serve .lvl to a single peer in 1180-byte chunks. */
static void server_handle_map_request(NetState *ns, NetPeer *peer,
                                      const Game *g, const uint8_t *body,
                                      int blen)
{
    if (blen < NET_MAP_REQUEST_BYTES - 1) return;
    const uint8_t *r = body;
    uint32_t crc           = r_u32(&r);
    uint32_t resume_offset = r_u32(&r);

    /* Stale request — peer asks for a crc we no longer have. Re-fire
     * INITIAL_STATE so they pick up the current descriptor and try
     * again. */
    if (crc != g->server_map_desc.crc32 || crc == 0) {
        LOG_W("server: MAP_REQUEST crc=%08x doesn't match current %08x — "
              "re-sending INITIAL_STATE",
              (unsigned)crc, (unsigned)g->server_map_desc.crc32);
        send_initial_state(peer->enet_peer, g);
        return;
    }
    if (g->server_map_serve_path[0] == '\0') {
        LOG_E("server: MAP_REQUEST but no serve_path — bug");
        return;
    }
    FILE *f = fopen(g->server_map_serve_path, "rb");
    if (!f) {
        LOG_E("server: MAP_REQUEST cannot open %s", g->server_map_serve_path);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long flen = ftell(f);
    if (flen < 0) { fclose(f); return; }
    if ((uint32_t)flen != g->server_map_desc.size_bytes) {
        LOG_W("server: serve file size %ld != descriptor %u (file changed under us)",
              flen, (unsigned)g->server_map_desc.size_bytes);
        fclose(f);
        return;
    }
    if (resume_offset > (uint32_t)flen) resume_offset = 0;
    if (fseek(f, (long)resume_offset, SEEK_SET) != 0) { fclose(f); return; }

    uint32_t off  = resume_offset;
    uint32_t total = (uint32_t)flen;
    int chunks = 0;
    while (off < total) {
        uint8_t buf[1 + NET_MAP_CHUNK_HEADER_BYTES + NET_MAP_CHUNK_PAYLOAD];
        uint32_t want = total - off;
        if (want > NET_MAP_CHUNK_PAYLOAD) want = NET_MAP_CHUNK_PAYLOAD;
        size_t got = fread(buf + 1 + NET_MAP_CHUNK_HEADER_BYTES, 1, want, f);
        if (got == 0) break;
        uint8_t *p = buf;
        w_u8 (&p, NET_MSG_MAP_CHUNK);
        w_u32(&p, crc);
        w_u32(&p, total);
        w_u32(&p, off);
        w_u16(&p, (uint16_t)got);
        uint8_t is_last = (off + (uint32_t)got >= total) ? 1 : 0;
        w_u8 (&p, is_last);
        w_u8 (&p, 0);   /* reserved */
        /* p now points to the payload area; got bytes already there */
        int total_pkt = 1 + NET_MAP_CHUNK_HEADER_BYTES + (int)got;
        enet_send_to(peer->enet_peer, NET_CH_LOBBY,
                     ENET_PACKET_FLAG_RELIABLE, buf, total_pkt);
        ns->bytes_sent  += (uint32_t)total_pkt;
        ns->packets_sent++;
        off += (uint32_t)got;
        chunks++;
    }
    fclose(f);
    LOG_I("server: MAP_REQUEST crc=%08x → streamed %d chunks (resume=%u)",
          (unsigned)crc, chunks, (unsigned)resume_offset);
}

static void server_handle_map_ready(NetPeer *peer, const uint8_t *body, int blen)
{
    if (blen < NET_MAP_READY_BYTES - 1) return;
    const uint8_t *r = body;
    uint32_t crc    = r_u32(&r);
    uint8_t  status = r_u8 (&r);
    /* reserved[2] follows */
    if (status == NET_MAP_READY_OK) {
        peer->map_ready_crc = crc;
        LOG_I("server: peer %u MAP_READY crc=%08x",
              (unsigned)peer->client_id, (unsigned)crc);
    } else {
        peer->map_ready_crc = 0;   /* not ready */
        LOG_W("server: peer %u MAP_READY status=%u crc=%08x — peer can't load this map",
              (unsigned)peer->client_id, (unsigned)status, (unsigned)crc);
    }
}

static void server_handle_input(NetState *ns, NetPeer *p,
                                const uint8_t *body, int blen, Game *g)
{
    if (p->state != NET_PEER_ACTIVE) return;
    /* Lazy mech_id resolution. p->mech_id is set to -1 at handshake
     * because the mech doesn't exist yet (it's spawned at round
     * start). The peer's mech is owned by its lobby slot, which gets
     * mech_id assigned by lobby_spawn_round_mechs. We resolve here on
     * every input — cheap (single linear walk over 32 slots) and
     * keeps net.c from needing a "after-spawn" hook into the host's
     * match-flow controller. Without this, server_handle_input
     * early-returned forever and remote players couldn't move. */
    if (p->mech_id < 0) {
        int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
        if (slot >= 0 && g->lobby.slots[slot].mech_id >= 0) {
            p->mech_id = g->lobby.slots[slot].mech_id;
        }
    }
    if (p->mech_id < 0) return;

    /* Phase 3 wire: tag has already been consumed. Body starts with
     * a u8 count followed by `count` 12-byte input records (oldest
     * first). The client always packs the most recent N=NET_INPUT_REDUNDANCY
     * inputs from its ring; the server applies any whose seq is
     * strictly newer than its `latest_input_seq` (cheap u16-wrap-safe
     * compare), dropping the rest as redundant resends. */
    if (blen < 1) return;
    const uint8_t *r = body;
    uint8_t count = r_u8(&r);
    if (count == 0 || count > NET_INPUT_REDUNDANCY) return;
    if (blen < 1 + (int)count * 12) return;

    int applied = 0;
    bool advanced = false;
    for (int i = 0; i < (int)count; ++i) {
        uint16_t seq     = r_u16(&r);
        uint16_t buttons = r_u16(&r);
        float    aim_x   = r_f32(&r);
        float    aim_y   = r_f32(&r);

        int16_t delta = (int16_t)(seq - p->latest_input_seq);
        if (p->latest_input_seq != 0 && delta <= 0) continue; /* dup / stale */

        p->latest_input.buttons = buttons;
        p->latest_input.seq     = seq;
        p->latest_input.aim_x   = aim_x;
        p->latest_input.aim_y   = aim_y;
        p->latest_input.dt      = 0.0f;     /* server uses its own dt */
        p->latest_input_seq     = seq;
        advanced = true;
        applied++;
    }
    if (!advanced) return;
    SHOT_LOG("net: input batch peer=%u count=%d applied=%d latest_seq=%u",
             (unsigned)p->client_id, (int)count, applied,
             (unsigned)p->latest_input_seq);

    /* Latch the most recent (whatever stayed in p->latest_input after
     * the loop) onto the mech for the next sim tick. */
    Mech *m = &g->world.mechs[p->mech_id];
    m->latched_input = p->latest_input;
    m->last_processed_input_seq = p->latest_input_seq;

    /* Lag-compensation view tick: the world tick the shooter was *seeing*
     * when they generated this input. The client renders remote players at
     *   client_render_time = latest_server_time - INTERP_DELAY_MS
     * and the input had to travel one-way to reach us, so the total
     * shooter→server view lag is `RTT/2 + INTERP_DELAY_MS`. Compute the
     * server tick that was current at that view moment so a follow-up
     * hitscan inside mech_try_fire can rewind bone history to those
     * positions. `weapons_fire_hitscan_lag_comp` falls back to the
     * current-time path when the value is 0 or out of LAG_HIST_TICKS
     * range, so stale-but-valid values stay safe. (See
     * [05-networking.md] §5.) */
    uint32_t rtt_ms = ((ENetPeer *)p->enet_peer)->roundTripTime;
    uint32_t view_lag_ms = (rtt_ms / 2u) + ns->interp_delay_ms;
    uint64_t view_lag_ticks = ((uint64_t)view_lag_ms * 60u + 999u) / 1000u;
    m->input_view_tick = (g->world.tick > view_lag_ticks)
                       ? (g->world.tick - view_lag_ticks)
                       : 0u;
}

/* ---- Client: handle inbound LOBBY/STATE/EVENT --------------------- */

static void client_handle_challenge(NetState *ns, const uint8_t *body, int blen) {
    if (blen < 8) return;
    const uint8_t *r = body;
    uint32_t nonce = r_u32(&r);
    uint32_t token = r_u32(&r);

    /* Echo straight back. The server is the one that validates. */
    uint8_t buf[16]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_CHALLENGE_RESPONSE);
    w_u32(&p, nonce);
    w_u32(&p, token);
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
    LOG_I("client: CHALLENGE received (nonce=0x%08x); response sent", nonce);
}

static void client_handle_accept(NetState *ns, const uint8_t *body, int blen) {
    if (blen < 18) return;
    const uint8_t *r = body;
    uint32_t client_id     = r_u32(&r);
    uint16_t mech_id       = r_u16(&r);
    uint32_t srv_ms        = r_u32(&r);
    uint64_t srv_tick      = r_u64(&r);
    (void)srv_ms; (void)srv_tick;
    ns->local_client_id        = client_id;
    ns->local_mech_id_assigned = (int)mech_id;
    /* Phase 2 — optional snapshot_hz suffix (u16). Missing from a
     * pre-Phase-2 host; we keep the default (30 Hz / 100 ms interp). */
    uint16_t hz_from_server = 0;
    if (blen >= 20) hz_from_server = r_u16(&r);
    if (hz_from_server >= 10 && hz_from_server <= 60) {
        ns->snapshot_hz       = hz_from_server;
        ns->snapshot_interval = 1.0 / (double)hz_from_server;
        /* wan-fixes-2 — local cfg override (from soldut.cfg's
         * `interp_delay_ms`) wins over the rate-derived value so
         * players can adapt to their own connection's jitter. */
        ns->interp_delay_ms   = (ns->interp_delay_override_ms > 0)
                                  ? ns->interp_delay_override_ms
                                  : net_interp_delay_for(hz_from_server);
    }
    LOG_I("client: ACCEPT client_id=%u mech_id=%u snapshot_hz=%u interp=%u ms%s",
          (unsigned)client_id, (unsigned)mech_id,
          (unsigned)ns->snapshot_hz, (unsigned)ns->interp_delay_ms,
          ns->interp_delay_override_ms > 0 ? " (cfg override)" : "");
    /* INITIAL_STATE follows on the same channel. */
}

static void client_handle_initial_state(NetState *ns, const uint8_t *body,
                                        int blen, Game *g)
{
    if (blen < 4 + LOBBY_LIST_WIRE_BYTES + MATCH_SNAPSHOT_WIRE_BYTES) {
        LOG_E("client: INITIAL_STATE too short (%d)", blen);
        return;
    }
    const uint8_t *r = body;
    uint32_t proto = r_u32(&r);
    if (proto != SOLDUT_PROTOCOL_ID) {
        LOG_E("client: INITIAL_STATE protocol mismatch");
        return;
    }
    lobby_decode_list(&g->lobby, r);    r += LOBBY_LIST_WIRE_BYTES;
    match_decode    (&g->match, r);     r += MATCH_SNAPSHOT_WIRE_BYTES;

    /* P07 — optional flag-state suffix. The flag-state encoding starts
     * with a single u8 flag_count, then 12 bytes per flag. We must
     * parse it before the descriptor (which is fixed-size at the tail
     * of the message). Compute its length first so we can offset to
     * the descriptor — decode_flag_state on a non-flag-state byte
     * stream would misinterpret the descriptor's leading byte
     * (descriptor[0] = low byte of crc32, often non-zero, which would
     * be read as flag_count). */
    int remaining = blen - (int)(r - body);
    if (remaining >= NET_MAP_DESCRIPTOR_BYTES) {
        int flag_remaining = remaining - NET_MAP_DESCRIPTOR_BYTES;
        if (flag_remaining > 0) {
            int n = decode_flag_state(r, flag_remaining, &g->world);
            if (n > 0) r += n;
        }
        /* P08 — MapDescriptor at the tail. */
        int n = decode_map_descriptor(r, NET_MAP_DESCRIPTOR_BYTES, &g->pending_map);
        if (n > 0) r += n;
    } else if (remaining > 0) {
        /* Older host with no descriptor; treat any trailing bytes as
         * flag-state. pending_map stays zeroed (= no map advertised). */
        int n = decode_flag_state(r, remaining, &g->world);
        if (n > 0) r += n;
        memset(&g->pending_map, 0, sizeof(g->pending_map));
    } else {
        memset(&g->pending_map, 0, sizeof(g->pending_map));
    }

    /* Map will be built when ROUND_START arrives. For now we keep the
     * world empty so the lobby UI has something to draw against. */
    g->world.authoritative = false;
    g->local_slot_id  = ns->local_mech_id_assigned;   /* repurposed: slot id */
    g->world.local_mech_id = -1;
    g->mode = MODE_LOBBY;

    ns->connected   = true;
    ns->server.state = NET_PEER_ACTIVE;
    LOG_I("client: INITIAL_STATE applied — in lobby (local_slot=%d, "
          "match_phase=%s map=%d)",
          g->local_slot_id, match_phase_name(g->match.phase), g->match.map_id);
    /* DIAG-sync: dump the slot table the server just shipped, so we can
     * see each peer's name + team + mech_id + loadout at INITIAL_STATE
     * time. mech_ids are -1 here (mechs spawn at round_start). */
    for (int i = 0; i < MAX_LOBBY_SLOTS; ++i) {
        const LobbySlot *s = &g->lobby.slots[i];
        if (!s->in_use) continue;
        LOG_I("DIAG-sync: INITIAL_STATE slot=%d name='%s' team=%d mech_id=%d "
              "loadout{chassis=%d primary=%d secondary=%d armor=%d jet=%d}",
              i, s->name, s->team, s->mech_id,
              s->loadout.chassis_id, s->loadout.primary_id,
              s->loadout.secondary_id, s->loadout.armor_id,
              s->loadout.jetpack_id);
    }

    /* P08 — kick off the resolve-or-download decision now that we know
     * what map the host is running. For a code-built descriptor
     * (crc=0 size=0) this is a one-shot MAP_READY ack. */
    LOG_I("client: pending map crc=%08x size=%u short=%s",
          (unsigned)g->pending_map.crc32,
          (unsigned)g->pending_map.size_bytes,
          g->pending_map.short_name);
    client_resolve_or_download(ns, g, &g->pending_map);
}

/* ---- M4 LOBBY-channel handlers (client side) -------------------- */

static void client_handle_lobby_list(const uint8_t *body, int blen, Game *g) {
    if (blen < LOBBY_LIST_WIRE_BYTES) return;
    lobby_decode_list(&g->lobby, body);
    /* One-line confirmation that the table arrived + our mech_id is
     * resolved. Useful when diagnosing a black-screen / camera-doesn't-
     * follow scenario; cheap (one log per state change on the server). */
    if (g->local_slot_id >= 0) {
        const LobbySlot *me = &g->lobby.slots[g->local_slot_id];
        LOG_I("client: lobby_list received — slot %d mech_id=%d in_use=%d",
              g->local_slot_id, me->mech_id, (int)me->in_use);
    }
}

static void client_handle_lobby_slot_update(const uint8_t *body, int blen, Game *g) {
    if (blen < LOBBY_SLOT_DELTA_WIRE_BYTES) return;
    lobby_decode_slot(&g->lobby, body);
}

static void client_handle_lobby_chat(const uint8_t *body, int blen, Game *g) {
    if (blen < LOBBY_CHAT_WIRE_BYTES) return;
    int idx = g->lobby.chat_count % LOBBY_CHAT_LINES;
    LobbyChatLine *line = &g->lobby.chat[idx];
    lobby_decode_chat_line(line, body);
    g->lobby.chat_count++;
}

static void client_handle_match_state(const uint8_t *body, int blen, Game *g) {
    if (blen < MATCH_SNAPSHOT_WIRE_BYTES) return;
    int prev_mode = g->mode;
    match_decode(&g->match, body);
    match_shot_log_phase("rx_match_state", &g->match);

    /* When the host transitions back to LOBBY after a SUMMARY (the
     * begin_next_lobby path), MATCH_STATE arrives with phase=LOBBY.
     * The client needs to leave MODE_SUMMARY/MATCH and follow —
     * without this, the summary screen sticks until the user clicks
     * Leave. ROUND_START still drives the MODE_LOBBY → MODE_MATCH
     * transition for the next round. */
    if (g->match.phase == MATCH_PHASE_LOBBY &&
        prev_mode != MODE_LOBBY && prev_mode != MODE_TITLE &&
        prev_mode != MODE_BROWSER && prev_mode != MODE_CONNECT)
    {
        lobby_clear_round_mechs(&g->lobby, &g->world);
        g->mode = MODE_LOBBY;
        LOG_I("client: MATCH_STATE phase=LOBBY → returning to lobby");
    }
}

static void client_handle_round_start(const uint8_t *body, int blen, Game *g) {
    if (blen < MATCH_SNAPSHOT_WIRE_BYTES) return;
    match_decode(&g->match, body);
    /* Rebuild the level for the chosen map. Reset the level arena so
     * we don't leak across rounds. P08 — when a descriptor is on hand
     * (host advertised a real .lvl), prefer the cached / shipped file
     * by CRC; otherwise fall back to the code-built map for this id. */
    arena_reset(&g->level_arena);
    map_build_for_descriptor(&g->world, &g->level_arena,
                             &g->pending_map, (MapId)g->match.map_id);
    decal_init((int)level_width_px(&g->world.level),
               (int)level_height_px(&g->world.level));
    /* Pools cleared so the snapshot stream can spawn mechs from
     * scratch. */
    g->world.particles.count   = 0;
    g->world.constraints.count = 0;
    g->world.projectiles.count = 0;
    g->world.mech_count        = 0;
    g->world.local_mech_id     = -1;
    g->world.dummy_mech_id     = -1;
    /* P05 — populate the spawner pool from the just-loaded level.
     * world.authoritative is false on the client, so practice-dummy
     * mechs aren't spawned here; the host's snapshot stream creates
     * them with SNAP_STATE_IS_DUMMY set. Subsequent transient pickups
     * (engineer repair packs) arrive via NET_MSG_PICKUP_STATE. */
    pickup_init_round(&g->world);
    /* P07 — populate flags[] from the just-loaded level. Same shape as
     * pickups: both sides run ctf_init_round so home_pos is locally
     * available without waiting on a network event. Subsequent state
     * transitions arrive via NET_MSG_FLAG_STATE. Mirror the mode onto
     * World so mech.c (which doesn't see Game) can branch in mech_kill. */
    g->world.match_mode_cached = (int)g->match.mode;
    ctf_init_round(&g->world, g->match.mode);

    /* P14 + post-P19 follow-up — apply per-map music + ambient on the
     * CLIENT too. Pre-fix, only the host's start_round called this,
     * so paired-window LAN tests heard music in one window only.
     * Idempotent on same-map round loops (audio_set_music_for_map
     * dedups by path). */
    audio_apply_for_level(&g->world.level);

    g->mode = MODE_MATCH;
    LOG_I("client: ROUND_START map=%d mode=%s",
          g->match.map_id, match_mode_name(g->match.mode));
    match_shot_log_phase("rx_round_start", &g->match);
}

static void client_handle_round_end(const uint8_t *body, int blen, Game *g) {
    if (blen < MATCH_SNAPSHOT_WIRE_BYTES) return;
    match_decode(&g->match, body);
    /* `summary_remaining` rides the wire as u8 deciseconds (post-P10
     * follow-up); the per-frame decay in the MODE_SUMMARY run-loop
     * case keeps the banner ticking smoothly between broadcasts. */
    g->mode = MODE_SUMMARY;
    LOG_I("client: ROUND_END mvp=%d winner_team=%d (summary %.1fs)",
          g->match.mvp_slot, g->match.winner_team,
          (double)g->match.summary_remaining);
    match_shot_log_phase("rx_round_end", &g->match);
}

static void client_handle_countdown(const uint8_t *body, int blen, Game *g) {
    if (blen < 5) return;
    const uint8_t *r = body;
    float remaining; uint32_t u = r_u32(&r); memcpy(&remaining, &u, 4);
    uint8_t reason = r_u8(&r);
    g->lobby.auto_start_remaining = remaining;
    g->lobby.auto_start_active    = (remaining > 0.0f);
    (void)reason;
}

static void client_handle_vote_state(const uint8_t *body, int blen, Game *g) {
    /* 1 + 1 + 1 + 1 + 4 + 4 + 4 + 4 = 19 bytes (active, a, b, c, ma, mb, mc, remaining_q). */
    if (blen < 19) return;
    const uint8_t *r = body;
    uint8_t active = r_u8(&r);
    uint8_t a      = r_u8(&r);
    uint8_t b      = r_u8(&r);
    uint8_t c      = r_u8(&r);
    uint32_t ma    = r_u32(&r);
    uint32_t mb    = r_u32(&r);
    uint32_t mc    = r_u32(&r);
    uint32_t rem_u = r_u32(&r);
    float remaining; memcpy(&remaining, &rem_u, 4);
    g->lobby.vote_active    = active ? true : false;
    g->lobby.vote_map_a     = (a == 0xFFu) ? -1 : (int)a;
    g->lobby.vote_map_b     = (b == 0xFFu) ? -1 : (int)b;
    g->lobby.vote_map_c     = (c == 0xFFu) ? -1 : (int)c;
    g->lobby.vote_mask_a    = ma;
    g->lobby.vote_mask_b    = mb;
    g->lobby.vote_mask_c    = mc;
    g->lobby.vote_remaining = remaining;
}

static void client_handle_snapshot(NetState *ns, const uint8_t *body,
                                   int blen, Game *g)
{
    if (!ns->connected) return;
    SnapshotFrame snap;
    if (!snapshot_decode(body, blen, NULL, &snap)) {
        LOG_W("client: snapshot decode failed (%d bytes)", blen);
        return;
    }
    /* wan-fixes-20 — reorder-tolerant admission.
     *
     * Pre-fix: snapshots strictly older than the latest-seen
     * server_time were dropped at the receive site. That was safe
     * for reconcile (which can't rewind to a stale ack point) but
     * silently threw away samples that would have been useful in
     * the per-mech interp ring. Live MN ↔ AZ playtests showed
     * ~10-37 reordered snapshots per second arriving in WAN
     * micro-burst windows; rejecting them caused render_time to
     * outpace newest_t in the ring, freezing remote-mech motion
     * for 30-80 ms at a time.
     *
     * Post-fix: full reconcile + apply only for snapshots that
     * advance server_time (existing behaviour). Reordered-but-
     * recent snapshots (within REMOTE_SNAP_STALE_MAX_MS = 250 ms
     * of the latest) take the ring-only path which updates each
     * remote mech's per-mech interp ring without touching the
     * local mech, health/weapon/team/aim fields, or the stale-
     * sweep. Snapshots older than that → drop. */
    bool advances = !ns->client_render_clock_armed ||
                    snap.header.server_time_ms >= ns->client_latest_server_time_ms;
    if (!advances) {
        uint32_t age = ns->client_latest_server_time_ms - snap.header.server_time_ms;
        if (age > REMOTE_SNAP_STALE_MAX_MS) return;
        snapshot_apply_remote_ring_only(&g->world, &snap);
        /* Don't bump snapshots_applied — this isn't a full apply
         * (no reconcile, no state advance). Net stats stay honest. */
        return;
    }
    /* One-shot log so we can see snapshots are flowing. */
    static int s_logged = 0;
    if (!s_logged) {
        s_logged = 1;
        LOG_I("client: first snapshot — %d ents, mech_count=%d, local_slot=%d",
              snap.ent_count, g->world.mech_count, g->local_slot_id);
    }
    /* P03 — advance the client render clock toward the snapshot's
     * server_time_ms. On the first snapshot, jam the clock to
     * `server_time_ms - INTERP_DELAY_MS` so we render at "100 ms in
     * the past" right away. On subsequent snapshots, only update
     * `latest_server_time_ms` (the per-tick advance in main.c keeps
     * `client_render_time_ms` moving smoothly). */
    if (snap.header.server_time_ms > ns->client_latest_server_time_ms) {
        ns->client_latest_server_time_ms = snap.header.server_time_ms;
    }
    if (!ns->client_render_clock_armed) {
        /* render_time stays double-precision and CAN go negative —
         * if first_snap.server_time < INTERP_DELAY_MS we want to be
         * "100 ms before snapshot 1 exists" so the offset to server
         * time is preserved. Clamping here would make render_time
         * track server_time directly (only ~16 ms behind in practice)
         * and snapshot_interp_remotes would always clamp to newest. */
        ns->client_render_time_ms =
            (double)snap.header.server_time_ms - (double)ns->interp_delay_ms;
        ns->client_render_clock_armed = true;
    }
    /* The snapshot's ack_input_seq tells us how far the server has
     * processed our inputs. Hand off to reconcile, which will
     * overwrite local-mech state and replay subsequent inputs.
     * Remote mechs are pushed to per-mech rings inside snapshot_apply
     * (called by reconcile_apply_snapshot) — they're written to the
     * particle pool each sim tick by snapshot_interp_remotes. */
    reconcile_apply_snapshot(&g->reconcile, &g->world, &snap,
                             snap.header.ack_input_seq, 1.0f / 60.0f);
    /* wan-fixes-17 — periodic NET_STATS reads this to compute
     * effective snapshot apply rate; if it diverges from the
     * configured snapshot_hz the link is dropping snapshots. */
    ns->snapshots_applied++;
}

/* Per-tick render-clock advance with adaptive drift correction.
 *
 * The naive contract is: `render_time += dt_ms` each sim tick,
 * matching the server's per-tick server_time advance, so the
 * INTERP_DELAY_MS gap stays constant. Two things break that:
 *
 *   1. The clock arms on the FIRST snapshot the client receives. If
 *      that arrives during LOBBY (where render_time DOES NOT advance
 *      because no MATCH simulate_step), the server keeps broadcasting
 *      and pushing `latest_server_time_ms` forward while render_time
 *      stays frozen. When the client enters MATCH, render_time and
 *      server_time advance at the same rate but the entire LOBBY /
 *      countdown duration is now baked in as permanent extra lag.
 *      Visible as: client renders the host's mech 1-2 SECONDS behind
 *      where it actually is.
 *
 *   2. WAN jitter or sim-rate skew between client and server lets
 *      latest_server_time creep ahead of render_time over time.
 *
 * Fix: each tick, slew render_time TOWARD the target
 * `latest_server_time - interp_delay_ms`. When close to the target,
 * advance at exactly `dt_ms` (smooth motion, the design intent). When
 * behind, advance up to 1.5x faster to catch up. When ahead (rendering
 * the future), slow to 0.5x. A hard snap fires only on extreme drift
 * (> 4 * interp_delay) to bound the worst case. */
void net_client_advance_render_clock(NetState *ns, double dt_ms) {
    if (!ns->client_render_clock_armed) return;

    /* The render-time target is `latest_server_time - interp_delay`.
     * Both render_time and target should advance at the same rate
     * (one server-tick / one client-tick = one snapshot). Any
     * misalignment (LOBBY froze the clock, sim-rate skew, packet
     * burst) shows up as render_time falling behind target.
     *
     * Strict tracking: never let render_time fall behind target.
     * Always advance by AT LEAST `dt_ms`; if that still leaves us
     * behind target, jump straight to target. The visual effect:
     * remote mechs may teleport forward by a few px when a stall
     * resolves, but they're never rendering 1+ second of stale state.
     * For a competitive WAN game, freshness > perfect smoothness. */
    double target   = (double)ns->client_latest_server_time_ms
                    - (double)ns->interp_delay_ms;
    double smooth   = ns->client_render_time_ms + dt_ms;
    double new_time = (smooth < target) ? target : smooth;

    if (new_time - ns->client_render_time_ms > dt_ms * 1.5) {
        SHOT_LOG("net: render_clock catch-up %.0f→%.0f (target=%.0f latest=%u)",
                 ns->client_render_time_ms, new_time, target,
                 (unsigned)ns->client_latest_server_time_ms);
    }
    ns->client_render_time_ms = new_time;
}

static void client_handle_kill_event(NetState *ns, const uint8_t *body, int blen, Game *g) {
    (void)ns;
    /* Minimum legal payload is the wan-fixes-13 layout (39 bytes after
     * the type tag — see net_server_broadcast_kill comment). Pre-fix
     * the client only consumed 6 of those bytes and never populated
     * the killfeed ring; the HUD's kill rail stayed empty for the
     * joiner the whole match. */
    if (blen < 6 + 1 + KILLFEED_NAME_BYTES * 2) return;
    const uint8_t *r = body;
    uint16_t killer = r_u16(&r);
    uint16_t victim = r_u16(&r);
    uint16_t weapon = r_u16(&r);
    uint8_t  flags  = r_u8 (&r);
    char killer_name[KILLFEED_NAME_BYTES];
    char victim_name[KILLFEED_NAME_BYTES];
    memcpy(killer_name, r, KILLFEED_NAME_BYTES);
    killer_name[KILLFEED_NAME_BYTES - 1] = '\0';
    r += KILLFEED_NAME_BYTES;
    memcpy(victim_name, r, KILLFEED_NAME_BYTES);
    victim_name[KILLFEED_NAME_BYTES - 1] = '\0';
    r += KILLFEED_NAME_BYTES;

    /* Append to the client's local killfeed ring exactly like the
     * server does in mech_kill, so draw_kill_feed picks it up. */
    int slot = g->world.killfeed_count % KILLFEED_CAPACITY;
    g->world.killfeed[slot] = (KillFeedEntry){
        .killer_mech_id = (killer == 0xFFFFu) ? -1 : (int)killer,
        .victim_mech_id = (int)victim,
        .weapon_id      = (int)weapon,
        .flags          = flags,
        .age            = 0.0f,
    };
    snprintf(g->world.killfeed[slot].killer_name,
             sizeof g->world.killfeed[slot].killer_name, "%s", killer_name);
    snprintf(g->world.killfeed[slot].victim_name,
             sizeof g->world.killfeed[slot].victim_name, "%s", victim_name);
    g->world.killfeed_count++;

    /* Keep the old single-line ribbon working too (it's read by the
     * legacy "last_event" overlay). */
    snprintf(g->world.last_event, sizeof g->world.last_event,
             "[KILL] %s -> %s", killer_name[0] ? killer_name : "world",
             victim_name[0] ? victim_name : "?");
    g->world.last_event_time = 0.0f;

    SHOT_LOG("t=%llu client_handle_kill_event killer=%d ('%s') victim=%d ('%s') weapon=%d flags=0x%02x",
             (unsigned long long)g->world.tick,
             (int)(int16_t)killer, killer_name,
             (int)victim, victim_name,
             (int)weapon, (unsigned)flags);
}

static void client_handle_fire_event(NetState *ns, const uint8_t *body, int blen, Game *g) {
    (void)ns;
    if (blen < 11) return;
    const uint8_t *r = body;
    uint16_t shooter = r_u16(&r);
    uint8_t  weapon  = r_u8 (&r);
    int16_t  oxq     = (int16_t)r_u16(&r);
    int16_t  oyq     = (int16_t)r_u16(&r);
    int16_t  dxq     = (int16_t)r_u16(&r);
    int16_t  dyq     = (int16_t)r_u16(&r);

    Vec2 origin = { (float)oxq / 4.0f, (float)oyq / 4.0f };
    Vec2 dir    = { (float)dxq / 32767.0f, (float)dyq / 32767.0f };

    const Weapon *wpn = weapon_def((int)weapon);
    if (!wpn) return;

    /* Skip-self gating depends on whether the local predict path
     * already drew this shot. The predict path
     * (`weapons_predict_local_fire`, called from simulate.c's client
     * branch) only fires when BTN_FIRE is held — never for
     * BTN_FIRE_SECONDARY (RMB) — and within that:
     *
     *   - HITSCAN: predict draws the long tracer + muzzle sparks
     *     + recoil. So a self HITSCAN FIRE_EVENT for the ACTIVE slot
     *     should be skipped (predict already drew it). But an
     *     RMB-fired inactive-slot HITSCAN (e.g., Sidearm via RMB
     *     while primary is active) was NOT predicted — the firer
     *     needs the FIRE_EVENT visual.
     *   - MELEE / PROJECTILE / SPREAD / BURST / THROW / GRAPPLE:
     *     predict only does muzzle sparks + recoil, never the
     *     swing tracer / projectile / head. The firer always needs
     *     the FIRE_EVENT visual.
     *
     * The way we tell active-slot fire from RMB-on-inactive after the
     * fact: compare the FIRE_EVENT's weapon_id against the firer's
     * currently active slot's weapon. If they match, the predict path
     * fired this shot. (Active slot only changes via BTN_SWAP, which
     * is a deliberate user action — short window for misclassification.) */
    bool is_self = ((int)shooter == g->world.local_mech_id);
    bool from_active_slot = false;
    if (is_self && shooter < g->world.mech_count) {
        const Mech *me = &g->world.mechs[shooter];
        int active_wid = (me->active_slot == 0) ? me->primary_id
                                                : me->secondary_id;
        from_active_slot = (active_wid == (int)weapon);
    }
    /* Split sparks vs SFX gates. The predict path
     * (`weapons_predict_local_fire`) spawns muzzle sparks for ALL fire
     * kinds, but plays the fire SFX only for WFIRE_HITSCAN. So:
     *
     *   - Sparks: drawn by predict for self+active regardless of fire
     *     kind → suppress here under the same condition.
     *   - SFX: played by predict only for self+active+HITSCAN. For
     *     self+active+non-HITSCAN the predict path stayed quiet AND
     *     this branch used to stay quiet too — silent fire on the
     *     firer's window for Riot Cannon / Plasma SMG / etc. (M6 Bug B.)
     *     Tighten the SFX gate to require the predict path actually
     *     played one. */
    bool predict_drew_sparks = is_self && from_active_slot;
    bool predict_drew_sfx    = predict_drew_sparks && wpn->fire == WFIRE_HITSCAN;
    if (!predict_drew_sparks) {
        for (int k = 0; k < 3; ++k) {
            fx_spawn_spark(&g->world.fx, origin,
                (Vec2){ dir.x * 350.0f, dir.y * 350.0f }, g->world.rng);
        }
    }
    if (!predict_drew_sfx) {
        /* P14 — fire SFX. The predict path plays its own cue for self
         * LMB-active hitscan; for everything else (remote shooters,
         * self RMB-on-inactive, self projectile / melee / grapple)
         * we play here. */
        SfxId fsfx = audio_sfx_for_weapon((int)weapon);
        if (wpn->fire == WFIRE_GRAPPLE) fsfx = SFX_GRAPPLE_FIRE;
        audio_play_at(fsfx, origin);
        SHOT_LOG("t=%llu client_fire_event sfx shooter=%d weapon=%d self=%d active=%d fire_kind=%d",
                 (unsigned long long)g->world.tick, (int)shooter,
                 (int)weapon, (int)is_self, (int)from_active_slot,
                 (int)wpn->fire);
    }

    if (wpn->fire == WFIRE_HITSCAN) {
        /* Predict drew the tracer only for active-slot LMB hitscan;
         * for RMB-on-inactive (Sidearm via RMB, etc.) the firer's
         * predict didn't run at all — we need to draw it here. */
        if (predict_drew_sparks) return;
        /* Tracer to the wall (or full range if open air). The actual
         * hit point lands via HIT_EVENT (blood/sparks at target);
         * the tracer just needs to look like the bullet's flight
         * path, so a level ray-cast is good enough. */
        float t_max = wpn->range_px;
        float wall_t;
        if (level_ray_hits(&g->world.level, origin,
                (Vec2){ origin.x + dir.x * wpn->range_px,
                        origin.y + dir.y * wpn->range_px },
                &wall_t)) {
            t_max = wall_t * wpn->range_px;
        }
        Vec2 end = { origin.x + dir.x * t_max,
                     origin.y + dir.y * t_max };
        fx_spawn_tracer(&g->world.fx, origin, end);
        SHOT_LOG("t=%llu client_fire_event hitscan shooter=%d self=%d active=%d",
                 (unsigned long long)g->world.tick, (int)shooter,
                 (int)is_self, (int)from_active_slot);
    } else if (wpn->fire == WFIRE_PROJECTILE ||
               wpn->fire == WFIRE_SPREAD     ||
               wpn->fire == WFIRE_BURST      ||
               wpn->fire == WFIRE_THROW)
    {
        /* Spawn a visual-only projectile. w->authoritative is false
         * on the client so projectile_step won't apply damage when
         * it hits a mech; it just renders + leaves sparks at impact.
         * Spawned for self too (predict didn't). */
        int sm = (int)shooter;
        ProjectileSpawn ps = {
            .kind          = wpn->projectile_kind,
            .weapon_id     = (int)weapon,
            .owner_mech_id = sm,
            .owner_team    = (sm < g->world.mech_count)
                              ? g->world.mechs[sm].team : 0,
            .origin        = origin,
            .velocity      = (Vec2){ dir.x * wpn->projectile_speed_pxs,
                                     dir.y * wpn->projectile_speed_pxs },
            .damage        = wpn->damage,
            .aoe_radius    = wpn->aoe_radius,
            .aoe_damage    = wpn->aoe_damage,
            .aoe_impulse   = wpn->aoe_impulse,
            .life          = wpn->projectile_life_sec,
            .gravity_scale = wpn->projectile_grav_scale,
            .drag          = wpn->projectile_drag,
            .bouncy        = wpn->bouncy,
        };
        projectile_spawn(&g->world, ps);
        /* Short muzzle tracer — only redundant when the predict path
         * already drew the muzzle visual (self LMB on the active
         * slot). For self RMB-on-inactive, predict didn't run, so we
         * draw it. */
        if (!predict_drew_sparks) {
            Vec2 spark_end = { origin.x + dir.x * 80.0f,
                               origin.y + dir.y * 80.0f };
            fx_spawn_tracer(&g->world.fx, origin, spark_end);
        }
    } else if (wpn->fire == WFIRE_GRAPPLE) {
        /* P09 — visual-only grapple head for remote viewers AND for
         * the firer themselves. The server owns the actual hit
         * decision (grapple_attach is gated on `w->authoritative` in
         * projectile_step) and snapshots the firer's grapple state,
         * so the client's head is purely cosmetic — it disappears on
         * tile/bone hit at the correct position because
         * `projectile_step`'s alive=0 path runs unconditionally. */
        int sm = (int)shooter;
        float speed = wpn->projectile_speed_pxs > 0.0f
                      ? wpn->projectile_speed_pxs : 1200.0f;
        float life  = wpn->projectile_life_sec > 0.0f
                      ? wpn->projectile_life_sec : (wpn->range_px / speed);
        ProjectileSpawn ps = {
            .kind          = (wpn->projectile_kind != 0)
                             ? wpn->projectile_kind : PROJ_GRAPPLE_HEAD,
            .weapon_id     = (int)weapon,
            .owner_mech_id = sm,
            .owner_team    = (sm < g->world.mech_count)
                              ? g->world.mechs[sm].team : 0,
            .origin        = origin,
            .velocity      = (Vec2){ dir.x * speed, dir.y * speed },
            .damage        = 0.0f,
            .aoe_radius    = 0.0f,
            .aoe_damage    = 0.0f,
            .aoe_impulse   = 0.0f,
            .life          = life,
            .gravity_scale = wpn->projectile_grav_scale,
            .drag          = wpn->projectile_drag,
            .bouncy        = false,
        };
        projectile_spawn(&g->world, ps);
        SHOT_LOG("t=%llu client_fire_event grapple shooter=%d self=%d",
                 (unsigned long long)g->world.tick, (int)shooter,
                 (int)is_self);
    } else if (wpn->fire == WFIRE_MELEE) {
        /* Predict path doesn't draw the swing tracer for any kind —
         * only the muzzle sparks. So the firer always needs the swing
         * here. (Pre-fix this was `if (is_self) return;` based on a
         * comment that didn't match the predict path's behavior.) */
        Vec2 end = { origin.x + dir.x * wpn->range_px,
                     origin.y + dir.y * wpn->range_px };
        fx_spawn_tracer(&g->world.fx, origin, end);
    }
}

/* PICKUP_STATE — apply a server-side state transition to the client's
 * local pool. Mirrors the 20-byte wire format from
 * net_server_broadcast_pickup_state. The client never initiates; it only
 * applies what the server reports.
 *
 * Two cases:
 *  - spawner_id < pool.count: existing entry. Update state +
 *    available_at_tick. Pos/kind/variant/flags should already match
 *    (level-defined entries are populated identically on both sides via
 *    pickup_init_round). We trust the wire and overwrite anyway —
 *    cheap, defends against any divergence.
 *  - spawner_id >= pool.count: new transient (engineer repair pack).
 *    Backfill entries up to spawner_id-1 with empty/consumed sentinels
 *    so the indexing stays in sync, then write the new spawner. */
static void client_handle_pickup_state(NetState *ns, const uint8_t *body, int blen, Game *g) {
    (void)ns;
    if (blen < 18) return;
    const uint8_t *r = body;
    uint8_t  spawner_id = r_u8 (&r);
    uint8_t  state      = r_u8 (&r);
    (void)r_u8(&r);                          /* reserved */
    uint64_t avail_tick = r_u64(&r);
    int16_t  pxq        = (int16_t)r_u16(&r);
    int16_t  pyq        = (int16_t)r_u16(&r);
    uint8_t  kind       = r_u8 (&r);
    uint8_t  variant    = r_u8 (&r);
    uint16_t flags      = r_u16(&r);

    PickupPool *p = &g->world.pickups;
    if ((int)spawner_id >= PICKUP_CAPACITY) {
        LOG_W("client: PICKUP_STATE id=%u out of range", (unsigned)spawner_id);
        return;
    }
    /* Backfill any missing entries up to spawner_id with consumed
     * sentinels — keeps the index alignment with the server. */
    while (p->count <= (int)spawner_id) {
        p->items[p->count++] = (PickupSpawner){
            .state             = PICKUP_STATE_COOLDOWN,
            .available_at_tick = (uint64_t)-1,
        };
    }
    PickupSpawner *s = &p->items[spawner_id];
    s->pos               = (Vec2){ (float)pxq / 4.0f, (float)pyq / 4.0f };
    s->kind              = kind;
    s->variant           = variant;
    s->state             = state;
    s->available_at_tick = avail_tick;
    s->flags             = flags;
}

static void client_handle_hit_event(NetState *ns, const uint8_t *body, int blen, Game *g) {
    (void)ns;
    if (blen < 12) return;
    const uint8_t *r = body;
    uint16_t victim = r_u16(&r);
    uint8_t  part   = r_u8 (&r);
    int16_t  pxq    = (int16_t)r_u16(&r);
    int16_t  pyq    = (int16_t)r_u16(&r);
    int16_t  dxq    = (int16_t)r_u16(&r);
    int16_t  dyq    = (int16_t)r_u16(&r);
    uint8_t  dmg    = r_u8 (&r);
    /* Dequant. */
    Vec2 hp  = { (float)pxq / 4.0f, (float)pyq / 4.0f };
    Vec2 dir = { (float)dxq / 32767.0f, (float)dyq / 32767.0f };
    /* Spawn FX. Same shape as mech_apply_damage on the server: 8
     * blood + 4 sparks per hit, scaled lightly with damage so a
     * heavy shot reads more visibly. */
    int n_blood = 8 + (int)(dmg / 16);
    if (n_blood > 16) n_blood = 16;
    int n_sparks = 4 + (int)(dmg / 32);
    if (n_sparks > 8) n_sparks = 8;
    for (int k = 0; k < n_blood; ++k) fx_spawn_blood(&g->world.fx, hp, dir, g->world.rng);
    for (int k = 0; k < n_sparks; ++k) fx_spawn_spark(&g->world.fx, hp, dir, g->world.rng);

    /* M6 P04 — flying damage-number glyph. Client-side / loopback spawn
     * into the UI-thread FxPool — this is the visible one on the host's
     * UI as well as every remote peer (per the wan-fixes-16 thread
     * split, the host's server thread runs on its own Game/FxPool which
     * never reaches the renderer). HIT_EVENT carries the post-armor
     * final damage byte, so host and client agree on digits + tier
     * without any new wire bytes. weapon_id = 0 because HIT_EVENT
     * doesn't carry a weapon id and v1 ignores it anyway. */
    if (dmg > 0) {
        fx_spawn_damage_number(&g->world.fx, hp, dir,
                               dmg, /*weapon_id=*/0, g->world.rng);
    }

    /* P14 — flesh-impact SFX on the client side. Server-authoritative
     * fire paths (weapons_fire_hitscan etc.) play this on the host;
     * client mirrors it via the wire. */
    audio_play_at(SFX_HIT_FLESH, hp);

    /* P12 — Replicate the server's per-event damage feedback so the
     * client matches host visuals beat-for-beat:
     *   - hit_flash_timer kicks the white-additive flash on the body
     *   - mech_record_damage_decal appends to the same per-limb ring
     *   - limb HP decrement so the per-limb smoke-threshold check in
     *     simulate_step fires identically on host + client (the
     *     dismember bit itself rides the next snapshot). The damage
     *     byte is the server's final post-armor / post-multiplier
     *     amount; rounding to u8 introduces ≤0.5 dmg of drift per
     *     hit, well below the 24 HP smoke threshold. */
    if ((int)victim < g->world.mech_count) {
        Mech *vm = &g->world.mechs[(int)victim];
        vm->hit_flash_timer = 0.10f;
        mech_record_damage_decal(&g->world, (int)victim, (int)part, hp, (float)dmg);

        float *limb_hp = NULL;
        switch (part) {
            case PART_HEAD:                                   limb_hp = &vm->hp_head;  break;
            case PART_L_SHOULDER: case PART_L_ELBOW:
            case PART_L_HAND:                                 limb_hp = &vm->hp_arm_l; break;
            case PART_R_SHOULDER: case PART_R_ELBOW:
            case PART_R_HAND:                                 limb_hp = &vm->hp_arm_r; break;
            case PART_L_HIP: case PART_L_KNEE: case PART_L_FOOT: limb_hp = &vm->hp_leg_l; break;
            case PART_R_HIP: case PART_R_KNEE: case PART_R_FOOT: limb_hp = &vm->hp_leg_r; break;
            default: break;
        }
        if (limb_hp) {
            *limb_hp -= (float)dmg;
            if (*limb_hp < 0.0f) *limb_hp = 0.0f;
        }
    }
    (void)dir;
}

/* wan-fixes-10 / wan-fixes-21 — client-side EXPLOSION handler.
 *
 * Spawns the visual explosion (sparks + sfx + screen shake) at the
 * SERVER's authoritative pos, and kills any matching visual-only
 * projectile the client still has alive from the original FIRE_EVENT
 * so the bouncing grenade vanishes the moment the boom appears.
 * Damage is NOT applied here (already happened on the server before
 * this broadcast); explosion_spawn's authoritative gate makes the
 * damage loop a no-op on the client.
 *
 * wan-fixes-21: if the client already spawned a PREDICTED explosion
 * for this projectile in `projectile.c::detonate`, suppress the
 * re-spawn here to avoid a double-flash. The predicted record
 * carries the local detonation pos; we don't care about
 * client/server pos divergence for visual purposes (the predicted
 * one is "good enough" and the user already saw it). The
 * authoritative damage runs server-side regardless. If we DON'T
 * have a prediction (because server's broadcast beat the client's
 * detonate), spawn normally and push a SERVER record so the later
 * detonate skips its prediction. */
static void client_handle_explosion(NetState *ns, const uint8_t *body, int blen, Game *g) {
    (void)ns;
    if (blen < 7) return;
    const uint8_t *r = body;
    int16_t  pxq    = (int16_t)r_u16(&r);
    int16_t  pyq    = (int16_t)r_u16(&r);
    uint16_t owner  = r_u16(&r);
    uint8_t  weapon = r_u8 (&r);

    Vec2 pos = { (float)pxq / 4.0f, (float)pyq / 4.0f };
    const Weapon *wpn = weapon_def((int)weapon);
    if (!wpn) return;

    /* Always log handler entry so the test-frag-grenade asserts on
     * handler count work regardless of whether the visual was
     * predicted-and-suppressed or spawned normally. */
    SHOT_LOG("t=%llu client_handle_explosion owner=%d weapon=%d at=(%.2f,%.2f)",
             (unsigned long long)g->world.tick, (int)owner, (int)weapon,
             pos.x, pos.y);

    /* Find the in-flight AOE projectile that matches this server-side
     * detonation. Used for: (a) inheriting its current position so the
     * FX spawns at the LOCAL visible pos rather than the server's
     * (which may have diverged), (b) applying the dying-frame fix to
     * the local projectile so the sprite reads at the explosion
     * center for one frame. */
    ProjectilePool *pp = &g->world.projectiles;
    int local_idx = -1;
    for (int i = 0; i < pp->count; ++i) {
        if (!pp->alive[i]) continue;
        if (pp->aoe_radius[i] <= 0.0f) continue;
        if ((int)pp->owner_mech[i] != (int)owner) continue;
        if ((int)pp->kind[i] != (int)wpn->projectile_kind) continue;
        local_idx = i;
        break;
    }

    /* wan-fixes-21 — de-dup against a recent CLIENT prediction.
     * M6 ship-prep: window bumped 30 → 600 ticks to cover bouncy frag
     * fuses (1.5 s) plus client/server bounce-path divergence plus
     * shot-mode local-tick drift (independent client world.tick when
     * not running snapshots in lockstep). Without the bigger window
     * the wire event would re-spawn the FX (and overwrite
     * last_explosion_pos), causing the camera to jump from the
     * client-predicted impact point to the server's impact point —
     * the bug the user described as "camera goes to explosion area
     * (sometimes wrong) then back to the mech."
     *
     * When the predict already fired we leave the local projectile
     * alone — projectile_step's exploded-flag path handles its
     * dying frame + kill on the next tick. */
    ExplosionRecord *pred = explosion_record_find_consume(
        &g->world, (int)owner, (int)weapon,
        EXPL_SRC_PREDICTED, /*max_age_ticks*/600);
    if (pred) {
        pred->valid = false;  /* consume; don't re-match a later event */
        return;
    }

    /* No client predict in flight. Server's wire event is the only
     * detonation signal we have. Use SERVER pos for the FX (matches
     * where damage was actually applied — bouncy grenades diverge
     * between client and server sim, so a "visible hit but no
     * damage" symptom can only be resolved by trusting the server's
     * authoritative position for BOTH damage and visual).
     *
     * Visual continuity: snap the local projectile's pos to the
     * server's pos for the dying frame. render_prev keeps the OLD
     * local pos so the next render frame lerps from LOCAL → SERVER
     * over alpha — the grenade visibly "snaps" to where it actually
     * exploded over ~16 ms. For small sim divergence (a few px) the
     * snap is invisible; for larger divergence (hundreds of px,
     * after several bounces in tight geometry) the player sees the
     * grenade rapidly translate to the true detonation point, then
     * boom. Better than "grenade vanishes here, sparks appear there
     * with no connection." */
    if (local_idx >= 0) {
        pp->exploded[local_idx]      = 1;
        pp->render_prev_x[local_idx] = pp->pos_x[local_idx];
        pp->render_prev_y[local_idx] = pp->pos_y[local_idx];
        pp->pos_x[local_idx]         = pos.x;
        pp->pos_y[local_idx]         = pos.y;
        /* alive stays 1 — projectile_step's top-of-loop check kills
         * it on the next tick after rendering the dying frame. */
    }

    int owner_team = ((int)owner < g->world.mech_count)
                         ? (int)g->world.mechs[owner].team : 0;
    explosion_spawn(&g->world, pos, wpn->aoe_radius, wpn->aoe_damage,
                    wpn->aoe_impulse, (int)owner, owner_team, (int)weapon);
    explosion_record_push(&g->world, pos, (int)owner, (int)weapon,
                          EXPL_SRC_SERVER);
}

static void client_handle_reject(NetState *ns, const uint8_t *body, int blen) {
    if (blen < 1) return;
    uint8_t reason = body[0];
    const char *r = "unknown";
    switch (reason) {
        case NET_REJECT_VERSION_MISMATCH: r = "version mismatch"; break;
        case NET_REJECT_SERVER_FULL:      r = "server full";      break;
        case NET_REJECT_BAD_NONCE:        r = "bad challenge";    break;
        case NET_REJECT_TIMEOUT:          r = "timeout";          break;
    }
    LOG_E("client: REJECT (%s)", r);
    ns->connected = false;
    ns->server.state = NET_PEER_FREE;
}

/* ---- Discovery (LAN) ---------------------------------------------- */

bool net_discovery_open(NetState *ns) {
    if (ns->discovery_socket >= 0) return true;

    ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s == ENET_SOCKET_NULL) {
        LOG_E("net_discovery_open: enet_socket_create failed");
        return false;
    }
    enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK,  1);
    enet_socket_set_option(s, ENET_SOCKOPT_BROADCAST, 1);
    enet_socket_set_option(s, ENET_SOCKOPT_REUSEADDR, 1);

    ENetAddress a;
    a.host = ENET_HOST_ANY;
    /* Server: bind to DEFAULT_PORT+1 so it can receive broadcast queries.
     * Client: bind ephemeral so its replies come back to a known port. */
    a.port = (ns->role == NET_ROLE_SERVER) ?
             (uint16_t)(SOLDUT_DEFAULT_PORT + 1) : 0;
    if (enet_socket_bind(s, &a) != 0) {
        LOG_E("net_discovery_open: bind to %u failed", (unsigned)a.port);
        enet_socket_destroy(s);
        return false;
    }
    ns->discovery_socket = (int)s;
    LOG_I("net_discovery_open: %s socket up (port=%u)",
          ns->role == NET_ROLE_SERVER ? "server" : "client",
          (unsigned)a.port);
    return true;
}

void net_discover_lan(NetState *ns) {
    if (ns->discovery_socket < 0) {
        if (!net_discovery_open(ns)) return;
    }
    uint8_t buf[8];
    uint8_t *p = buf;
    w_u8 (&p, NET_MSG_DISCOVERY_QUERY);
    w_u32(&p, SOLDUT_PROTOCOL_ID);

    ENetAddress a;
    a.host = ENET_HOST_BROADCAST;
    a.port = (uint16_t)(SOLDUT_DEFAULT_PORT + 1);
    ENetBuffer b;
    b.data = buf; b.dataLength = (size_t)(p - buf);
    int rc = enet_socket_send((ENetSocket)ns->discovery_socket, &a, &b, 1);
    if (rc < 0) {
        LOG_W("net_discover_lan: send failed");
    } else {
        LOG_I("net_discover_lan: query broadcast (%d bytes)", rc);
    }
}

static void discovery_pump(NetState *ns) {
    if (ns->discovery_socket < 0) return;

    for (;;) {
        uint8_t buf[256];
        ENetBuffer b; b.data = buf; b.dataLength = sizeof buf;
        ENetAddress from;
        int rc = enet_socket_receive((ENetSocket)ns->discovery_socket,
                                     &from, &b, 1);
        if (rc <= 0) break;

        const uint8_t *r = buf;
        const uint8_t *end = buf + rc;
        if (r >= end) continue;
        uint8_t tag = r_u8(&r);

        if (tag == NET_MSG_DISCOVERY_QUERY && ns->role == NET_ROLE_SERVER) {
            /* Validate proto, then reply. */
            if (end - r < 4) continue;
            uint32_t proto = r_u32(&r);
            if (proto != SOLDUT_PROTOCOL_ID) continue;
            uint8_t out[64]; uint8_t *q = out;
            w_u8 (&q, NET_MSG_DISCOVERY_REPLY);
            w_u32(&q, SOLDUT_PROTOCOL_ID);
            w_u16(&q, ns->bind_port);
            w_u16(&q, (uint16_t)ns->peer_count);
            w_u16(&q, (uint16_t)NET_MAX_PEERS);
            w_u16(&q, /*map_id*/0);
            char name[24] = {0};
            snprintf(name, sizeof name, "soldut-host");
            w_bytes(&q, name, 24);
            ENetBuffer ob; ob.data = out; ob.dataLength = (size_t)(q - out);
            enet_socket_send((ENetSocket)ns->discovery_socket, &from, &ob, 1);
        }
        else if (tag == NET_MSG_DISCOVERY_REPLY) {
            if (end - r < 4 + 2 + 2 + 2 + 2 + 24) continue;
            uint32_t proto = r_u32(&r);
            if (proto != SOLDUT_PROTOCOL_ID) continue;
            if (ns->discovery_count >= NET_MAX_DISCOVERIES) continue;
            NetDiscoveryEntry *e = &ns->discoveries[ns->discovery_count++];
            memset(e, 0, sizeof *e);
            e->addr_host   = from.host;
            e->port        = r_u16(&r);
            e->players     = r_u16(&r);
            e->max_players = r_u16(&r);
            e->map_id      = r_u16(&r);
            r_bytes(&r, e->name, 24);
            e->name[sizeof e->name - 1] = '\0';
            char abuf[48]; net_format_addr(e->addr_host, e->port, abuf, sizeof abuf);
            LOG_I("net_discovery: %s '%s' %u/%u",
                  abuf, e->name, e->players, e->max_players);
        }
    }
}

int net_discover_drain(NetState *ns, NetDiscoveryEntry *out, int max) {
    int n = ns->discovery_count;
    if (n > max) n = max;
    for (int i = 0; i < n; ++i) out[i] = ns->discoveries[i];
    ns->discovery_count = 0;
    return n;
}

/* ---- Per-frame pump ----------------------------------------------- */

static void server_pump_events(NetState *ns, Game *g) {
    ENetEvent ev;
    while (enet_host_service((ENetHost *)ns->enet_host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                NetPeer *p = server_alloc_peer(ns, ev.peer);
                if (!p) {
                    LOG_W("server: out of peer slots");
                    enet_peer_disconnect_now(ev.peer, 0);
                    break;
                }
                p->remote_addr_host = ev.peer->address.host;
                p->remote_port      = ev.peer->address.port;
                ev.peer->data = p;

                /* Phase 3 — tune ENet throttle so unreliable snapshots
                 * don't get internally dropped under transient WAN
                 * jitter. The default 5 s recalc interval is too lazy
                 * for an action game; 1 s lets the throttle track
                 * conditions tighter and recover faster. accel/decel
                 * stay at default (2/2). Also lower the disconnect
                 * timeout band so a real drop is detected in ~5..15 s
                 * (was 5..30 s) — matches `documents/05-networking.md`
                 * §"Disconnect / reconnect". */
                enet_peer_throttle_configure(ev.peer,
                    /*interval_ms*/ 1000u,
                    /*accel*/       ENET_PEER_PACKET_THROTTLE_ACCELERATION,
                    /*decel*/       ENET_PEER_PACKET_THROTTLE_DECELERATION);
                enet_peer_timeout(ev.peer, /*limit*/32u,
                                  /*timeout_min_ms*/5000u,
                                  /*timeout_max_ms*/15000u);

                char abuf[48];
                net_format_addr(p->remote_addr_host, p->remote_port, abuf, sizeof abuf);
                LOG_I("server: peer connected (%s) — slot %u (throttle_interval=1000ms timeout=5..15s)",
                      abuf, (unsigned)p->client_id);
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                NetPeer *p = (NetPeer *)ev.peer->data;
                if (!p) p = server_find_peer(ns, ev.peer);
                if (!p) { enet_packet_destroy(ev.packet); break; }

                if (ev.packet->dataLength == 0) {
                    enet_packet_destroy(ev.packet);
                    break;
                }
                uint8_t tag = ev.packet->data[0];
                const uint8_t *body = ev.packet->data + 1;
                int blen = (int)ev.packet->dataLength - 1;

                ns->bytes_recv  += (uint32_t)ev.packet->dataLength;
                ns->packets_recv++;
                p->bytes_recv   += (uint32_t)ev.packet->dataLength;

                switch (tag) {
                    case NET_MSG_CONNECT_REQUEST:
                        server_handle_connect_request(ns, p, body, blen, g); break;
                    case NET_MSG_CHALLENGE_RESPONSE:
                        server_handle_challenge_response(ns, p, body, blen, g); break;
                    case NET_MSG_INPUT:
                        server_handle_input(ns, p, body, blen, g); break;
                    case NET_MSG_LOBBY_LOADOUT:
                        server_handle_lobby_loadout(p, body, blen, g); break;
                    case NET_MSG_LOBBY_READY:
                        server_handle_lobby_ready(p, body, blen, g); break;
                    case NET_MSG_LOBBY_TEAM_CHANGE:
                        server_handle_lobby_team(p, body, blen, g); break;
                    case NET_MSG_LOBBY_CHAT:
                        server_handle_lobby_chat(ns, p, body, blen, g); break;
                    case NET_MSG_LOBBY_MAP_VOTE:
                        server_handle_lobby_vote(ns, p, body, blen, g); break;
                    case NET_MSG_LOBBY_KICK:
                        server_handle_lobby_kick_or_ban(ns, p, body, blen, g, false); break;
                    case NET_MSG_LOBBY_BAN:
                        server_handle_lobby_kick_or_ban(ns, p, body, blen, g, true); break;
                    case NET_MSG_LOBBY_HOST_SETUP:
                        server_handle_lobby_host_setup(ns, p, body, blen, g); break;
                    case NET_MSG_LOBBY_ADD_BOT:
                        server_handle_lobby_add_bot(ns, p, body, blen, g); break;
                    case NET_MSG_LOBBY_BOT_TEAM:
                        server_handle_lobby_bot_team(ns, p, body, blen, g); break;
                    case NET_MSG_MAP_REQUEST:
                        server_handle_map_request(ns, p, g, body, blen); break;
                    case NET_MSG_MAP_READY:
                        server_handle_map_ready(p, body, blen); break;
                    case NET_MSG_CHAT:   /* legacy in-match chat — fan out unscrubbed at M4 */
                        break;
                    default:
                        LOG_D("server: unhandled tag %u from peer %u",
                              tag, (unsigned)p->client_id);
                        break;
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                NetPeer *p = (NetPeer *)ev.peer->data;
                if (!p) p = server_find_peer(ns, ev.peer);
                if (p) {
                    int slot = lobby_find_slot_by_peer(&g->lobby, (int)p->client_id);
                    LOG_I("server: peer %u (slot %d, mech %d) disconnected",
                          (unsigned)p->client_id, slot, p->mech_id);
                    /* Mark the mech dead so client snapshots reflect it. */
                    if (p->mech_id >= 0 && p->mech_id < g->world.mech_count) {
                        Mech *m = &g->world.mechs[p->mech_id];
                        m->alive = false;
                    }
                    if (slot >= 0) {
                        char msg[80];
                        snprintf(msg, sizeof msg, "%s left",
                                 g->lobby.slots[slot].name);
                        lobby_chat_system(&g->lobby, msg);
                        lobby_remove_slot(&g->lobby, slot);
                    }
                    server_free_peer(ns, p);
                }
                ev.peer->data = NULL;
                break;
            }
            default: break;
        }
    }
}

/* ---- M5 P08 — client-side map sharing ---------------------------- */

#define NET_MAP_CACHE_CAP_BYTES (64ull * 1024ull * 1024ull)

/* Verify CRC and either write to cache + send MAP_READY ok, or send
 * one of the failure statuses. Always clears the download active flag
 * so subsequent INITIAL_STATEs can start fresh. */
static void client_finalize_map_download(NetState *ns, Game *g) {
    MapDownload *d = &g->map_download;
    if (!d->complete) {
        LOG_E("client: finalize called before complete");
        d->active = false;
        return;
    }
    uint32_t computed = level_compute_buffer_crc(d->buffer, (int)d->total_size);
    if (computed != d->crc32) {
        LOG_E("client: MAP_CHUNK reassembly CRC mismatch (got %08x, want %08x)",
              (unsigned)computed, (unsigned)d->crc32);
        net_client_send_map_ready(ns, d->crc32, NET_MAP_READY_CRC_MISMATCH);
        d->active   = false;
        d->complete = false;
        return;
    }
    if (!map_cache_write(d->crc32, d->buffer, d->total_size)) {
        LOG_E("client: failed to write cache for crc=%08x", (unsigned)d->crc32);
        net_client_send_map_ready(ns, d->crc32, NET_MAP_READY_PARSE_FAILURE);
        d->active   = false;
        d->complete = false;
        return;
    }
    /* Evict after write so we don't briefly trip a cap that the new
     * file just lifted us over. */
    map_cache_evict_lru(NET_MAP_CACHE_CAP_BYTES);

    LOG_I("client: map crc=%08x cached + ready (%u bytes)",
          (unsigned)d->crc32, (unsigned)d->total_size);
    net_client_send_map_ready(ns, d->crc32, NET_MAP_READY_OK);
    d->active   = false;
    d->complete = false;
}

static void client_handle_map_chunk(NetState *ns, Game *g,
                                    const uint8_t *body, int blen)
{
    if (blen < NET_MAP_CHUNK_HEADER_BYTES) {
        LOG_W("client: short MAP_CHUNK (%d)", blen);
        return;
    }
    const uint8_t *r = body;
    uint32_t crc          = r_u32(&r);
    uint32_t total_size   = r_u32(&r);
    uint32_t chunk_offset = r_u32(&r);
    uint16_t chunk_len    = r_u16(&r);
    uint8_t  is_last      = r_u8 (&r);
    (void)r_u8(&r);   /* reserved */

    int payload = blen - NET_MAP_CHUNK_HEADER_BYTES;
    if ((int)chunk_len > payload) {
        LOG_W("client: MAP_CHUNK payload underrun (len=%u, have %d)",
              (unsigned)chunk_len, payload);
        return;
    }
    if (!g->map_download.active) {
        /* Stale — server flushing chunks for a download we cancelled
         * or never started. Ignore. */
        return;
    }
    if (g->map_download.crc32 != crc) {
        LOG_W("client: MAP_CHUNK crc=%08x ≠ active download %08x — ignoring",
              (unsigned)crc, (unsigned)g->map_download.crc32);
        return;
    }
    if (total_size != g->map_download.total_size) {
        LOG_W("client: MAP_CHUNK total_size=%u ≠ descriptor %u — aborting",
              (unsigned)total_size, (unsigned)g->map_download.total_size);
        map_download_cancel(&g->map_download);
        return;
    }
    bool done = map_download_apply_chunk(&g->map_download,
                                         chunk_offset, r, chunk_len,
                                         ns->server_time);
    /* `done` from apply_chunk is "this chunk completed the file"; we
     * can also reach completion via is_last + last chunk (paranoia
     * check; apply_chunk already covers it). */
    (void)is_last;
    if (done) {
        client_finalize_map_download(ns, g);
    }
}

static void client_handle_map_descriptor(NetState *ns, Game *g,
                                         const uint8_t *body, int blen)
{
    if (blen < NET_MAP_DESCRIPTOR_BYTES) return;
    MapDescriptor desc;
    decode_map_descriptor(body, blen, &desc);
    /* No-op if the descriptor matches what we already have. */
    if (desc.crc32 == g->pending_map.crc32 &&
        desc.size_bytes == g->pending_map.size_bytes) {
        return;
    }
    LOG_I("client: MAP_DESCRIPTOR update — crc=%08x size=%u short=%s",
          (unsigned)desc.crc32, (unsigned)desc.size_bytes, desc.short_name);
    g->pending_map = desc;
    /* Cancel any in-flight download so we restart cleanly. */
    if (g->map_download.active) map_download_cancel(&g->map_download);
    client_resolve_or_download(ns, g, &desc);
}

/* Route one event we just got from enet_host_service. Used both by
 * the per-frame pump and by the blocking handshake loop in
 * net_client_connect — keeps the dispatch logic in one place. */
static void client_dispatch_event(NetState *ns, Game *g, ENetEvent *ev) {
    switch (ev->type) {
        case ENET_EVENT_TYPE_RECEIVE: {
            if (ev->packet->dataLength == 0) {
                enet_packet_destroy(ev->packet);
                break;
            }
            uint8_t tag = ev->packet->data[0];
            const uint8_t *body = ev->packet->data + 1;
            int blen = (int)ev->packet->dataLength - 1;

            ns->bytes_recv  += (uint32_t)ev->packet->dataLength;
            ns->packets_recv++;

            switch (tag) {
                case NET_MSG_CHALLENGE:
                    client_handle_challenge(ns, body, blen); break;
                case NET_MSG_ACCEPT:
                    client_handle_accept(ns, body, blen); break;
                case NET_MSG_REJECT:
                    client_handle_reject(ns, body, blen); break;
                case NET_MSG_INITIAL_STATE:
                    client_handle_initial_state(ns, body, blen, g); break;
                case NET_MSG_SNAPSHOT:
                    client_handle_snapshot(ns, body, blen, g); break;
                case NET_MSG_KILL_EVENT:
                    client_handle_kill_event(ns, body, blen, g); break;
                case NET_MSG_HIT_EVENT:
                    client_handle_hit_event(ns, body, blen, g); break;
                case NET_MSG_FIRE_EVENT:
                    client_handle_fire_event(ns, body, blen, g); break;
                case NET_MSG_PICKUP_STATE:
                    client_handle_pickup_state(ns, body, blen, g); break;
                case NET_MSG_FLAG_STATE:
                    client_handle_flag_state(ns, body, blen, g); break;
                case NET_MSG_EXPLOSION:
                    client_handle_explosion(ns, body, blen, g); break;
                case NET_MSG_LOBBY_LIST:
                    client_handle_lobby_list(body, blen, g); break;
                case NET_MSG_LOBBY_SLOT_UPDATE:
                    client_handle_lobby_slot_update(body, blen, g); break;
                case NET_MSG_LOBBY_CHAT:
                    client_handle_lobby_chat(body, blen, g); break;
                case NET_MSG_LOBBY_MATCH_STATE:
                    client_handle_match_state(body, blen, g); break;
                case NET_MSG_LOBBY_ROUND_START:
                    client_handle_round_start(body, blen, g); break;
                case NET_MSG_LOBBY_ROUND_END:
                    client_handle_round_end(body, blen, g); break;
                case NET_MSG_LOBBY_COUNTDOWN:
                    client_handle_countdown(body, blen, g); break;
                case NET_MSG_LOBBY_VOTE_STATE:
                    client_handle_vote_state(body, blen, g); break;
                case NET_MSG_MAP_CHUNK:
                    client_handle_map_chunk(ns, g, body, blen); break;
                case NET_MSG_MAP_DESCRIPTOR:
                    client_handle_map_descriptor(ns, g, body, blen); break;
                default:
                    LOG_D("client: unhandled tag %u", tag);
                    break;
            }
            enet_packet_destroy(ev->packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            LOG_I("client: server disconnected");
            ns->connected = false;
            ns->server.state = NET_PEER_FREE;
            break;
        }
        default: break;
    }
}

static void client_pump_events(NetState *ns, Game *g) {
    ENetEvent ev;
    while (enet_host_service((ENetHost *)ns->enet_host, &ev, 0) > 0) {
        client_dispatch_event(ns, g, &ev);
    }
}

void net_poll(NetState *ns, Game *g, double dt_real) {
    if (!ns->enet_host) return;

    ns->server_time += dt_real;

    if (ns->role == NET_ROLE_SERVER) {
        server_pump_events(ns, g);

        /* If the lobby was mutated this frame, broadcast the new slot
         * table reliably. Cheap at 32 players (~1.3 KB) and rare. */
        if (g->lobby.dirty) {
            net_server_broadcast_lobby_list(ns, &g->lobby);
            g->lobby.dirty = false;
        }

        /* Snapshot broadcast — during ACTIVE, plus COUNTDOWN so the
         * client sees the spawned mechs in their pre-round positions
         * (M6 P07: world is now prepped at COUNTDOWN start, not at
         * COUNTDOWN end). Clients in the lobby don't need world
         * snapshots, and broadcasting during SUMMARY would tear down
         * the corpses we want frozen. */
        if (g->match.phase == MATCH_PHASE_ACTIVE ||
            g->match.phase == MATCH_PHASE_COUNTDOWN)
        {
            ns->snapshot_accum += dt_real;
            while (ns->snapshot_accum >= ns->snapshot_interval) {
                ns->snapshot_accum -= ns->snapshot_interval;
                net_server_broadcast_snapshot(ns, &g->world);
            }
        }
    } else if (ns->role == NET_ROLE_CLIENT) {
        client_pump_events(ns, g);
        /* P08 — stall watchdog. If the active download hasn't received
         * a fresh chunk in 30 s, give up and disconnect. ENet's
         * reliable channel should keep retrying chunks; a true 30 s
         * gap usually means the host died. */
        if (map_download_is_stalled(&g->map_download, ns->server_time, 30.0)) {
            LOG_E("client: map download stalled for >30s — disconnecting");
            net_client_send_map_ready(ns, g->map_download.crc32,
                                      NET_MAP_READY_PARSE_FAILURE);
            map_download_cancel(&g->map_download);
            if (ns->server.enet_peer) {
                enet_peer_disconnect((ENetPeer *)ns->server.enet_peer, 0);
            }
        }
    }

    discovery_pump(ns);

    /* Update RTT from ENet's view for stat overlay. */
    if (ns->role == NET_ROLE_SERVER) {
        for (int i = 0; i < NET_MAX_PEERS; ++i) {
            if (ns->peers[i].state == NET_PEER_FREE) continue;
            ENetPeer *ep = (ENetPeer *)ns->peers[i].enet_peer;
            if (ep) ns->peers[i].round_trip_ms = ep->roundTripTime;
        }
    } else if (ns->role == NET_ROLE_CLIENT && ns->server.enet_peer) {
        ENetPeer *ep = (ENetPeer *)ns->server.enet_peer;
        ns->server.round_trip_ms = ep->roundTripTime;
    }

    /* wan-fixes-17 — periodic NET_STATS dump for WAN debugging.
     * One LOG_I line per active peer + a global summary line each
     * `stats_log_interval_s`. ENet's packetLoss is a fixed-point
     * ratio scaled by ENET_PEER_PACKET_LOSS_SCALE (= 1<<16); divide
     * to get 0..1. KB/s rates are computed from the (current - prev)
     * delta over the actual elapsed accumulator (slightly > interval
     * if the polling frame ran long, which keeps the rate honest). */
    if (ns->stats_log_interval_s > 0.0) {
        ns->stats_log_accum_s += dt_real;
        if (ns->stats_log_accum_s >= ns->stats_log_interval_s) {
            double elapsed = ns->stats_log_accum_s;
            if (elapsed < 1e-6) elapsed = ns->stats_log_interval_s;
            ns->stats_log_accum_s = 0.0;

            uint32_t pkts_out = ns->packets_sent - ns->prev_packets_sent;
            uint32_t pkts_in  = ns->packets_recv - ns->prev_packets_recv;
            uint32_t bytes_out_g = ns->bytes_sent - ns->prev_bytes_sent_global;
            uint32_t bytes_in_g  = ns->bytes_recv - ns->prev_bytes_recv_global;
            ns->prev_packets_sent      = ns->packets_sent;
            ns->prev_packets_recv      = ns->packets_recv;
            ns->prev_bytes_sent_global = ns->bytes_sent;
            ns->prev_bytes_recv_global = ns->bytes_recv;

            uint32_t snaps_applied =
                ns->snapshots_applied - ns->prev_snapshots_applied;
            ns->prev_snapshots_applied = ns->snapshots_applied;

            if (ns->role == NET_ROLE_SERVER) {
                int active = 0;
                for (int i = 0; i < NET_MAX_PEERS; ++i) {
                    NetPeer *p = &ns->peers[i];
                    if (p->state != NET_PEER_ACTIVE) continue;
                    active++;
                    ENetPeer *ep = (ENetPeer *)p->enet_peer;
                    if (ep) {
                        p->packet_loss_ratio =
                            (float)ep->packetLoss /
                            (float)ENET_PEER_PACKET_LOSS_SCALE;
                    }
                    uint32_t db_in  = p->bytes_recv - p->prev_bytes_recv;
                    p->prev_bytes_sent = p->bytes_sent;
                    p->prev_bytes_recv = p->bytes_recv;
                    char addr_buf[32];
                    net_format_addr(p->remote_addr_host, p->remote_port,
                                    addr_buf, sizeof addr_buf);
                    LOG_I("NET_STATS server peer=%d slot=%d %s "
                          "rtt=%u ms loss=%.2f%% recv=%.2f KB/s",
                          i, p->mech_id, addr_buf,
                          (unsigned)p->round_trip_ms,
                          (double)p->packet_loss_ratio * 100.0,
                          (double)(db_in / 1024.0) / elapsed);
                }
                LOG_I("NET_STATS server total peers=%d snap_hz=%u "
                      "out=%.2f KB/s in=%.2f KB/s "
                      "pkts_out=%.1f/s pkts_in=%.1f/s",
                      active, (unsigned)ns->snapshot_hz,
                      (double)(bytes_out_g / 1024.0) / elapsed,
                      (double)(bytes_in_g  / 1024.0) / elapsed,
                      (double)pkts_out / elapsed,
                      (double)pkts_in / elapsed);
            } else if (ns->role == NET_ROLE_CLIENT && ns->connected) {
                NetPeer *p = &ns->server;
                ENetPeer *ep = (ENetPeer *)p->enet_peer;
                if (ep) {
                    p->packet_loss_ratio =
                        (float)ep->packetLoss /
                        (float)ENET_PEER_PACKET_LOSS_SCALE;
                }
                /* Client uses GLOBAL byte counters: client_pump_events
                 * increments ns->bytes_sent/recv but never the per-peer
                 * fields (only the server-pump path does that). With one
                 * peer that's equivalent. */
                char addr_buf[32];
                net_format_addr(p->remote_addr_host, p->remote_port,
                                addr_buf, sizeof addr_buf);
                LOG_I("NET_STATS client %s rtt=%u ms loss=%.2f%% "
                      "out=%.2f KB/s in=%.2f KB/s "
                      "snaps=%.1f/s (cfg=%u Hz) "
                      "pkts_out=%.1f/s pkts_in=%.1f/s",
                      addr_buf,
                      (unsigned)p->round_trip_ms,
                      (double)p->packet_loss_ratio * 100.0,
                      (double)(bytes_out_g / 1024.0) / elapsed,
                      (double)(bytes_in_g  / 1024.0) / elapsed,
                      (double)snaps_applied / elapsed,
                      (unsigned)ns->snapshot_hz,
                      (double)pkts_out / elapsed,
                      (double)pkts_in / elapsed);
            }
        }
    }
}

/* ---- Snapshot broadcast -------------------------------------------- */

void net_server_broadcast_snapshot(NetState *ns, World *w) {
    if (ns->role != NET_ROLE_SERVER || ns->peer_count == 0) return;

    enum { CAP = 4 + SNAPSHOT_HEADER_WIRE_BYTES +
                  MAX_MECHS * (ENTITY_SNAPSHOT_WIRE_BYTES + 2) };

    for (int i = 0; i < NET_MAX_PEERS; ++i) {
        NetPeer *p = &ns->peers[i];
        if (p->state != NET_PEER_ACTIVE) continue;

        SnapshotFrame snap;
        snapshot_capture(w, &snap, p->latest_input_seq);

        uint8_t buf[CAP];
        uint8_t *out = buf;
        w_u8(&out, NET_MSG_SNAPSHOT);
        int room = (int)(sizeof buf - 1);
        int n = snapshot_encode(&snap, NULL, out, room);
        if (n <= 0) continue;
        ENetPacket *pkt = enet_packet_create(buf, (size_t)(1 + n),
                                              /*flags*/0); /* unreliable */
        enet_peer_send((ENetPeer *)p->enet_peer, NET_CH_STATE, pkt);
        ns->bytes_sent += (uint32_t)(1 + n);
        ns->packets_sent++;
    }
}

void net_client_send_input(NetState *ns, ClientInput in) {
    if (ns->role != NET_ROLE_CLIENT || !ns->connected) return;

    /* Phase 3 — push to the redundancy ring, then ship oldest→newest. */
    int head = ns->recent_input_head;
    ns->recent_inputs[head] = in;
    ns->recent_input_head   = (head + 1) % NET_INPUT_REDUNDANCY;
    if (ns->recent_input_count < NET_INPUT_REDUNDANCY) ns->recent_input_count++;

    /* Wire: tag(1) + count(1) + count * [seq(2) + buttons(2) + ax(4) + ay(4)]
     * For count=4: 1 + 1 + 48 = 50 bytes. */
    enum { ONE = 12 };
    uint8_t buf[2 + NET_INPUT_REDUNDANCY * ONE];
    uint8_t *p = buf;
    w_u8(&p, NET_MSG_INPUT);
    w_u8(&p, (uint8_t)ns->recent_input_count);
    int oldest = (ns->recent_input_head - ns->recent_input_count
                  + NET_INPUT_REDUNDANCY) % NET_INPUT_REDUNDANCY;
    for (int i = 0; i < ns->recent_input_count; ++i) {
        const ClientInput *r = &ns->recent_inputs[(oldest + i) % NET_INPUT_REDUNDANCY];
        w_u16(&p, r->seq);
        w_u16(&p, r->buttons);
        w_f32(&p, r->aim_x);
        w_f32(&p, r->aim_y);
    }
    enet_send_to(ns->server.enet_peer, NET_CH_STATE, /*flags*/0,
                 buf, (int)(p - buf));
    ns->bytes_sent += (uint32_t)(p - buf);
    ns->packets_sent++;
}

/* wan-fixes-13 — KILL_EVENT wire layout (40 bytes):
 *   u8  type = NET_MSG_KILL_EVENT
 *   u16 killer_mech_id   (0xFFFF for environmental / world kill)
 *   u16 victim_mech_id
 *   u16 weapon_id
 *   u8  flags            (KILLFLAG_* bits — headshot/gib/overkill/etc.)
 *   16  killer_name      (NUL-padded; empty for environmental kill)
 *   16  victim_name      (NUL-padded)
 *
 * Pre-fix the wire only carried killer/victim/weapon (7 bytes), the
 * flags were dropped, and `client_handle_kill_event` only set
 * `world.last_event` — `world.killfeed[]` was never populated on
 * clients so the HUD's kill-feed rail stayed empty for the joiner.
 * Names are fixed-width to keep parsing trivial; KILLFEED_NAME_BYTES
 * (16) holds enough to show "ClientA" / "EthansFriend" etc. without
 * truncating realistic 24-byte player_names too aggressively. */
void net_server_broadcast_kill(NetState *ns, int killer_mech_id,
                               int victim_mech_id, int weapon_id,
                               uint32_t flags,
                               const char *killer_name,
                               const char *victim_name)
{
    if (ns->role != NET_ROLE_SERVER) return;
    uint8_t buf[64]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_KILL_EVENT);
    w_u16(&p, (uint16_t)killer_mech_id);
    w_u16(&p, (uint16_t)victim_mech_id);
    w_u16(&p, (uint16_t)weapon_id);
    w_u8 (&p, (uint8_t)(flags & 0xffu));
    char nk[KILLFEED_NAME_BYTES] = {0};
    char nv[KILLFEED_NAME_BYTES] = {0};
    if (killer_name) snprintf(nk, sizeof nk, "%s", killer_name);
    if (victim_name) snprintf(nv, sizeof nv, "%s", victim_name);
    memcpy(p, nk, KILLFEED_NAME_BYTES); p += KILLFEED_NAME_BYTES;
    memcpy(p, nv, KILLFEED_NAME_BYTES); p += KILLFEED_NAME_BYTES;
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
}

/* FIRE_EVENT wire layout (12 bytes):
 *   u8  type = NET_MSG_FIRE_EVENT
 *   u16 shooter_mech_id
 *   u8  weapon_id
 *   i16 origin_x_q  (1/4 px, same quant as snapshot pos)
 *   i16 origin_y_q
 *   i16 dir_x_q     (Q1.15 normalized direction)
 *   i16 dir_y_q
 */
void net_server_broadcast_fire(NetState *ns, int shooter_mech_id, int weapon_id,
                               float origin_x, float origin_y,
                               float dir_x, float dir_y)
{
    if (ns->role != NET_ROLE_SERVER) return;
    float qx = origin_x * 4.0f;
    float qy = origin_y * 4.0f;
    if (qx >  32760.0f) qx =  32760.0f;
    if (qx < -32760.0f) qx = -32760.0f;
    if (qy >  32760.0f) qy =  32760.0f;
    if (qy < -32760.0f) qy = -32760.0f;
    int16_t origin_x_q = (int16_t)(qx < 0 ? qx - 0.5f : qx + 0.5f);
    int16_t origin_y_q = (int16_t)(qy < 0 ? qy - 0.5f : qy + 0.5f);
    if (dir_x >  1.0f) dir_x =  1.0f;
    if (dir_x < -1.0f) dir_x = -1.0f;
    if (dir_y >  1.0f) dir_y =  1.0f;
    if (dir_y < -1.0f) dir_y = -1.0f;
    int16_t dir_x_q = (int16_t)(dir_x * 32767.0f);
    int16_t dir_y_q = (int16_t)(dir_y * 32767.0f);

    uint8_t buf[16]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_FIRE_EVENT);
    w_u16(&p, (uint16_t)shooter_mech_id);
    w_u8 (&p, (uint8_t)weapon_id);
    w_u16(&p, (uint16_t)origin_x_q);
    w_u16(&p, (uint16_t)origin_y_q);
    w_u16(&p, (uint16_t)dir_x_q);
    w_u16(&p, (uint16_t)dir_y_q);
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
}

/* HIT_EVENT wire layout (14 bytes):
 *   u8  type = NET_MSG_HIT_EVENT
 *   u16 victim_mech_id
 *   u8  hit_part (PART_*)
 *   i16 pos_x_q  (1/4 px, same quant as snapshot pos)
 *   i16 pos_y_q
 *   i16 dir_x_q  (Q1.15 — multiply by 32767, normalized direction)
 *   i16 dir_y_q
 *   u8  damage   (clamped to 255 — final dmg after armor)
 */
void net_server_broadcast_hit(NetState *ns, int victim_mech_id, int hit_part,
                              float pos_x, float pos_y, float dir_x, float dir_y,
                              int damage)
{
    if (ns->role != NET_ROLE_SERVER) return;
    /* Quantize pos at 1/4 px (matches snapshot pos_q). */
    float qx = pos_x * 4.0f;
    float qy = pos_y * 4.0f;
    if (qx >  32760.0f) qx =  32760.0f;
    if (qx < -32760.0f) qx = -32760.0f;
    if (qy >  32760.0f) qy =  32760.0f;
    if (qy < -32760.0f) qy = -32760.0f;
    int16_t pos_x_q = (int16_t)(qx < 0 ? qx - 0.5f : qx + 0.5f);
    int16_t pos_y_q = (int16_t)(qy < 0 ? qy - 0.5f : qy + 0.5f);
    /* Quantize dir as Q1.15 (signed fraction of [-1, 1]). */
    if (dir_x > 1.0f) dir_x = 1.0f;
    if (dir_x < -1.0f) dir_x = -1.0f;
    if (dir_y > 1.0f) dir_y = 1.0f;
    if (dir_y < -1.0f) dir_y = -1.0f;
    int16_t dir_x_q = (int16_t)(dir_x * 32767.0f);
    int16_t dir_y_q = (int16_t)(dir_y * 32767.0f);
    int dmg_clamped = damage < 0 ? 0 : (damage > 255 ? 255 : damage);

    uint8_t buf[16]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_HIT_EVENT);
    w_u16(&p, (uint16_t)victim_mech_id);
    w_u8 (&p, (uint8_t)hit_part);
    w_u16(&p, (uint16_t)pos_x_q);
    w_u16(&p, (uint16_t)pos_y_q);
    w_u16(&p, (uint16_t)dir_x_q);
    w_u16(&p, (uint16_t)dir_y_q);
    w_u8 (&p, (uint8_t)dmg_clamped);
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
}

/* wan-fixes-10 — EXPLOSION wire layout (7 bytes):
 *   u8  type = NET_MSG_EXPLOSION
 *   i16 pos_x_q  (1/4 px, same quant as snapshot pos)
 *   i16 pos_y_q
 *   u16 owner_mech_id  (so client can find + kill its visual-only
 *                       projectile spawned from the matching
 *                       FIRE_EVENT — passes through to
 *                       client_handle_explosion)
 *   u8  weapon_id      (lookup table on the client supplies
 *                       aoe_radius / damage / impulse; saves 6
 *                       bytes per event vs shipping them inline)
 *
 * Damage / impulse stay server-authoritative — this is purely a
 * visual sync event. The server's per-mech damage pass already ran
 * in explosion_spawn before this broadcast was queued. */
void net_server_broadcast_explosion(NetState *ns, int owner_mech_id,
                                    int weapon_id, float pos_x, float pos_y)
{
    if (ns->role != NET_ROLE_SERVER) return;
    float qx = pos_x * 4.0f;
    float qy = pos_y * 4.0f;
    if (qx >  32760.0f) qx =  32760.0f;
    if (qx < -32760.0f) qx = -32760.0f;
    if (qy >  32760.0f) qy =  32760.0f;
    if (qy < -32760.0f) qy = -32760.0f;
    int16_t pos_x_q = (int16_t)(qx < 0 ? qx - 0.5f : qx + 0.5f);
    int16_t pos_y_q = (int16_t)(qy < 0 ? qy - 0.5f : qy + 0.5f);

    uint8_t buf[8]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_EXPLOSION);
    w_u16(&p, (uint16_t)pos_x_q);
    w_u16(&p, (uint16_t)pos_y_q);
    w_u16(&p, (uint16_t)owner_mech_id);
    w_u8 (&p, (uint8_t)weapon_id);
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
}

/* PICKUP_STATE wire layout (20 bytes):
 *   u8  type = NET_MSG_PICKUP_STATE
 *   u8  spawner_id           (index into world.pickups.items)
 *   u8  state                (PickupState)
 *   u8  reserved
 *   u64 available_at_tick    (server tick when COOLDOWN expires;
 *                             UINT64_MAX = permanently consumed)
 *   i16 pos_x_q              (1/4 px — only meaningful for transients
 *                             but always shipped so the client can
 *                             populate fresh entries uniformly)
 *   i16 pos_y_q
 *   u8  kind                 (PickupKind)
 *   u8  variant              (HealthVariant / PowerupVariant / weapon_id / armor_id)
 *   u16 flags                (CONTESTED / RARE / HOST_ONLY / TRANSIENT bit 8)
 *
 * Spec doc 04-pickups.md called for 12 bytes (state-only). M5 P05
 * widened to 20 to support transient-spawner replication for the
 * engineer's deployable repair pack — clients allocating new pool
 * entries need pos/kind/variant/flags. Bandwidth still trivial:
 * 20 bytes × ~10 events/min × 16 players ≈ 53 B/s aggregate, well
 * under the 5 KB/s/client budget. */
void net_server_broadcast_pickup_state(NetState *ns, int spawner_id,
                                       const struct PickupSpawner *s)
{
    if (!ns || !s || ns->role != NET_ROLE_SERVER) return;
    if (spawner_id < 0 || spawner_id > 255) return;
    float qx = s->pos.x * 4.0f;
    float qy = s->pos.y * 4.0f;
    if (qx >  32760.0f) qx =  32760.0f;
    if (qx < -32760.0f) qx = -32760.0f;
    if (qy >  32760.0f) qy =  32760.0f;
    if (qy < -32760.0f) qy = -32760.0f;
    int16_t pos_x_q = (int16_t)(qx < 0 ? qx - 0.5f : qx + 0.5f);
    int16_t pos_y_q = (int16_t)(qy < 0 ? qy - 0.5f : qy + 0.5f);

    uint8_t buf[24]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_PICKUP_STATE);
    w_u8 (&p, (uint8_t)spawner_id);
    w_u8 (&p, s->state);
    w_u8 (&p, 0);
    w_u64(&p, s->available_at_tick);
    w_u16(&p, (uint16_t)pos_x_q);
    w_u16(&p, (uint16_t)pos_y_q);
    w_u8 (&p, s->kind);
    w_u8 (&p, s->variant);
    w_u16(&p, s->flags);
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
    ns->bytes_sent += (uint32_t)(p - buf);
    ns->packets_sent++;
}

/* ---- Stats ---------------------------------------------------------- */

void net_get_stats(const NetState *ns, NetStats *out) {
    memset(out, 0, sizeof *out);
    out->role         = ns->role;
    out->peer_count   = ns->peer_count;
    out->bytes_sent   = ns->bytes_sent;
    out->bytes_recv   = ns->bytes_recv;
    out->packets_sent = ns->packets_sent;
    out->packets_recv = ns->packets_recv;
    uint32_t worst = 0;
    if (ns->role == NET_ROLE_SERVER) {
        for (int i = 0; i < NET_MAX_PEERS; ++i) {
            if (ns->peers[i].state != NET_PEER_FREE &&
                ns->peers[i].round_trip_ms > worst) worst = ns->peers[i].round_trip_ms;
        }
    } else if (ns->role == NET_ROLE_CLIENT) {
        worst = ns->server.round_trip_ms;
    }
    out->rtt_ms_max = worst;
}

/* ---- M4 broadcast helpers ----------------------------------------- */

void net_server_broadcast_lobby_list(NetState *ns, const LobbyState *lobby) {
    if (!ns || !lobby || ns->role != NET_ROLE_SERVER) return;
    enum { CAP = 1 + LOBBY_LIST_WIRE_BYTES };
    uint8_t buf[CAP];
    buf[0] = NET_MSG_LOBBY_LIST;
    lobby_encode_list(lobby, buf + 1);
    ENetPacket *pkt = enet_packet_create(buf, CAP, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_LOBBY, pkt);
    ns->bytes_sent += CAP;
    ns->packets_sent++;
}

void net_server_broadcast_chat(NetState *ns, int sender_slot, uint8_t team,
                               const char *text)
{
    if (!ns || !text || ns->role != NET_ROLE_SERVER) return;
    LobbyChatLine line = {0};
    line.sender_slot = sender_slot;
    line.sender_team = team;
    snprintf(line.text, sizeof line.text, "%s", text);

    enum { CAP = 1 + LOBBY_CHAT_WIRE_BYTES };
    uint8_t buf[CAP];
    buf[0] = NET_MSG_LOBBY_CHAT;
    lobby_encode_chat_line(&line, buf + 1);
    ENetPacket *pkt = enet_packet_create(buf, CAP, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_LOBBY, pkt);
    ns->bytes_sent += CAP;
    ns->packets_sent++;
}

static void broadcast_match_state_tag(NetState *ns, const MatchState *m,
                                      uint8_t tag)
{
    if (!ns || !m || ns->role != NET_ROLE_SERVER) return;
    enum { CAP = 1 + MATCH_SNAPSHOT_WIRE_BYTES };
    uint8_t buf[CAP];
    buf[0] = tag;
    match_encode(m, buf + 1);
    ENetPacket *pkt = enet_packet_create(buf, CAP, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_LOBBY, pkt);
    ns->bytes_sent += CAP;
    ns->packets_sent++;
}

void net_server_broadcast_round_start(NetState *ns, const MatchState *m) {
    broadcast_match_state_tag(ns, m, NET_MSG_LOBBY_ROUND_START);
}

void net_server_broadcast_round_end(NetState *ns, const MatchState *m) {
    broadcast_match_state_tag(ns, m, NET_MSG_LOBBY_ROUND_END);
}

void net_server_broadcast_match_state(NetState *ns, const MatchState *m) {
    broadcast_match_state_tag(ns, m, NET_MSG_LOBBY_MATCH_STATE);
}

void net_server_broadcast_vote_state(NetState *ns, const LobbyState *lobby) {
    if (!ns || !lobby || ns->role != NET_ROLE_SERVER) return;
    /* 1 (tag) + 1+1+1+1 (active,a,b,c) + 4+4+4 (ma,mb,mc) + 4 (remaining). */
    enum { CAP = 1 + 1 + 1 + 1 + 1 + 4 + 4 + 4 + 4 };
    uint8_t buf[CAP];
    uint8_t *p = buf;
    w_u8(&p, NET_MSG_LOBBY_VOTE_STATE);
    w_u8(&p, lobby->vote_active ? 1u : 0u);
    w_u8(&p, lobby->vote_map_a < 0 ? 0xFFu : (uint8_t)lobby->vote_map_a);
    w_u8(&p, lobby->vote_map_b < 0 ? 0xFFu : (uint8_t)lobby->vote_map_b);
    w_u8(&p, lobby->vote_map_c < 0 ? 0xFFu : (uint8_t)lobby->vote_map_c);
    w_u32(&p, lobby->vote_mask_a);
    w_u32(&p, lobby->vote_mask_b);
    w_u32(&p, lobby->vote_mask_c);
    uint32_t u; memcpy(&u, &lobby->vote_remaining, 4);
    w_u32(&p, u);
    ENetPacket *pkt = enet_packet_create(buf, CAP, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_LOBBY, pkt);
    ns->bytes_sent += CAP;
    ns->packets_sent++;
}

void net_server_broadcast_countdown(NetState *ns, float remaining,
                                    uint8_t reason)
{
    if (!ns || ns->role != NET_ROLE_SERVER) return;
    enum { CAP = 1 + 4 + 1 };
    uint8_t buf[CAP];
    uint8_t *p = buf;
    w_u8(&p, NET_MSG_LOBBY_COUNTDOWN);
    uint32_t u; memcpy(&u, &remaining, 4); w_u32(&p, u);
    w_u8(&p, reason);
    ENetPacket *pkt = enet_packet_create(buf, CAP, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_LOBBY, pkt);
    ns->bytes_sent += CAP;
    ns->packets_sent++;
}

/* ---- Client outgoing helpers -------------------------------------- */

void net_client_send_loadout(NetState *ns, MechLoadout lo) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    uint8_t buf[8]; uint8_t *p = buf;
    w_u8(&p, NET_MSG_LOBBY_LOADOUT);
    w_u8(&p, (uint8_t)lo.chassis_id);
    w_u8(&p, (uint8_t)lo.primary_id);
    w_u8(&p, (uint8_t)lo.secondary_id);
    w_u8(&p, (uint8_t)lo.armor_id);
    w_u8(&p, (uint8_t)lo.jetpack_id);
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

void net_client_send_ready(NetState *ns, bool ready) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    uint8_t buf[2] = { NET_MSG_LOBBY_READY, ready ? 1u : 0u };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 2);
}

void net_client_send_team_change(NetState *ns, int team) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    uint8_t buf[2] = { NET_MSG_LOBBY_TEAM_CHANGE, (uint8_t)team };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 2);
}

void net_client_send_chat(NetState *ns, const char *text) {
    if (!ns || !text || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    int n = (int)strlen(text);
    if (n <= 0) return;
    if (n > LOBBY_CHAT_BYTES - 1) n = LOBBY_CHAT_BYTES - 1;
    uint8_t buf[1 + LOBBY_CHAT_BYTES];
    buf[0] = NET_MSG_LOBBY_CHAT;
    memcpy(buf + 1, text, (size_t)n);
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 1 + n);
}

void net_client_send_map_vote(NetState *ns, int choice) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    if (choice < 0 || choice > 2) return;
    uint8_t buf[2] = { NET_MSG_LOBBY_MAP_VOTE, (uint8_t)choice };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 2);
}

void net_client_send_kick(NetState *ns, int target_slot) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    uint8_t buf[2] = { NET_MSG_LOBBY_KICK, (uint8_t)target_slot };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 2);
}

void net_client_send_add_bot(NetState *ns, uint8_t tier) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    uint8_t buf[2] = { NET_MSG_LOBBY_ADD_BOT, tier };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 2);
}

void net_client_send_ban(NetState *ns, int target_slot) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    uint8_t buf[2] = { NET_MSG_LOBBY_BAN, (uint8_t)target_slot };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 2);
}

void net_client_send_bot_team(NetState *ns, int target_slot, int new_team) {
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    if (target_slot < 0 || target_slot >= 256) return;
    uint8_t buf[3] = {
        NET_MSG_LOBBY_BOT_TEAM,
        (uint8_t)target_slot,
        (uint8_t)new_team,
    };
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, 3);
}

void net_client_send_host_setup(NetState *ns,
                                int mode, int map_id,
                                int score_limit, int time_limit_s,
                                int rounds_per_match,
                                bool friendly_fire)
{
    if (!ns || ns->role != NET_ROLE_CLIENT || !ns->connected) return;
    /* 1 byte tag + 1 mode + 2 map + 2 score + 2 time + 1 rounds + 1 ff = 10. */
    uint8_t buf[10]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_LOBBY_HOST_SETUP);
    w_u8 (&p, (uint8_t)mode);
    w_u16(&p, (uint16_t)map_id);
    w_u16(&p, (uint16_t)(score_limit > 0 ? score_limit : 0));
    w_u16(&p, (uint16_t)(time_limit_s > 0 ? time_limit_s : 0));
    /* Clamp to a sensible u8 range — server clamps too, but we
     * defensively also avoid sending nonsense from a misconfigured
     * UI. 0 means "no change" on the server side. */
    int rpm = (rounds_per_match > 0 && rounds_per_match <= 32)
              ? rounds_per_match : 0;
    w_u8 (&p, (uint8_t)rpm);
    w_u8 (&p, friendly_fire ? 1u : 0u);
    enet_send_to(ns->server.enet_peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

/* ---- Server: spawn slot helper (used by handshake) ----------------- */

/* Pick a spawn position for a new peer. Stays inside the tutorial map
 * (built by main.c via level_build_tutorial). We stagger horizontally
 * so peers don't telefrag. (Legacy from M2; map_spawn_point in maps.c
 * is the M4 path.) */
Vec2 net_default_spawn_for_slot(World *w, int peer_index) {
    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;
    float floor_y = (float)(w->level.height - 4) * (float)w->level.tile_size
                  - feet_below_pelvis - foot_clearance;
    /* Spread N slots from the player spawn (~16) to the dummy area (~80). */
    float tx = 16.0f + (float)((peer_index * 8) % 60);
    return (Vec2){ tx * (float)w->level.tile_size + 8.0f, floor_y };
}
