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

// Or on a microSD card (FAT32; exFAT is not supported by ESP-IDF): set SD_CARD to 1 to
// keep the world in one file on the card instead of NBD. The file is created the first
// time (contiguous; 128 KB per saved chunk: 2048 MB hold 16 000 chunks). The pins
// below are the Waveshare ESP32-S3-Touch-AMOLED-1.8's (1-bit SDMMC); other boards: set
// SD_PIN_D1..D3 for 4-bit SDMMC, or SD_MODE_SPI (CMD = MOSI, D0 = MISO, CLK = SCK, CS).
#define SD_CARD             0
#define SD_MODE             SD_MODE_SDMMC   // SD_MODE_SDMMC or SD_MODE_SPI
#define SD_MODE_SDMMC       0
#define SD_MODE_SPI         1
#define SD_PIN_CLK          2
#define SD_PIN_CMD          1
#define SD_PIN_D0           3
#define SD_PIN_D1           -1
#define SD_PIN_D2           -1
#define SD_PIN_D3           -1
#define SD_PIN_CS           -1
#define SD_WORLD_FILE       "world.img"
#define SD_WORLD_SIZE_MB    2048   // 0: as large as fits (90% of the free space, at most 4095 MB)
#define SD_FORMAT_IF_NEEDED 0      // 1: format a card that does not mount (erases it!)

#endif
