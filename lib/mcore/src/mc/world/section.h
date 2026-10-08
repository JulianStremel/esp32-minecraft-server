// A 16x16x16 block section stored as a paletted container whose bit layout is
// identical to the network format (entries never straddle a long), so a section can
// be sent to a client without conversion.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/platform.h"
#include "mc/io.h"

namespace mc {

constexpr int SECTION_BLOCKS = 4096;
constexpr int GLOBAL_PALETTE_BITS = 15;   // ceil(log2(27946 block states))

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

    // out[i] = f(state of block i) for all 4096 blocks, calling f once per palette entry
    // (once per block only for the 15-bit direct palette). For bulk passes like lighting.
    template <class F>
    void mapStates(uint8_t* out, F f) const {
        if (bits_ == 0) {
            uint8_t v = f(single_);
            for (int i = 0; i < SECTION_BLOCKS; i++) out[i] = v;
            return;
        }
        const bool direct = bits_ == GLOBAL_PALETTE_BITS;
        uint8_t lut[256] = {0};
        if (!direct)
            for (int i = 0; i < palCount_; i++) lut[i] = f(palette()[i]);
        const uint64_t* d = data();
        if (bits_ == 4) {   // the common case: 16 entries per long
            for (int l = 0; l < SECTION_BLOCKS / 16; l++) {
                uint64_t v = d[l];
                uint8_t* o = out + l * 16;
                for (int k = 0; k < 16; k++, v >>= 4) o[k] = lut[v & 15];
            }
            return;
        }
        const int per = perLong(bits_), b = bits_;
        const uint64_t mask = (1ull << b) - 1;
        int idx = 0;
        for (int l = 0; idx < SECTION_BLOCKS; l++) {
            uint64_t v = d[l];
            for (int k = 0; k < per && idx < SECTION_BLOCKS; k++, idx++, v >>= b) {
                uint32_t e = (uint32_t)(v & mask);
                out[idx] = direct ? f((uint16_t)e) : lut[e & 255];
            }
        }
    }

    // true (and v) if f gives the same value for every block of the section, judged from
    // the palette alone (false for the direct palette, or when entries differ).
    template <class F>
    bool mapsUniformly(F f, uint8_t& v) const {
        if (bits_ == 0) { v = f(single_); return true; }
        if (bits_ == GLOBAL_PALETTE_BITS || palCount_ == 0) return false;
        v = f(palette()[0]);
        for (int i = 1; i < palCount_; i++)
            if (f(palette()[i]) != v) return false;
        return true;
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
