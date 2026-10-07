// Compile-time capacity limits. Override with -D flags (platformio.ini build_flags).
#pragma once

#ifndef MC_MAX_PLAYERS
#define MC_MAX_PLAYERS 8
#endif
#ifndef MC_MAX_VIEW_DISTANCE
#define MC_MAX_VIEW_DISTANCE 10
#endif
#ifndef MC_MAX_ENTITIES          // non-player entities (items, mobs, falling blocks, ...)
#define MC_MAX_ENTITIES 128
#endif
#ifndef MC_IN_BUF                // per-connection receive buffer (largest accepted packet)
#define MC_IN_BUF 4096
#endif
#ifndef MC_OUT_BUF               // per-connection send buffer
#define MC_OUT_BUF 8192
#endif
#ifndef MC_PACKET_SCRATCH        // scratch buffer for building one packet
#define MC_PACKET_SCRATCH 8192
#endif
#ifndef MC_INFLATE_MAX           // largest decompressed client packet
#define MC_INFLATE_MAX 8192
#endif
#ifndef MC_LIGHT_QUEUE           // light flood-fill queue entries (overflow falls back to sweeps)
#define MC_LIGHT_QUEUE 4096
#endif
#ifndef MC_SCHED_TICKS           // pending timer-wheel events: block ticks, furnaces, mob timers (PSRAM)
#define MC_SCHED_TICKS 4096
#endif
#ifndef MC_COMPRESS_BUF          // compressed packets up to this size are deflated only once
#define MC_COMPRESS_BUF 16384
#endif
