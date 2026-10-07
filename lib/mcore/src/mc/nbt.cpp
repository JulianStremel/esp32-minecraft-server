#include "mc/nbt.h"

namespace mc {

static bool skipString(Reader& r) {
    uint16_t n = r.u16();
    r.skip(n);
    return r.ok();
}

bool nbtSkipPayload(Reader& r, uint8_t type, int depth) {
    if (depth > 24) { r.fail(); return false; }
    switch (type) {
        case NBT_END: break;
        case NBT_BYTE: r.skip(1); break;
        case NBT_SHORT: r.skip(2); break;
        case NBT_INT: case NBT_FLOAT: r.skip(4); break;
        case NBT_LONG: case NBT_DOUBLE: r.skip(8); break;
        case NBT_BYTE_ARRAY: { int32_t n = r.i32(); if (n < 0) { r.fail(); break; } r.skip((size_t)n); break; }
        case NBT_INT_ARRAY: { int32_t n = r.i32(); if (n < 0) { r.fail(); break; } r.skip((size_t)n * 4); break; }
        case NBT_LONG_ARRAY: { int32_t n = r.i32(); if (n < 0) { r.fail(); break; } r.skip((size_t)n * 8); break; }
        case NBT_STRING: skipString(r); break;
        case NBT_LIST: {
            uint8_t et = r.u8();
            int32_t n = r.i32();
            if (n < 0) { r.fail(); break; }
            for (int32_t i = 0; i < n && r.ok(); i++) nbtSkipPayload(r, et, depth + 1);
            break;
        }
        case NBT_COMPOUND: {
            while (r.ok()) {
                uint8_t t = r.u8();
                if (t == NBT_END) break;
                if (!skipString(r)) break;
                nbtSkipPayload(r, t, depth + 1);
            }
            break;
        }
        default: r.fail(); break;
    }
    return r.ok();
}

bool nbtSkipRoot(Reader& r) {
    uint8_t t = r.u8();
    if (t == NBT_END) return r.ok();
    if (!skipString(r)) return false;
    return nbtSkipPayload(r, t, 0);
}

bool nbtVisitRoot(Reader& r, NbtVisitor v, void* ctx) {
    uint8_t t = r.u8();
    if (t == NBT_END) return r.ok();
    if (t != NBT_COMPOUND) { r.fail(); return false; }
    if (!skipString(r)) return false;
    while (r.ok()) {
        uint8_t et = r.u8();
        if (et == NBT_END) break;
        char name[48];
        uint16_t nl = r.u16();
        if (nl > r.remaining()) { r.fail(); return false; }
        size_t store = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
        const uint8_t* np = r.take(nl);
        if (!np) return false;
        memcpy(name, np, store);
        name[store] = 0;
        // Visitors get a copy of the reader so a mismatch cannot desynchronise us.
        Reader sub = r;
        if (v(ctx, et, name, sub)) {
            r.skip((size_t)(sub.cursor() - r.cursor()));
            if (!sub.ok()) { r.fail(); return false; }
        } else {
            nbtSkipPayload(r, et, 1);
        }
    }
    return r.ok();
}

}  // namespace mc
