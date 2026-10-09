// The configuration of the prebuilt firmware (GitHub releases, the web flasher), built
// with tools/idf/build.sh --release in place of include/config.h. Public firmware
// carries nobody's WiFi credentials or LAN addresses: WiFi is set from the web flasher
// (Improv Serial), and the world is kept on an SD card if the board has a usable one.
#include "config_edit_me.h"
#undef WIFI_SSID
#undef WIFI_PASSWORD
#undef NBD_HOST
#undef MC_OPS
#undef SD_CARD
#undef SD_FORMAT_IF_NEEDED
#define WIFI_SSID           ""   // none: wait for an Improv client
#define WIFI_PASSWORD       ""
#define NBD_HOST            ""
#define MC_OPS              ""
#define SD_CARD             2    // the card if there is a usable one
#define SD_FORMAT_IF_NEEDED 0    // never erase a card that does not mount
