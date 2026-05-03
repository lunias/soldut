#pragma once

#include <stdint.h>

#define SOLDUT_VERSION_MAJOR  0
#define SOLDUT_VERSION_MINOR  0
#define SOLDUT_VERSION_PATCH  3
#define SOLDUT_VERSION_STRING "0.0.3-m2"

/* 'S0LD' — stamped on every connection handshake. Bump on netcode changes.
 * Mismatched IDs are rejected at handshake with a "version mismatch" error. */
#define SOLDUT_PROTOCOL_ID    ((uint32_t)0x53304C44u)

/* Default UDP port for the listen socket. 23073 is a Soldat homage
 * (the original used the same port). */
#define SOLDUT_DEFAULT_PORT   ((uint16_t)23073u)
