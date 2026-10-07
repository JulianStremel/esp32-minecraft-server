// Ethernet for the firmware when it runs in Espressif's QEMU (no WiFi there).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Brings up the emulated OpenCores Ethernet MAC with DHCP (QEMU user networking).
// Returns 0 once an IP address was obtained, -1 on failure.
int qemu_eth_start(char* ipOut, int ipCap);
#ifdef __cplusplus
}
#endif
