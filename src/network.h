#pragma once
#include <cstdint>
// Initialize the chip's network interface and wait for a DHCP lease.
void networkStart();
bool networkProvision(const char* ssid, const char* password, uint32_t timeoutMs = 20000);
bool networkHasIp();
bool networkConfigured();
