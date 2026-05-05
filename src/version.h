#pragma once

#include <stdint.h>

#define SOLDUT_VERSION_MAJOR  0
#define SOLDUT_VERSION_MINOR  0
#define SOLDUT_VERSION_PATCH  7
#define SOLDUT_VERSION_STRING "0.0.7-m5p08"

/* 'S0LD' — stamped on every connection handshake. Bump on netcode
 * changes. Lineage:
 *   M2 = 0x53304C44 ('S0LD')
 *   M3 = 0x53304C45 ('S0LE')  (widened EntitySnapshot)
 *   M4 = 0x53304C46 ('S0LF')  (lobby flow + LOBBY_* messages, no longer
 *                              spawns the joining client straight into
 *                              the world).
 *   M5/P03 = 0x53304C47 ('S0LG')  (state_bits widened u8 → u16 for
 *                                  SNAP_STATE_IS_DUMMY). */
#define SOLDUT_PROTOCOL_ID    ((uint32_t)0x53304C47u)   /* 'S0LG' */

/* Default UDP port for the listen socket. 23073 is a Soldat homage
 * (the original used the same port). */
#define SOLDUT_DEFAULT_PORT   ((uint16_t)23073u)
