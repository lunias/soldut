#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include "decal.h"
#include "game.h"
#include "hash.h"
#include "level.h"
#include "log.h"
#include "mech.h"
#include "reconcile.h"
#include "snapshot.h"
#include "weapons.h"
#include "world.h"

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
    ns->snapshot_interval = 1.0 / 30.0;     /* 30 Hz */

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

    mint_secret(ns->secret);
    g->world.authoritative = true;

    LOG_I("net_server_start: listening on port %u (max %d peers)",
          (unsigned)ns->bind_port, NET_MAX_PEERS);
    return true;
}

bool net_client_connect(NetState *ns, const char *host, uint16_t port,
                        const char *display_name, Game *g)
{
    memset(ns, 0, sizeof *ns);
    ns->role = NET_ROLE_CLIENT;
    ns->discovery_socket = -1;
    ns->snapshot_interval = 1.0 / 30.0;
    ns->local_mech_id_assigned = -1;

    /* Outgoing-only ENet host: 1 peer, NET_CH_COUNT channels. */
    ENetHost *eh = enet_host_create(NULL, 1, NET_CH_COUNT, 0, 0);
    if (!eh) {
        LOG_E("net_client_connect: enet_host_create (client) failed");
        memset(ns, 0, sizeof *ns);
        return false;
    }
    ns->enet_host = eh;

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
    while (enet_host_service(eh, &ev, deadline) > 0) {
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
        enet_peer_reset(peer);
        enet_host_destroy(eh);
        memset(ns, 0, sizeof *ns);
        return false;
    }
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
                        uint32_t server_time_ms, uint64_t server_tick)
{
    uint8_t buf[24]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_ACCEPT);
    w_u32(&p, client_id);
    w_u16(&p, (uint16_t)mech_id);
    w_u32(&p, server_time_ms);
    w_u64(&p, server_tick);
    enet_send_to(peer, NET_CH_LOBBY, ENET_PACKET_FLAG_RELIABLE,
                 buf, (int)(p - buf));
}

/* ACCEPT is followed by INITIAL_STATE which carries the full snapshot
 * + level info needed to bootstrap the client. */
static void send_initial_state(void *peer, World *w) {
    SnapshotFrame snap;
    snapshot_capture(w, &snap, /*ack_input_seq*/0);

    enum { CAP = 4 + 4 + SNAPSHOT_HEADER_WIRE_BYTES +
                  MAX_MECHS * (ENTITY_SNAPSHOT_WIRE_BYTES + 2) };
    uint8_t buf[CAP];
    uint8_t *p = buf;
    w_u8 (&p, NET_MSG_INITIAL_STATE);
    w_u32(&p, SOLDUT_PROTOCOL_ID);   /* echo so version mismatches show twice */
    w_u32(&p, (uint32_t)w->level.width);
    w_u32(&p, (uint32_t)w->level.height);
    w_u32(&p, (uint32_t)w->level.tile_size);
    int snap_room = (int)(sizeof buf - (p - buf));
    int n = snapshot_encode(&snap, NULL, p, snap_room);
    if (n <= 0) {
        LOG_E("send_initial_state: snapshot did not fit");
        return;
    }
    p += n;
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

    /* Spawn a mech for this peer on the authoritative world. */
    World *w = &g->world;
    if (w->mech_count >= MAX_MECHS) {
        send_reject(p->enet_peer, NET_REJECT_SERVER_FULL);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }
    /* Spawn position: derive from a simple offset of the existing
     * spawn pattern. We let mech_create handle pool reservation; the
     * physical spawn coords are pulled from main.c's seed_world
     * tutorial layout. (See main.c). For M2 we hand each new client
     * a slot near the player spawn with a small offset. */
    extern Vec2 net_default_spawn_for_slot(World *w, int peer_index);
    Vec2 spawn = net_default_spawn_for_slot(w, (int)p->client_id);
    int mid = mech_create(w, CHASSIS_TROOPER, spawn,
                          /*team*/2, /*is_dummy*/false);
    if (mid < 0) {
        send_reject(p->enet_peer, NET_REJECT_SERVER_FULL);
        enet_peer_disconnect_later((ENetPeer *)p->enet_peer, 0);
        return;
    }
    p->mech_id = mid;
    p->state   = NET_PEER_ACTIVE;

    LOG_I("server: ACCEPT peer %u → mech %d (spawn %.0f,%.0f)",
          (unsigned)p->client_id, mid, spawn.x, spawn.y);

    uint32_t srv_ms = (uint32_t)(ns->server_time * 1000.0);
    send_accept(p->enet_peer, p->client_id, mid, srv_ms, w->tick);
    send_initial_state(p->enet_peer, w);
}

static void server_handle_input(NetState *ns, NetPeer *p,
                                const uint8_t *body, int blen, Game *g)
{
    if (p->state != NET_PEER_ACTIVE || p->mech_id < 0) return;
    if (blen < 12) return;
    const uint8_t *r = body;
    uint16_t seq     = r_u16(&r);
    uint16_t buttons = r_u16(&r);
    float    aim_x   = r_f32(&r);
    float    aim_y   = r_f32(&r);

    /* Drop out-of-order inputs (uint16 seq with wrap). */
    int16_t delta = (int16_t)(seq - p->latest_input_seq);
    if (p->latest_input_seq != 0 && delta <= 0) return;

    p->latest_input.buttons = buttons;
    p->latest_input.seq     = seq;
    p->latest_input.aim_x   = aim_x;
    p->latest_input.aim_y   = aim_y;
    p->latest_input.dt      = 0.0f;     /* server uses its own dt */
    p->latest_input_seq     = seq;

    /* Latch onto the mech for the next sim tick. */
    Mech *m = &g->world.mechs[p->mech_id];
    m->latched_input = p->latest_input;
    m->last_processed_input_seq = seq;
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
    LOG_I("client: ACCEPT client_id=%u mech_id=%u",
          (unsigned)client_id, (unsigned)mech_id);
    /* INITIAL_STATE follows on the same channel. */
}

static void client_handle_initial_state(NetState *ns, const uint8_t *body,
                                        int blen, Game *g)
{
    if (blen < 16) return;
    const uint8_t *r = body;
    uint32_t proto = r_u32(&r);
    uint32_t lw    = r_u32(&r);
    uint32_t lh    = r_u32(&r);
    uint32_t lt    = r_u32(&r);
    if (proto != SOLDUT_PROTOCOL_ID) {
        LOG_E("client: INITIAL_STATE protocol mismatch");
        return;
    }

    /* Build the level locally (M2 uses the hard-coded tutorial map; we
     * don't transfer geometry over the wire yet. The dimensions are
     * sanity-checked.) */
    if (g->world.level.tiles == NULL) {
        level_build_tutorial(&g->world.level, &g->level_arena);
        decal_init((int)level_width_px(&g->world.level),
                   (int)level_height_px(&g->world.level));
    }
    if ((int)lw != g->world.level.width ||
        (int)lh != g->world.level.height ||
        (int)lt != g->world.level.tile_size)
    {
        LOG_W("client: INITIAL_STATE level dimensions disagree (server %ux%u, "
              "client %dx%d) — proceeding anyway",
              (unsigned)lw, (unsigned)lh,
              g->world.level.width, g->world.level.height);
    }

    /* Decode the snapshot and apply. snapshot_apply spawns missing
     * mechs and mirrors server state into our local world. */
    int snap_blen = blen - 16;
    SnapshotFrame snap;
    if (!snapshot_decode(r, snap_blen, NULL, &snap)) {
        LOG_E("client: INITIAL_STATE snapshot decode failed");
        return;
    }
    snapshot_apply(&g->world, &snap);

    g->world.local_mech_id = ns->local_mech_id_assigned;
    g->world.authoritative = false;
    ns->connected = true;
    ns->server.state = NET_PEER_ACTIVE;
    LOG_I("client: INITIAL_STATE applied — entering match (local mech=%d)",
          g->world.local_mech_id);
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
    /* The snapshot's ack_input_seq tells us how far the server has
     * processed our inputs. Hand off to reconcile, which will
     * overwrite world state and replay subsequent inputs. */
    reconcile_apply_snapshot(&g->reconcile, &g->world, &snap,
                             snap.header.ack_input_seq, 1.0f / 60.0f);
}

static void client_handle_kill_event(NetState *ns, const uint8_t *body, int blen, Game *g) {
    (void)ns;
    if (blen < 6) return;
    const uint8_t *r = body;
    uint16_t killer = r_u16(&r);
    uint16_t victim = r_u16(&r);
    uint16_t weapon = r_u16(&r);
    (void)weapon;
    /* We mirror the kill into our local world's last_event so the HUD
     * shows the kill feed. The actual mech "dies" via the snapshot
     * apply; this is just the ribbon. */
    snprintf(g->world.last_event, sizeof g->world.last_event,
             "[KILL] mech #%u → mech #%u", (unsigned)killer, (unsigned)victim);
    g->world.last_event_time = 0.0f;
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
                char abuf[48];
                net_format_addr(p->remote_addr_host, p->remote_port, abuf, sizeof abuf);
                LOG_I("server: peer connected (%s) — slot %u",
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
                    case NET_MSG_CHAT:
                        /* Server forwards to all (M4 lobby work).
                         * For M2 we accept and drop. */
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
                    LOG_I("server: peer %u (mech %d) disconnected",
                          (unsigned)p->client_id, p->mech_id);
                    /* Mark the mech dead so client snapshots reflect it. */
                    if (p->mech_id >= 0) {
                        Mech *m = &g->world.mechs[p->mech_id];
                        m->alive = false;
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

        /* Snapshot broadcast at SNAPSHOT_HZ. */
        ns->snapshot_accum += dt_real;
        while (ns->snapshot_accum >= ns->snapshot_interval) {
            ns->snapshot_accum -= ns->snapshot_interval;
            net_server_broadcast_snapshot(ns, &g->world);
        }
    } else if (ns->role == NET_ROLE_CLIENT) {
        client_pump_events(ns, g);
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
    uint8_t buf[16]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_INPUT);
    w_u16(&p, in.seq);
    w_u16(&p, in.buttons);
    w_f32(&p, in.aim_x);
    w_f32(&p, in.aim_y);
    enet_send_to(ns->server.enet_peer, NET_CH_STATE, /*flags*/0,
                 buf, (int)(p - buf));
    ns->bytes_sent += (uint32_t)(p - buf);
    ns->packets_sent++;
}

void net_server_broadcast_kill(NetState *ns, int killer_mech_id,
                               int victim_mech_id, int weapon_id)
{
    if (ns->role != NET_ROLE_SERVER) return;
    uint8_t buf[16]; uint8_t *p = buf;
    w_u8 (&p, NET_MSG_KILL_EVENT);
    w_u16(&p, (uint16_t)killer_mech_id);
    w_u16(&p, (uint16_t)victim_mech_id);
    w_u16(&p, (uint16_t)weapon_id);
    ENetPacket *pkt = enet_packet_create(buf, (size_t)(p - buf),
                                          ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast((ENetHost *)ns->enet_host, NET_CH_EVENT, pkt);
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

/* ---- Server: spawn slot helper (used by handshake) ----------------- */

/* Pick a spawn position for a new peer. Stays inside the tutorial map
 * (built by main.c via level_build_tutorial). We stagger horizontally
 * so peers don't telefrag. */
Vec2 net_default_spawn_for_slot(World *w, int peer_index) {
    const float feet_below_pelvis = 36.0f;
    const float foot_clearance    = 4.0f;
    float floor_y = (float)(w->level.height - 4) * (float)w->level.tile_size
                  - feet_below_pelvis - foot_clearance;
    /* Spread N slots from the player spawn (~16) to the dummy area (~80). */
    float tx = 16.0f + (float)((peer_index * 8) % 60);
    return (Vec2){ tx * (float)w->level.tile_size + 8.0f, floor_y };
}
