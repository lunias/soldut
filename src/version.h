#pragma once

#include <stdint.h>

#define SOLDUT_VERSION_MAJOR  0
#define SOLDUT_VERSION_MINOR  0
#define SOLDUT_VERSION_PATCH  7
#define SOLDUT_VERSION_STRING "0.0.7-m5p11"

/* 'S0LD' — stamped on every connection handshake. Bump on netcode
 * changes. Lineage:
 *   M2 = 0x53304C44 ('S0LD')
 *   M3 = 0x53304C45 ('S0LE')  (widened EntitySnapshot)
 *   M4 = 0x53304C46 ('S0LF')  (lobby flow + LOBBY_* messages, no longer
 *                              spawns the joining client straight into
 *                              the world).
 *   M5/P03 = 0x53304C47 ('S0LG')  (state_bits widened u8 → u16 for
 *                                  SNAP_STATE_IS_DUMMY).
 *   M5/P10-followup = 0x53304C48 ('S0LH')  (EntitySnapshot widened
 *                                  by 1 byte — primary_id now on the
 *                                  wire so a mid-round-join client
 *                                  reads the right active_slot when
 *                                  the host's mech is on its
 *                                  secondary at first snapshot time).
 *   net-phase-3 = 0x53304C49 ('S0LI') (NET_MSG_INPUT is now a length-
 *                                  prefixed batch — each datagram
 *                                  carries the last N=4 client inputs
 *                                  so a single dropped UDP packet
 *                                  doesn't desync prediction;
 *                                  server dedupes by seq). */
#define SOLDUT_PROTOCOL_ID    ((uint32_t)0x53304C49u)   /* 'S0LI' */

/* Default UDP port for the listen socket. 23073 is a Soldat homage
 * (the original used the same port). */
#define SOLDUT_DEFAULT_PORT   ((uint16_t)23073u)
