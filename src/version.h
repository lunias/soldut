#pragma once

#include <stdint.h>

#define SOLDUT_VERSION_MAJOR  0
#define SOLDUT_VERSION_MINOR  0
#define SOLDUT_VERSION_PATCH  2
#define SOLDUT_VERSION_STRING "0.0.2-m1"

/* 'S0LD' — stamped on every connection handshake. Bump on netcode changes. */
#define SOLDUT_PROTOCOL_ID    ((uint32_t)0x53304C44u)
