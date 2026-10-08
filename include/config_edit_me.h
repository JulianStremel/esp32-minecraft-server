// Copy this file to include/config.h and edit it (config.h is git-ignored).
#ifndef CONFIG_H
#define CONFIG_H

// ------------------------------------------------------------------ WiFi
#define WIFI_SSID      "your ssid"
#define WIFI_PASSWORD  "your password"
#define MC_HOSTNAME    "esp32-minecraft"   // also announced via mDNS (esp32-minecraft.local)

// ------------------------------------------------------------------ server
#define MC_PORT            25565
#define MC_MOTD            "A Minecraft server running on an ESP32!"
#define MC_MAX_ONLINE      5        // at most MC_MAX_PLAYERS (build limit, see lib/mcore/CMakeLists.txt)
#define MC_VIEW_DISTANCE   4        // chunks sent around each player
#define MC_GAMEMODE        0        // 0 survival, 1 creative, 2 adventure, 3 spectator
#define MC_DIFFICULTY      2        // 0 peaceful, 1 easy, 2 normal, 3 hard
#define MC_PVP             true
#define MC_SPAWN_MOBS      true
#define MC_OPS             "YourName"   // comma separated operator names
#define MC_WHITELIST       ""           // comma separated; empty = everybody may join

// The following only apply when a new world is created
#define MC_SEED            0        // 0 = random
#define MC_WORLD_TYPE      0        // 0 normal terrain, 1 superflat, 2 void
#define MC_WORLD_RADIUS    64       // world border radius in chunks, up to 1874999 (vanilla: 29 999 984 blocks)

// ------------------------------------------------------------------ storage
// The world is stored on a Network Block Device export (any NBD server:
// tools/nbd_server.py, nbdkit, qemu-nbd, nbd-server). See README.md.
// Leave NBD_HOST empty to run without persistence (world resets on reboot).
#define NBD_HOST    "192.168.1.10"
#define NBD_PORT    10809
#define NBD_EXPORT  ""             // export name ("" = the server's default export)

#endif
