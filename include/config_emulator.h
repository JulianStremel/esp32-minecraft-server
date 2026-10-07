// Fixed public test configuration for esp-emulator; no hardware credentials.
#pragma once
#define WIFI_SSID "myssid"
#define WIFI_PASSWORD "mypassword"
#define MC_HOSTNAME "esp32-minecraft-emulator"
#define MC_PORT 25565
#define MC_MOTD "ESP-IDF Minecraft server (esp-emulator)"
#define MC_MAX_ONLINE 8
#define MC_VIEW_DISTANCE 4
#define MC_GAMEMODE 0
#define MC_DIFFICULTY 2
#define MC_PVP true
#define MC_SPAWN_MOBS true
#define MC_OPS "Bot0,Tester"
#define MC_WHITELIST ""
#define MC_SEED 42
#define MC_WORLD_TYPE 0
#define MC_WORLD_RADIUS 64
#ifndef NBD_HOST
// esp-emulator user networking redirects its gateway to host loopback.
#define NBD_HOST "192.168.4.1"
#endif
#define NBD_PORT MC_NBD_PORT
#define NBD_EXPORT ""
