#include "mc/world/section.h"
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"

namespace mc {

static inline bool airState(uint16_t s) { return s == 0 || s == bs::CaveAir || s == bs::VoidAir; }

Section::~Section() { plat::bigFree(mem_); }

uint64_t* Section::data() const {
    size_t palBytes = (size_t)paletteCap() * 2;
    return (uint64_t*)(mem_ + ((palBytes + 7) & ~(size_t)7));
}

size_t Section::memoryBytes() const {
    if (!mem_) return sizeof(Section);
    size_t palBytes = (size_t)paletteCap() * 2;
    return sizeof(Section) + ((palBytes + 7) & ~(size_t)7) + (size_t)dataLongs(bits_) * 8;
}

void Section::allocate(int bits) {
    plat::bigFree(mem_);
    mem_ = nullptr;
    bits_ = (uint8_t)bits;
    if (bits == 0) return;
    size_t palBytes = (bits >= 4 && bits <= 8) ? (size_t)(1 << bits) * 2 : 0;
    size_t total = ((palBytes + 7) & ~(size_t)7) + (size_t)dataLongs(bits) * 8;
    // block data goes to PSRAM on the ESP32 (sections are ~2-8 KB each)
    mem_ = (uint8_t*)plat::bigAlloc(total);
    if (mem_) memset(mem_, 0, total);
}

bool Section::copyFrom(const Section& o) {
    allocate(o.bits_);
    if (o.bits_ && !mem_) return false;
    if (o.bits_) {
        size_t palBytes = (o.bits_ >= 4 && o.bits_ <= 8) ? (size_t)(1 << o.bits_) * 2 : 0;
        size_t total = ((palBytes + 7) & ~(size_t)7) + (size_t)dataLongs(o.bits_) * 8;
        memcpy(mem_, o.mem_, total);
    }
    single_ = o.single_;
    palCount_ = o.palCount_;
    nonAir_ = o.nonAir_;
    return true;
}

uint16_t Section::get(int idx) const {
    switch (bits_) {
        case 0: return single_;
        case 4: return palette()[(data()[idx >> 4] >> ((idx & 15) << 2)) & 15];   // the common case
        case 8: return palette()[(data()[idx >> 3] >> ((idx & 7) << 3)) & 255];
        default: break;
    }
    int per = perLong(bits_);
    uint64_t v = data()[idx / per] >> ((idx % per) * bits_);
    uint32_t mask = (1u << bits_) - 1;
    uint32_t e = (uint32_t)(v & mask);
    return bits_ == GLOBAL_PALETTE_BITS ? (uint16_t)e : palette()[e];
}

int Section::paletteIndexOf(uint16_t state) const {
    const uint16_t* p = palette();
    for (int i = 0; i < palCount_; i++)
        if (p[i] == state) return i;
    return -1;
}

void Section::repack(int newBits) {
    // decode everything with the old layout, then re-encode
    uint16_t* tmp = (uint16_t*)malloc(SECTION_BLOCKS * 2);
    for (int i = 0; i < SECTION_BLOCKS; i++) tmp[i] = get(i);
    allocate(newBits);
    palCount_ = 0;
    int per = perLong(newBits);
    uint64_t* d = data();
    for (int i = 0; i < SECTION_BLOCKS; i++) {
        uint32_t e;
        if (newBits == GLOBAL_PALETTE_BITS) {
            e = tmp[i];
        } else {
            int pi = paletteIndexOf(tmp[i]);
            if (pi < 0) { pi = palCount_; palette()[palCount_++] = tmp[i]; }
            e = (uint32_t)pi;
        }
        d[i / per] |= (uint64_t)e << ((i % per) * newBits);
    }
    free(tmp);
}

// Returns the data-array entry for a state, growing the palette / bit width if needed.
uint32_t Section::entryFor(uint16_t state) {
    if (bits_ == GLOBAL_PALETTE_BITS) return state;
    int pi = paletteIndexOf(state);
    if (pi >= 0) return (uint32_t)pi;
    if (palCount_ < paletteCap()) {
        palette()[palCount_] = state;
        return palCount_++;
    }
    // Palette full: widen. repack() rebuilds the palette from the actual contents.
    repack(bits_ < 8 ? bits_ + 1 : GLOBAL_PALETTE_BITS);
    return entryFor(state);
}

uint16_t Section::set(int idx, uint16_t state) {
    uint16_t old;
    if (bits_ == 0) {
        old = single_;
        if (old == state) return old;
        // a uniform section becomes a 4-bit indirect one (zeroed data == palette index 0)
        allocate(4);
        palette()[0] = old;
        palCount_ = 1;
    } else {
        old = get(idx);
        if (old == state) return old;
    }
    uint32_t e = entryFor(state);
    int per = perLong(bits_);
    int shift = (idx % per) * bits_;
    uint64_t mask = ((uint64_t)1 << bits_) - 1;
    uint64_t* d = data();
    d[idx / per] = (d[idx / per] & ~(mask << shift)) | ((uint64_t)e << shift);
    if (airState(old) && !airState(state)) nonAir_++;
    else if (!airState(old) && airState(state)) nonAir_--;
    return old;
}

void Section::fill(uint16_t state) {
    plat::bigFree(mem_);
    mem_ = nullptr;
    bits_ = 0;
    palCount_ = 0;
    single_ = state;
    nonAir_ = airState(state) ? 0 : SECTION_BLOCKS;
}

void Section::recount() {
    int n = 0;
    for (int i = 0; i < SECTION_BLOCKS; i++)
        if (!airState(get(i))) n++;
    nonAir_ = (uint16_t)n;
}

void Section::optimize() {
    if (bits_ == 0) return;
    // fast path for 4-bit sections (they cannot get smaller unless uniform): uniform data
    // means every long holds the same nibble 16 times
    if (bits_ == 4) {
        const uint64_t* d = data();
        uint64_t first = d[0];
        uint64_t nib = first & 15;
        bool uniform = first == nib * 0x1111111111111111ull;
        for (int i = 1; i < 256 && uniform; i++) uniform = d[i] == first;
        if (uniform) fill(palette()[nib]);
        return;
    }
    // find distinct states actually used
    uint16_t used[256];
    int nused = 0;
    bool many = false;
    for (int i = 0; i < SECTION_BLOCKS && !many; i++) {
        uint16_t s = get(i);
        int k = 0;
        for (; k < nused; k++)
            if (used[k] == s) break;
        if (k == nused) {
            if (nused == 256) many = true;
            else used[nused++] = s;
        }
    }
    int want;
    if (many) want = GLOBAL_PALETTE_BITS;
    else if (nused == 1) want = 0;
    else {
        want = 4;
        while ((1 << want) < nused) want++;
    }
    if (want == bits_ && (many || palCount_ == nused)) return;
    if (want == 0) {
        uint16_t s = used[0];
        fill(s);
        return;
    }
    repack(want);
    if (want != GLOBAL_PALETTE_BITS && palCount_ != nused) { /* palette rebuilt in repack from contents */ }
}

// ------------------------------------------------------------- wire format
size_t Section::wireSize() const {
    size_t n = 2 + 1;  // block count + bits per block
    if (bits_ == 0) {
        n += varintSize(1) + varintSize(single_) + varintSize(256) + 2048;
    } else {
        if (bits_ != GLOBAL_PALETTE_BITS) {
            n += varintSize(palCount_);
            for (int i = 0; i < palCount_; i++) n += varintSize(palette()[i]);
        }
        int longs = dataLongs(bits_);
        n += varintSize(longs) + (size_t)longs * 8;
    }
    return n;
}

void Section::writeWire(Writer& w) const {
    w.i16((int16_t)nonAir_);
    if (bits_ == 0) {
        w.u8(4);
        w.varint(1);
        w.varint(single_);
        w.varint(256);
        w.zeros(2048);
        return;
    }
    w.u8(bits_);
    if (bits_ != GLOBAL_PALETTE_BITS) {
        w.varint(palCount_);
        for (int i = 0; i < palCount_; i++) w.varint(palette()[i]);
    }
    int longs = dataLongs(bits_);
    w.varint(longs);
    const uint64_t* d = data();
    for (int i = 0; i < longs; i++) w.u64(d[i]);
}

// ------------------------------------------------------------- storage format
// u8 bits | (bits==0) u16 state | (4..8) u16 palCount, u16[palCount] | data longs (LE)
size_t Section::storeSize() const {
    if (bits_ == 0) return 1 + 2;
    size_t n = 1;
    if (bits_ != GLOBAL_PALETTE_BITS) n += 2 + (size_t)palCount_ * 2;
    return n + (size_t)dataLongs(bits_) * 8;
}

void Section::writeStore(Writer& w) const {
    w.u8(bits_);
    if (bits_ == 0) {
        w.u16(single_);
        return;
    }
    if (bits_ != GLOBAL_PALETTE_BITS) {
        w.u16(palCount_);
        for (int i = 0; i < palCount_; i++) w.u16(palette()[i]);
    }
    int longs = dataLongs(bits_);
    const uint64_t* d = data();
    for (int i = 0; i < longs; i++) w.u64(d[i]);
}

bool Section::readStore(Reader& r) {
    int bits = r.u8();
    if (!r.ok()) return false;
    if (bits == 0) {
        uint16_t s = r.u16();
        if (!r.ok() || s >= NUM_STATES) return false;
        fill(s);
        return true;
    }
    if (!((bits >= 4 && bits <= 8) || bits == GLOBAL_PALETTE_BITS)) return false;
    uint16_t pal[256];
    int palCount = 0;
    if (bits != GLOBAL_PALETTE_BITS) {
        palCount = r.u16();
        if (palCount < 1 || palCount > (1 << bits)) return false;
        for (int i = 0; i < palCount; i++) {
            pal[i] = r.u16();
            if (pal[i] >= NUM_STATES) return false;
        }
    }
    int longs = dataLongs(bits);
    if (r.remaining() < (size_t)longs * 8) return false;
    allocate(bits);
    palCount_ = (uint16_t)palCount;
    if (bits != GLOBAL_PALETTE_BITS) memcpy(palette(), pal, (size_t)palCount * 2);
    uint64_t* d = data();
    for (int i = 0; i < longs; i++) d[i] = r.u64();
    // validate palette indices
    if (bits != GLOBAL_PALETTE_BITS) {
        for (int i = 0; i < SECTION_BLOCKS; i++) {
            int per = perLong(bits);
            uint32_t e = (uint32_t)((d[i / per] >> ((i % per) * bits)) & ((1u << bits) - 1));
            if ((int)e >= palCount) return false;
        }
    }
    recount();
    return r.ok();
}

}  // namespace mc
