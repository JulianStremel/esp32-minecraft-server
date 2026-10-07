// Byte-level serialization helpers (big-endian, Minecraft VarInt etc).
// No exceptions: Reader sets an error flag instead, Writer sinks track overflow.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mc {

// ---------------------------------------------------------------- Reader
class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len), err_(false) {}

    bool ok() const { return !err_; }
    size_t remaining() const { return (size_t)(end_ - p_); }
    const uint8_t* cursor() const { return p_; }
    void fail() { err_ = true; p_ = end_; }

    uint8_t u8() {
        if (p_ >= end_) { fail(); return 0; }
        return *p_++;
    }
    int8_t i8() { return (int8_t)u8(); }
    bool boolean() { return u8() != 0; }
    uint16_t u16() { uint16_t a = u8(); return (uint16_t)((a << 8) | u8()); }
    int16_t i16() { return (int16_t)u16(); }
    uint32_t u32() { uint32_t a = u16(); return (a << 16) | u16(); }
    int32_t i32() { return (int32_t)u32(); }
    uint64_t u64() { uint64_t a = u32(); return (a << 32) | u32(); }
    int64_t i64() { return (int64_t)u64(); }
    float f32() { uint32_t v = u32(); float f; memcpy(&f, &v, 4); return f; }
    double f64() { uint64_t v = u64(); double d; memcpy(&d, &v, 8); return d; }

    int32_t varint() {
        uint32_t result = 0;
        for (int i = 0; i < 5; i++) {
            uint8_t b = u8();
            result |= (uint32_t)(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) return (int32_t)result;
            if (err_) return 0;
        }
        fail();
        return 0;
    }
    int64_t varlong() {
        uint64_t result = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t b = u8();
            result |= (uint64_t)(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) return (int64_t)result;
            if (err_) return 0;
        }
        fail();
        return 0;
    }

    void skip(size_t n) {
        if (n > remaining()) { fail(); return; }
        p_ += n;
    }
    // Returns pointer to n bytes inside the buffer (or nullptr on underflow).
    const uint8_t* take(size_t n) {
        if (n > remaining()) { fail(); return nullptr; }
        const uint8_t* r = p_;
        p_ += n;
        return r;
    }
    void bytes(uint8_t* out, size_t n) {
        const uint8_t* s = take(n);
        if (s) memcpy(out, s, n);
    }
    // Reads a VarInt-prefixed UTF-8 string into out (always NUL-terminated).
    // Strings longer than cap-1 are truncated (the rest is consumed). Returns length stored.
    size_t string(char* out, size_t cap) {
        int32_t n = varint();
        if (n < 0 || (size_t)n > remaining()) { fail(); out[0] = 0; return 0; }
        size_t store = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(out, p_, store);
        out[store] = 0;
        p_ += n;
        return store;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    bool err_;
};

// ---------------------------------------------------------------- Sink
class Sink {
public:
    virtual ~Sink() {}
    virtual void put(const uint8_t* data, size_t n) = 0;
};

// Counts bytes only.
class CountSink : public Sink {
public:
    size_t count = 0;
    void put(const uint8_t*, size_t n) override { count += n; }
};

// Fixed-capacity memory buffer.
class BufSink : public Sink {
public:
    BufSink(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}
    void put(const uint8_t* data, size_t n) override {
        if (len_ + n > cap_) { overflow_ = true; return; }
        memcpy(buf_ + len_, data, n);
        len_ += n;
    }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }
    uint8_t* data() { return buf_; }
    void reset() { len_ = 0; overflow_ = false; }

private:
    uint8_t* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool overflow_ = false;
};

// ---------------------------------------------------------------- Writer
class Writer {
public:
    explicit Writer(Sink& s) : s_(&s) {}
    Sink& sink() { return *s_; }

    void u8(uint8_t v) { s_->put(&v, 1); }
    void i8(int8_t v) { u8((uint8_t)v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v) { uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v}; s_->put(b, 2); }
    void i16(int16_t v) { u16((uint16_t)v); }
    void u32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
        s_->put(b, 4);
    }
    void i32(int32_t v) { u32((uint32_t)v); }
    void u64(uint64_t v) { u32((uint32_t)(v >> 32)); u32((uint32_t)v); }
    void i64(int64_t v) { u64((uint64_t)v); }
    void f32(float f) { uint32_t v; memcpy(&v, &f, 4); u32(v); }
    void f64(double d) { uint64_t v; memcpy(&v, &d, 8); u64(v); }

    void varint(int32_t value) {
        uint32_t v = (uint32_t)value;
        uint8_t b[5];
        int n = 0;
        do {
            uint8_t t = v & 0x7F;
            v >>= 7;
            if (v) t |= 0x80;
            b[n++] = t;
        } while (v);
        s_->put(b, n);
    }
    void varlong(int64_t value) {
        uint64_t v = (uint64_t)value;
        uint8_t b[10];
        int n = 0;
        do {
            uint8_t t = v & 0x7F;
            v >>= 7;
            if (v) t |= 0x80;
            b[n++] = t;
        } while (v);
        s_->put(b, n);
    }
    void bytes(const uint8_t* d, size_t n) { if (n) s_->put(d, n); }
    void zeros(size_t n) {
        static const uint8_t z[64] = {0};
        while (n) { size_t c = n < 64 ? n : 64; s_->put(z, c); n -= c; }
    }
    void string(const char* str) { string(str, strlen(str)); }
    void string(const char* str, size_t len) {
        varint((int32_t)len);
        s_->put((const uint8_t*)str, len);
    }
    void uuid(const uint8_t u[16]) { s_->put(u, 16); }

private:
    Sink* s_;
};

inline int varintSize(uint32_t v) {
    int n = 1;
    while (v >= 0x80) { v >>= 7; n++; }
    return n;
}

}  // namespace mc
