#pragma once
#include <cstddef>
#include <cstdint>
// Initialize the chip's network interface and wait for a DHCP lease. Without WiFi
// credentials (none built in or provisioned) it waits for an Improv client.
void networkStart();
// Joins a WiFi network and, once connected, keeps the credentials (NVS) for the next
// boot. On failure the previous network is joined again.
bool networkProvision(const char* ssid, const char* password, uint32_t timeoutMs = 20000);
bool networkHasIp();
bool networkConfigured();
bool networkIp(char* buf, size_t cap);   // dotted quad, false without a lease

struct WifiNetwork {
    char ssid[33];
    int8_t rssi;
    bool secured;
};
// Nearby WiFi networks, strongest first, each name once; returns how many (0 on Ethernet).
int networkScan(WifiNetwork* out, int max);
