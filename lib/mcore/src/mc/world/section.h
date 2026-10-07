// A 16x16x16 block section stored as a paletted container whose bit layout is
// identical to the 1.16 network format (entries never straddle a long), so a
// section can be sent to a client without conversion.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/platform.h"
#include "mc/io.h"

namespace mc {

constexpr int SECTION_BLOCKS = 4096;
constexpr int GLOBAL_PALETTE_BITS = 15;   // ceil(log2(17112 block states))

inline int sectionIndex(int x, int y, int z) { return ((y & 15) << 8) | ((z & 15) << 4) | (x & 15); }

class Section {
public:
    Section() {}
    ~Section();
    Section(const Section&) = delete;
    // world data lives in PSRAM on the ESP32
    static void* operator new(size_t n) noexcept { return plat::bigAlloc(n); }
    static void operator delete(void* p) { plat::bigFree(p); }
    Section& operator=(const Section&) = delete;

    uint16_t get(int idx) const;
    uint16_t get(int x, int y, int z) const { return get(sectionIndex(x, y, z)); }
    // Returns the previous state.
    uint16_t set(int idx, uint16_t state);
    uint16_t set(int x, int y, int z, uint16_t state) { return set(sectionIndex(x, y, z), state); }
    void fill(uint16_t state);
    // Makes this section an exact copy of o. false when out of memory.
    bool copyFrom(const Section& o);

    int nonAirCount() const { return nonAir_; }
    bool isUniform() const { return bits_ == 0; }
    uint16_t uniformState() const { return single_; }
    int bits() const { return bits_; }
    int paletteSize() const { return palCount_; }
    size_t memoryBytes() const;

    // Shrinks the palette / bit width after heavy editing.
    void optimize();

    // true if any block state in the section satisfies pred (uses the palette when possible)
    template <class F>
    bool anyState(F pred) const {
        if (bits_ == 0) return pred(single_);
        if (bits_ != GLOBAL_PALETTE_BITS) {
            for (int i = 0; i < palCount_; i++)
                if (pred(palette()[i])) return true;
            return false;
        }
        for (int i = 0; i < SECTION_BLOCKS; i++)
            if (pred(get(i))) return true;
        return false;
    }

    // Network encoding (1.16 chunk section: block count, bits, palette, data longs).
    size_t wireSize() const;
    void writeWire(Writer& w) const;

    // Storage encoding (compact, self-describing; see world_store).
    size_t storeSize() const;
    void writeStore(Writer& w) const;
    bool readStore(Reader& r);

    // Count of entries per 64-bit long for a given bit width (no straddling).
    static int perLong(int bits) { return 64 / bits; }
    static int dataLongs(int bits) { int per = perLong(bits); return (SECTION_BLOCKS + per - 1) / per; }

private:
    void allocate(int bits);
    void repack(int newBits);
    int paletteIndexOf(uint16_t state) const;
    uint32_t entryFor(uint16_t state);
    uint16_t* palette() const { return (uint16_t*)mem_; }
    uint64_t* data() const;
    int paletteCap() const { return bits_ >= 4 && bits_ <= 8 ? (1 << bits_) : 0; }
    void recount();

    uint8_t* mem_ = nullptr;   // [palette u16 * cap][pad][data u64 * longs]
    uint16_t single_ = 0;      // value when bits_ == 0
    uint16_t palCount_ = 0;
    uint16_t nonAir_ = 0;
    uint8_t bits_ = 0;         // 0 uniform, 4..8 indirect, 15 direct
};

}  // namespace mc
