// Tiny MD5 (RFC 1321) used for Mojang-compatible offline-mode UUIDs.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mc {
void md5(const uint8_t* data, size_t len, uint8_t out[16]);
// UUID v3 of "OfflinePlayer:<name>" exactly as the vanilla server derives it.
void offlineUuid(const char* name, uint8_t out[16]);
}
