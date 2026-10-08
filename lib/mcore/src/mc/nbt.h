// Minimal NBT support: a streaming writer and a skipping/visiting reader.
// The server's own item NBT has a (usually empty) name on the root tag, as the network
// NBT of 1.16.5 had; the network NBT of 1.20.2+ has none (nbtSkipNetwork, text.h).
#pragma once
#include "mc/io.h"

namespace mc {

enum NbtType : uint8_t {
    NBT_END = 0, NBT_BYTE = 1, NBT_SHORT = 2, NBT_INT = 3, NBT_LONG = 4, NBT_FLOAT = 5,
    NBT_DOUBLE = 6, NBT_BYTE_ARRAY = 7, NBT_STRING = 8, NBT_LIST = 9, NBT_COMPOUND = 10,
    NBT_INT_ARRAY = 11, NBT_LONG_ARRAY = 12
};

class NbtWriter {
public:
    explicit NbtWriter(Writer& w) : w_(w) {}

    void beginRoot() { w_.u8(NBT_COMPOUND); w_.u16(0); }   // named "" compound
    void beginCompound(const char* name) { header(NBT_COMPOUND, name); }
    void end() { w_.u8(NBT_END); }

    void i8(const char* n, int8_t v) { header(NBT_BYTE, n); w_.i8(v); }
    void i16(const char* n, int16_t v) { header(NBT_SHORT, n); w_.i16(v); }
    void i32(const char* n, int32_t v) { header(NBT_INT, n); w_.i32(v); }
    void i64(const char* n, int64_t v) { header(NBT_LONG, n); w_.i64(v); }
    void f32(const char* n, float v) { header(NBT_FLOAT, n); w_.f32(v); }
    void f64(const char* n, double v) { header(NBT_DOUBLE, n); w_.f64(v); }
    void str(const char* n, const char* v) {
        header(NBT_STRING, n);
        size_t l = strlen(v);
        w_.u16((uint16_t)l);
        w_.bytes((const uint8_t*)v, l);
    }
    void longArrayHeader(const char* n, int32_t count) { header(NBT_LONG_ARRAY, n); w_.i32(count); }
    void listHeader(const char* n, NbtType elem, int32_t count) {
        header(NBT_LIST, n);
        w_.u8(elem);
        w_.i32(count);
    }
    Writer& raw() { return w_; }

private:
    void header(NbtType t, const char* name) {
        w_.u8(t);
        size_t l = strlen(name);
        w_.u16((uint16_t)l);
        w_.bytes((const uint8_t*)name, l);
    }
    Writer& w_;
};

// Skips the payload of a tag of the given type. depth guards recursion.
bool nbtSkipPayload(Reader& r, uint8_t type, int depth = 0);

// Skips a complete network NBT value (type byte [+ name + payload]). TAG_End alone is valid
// (that is how an item without tags is encoded).
bool nbtSkipRoot(Reader& r);

// Skips a network NBT value of 1.20.2+ (type byte and payload: the root has no name).
// TAG_End alone is an absent value.
bool nbtSkipNetwork(Reader& r);

// Visits the entries of the root compound. The visitor is handed the reader positioned at
// the entry payload and must return true if it consumed the payload; if it returns false
// the payload is skipped automatically.
typedef bool (*NbtVisitor)(void* ctx, uint8_t type, const char* name, Reader& r);
bool nbtVisitRoot(Reader& r, NbtVisitor v, void* ctx);

}  // namespace mc
