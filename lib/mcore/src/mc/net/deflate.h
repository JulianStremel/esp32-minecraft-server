// Small zlib (RFC 1950/1951) implementation for packet compression.
//  * DeflateSink: streaming LZ77 compressor with fixed Huffman codes and a 4 KiB
//    window. Deterministic, so a packet can be compressed once to measure its size
//    and again to send it. Needs a 16 KiB workspace: the game loop thread uses a
//    shared one, worker threads pass their own.
//  * inflateZlib: complete decompressor (stored, fixed and dynamic blocks).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/io.h"

namespace mc {

class DeflateSink : public Sink {
public:
    static const size_t WORKSPACE = 16384;
    // workspace: WORKSPACE bytes owned by the calling thread. nullptr uses the shared
    // workspace, which only the game loop thread may use.
    explicit DeflateSink(Sink& out, uint8_t* workspace = nullptr);
    ~DeflateSink() override;
    void put(const uint8_t* d, size_t n) override;
    void finish();

private:
    void compressBlock(bool final);
    void bits(uint32_t value, int count);
    void huff(uint32_t code, int len);     // writes a Huffman code MSB-first
    void literal(uint8_t b);
    void match(int len, int dist);
    void flushOut();

    Sink& out_;
    uint8_t* buf_;            // [history (<=WINDOW)][pending input (<=BLOCK)]
    uint16_t* head_;
    size_t hist_ = 0;         // bytes of history at the front of buf_
    size_t pend_ = 0;         // pending input bytes after the history
    uint32_t absBase_ = 0;    // absolute stream offset of buf_[0]
    uint32_t adlerA_ = 1, adlerB_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
    uint8_t obuf_[64];
    int olen_ = 0;
    bool finished_ = false;
    bool stored_ = false;     // workspace unavailable: emit stored blocks
    bool sharedWs_ = false;   // holds the shared workspace
};

// Decompresses a complete zlib stream. Returns false on malformed input or overflow.
bool inflateZlib(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap, size_t& outLen);

}  // namespace mc
