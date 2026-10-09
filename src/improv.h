#pragma once
#include <cstddef>
#include <cstdint>

// Feeds console bytes through the Improv Serial parser. Bytes that are not part of an
// Improv frame are copied to rest (room for size + 5 bytes) for the console; returns
// how many.
size_t improvHandleSerialData(const uint8_t* data, size_t size, uint8_t* rest);
