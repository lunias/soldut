#pragma once

#include "input.h"
#include "match.h"
#include "mech.h"
#include "version.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Networking. Wraps ENet behind a small surface so the rest of the
 * codebase doesn't see ENetPeer / ENetPacket / ENetEvent.
 *
 *   - net_init / net_shutdown        — process-wide enet init.
 *   - net_server_start               — bind a UDP port, listen.
 *   - net_client_connect             — connect to host:port.
 *   - net_poll                       — pump all events; called once/frame.
 *   - net_server_broadcast_snapshot  — encode and send a world snapshot
 *                                      to every connected peer (30 Hz).
 *   - net_client_send_input          — ship one ClientInput to server (60 Hz).
 *   - net_send_chat / net_send_kill_event — small reliable broadcasts.
 *   - net_discover_lan / net_discover_drain — LAN server browser.
 *
 * The threat-model story is in [05-networking.md] §"Cheating": the
 * authoritative server is the only one that mutates health, position,
 * etc. Clients ship inputs and consume snapshots. A cooperating client
 * cannot lie about its position because we never let its packets write
 * to the server's authoritative state.
 */

struct World;
struct Game;

/* The four channels we use, matching the ENet channel ids we open. */
enum {
    NET_CH_STATE = 0,    /* unreliable, unsequenced — snapshots */
    NET_CH_EVENT = 1,    /* reliable, ordered — kills, dismembers */
    NET_CH_CHAT  = 2,    /* reliable, ordered — chat */
    NET_CH_LOBBY = 3,    /* reliable, ordered — handshake, accept, etc. */
    NET_CH_COUNT = 4,
};

/* Application-level message types carried inside an ENet packet on
 * one of the channels above. The first byte of every packet payload
 * is one of these. (We encode the rest of the payload after the
 * tag — see net.c for the wire format of each.) */
enum {
    NET_MSG_CONNECT_REQUEST    =  1,    /* client → server (LOBBY) */
    NET_MSG_CHALLENGE          =  2,    /* server → client (LOBBY) */
    NET_MSG_CHALLENGE_RESPONSE =  3,    /* client → server (LOBBY) */
    NET_MSG_ACCEPT             =  4,    /* server → client (LOBBY) */
    NET_MSG_REJECT             =  5,    /* server → client (LOBBY) */
    NET_MSG_INITIAL_STATE      =  6,    /* server → client (LOBBY) */
    NET_MSG_INPUT              =  7,    /* client → server (STATE) */
    NET_MSG_SNAPSHOT           =  8,    /* server → client (STATE) */
    NET_MSG_KILL_EVENT         =  9,    /* server → client (EVENT) */
    NET_MSG_HIT_EVENT          = 13,    /* server → client (EVENT) — hit pos+dir+part for blood/spark FX */
    NET_MSG_FIRE_EVENT         = 14,    /* server → client (EVENT) — fire origin+dir+weapon for tracer/projectile FX */
    NET_MSG_PICKUP_STATE       = 15,    /* server → client (EVENT) — pickup spawner state transition (P05) */
    NET_MSG_CHAT               = 10,    /* both directions   (CHAT) */
    NET_MSG_DISCOVERY_QUERY    = 11,    /* connectionless broadcast */
    NET_MSG_DISCOVERY_REPLY    = 12,    /* connectionless reply    */

    /* M4 lobby + match flow. All ride NET_CH_LOBBY (reliable, ordered)
     * unless noted. */
    NET_MSG_LOBBY_LIST         = 20,    /* server → client: full slot table   */
    NET_MSG_LOBBY_SLOT_UPDATE  = 21,    /* server → client: one slot delta    */
    NET_MSG_LOBBY_LOADOUT      = 22,    /* client → server: pick loadout      */
    NET_MSG_LOBBY_READY        = 23,    /* client → server: toggle ready      */
    NET_MSG_LOBBY_TEAM_CHANGE  = 24,    /* client → server                    */
    NET_MSG_LOBBY_CHAT         = 25,    /* both directions; server fan-outs   */
    NET_MSG_LOBBY_MAP_VOTE     = 26,    /* client → server: vote for map a/b/c*/
    NET_MSG_LOBBY_VOTE_STATE   = 27,    /* server → client: vote candidates + tallies */
    NET_MSG_LOBBY_KICK         = 28,    /* host → server (host only)          */
    NET_MSG_LOBBY_BAN          = 29,    /* host → server (host only)          */
    NET_MSG_LOBBY_COUNTDOWN    = 30,    /* server → client: auto-start tick   */
    NET_MSG_LOBBY_ROUND_START  = 31,    /* server → client: enter MATCH       */
    NET_MSG_LOBBY_ROUND_END    = 32,    /* server → client: enter SUMMARY     */
    NET_MSG_LOBBY_MATCH_STATE  = 33,    /* server → client: MatchState delta  */
};

/* Reject reasons sent in NET_MSG_REJECT body. */
enum {
    NET_REJECT_VERSION_MISMATCH = 1,
    NET_REJECT_SERVER_FULL      = 2,
    NET_REJECT_BAD_NONCE        = 3,
    NET_REJECT_TIMEOUT          = 4,
};

typedef enum {
    NET_ROLE_OFFLINE = 0,    /* single-player M1 path (default) */
    NET_ROLE_SERVER,         /* host: runs authoritative sim + serves clients */
    NET_ROLE_CLIENT,         /* pure client connected to a server */
} NetRole;

/* Per-peer state on the server (or the lone server peer on a client). */
typedef enum {
    NET_PEER_FREE = 0,       /* slot unused */
    NET_PEER_CONNECTING,     /* enet handshake in progress */
    NET_PEER_CHALLENGED,     /* sent CHALLENGE, waiting on response */
    NET_PEER_ACTIVE,         /* fully joined, in the game */
    NET_PEER_DISCONNECTING,
} NetPeerState;

#define NET_MAX_PEERS  32

typedef struct NetPeer {
    void        *enet_peer;       /* ENetPeer*; opaque to keep enet out of public API */
    NetPeerState state;
    uint32_t     client_id;       /* assigned by server; matches mech slot */
    int          mech_id;         /* mech this peer drives; -1 until accepted */
    char         name[24];
    uint32_t     nonce;           /* server-issued challenge nonce */
    uint32_t     token;           /* HMAC over (nonce + addr + secret) */
    uint32_t     remote_addr_host;
    uint16_t     remote_port;

    /* Latest input we've received from this peer. Server copies this
     * to the corresponding mech's latched_input each tick. */
    ClientInput  latest_input;
    uint16_t     latest_input_seq;

    /* Stats. */
    uint32_t     round_trip_ms;   /* enet's view of RTT */
    uint32_t     bytes_sent;
    uint32_t     bytes_recv;
} NetPeer;

/* A discovery result, populated by net_poll when DISCOVERY_REPLY
 * datagrams arrive. */
typedef struct NetDiscoveryEntry {
    uint32_t addr_host;           /* IPv4 in network byte order */
    uint16_t port;
    uint16_t players;
    uint16_t max_players;
    uint16_t map_id;              /* small int we ship now; future maps live in this slot */
    char     name[24];
} NetDiscoveryEntry;

#define NET_MAX_DISCOVERIES 16

typedef struct NetState {
    NetRole  role;
    uint16_t bind_port;           /* server: listen port; client: ephemeral */
    void    *enet_host;           /* ENetHost*; opaque */

    /* Server: per-peer table, indexed [0..NET_MAX_PEERS).
     * Client: only `server` is used. */
    NetPeer  peers[NET_MAX_PEERS];
    int      peer_count;          /* # active peers (server) */

    /* Client-only handles. */
    NetPeer  server;              /* the one server we're connected to */
    bool     connected;
    uint32_t local_client_id;     /* server's assigned id */
    int      local_mech_id_assigned;  /* server's assigned mech slot */

    /* Per-server HMAC secret, randomized at server start. Used to
     * sign challenge nonces so a stale CHALLENGE_RESPONSE from a
     * different server / different process isn't accepted. */
    uint32_t secret[4];

    /* Optional separate UDP socket for connectionless LAN discovery.
     * Both client and server open this; the server replies to broadcast
     * queries. (ENet doesn't speak connectionless — its Host requires a
     * peer for every send/receive.) */
    int      discovery_socket;    /* -1 if not opened */
    NetDiscoveryEntry discoveries[NET_MAX_DISCOVERIES];
    int      discovery_count;

    /* Counters useful in the F4 net overlay. */
    uint32_t bytes_sent;
    uint32_t bytes_recv;
    uint32_t packets_sent;
    uint32_t packets_recv;
    double   server_time;         /* seconds since net_server_start */

    /* Snapshot rate gate (server-only). */
    double   snapshot_accum;
    double   snapshot_interval;   /* 1.0 / snapshot_hz */

    /* Cached server-side snapshots indexed [0..NET_MAX_PEERS) for
     * delta encoding: for each peer, the most recent snapshot the
     * client has acked. */
    void    *baseline_snapshots;  /* pointer into permanent arena, sized at start */

    /* P03 — client-side render clock for snapshot interpolation. Tracks
     * the server's `server_time_ms` axis. We render remote mechs at
     * `client_render_time_ms`, advanced each sim tick by exactly
     * TICK_DT*1000 (in double precision — uint32 truncation drifts
     * 0.67 ms/tick = 40 ms/sec, which silently put us hundreds of ms
     * behind the server in P03's first ship). Initialized on the
     * first received snapshot to `server_time_ms - INTERP_DELAY_MS`. */
    double   client_render_time_ms;
    uint32_t client_latest_server_time_ms;
    bool     client_render_clock_armed;
} NetState;

/* Render-time delay for remote-mech interpolation. 100 ms covers a
 * 30 Hz snapshot stream comfortably (3 snapshot intervals of buffer)
 * and matches the design canon ([documents/05-networking.md] §4). */
#define NET_INTERP_DELAY_MS 100u

/* ---- Lifecycle (process global) ----------------------------------- */

bool net_init(void);
void net_shutdown(void);

/* ---- Per-NetState lifecycle --------------------------------------- */

bool net_server_start(NetState *ns, uint16_t port, struct Game *g);

/* Connect to host:port. Returns true on connect ACK; populates
 * ns->local_mech_id_assigned with the server's choice (>=0). On
 * failure (bad host, version mismatch, server full, timeout) returns
 * false and ns->role is left at NET_ROLE_OFFLINE. */
bool net_client_connect(NetState *ns, const char *host, uint16_t port,
                        const char *display_name, struct Game *g);

void net_close(NetState *ns);

/* ---- Per-frame pump ----------------------------------------------- */

/* Drain ENet events. On the server: receives inputs, completes
 * handshakes, removes timed-out peers. On the client: receives
 * snapshots, accepts/rejects, kill events. Handles delivered to game
 * state via `g` (snapshot apply, mech spawn/despawn).
 *
 * `dt_real` is wall-clock seconds since the last call — used to gate
 * the server's 30 Hz snapshot broadcast. */
void net_poll(NetState *ns, struct Game *g, double dt_real);

/* ---- Outgoing messages -------------------------------------------- */

/* Server-only. Encodes the world's state (delta-encoded per peer) and
 * broadcasts on STATE channel. Caller manages the rate; net_poll
 * already calls this on its own at the right cadence, so most callers
 * don't need to invoke this directly. */
void net_server_broadcast_snapshot(NetState *ns, struct World *w);

/* Client-only. Send one input frame. We call this every tick of the
 * client's local sim loop, after input has been sampled and aim
 * converted to world space. */
void net_client_send_input(NetState *ns, ClientInput in);

/* A kill event — fires reliably so missed packets don't desync the
 * kill feed. Server-only. */
void net_server_broadcast_kill(NetState *ns, int killer_mech_id,
                               int victim_mech_id, int weapon_id);

/* A hit event — broadcast on every damage application so clients can
 * spawn blood / sparks at the actual hit position with the actual
 * shot direction. Without this the client falls back to spawning
 * blood at PART_CHEST with a facing-derived direction (snapshot.c),
 * which renders visibly differently from the server view. */
struct Vec2;
void net_server_broadcast_hit(NetState *ns, int victim_mech_id, int hit_part,
                              float pos_x, float pos_y, float dir_x, float dir_y,
                              int damage);

/* A fire event — broadcast on every weapon discharge so clients can
 * spawn matching tracers (hitscan) and visual-only projectiles
 * (everything else). Without this, clients see NOTHING when remote
 * players fire — only the local shooter's predict path puts a
 * tracer / projectile in front of them, leaving an asymmetric
 * "host fires but client sees no muzzle FX or projectile" feel. */
void net_server_broadcast_fire(NetState *ns, int shooter_mech_id, int weapon_id,
                               float origin_x, float origin_y,
                               float dir_x, float dir_y);

/* Pickup state event (P05) — full spawner data so clients can both
 * mirror state transitions on level-defined pickups AND learn about
 * transient spawners (engineer-deployed repair packs). 20 bytes.
 * Reliable on NET_CH_EVENT. */
struct PickupSpawner;
void net_server_broadcast_pickup_state(NetState *ns, int spawner_id,
                                       const struct PickupSpawner *s);

/* ---- M4 lobby/match outgoing -------------------------------------- */

/* Server → all clients: ship the full slot table. Called whenever the
 * lobby state has been mutated (a join, leave, ready, loadout change,
 * team change). */
struct LobbyState;
struct MatchState;
void net_server_broadcast_lobby_list(NetState *ns,
                                     const struct LobbyState *lobby);

/* Server → all clients: fan-out one new chat line. */
void net_server_broadcast_chat(NetState *ns, int sender_slot, uint8_t team,
                               const char *text);

/* Server → all clients: announce that the round transition fires now.
 * Carries the new MatchState (mode, map, limits) so clients can update
 * their HUDs immediately. */
void net_server_broadcast_round_start(NetState *ns,
                                      const struct MatchState *match);
void net_server_broadcast_round_end  (NetState *ns,
                                      const struct MatchState *match);
void net_server_broadcast_match_state(NetState *ns,
                                      const struct MatchState *match);
void net_server_broadcast_vote_state (NetState *ns,
                                      const struct LobbyState *lobby);
void net_server_broadcast_countdown  (NetState *ns, float remaining,
                                      uint8_t reason);

/* Client → server: notify of loadout / ready / team / chat / vote /
 * kick / ban changes. The server validates and (where applicable) fans
 * out the result. */
void net_client_send_loadout    (NetState *ns, MechLoadout lo);
void net_client_send_ready      (NetState *ns, bool ready);
void net_client_send_team_change(NetState *ns, int team);
void net_client_send_chat       (NetState *ns, const char *text);
void net_client_send_map_vote   (NetState *ns, int choice /*0/1/2*/);
void net_client_send_kick       (NetState *ns, int target_slot);
void net_client_send_ban        (NetState *ns, int target_slot);

/* ---- LAN discovery ----------------------------------------------- */

/* Open the connectionless discovery socket. Servers should call this
 * after net_server_start so they can answer queries. Clients call it
 * before net_discover_lan. Idempotent. */
bool net_discovery_open(NetState *ns);

/* Send a DISCOVERY_QUERY broadcast to 255.255.255.255:DEFAULT_PORT.
 * Replies arrive asynchronously and are buffered into ns->discoveries.
 * Caller flushes by reading ns->discovery_count / ns->discoveries
 * directly (or by calling net_discover_drain). */
void net_discover_lan(NetState *ns);

/* Read up to `max` discovered servers since the last drain. Resets
 * ns->discovery_count to 0. */
int  net_discover_drain(NetState *ns, NetDiscoveryEntry *out, int max);

/* ---- Address helpers ---------------------------------------------- */

/* "1.2.3.4:5000" or "1.2.3.4" → host (network byte order) + port.
 * Returns false on parse failure. */
bool net_parse_addr(const char *s, uint32_t *out_host, uint16_t *out_port,
                    uint16_t default_port);

/* Format `host` (network byte order) + `port` into `buf`. */
void net_format_addr(uint32_t host, uint16_t port, char *buf, size_t buf_len);

/* ---- Stats -------------------------------------------------------- */

/* For F4 overlay: snapshot of our network counters. */
typedef struct {
    NetRole  role;
    int      peer_count;
    uint32_t bytes_sent;
    uint32_t bytes_recv;
    uint32_t packets_sent;
    uint32_t packets_recv;
    uint32_t rtt_ms_max;
} NetStats;
void net_get_stats(const NetState *ns, NetStats *out);
