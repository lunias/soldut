#pragma once

#include <stdint.h>

#define SOLDUT_VERSION_MAJOR  0
#define SOLDUT_VERSION_MINOR  0
#define SOLDUT_VERSION_PATCH  4
#define SOLDUT_VERSION_STRING "0.0.4-m3"

/* 'S0LD' — stamped on every connection handshake. Bump on netcode
 * changes. M3 widens EntitySnapshot to carry chassis + armor + jetpack
 * + secondary loadout fields, so the wire format is incompatible with
 * the M2 protocol id (0x53304C44 / 'S0LD'). */
#define SOLDUT_PROTOCOL_ID    ((uint32_t)0x53304C45u)   /* 'S0LE' */

/* Default UDP port for the listen socket. 23073 is a Soldat homage
 * (the original used the same port). */
#define SOLDUT_DEFAULT_PORT   ((uint16_t)23073u)
