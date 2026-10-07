// Configuration of the esp32s3-qemu build (tools/qemu/run.sh). Unlike config.h this
// file is committed: inside QEMU the network is QEMU's user-mode network. The server
// port is forwarded to the host with hostfwd, and 10.0.2.100:10809 is forwarded with
// guestfwd to the NBD server that run.sh starts on the host (any port, see --nbd-port).
#ifndef CONFIG_QEMU_H
#define CONFIG_QEMU_H

#define MC_HOSTNAME        "esp32-minecraft-qemu"
#define MC_PORT            25565
#define MC_MOTD            "ESP32-S3 Minecraft server (QEMU)"
#define MC_MAX_ONLINE      8
#define MC_VIEW_DISTANCE   4
#define MC_GAMEMODE        0
#define MC_DIFFICULTY      2
#define MC_PVP             true
#define MC_SPAWN_MOBS      true
#define MC_OPS             "Bot0,Tester"
#define MC_WHITELIST       ""
#define MC_SEED            42
#define MC_WORLD_TYPE      0
#define MC_WORLD_RADIUS    64

// tools/qemu/run.sh starts tools/nbd_server.py on the host; empty = no persistence
#ifndef NBD_HOST
#define NBD_HOST    "10.0.2.100"
#endif
#define NBD_PORT    10809
#define NBD_EXPORT  ""

#endif
