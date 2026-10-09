#pragma once
#ifdef MC_QEMU_CAPTURE
// QEMU's user-network gateway reaches the host's loopback NBD server.
#define NBD_HOST "10.0.2.2"
#include "config_emulator.h"
#undef MC_MOTD
#define MC_MOTD "ESP-IDF Minecraft server (QEMU GIF capture)"
#elif defined(MC_EMULATOR)
#include "config_emulator.h"
#elif defined(MC_RELEASE_CONFIG)
#include "../tools/release/config.h"   // the prebuilt firmware (no credentials)
#elif __has_include("config.h")
#include "config.h"
#else
#include "config_edit_me.h"
#endif
#ifndef MC_DASHBOARD_PORT
#define MC_DASHBOARD_PORT 80   // the status dashboard's port (builds with -D MC_DASHBOARD=ON)
#endif
#ifndef MC_DASHBOARD_TOKEN
#define MC_DASHBOARD_TOKEN ""   // "": a random one, kept in NVS
#endif
