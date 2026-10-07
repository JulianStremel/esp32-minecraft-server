#include "mc/storage/world_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/io.h"
#include "mc/net/deflate.h"
#include "mc/platform.h"
#include "mc/registry.h"

namespace mc {

static const char SUPER_MAGIC[8] = {'E', 'S', 'P', 'M', 'C', 'W', '0', '1'};
static const uint32_t CHUNK_MAGIC = 0x43484B31;   // "CHK1"
static const uint32_t PLAYER_MAGIC = 0x504C5931;  // "PLY1"
// Format 2 is format 1 for worlds of generator version 2 or later: builds from before
// generator versions were stored read format 1 only, so they refuse these worlds
// instead of generating their terrain with version 1 (and storing version 0 = 1).
static const uint32_t FORMAT_VERSION = 1;
static const uint32_t FORMAT_VERSION_GEN2 = 2;
static const uint32_t PLAYER_SLOT = 512;
static const uint32_t CHUNK_HEADER = 32;
static const uint32_t FLAG_ZLIB = 1;
static const uint32_t FLAG_TICKS = 2;   // the payload ends with the chunk's scheduled ticks (v2)
static const uint16_t RECORD_VERSION = 2;
static const uint32_t SUPER_HAS_WORLD = 1;

WorldStore::WorldStore(BlockDevice* dev) : dev_(dev) {}

uint64_t WorldStore::requiredSize() const {
    uint64_t side = (uint64_t)radius_ * 2;
    return chunkOff_ + side * side * 2 * slotSize_;
}

uint64_t WorldStore::chunkBase(int cx, int cz) const {
    uint64_t side = (uint64_t)radius_ * 2;
    uint64_t idx = (uint64_t)(cz + radius_) * side + (uint64_t)(cx + radius_);
    return chunkOff_ + idx * 2 * slotSize_;
}

bool WorldStore::chunkInRange(int cx, int cz) const {
    return open_ && cx >= -radius_ && cx < radius_ && cz >= -radius_ && cz < radius_;
}

// ------------------------------------------------------------------ superblock
static void encodeSuper(uint8_t* b, uint32_t seq, int radius, uint32_t slotSize, uint32_t playerSlots, uint64_t playerOff,
                        uint64_t chunkOff, bool hasWorld, const WorldMeta& m) {
    memset(b, 0, 512);
    BufSink s(b, 512);
    Writer w(s);
    w.bytes((const uint8_t*)SUPER_MAGIC, 8);
    w.u32(m.generatorVersion >= 2 ? FORMAT_VERSION_GEN2 : FORMAT_VERSION);
    w.u32(seq);
    w.i32(radius);
    w.u32(slotSize);
    w.u32(playerSlots);
    w.u32(PLAYER_SLOT);
    w.u64(playerOff);
    w.u64(chunkOff);
    w.u64(m.seed);
    w.u8(m.worldType);
    w.u8(m.raining);
    w.u16(m.generatorVersion);   // was reserved (0): older worlds read as version 0 = 1
    w.i32(m.spawnX);
    w.i32(m.spawnY);
    w.i32(m.spawnZ);
    w.i64(m.worldAge);
    w.i64(m.timeOfDay);
    w.i32(m.weatherTimer);
    w.u32(hasWorld ? SUPER_HAS_WORLD : 0);
    uint32_t c = crc32(b, 508);
    b[508] = (uint8_t)(c >> 24);
    b[509] = (uint8_t)(c >> 16);
    b[510] = (uint8_t)(c >> 8);
    b[511] = (uint8_t)c;
}

bool WorldStore::readSuper() {
    uint8_t* buf = (uint8_t*)malloc(1024);
    if (!buf) return false;
    ReadOp ops[2] = {{0, buf, 512}, {512, buf + 512, 512}};
    if (!dev_->readMany(ops, 2)) { free(buf); return false; }
    int best = -1;
    uint32_t bestSeq = 0;
    for (int k = 0; k < 2; k++) {
        const uint8_t* b = buf + k * 512;
        if (memcmp(b, SUPER_MAGIC, 8)) continue;
        uint32_t c = crc32(b, 508);
        uint32_t stored = (uint32_t)b[508] << 24 | (uint32_t)b[509] << 16 | (uint32_t)b[510] << 8 | b[511];
        if (c != stored) continue;
        Reader r(b + 12, 4);
        uint32_t seq = r.u32();
        if (best < 0 || seq > bestSeq) { best = k; bestSeq = seq; }
    }
    if (best < 0) { free(buf); return false; }
    Reader r(buf + best * 512 + 8, 500);
    uint32_t version = r.u32();
    if (version != FORMAT_VERSION && version != FORMAT_VERSION_GEN2) {
        MC_LOGE("storage: unsupported format version %u", (unsigned)version);
        free(buf);
        return false;
    }
    superSeq_ = r.u32();
    radius_ = r.i32();
    slotSize_ = r.u32();
    playerSlots_ = r.u32();
    r.u32();
    playerOff_ = r.u64();
    chunkOff_ = r.u64();
    meta_.seed = r.u64();
    meta_.worldType = r.u8();
    meta_.raining = r.u8();
    meta_.generatorVersion = (uint8_t)r.u16();
    meta_.spawnX = r.i32();
    meta_.spawnY = r.i32();
    meta_.spawnZ = r.i32();
    meta_.worldAge = r.i64();
    meta_.timeOfDay = r.i64();
    meta_.weatherTimer = r.i32();
    uint32_t flags = r.u32();
    haveWorld_ = (flags & SUPER_HAS_WORLD) != 0;
    meta_.radius = radius_;
    free(buf);
    return r.ok();
}

bool WorldStore::writeSuper() {
    uint8_t b[512];
    superSeq_++;
    encodeSuper(b, superSeq_, radius_, slotSize_, playerSlots_, playerOff_, chunkOff_, haveWorld_, meta_);
    return dev_->write((uint64_t)(superSeq_ & 1) * 512, b, 512);
}

bool WorldStore::open(const StoreParams& params, bool allowFormat) {
    open_ = false;
    compress_ = params.compress;
    if (!dev_->available()) return false;
    if (readSuper()) {
        open_ = true;
        if (requiredSize() > dev_->size()) {
            MC_LOGE("storage: device is smaller (%llu) than the stored layout needs (%llu)",
                    (unsigned long long)dev_->size(), (unsigned long long)requiredSize());
            open_ = false;
            return false;
        }
        MC_LOGI("storage: opened %s (radius %d chunks, %s)", dev_->describe(), radius_, haveWorld_ ? "world present" : "empty");
        return true;
    }
    // refuse to format something that is not blank: it may be somebody's data
    uint8_t probe[4096];
    if (!dev_->read(0, probe, sizeof(probe))) return false;
    bool blank = true;
    for (size_t i = 0; i < sizeof(probe); i++)
        if (probe[i]) { blank = false; break; }
    if (!blank && !allowFormat) {
        MC_LOGE("storage: device holds unknown data, refusing to format it");
        return false;
    }
    if (!blank) MC_LOGW("storage: device holds unknown data; formatting anyway (allowFormat)");
    playerSlots_ = params.playerSlots;
    slotSize_ = params.chunkSlotSize;
    playerOff_ = 4096;
    chunkOff_ = (playerOff_ + (uint64_t)playerSlots_ * PLAYER_SLOT + 65535) & ~(uint64_t)65535;
    radius_ = params.radius;
    uint64_t avail = dev_->size() > chunkOff_ ? dev_->size() - chunkOff_ : 0;
    // shrink the world to what fits
    while (radius_ > 1 && (uint64_t)radius_ * 2 * radius_ * 2 * 2 * slotSize_ > avail) radius_--;
    if ((uint64_t)radius_ * 2 * radius_ * 2 * 2 * slotSize_ > avail) {
        MC_LOGE("storage: device too small (%llu bytes)", (unsigned long long)dev_->size());
        return false;
    }
    if (radius_ != params.radius)
        MC_LOGW("storage: device size limits the world to a radius of %d chunks (%d blocks)", radius_, radius_ * 16);
    haveWorld_ = false;
    meta_ = WorldMeta();
    meta_.radius = radius_;
    superSeq_ = 0;
    // wipe both superblock copies + player table header region is assumed zero (sparse)
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    if (!dev_->write(512, zero, 512)) return false;
    if (!writeSuper() || !dev_->flush()) return false;
    open_ = true;
    MC_LOGI("storage: formatted %s: radius %d chunks, %u player slots, %u KiB per chunk copy, needs %llu MiB",
            dev_->describe(), radius_, (unsigned)playerSlots_, (unsigned)(slotSize_ / 1024),
            (unsigned long long)(requiredSize() >> 20));
    return true;
}

bool WorldStore::loadMeta(WorldMeta& m) {
    m.radius = radius_;
    if (!open_ || !haveWorld_) return false;
    m = meta_;
    m.radius = radius_;
    return true;
}

bool WorldStore::saveMeta(const WorldMeta& m) {
    if (!open_) return false;
    meta_ = m;
    meta_.radius = radius_;
    haveWorld_ = true;
    return writeSuper();
}

bool WorldStore::flush() { return open_ && dev_->flush(); }

void WorldStore::statusLine(char* buf, size_t cap) {
    const DeviceStats& s = dev_->stats();
    snprintf(buf, cap, "%s | %u KB read, %u KB written, %u chunks saved, %u loaded, %u ms latency, %u errors%s",
             dev_->describe(), (unsigned)(s.bytesRead / 1024), (unsigned)(s.bytesWritten / 1024), (unsigned)chunksWritten_,
             (unsigned)chunksRead_, (unsigned)s.lastLatencyMs, (unsigned)s.errors, s.reconnects ? " (reconnected)" : "");
}

// ------------------------------------------------------------------ chunk payload
static void writeStack(Writer& w, const ItemStack& s) {
    w.u16(s.empty() ? 0 : s.id);
    w.u8(s.empty() ? 0 : s.count);
    w.u16(s.damage);
}

static bool readStack(Reader& r, ItemStack& s) {
    uint16_t id = r.u16();
    uint8_t count = r.u8();
    uint16_t dmg = r.u16();
    s.clear();
    if (id && count && id < NUM_ITEMS) {
        s.id = id;
        s.count = count;
        s.damage = dmg;
    }
    return r.ok();
}

static void writePayload(Writer& w, const Chunk& c) {
    uint16_t mask = 0;
    for (int s = 0; s < NUM_SECTIONS; s++)
        if (c.section(s) && c.section(s)->nonAirCount() > 0) mask |= (uint16_t)(1 << s);
    w.u16(mask);
    w.bytes(c.biomeCells(), 16);
    for (int s = 0; s < NUM_SECTIONS; s++)
        if (mask & (1 << s)) c.section(s)->writeStore(w);
    w.u16((uint16_t)c.tileCount());
    for (TileEntity* t = c.tiles(); t; t = t->next) {
        w.u8(t->type);
        w.u8(t->lx);
        w.u8(t->y);
        w.u8(t->lz);
        if (t->type == TILE_CHEST || t->type == TILE_BARREL) {
            for (int i = 0; i < 27; i++) writeStack(w, t->items[i]);
        } else if (t->type == TILE_FURNACE) {
            for (int i = 0; i < 3; i++) writeStack(w, t->items[i]);
            w.i16(t->burnTime);
            w.i16(t->burnTotal);
            w.i16(t->cookTime);
        } else if (t->type == TILE_SIGN) {
            for (int i = 0; i < 4; i++) {
                size_t l = strlen(t->text[i]);
                w.u8((uint8_t)l);
                w.bytes((const uint8_t*)t->text[i], l);
            }
        }
    }
    // scheduled block ticks (FLAG_TICKS), delays relative to the time of saving
    w.u16(c.tickCount);
    for (int i = 0; i < c.tickCount; i++) {
        const ChunkTick& k = c.ticks[i];
        w.u8((uint8_t)(k.lx | k.lz << 4));
        w.u8(k.y);
        w.u16(k.block);
        w.i32(k.delay);
        w.i8(k.prio);
    }
}

static bool readPayload(Reader& r, Chunk& c, uint32_t flags) {
    uint16_t mask = r.u16();
    uint8_t biomes[16];
    r.bytes(biomes, 16);
    for (int i = 0; i < 16; i++) c.setBiomeCell(i & 3, i >> 2, biomes[i]);
    for (int s = 0; s < NUM_SECTIONS; s++) {
        if (!(mask & (1 << s))) continue;
        if (!c.ensureSection(s)->readStore(r)) return false;
    }
    int tiles = r.u16();
    for (int i = 0; i < tiles && r.ok(); i++) {
        uint8_t type = r.u8(), lx = r.u8(), y = r.u8(), lz = r.u8();
        if (lx > 15 || lz > 15) return false;
        TileEntity* t = c.addTile(type, lx, y, lz);
        if (type == TILE_CHEST || type == TILE_BARREL) {
            for (int k = 0; k < 27; k++) readStack(r, t->items[k]);
        } else if (type == TILE_FURNACE) {
            for (int k = 0; k < 3; k++) readStack(r, t->items[k]);
            t->burnTime = r.i16();
            t->burnTotal = r.i16();
            t->cookTime = r.i16();
        } else if (type == TILE_SIGN) {
            for (int k = 0; k < 4; k++) {
                uint8_t l = r.u8();
                if (l >= sizeof(t->text[k])) return false;
                r.bytes((uint8_t*)t->text[k], l);
                t->text[k][l] = 0;
            }
        } else {
            return false;
        }
    }
    if ((flags & FLAG_TICKS) && r.ok()) {
        int n = r.u16();
        ChunkTick* t = n ? (ChunkTick*)plat::bigAlloc(sizeof(ChunkTick) * (size_t)n) : nullptr;
        if (n && !t) return false;
        for (int i = 0; i < n && r.ok(); i++) {
            uint8_t xz = r.u8();
            t[i].lx = xz & 15;
            t[i].lz = xz >> 4;
            t[i].y = r.u8();
            t[i].block = r.u16();
            t[i].delay = r.i32();
            t[i].prio = r.i8();
        }
        bool ok = r.ok() && c.setTicks(t, n);
        plat::bigFree(t);
        if (!ok) return false;
    }
    c.dropEmptySections();
    return r.ok();
}

// ------------------------------------------------------------------ chunk IO
namespace {

struct ChunkHeader {
    uint32_t magic, seq;
    int32_t cx, cz;
    uint32_t stored, raw, crc;
    uint16_t flags, version;
};

void encodeHeader(uint8_t* b, const ChunkHeader& h) {
    BufSink s(b, CHUNK_HEADER);
    Writer w(s);
    w.u32(h.magic);
    w.u32(h.seq);
    w.i32(h.cx);
    w.i32(h.cz);
    w.u32(h.stored);
    w.u32(h.raw);
    w.u32(h.crc);
    w.u16(h.flags);
    w.u16(h.version);
}

ChunkHeader decodeHeader(const uint8_t* b) {
    Reader r(b, CHUNK_HEADER);
    ChunkHeader h;
    h.magic = r.u32();
    h.seq = r.u32();
    h.cx = r.i32();
    h.cz = r.i32();
    h.stored = r.u32();
    h.raw = r.u32();
    h.crc = r.u32();
    h.flags = r.u16();
    h.version = r.u16();
    return h;
}

struct CrcCountSink : Sink {
    uint32_t crc = 0;
    size_t n = 0;
    void put(const uint8_t* d, size_t len) override {
        crc = crc32(d, len, crc);
        n += len;
    }
};

// buffers small writes into the device's streaming write
struct DeviceSink : Sink {
    BlockDevice* dev;
    uint8_t buf[1024];
    size_t len = 0;
    bool ok = true;
    explicit DeviceSink(BlockDevice* d) : dev(d) {}
    void put(const uint8_t* d, size_t n) override {
        while (n && ok) {
            size_t c = sizeof(buf) - len < n ? sizeof(buf) - len : n;
            memcpy(buf + len, d, c);
            len += c;
            d += c;
            n -= c;
            if (len == sizeof(buf)) drain();
        }
    }
    void drain() {
        if (len && ok) ok = dev->writeData(buf, (uint32_t)len);
        len = 0;
    }
};

}  // namespace

enum DecodeStatus { DEC_OK, DEC_BAD_CRC, DEC_BAD_ZLIB, DEC_MALFORMED, DEC_NO_MEMORY };

// Verifies and decodes one stored copy. Pure (no I/O, no shared state): runs on workers too.
static DecodeStatus decodeStored(const uint8_t* stored, uint32_t storedLen, uint32_t rawLen, uint32_t crc,
                                 uint16_t flags, Chunk& c) {
    if (crc32(stored, storedLen) != crc) return DEC_BAD_CRC;
    const uint8_t* raw = stored;
    size_t len = storedLen;
    uint8_t* buf = nullptr;
    if (flags & FLAG_ZLIB) {
        buf = (uint8_t*)plat::bigAlloc(rawLen ? rawLen : 1);
        if (!buf) return DEC_NO_MEMORY;
        size_t got = 0;
        if (!inflateZlib(stored, storedLen, buf, rawLen, got) || got != rawLen) {
            plat::bigFree(buf);
            return DEC_BAD_ZLIB;
        }
        raw = buf;
        len = got;
    }
    Reader r(raw, len);
    bool ok = readPayload(r, c, flags);
    plat::bigFree(buf);
    return ok ? DEC_OK : DEC_MALFORMED;
}

// Reads both copies' headers; order[0] is the newest valid copy. Returns the number of
// valid copies, or -1 on an I/O error.
int WorldStore::readHeaders(int cx, int cz, ChunkHeaderInfo h[2], int order[2]) {
    uint64_t base = chunkBase(cx, cz);
    uint8_t hb[2][CHUNK_HEADER];
    ReadOp ops[2] = {{base, hb[0], CHUNK_HEADER}, {base + slotSize_, hb[1], CHUNK_HEADER}};
    if (!dev_->readMany(ops, 2)) return -1;
    return parseHeaders(cx, cz, hb[0], hb[1], h, order);
}

int WorldStore::parseHeaders(int cx, int cz, const uint8_t* hb0, const uint8_t* hb1, ChunkHeaderInfo h[2],
                             int order[2]) const {
    const uint8_t* hb[2] = {hb0, hb1};
    bool valid[2];
    bool anything = false;
    for (int k = 0; k < 2; k++) {
        ChunkHeader d = decodeHeader(hb[k]);
        h[k].seq = d.seq;
        h[k].stored = d.stored;
        h[k].raw = d.raw;
        h[k].crc = d.crc;
        h[k].flags = d.flags;
        valid[k] = d.magic == CHUNK_MAGIC && d.cx == cx && d.cz == cz && d.stored + CHUNK_HEADER <= slotSize_ &&
                   d.raw <= 4u * 1024 * 1024;
        h[k].valid = valid[k];
        if (d.magic != 0) anything = true;
    }
    if (!valid[0] && !valid[1]) {
        if (anything) MC_LOGW("storage: chunk %d,%d has no valid record, treating as new", cx, cz);
        return 0;
    }
    order[0] = 0;
    order[1] = 1;
    if (!valid[0] || (valid[1] && h[1].seq > h[0].seq)) { order[0] = 1; order[1] = 0; }
    return (valid[0] ? 1 : 0) + (valid[1] ? 1 : 0);
}

LoadResult WorldStore::loadChunk(Chunk& c) {
    if (!chunkInRange(c.cx, c.cz)) return LOAD_ABSENT;
    ChunkHeaderInfo h[2];
    int order[2];
    int n = readHeaders(c.cx, c.cz, h, order);
    if (n < 0) return LOAD_ERROR;
    if (n == 0) return LOAD_ABSENT;
    uint64_t base = chunkBase(c.cx, c.cz);
    // newest valid copy first, fall back to the other one
    for (int i = 0; i < 2; i++) {
        int k = order[i];
        if (!h[k].valid) continue;
        uint8_t* stored = (uint8_t*)plat::bigAlloc(h[k].stored ? h[k].stored : 1);
        if (!stored) return LOAD_ERROR;
        if (!dev_->read(base + (uint64_t)k * slotSize_ + CHUNK_HEADER, stored, h[k].stored)) {
            plat::bigFree(stored);
            return LOAD_ERROR;
        }
        DecodeStatus st = decodeStored(stored, h[k].stored, h[k].raw, h[k].crc, h[k].flags, c);
        plat::bigFree(stored);
        switch (st) {
            case DEC_OK:
                c.storeSeq = h[k].seq;
                c.storeSlot = (int8_t)k;
                chunksRead_++;
                return LOAD_OK;
            case DEC_BAD_CRC:
                MC_LOGW("storage: chunk %d,%d copy %d fails its checksum", c.cx, c.cz, k);
                continue;
            case DEC_BAD_ZLIB:
                MC_LOGW("storage: chunk %d,%d copy %d does not decompress", c.cx, c.cz, k);
                continue;
            case DEC_MALFORMED:
                MC_LOGW("storage: chunk %d,%d copy %d is malformed", c.cx, c.cz, k);
                return LOAD_ERROR;  // chunk object partly filled: let the caller start over
            case DEC_NO_MEMORY:
                return LOAD_ERROR;
        }
    }
    return LOAD_ERROR;
}

void WorldStore::fetchChunks(int n, const int32_t* cx, const int32_t* cz, ChunkRecord* const* recs,
                             LoadResult* res) {
    // round trip 1: both header copies of every chunk; round trip 2: the newest valid
    // record of the chunks that have one (never-stored chunks cost only the first)
    const int MAX = 16;
    for (int start = 0; start < n; start += MAX) {
        int m = n - start < MAX ? n - start : MAX;
        uint8_t hb[MAX][2][CHUNK_HEADER];
        ReadOp ops[2 * MAX];
        int nops = 0;
        for (int i = 0; i < m; i++) {
            int j = start + i;
            res[j] = LOAD_ABSENT;
            if (!chunkInRange(cx[j], cz[j])) continue;
            uint64_t base = chunkBase(cx[j], cz[j]);
            ops[nops++] = {base, hb[i][0], CHUNK_HEADER};
            ops[nops++] = {base + slotSize_, hb[i][1], CHUNK_HEADER};
            res[j] = LOAD_OK;  // provisional: has headers to look at
        }
        if (nops && !dev_->readMany(ops, nops)) {
            for (int i = 0; i < m; i++)
                if (res[start + i] == LOAD_OK) res[start + i] = LOAD_ERROR;
            continue;
        }
        ReadOp rops[MAX];
        int rmap[MAX];
        int nr = 0;
        for (int i = 0; i < m; i++) {
            int j = start + i;
            if (res[j] != LOAD_OK) continue;
            ChunkHeaderInfo h[2];
            int order[2];
            if (parseHeaders(cx[j], cz[j], hb[i][0], hb[i][1], h, order) == 0) {
                res[j] = LOAD_ABSENT;
                continue;
            }
            int k = order[0];
            ChunkRecord& rec = *recs[j];
            rec.bytes.clear();
            uint8_t* p = rec.bytes.append(h[k].stored);
            if (!p && h[k].stored) {
                res[j] = LOAD_ERROR;
                continue;
            }
            rec.raw = h[k].raw;
            rec.crc = h[k].crc;
            rec.seq = h[k].seq;
            rec.slot = (int8_t)k;
            rec.flags = h[k].flags;
            if (h[k].stored) {
                rops[nr] = {chunkBase(cx[j], cz[j]) + (uint64_t)k * slotSize_ + CHUNK_HEADER, p, h[k].stored};
                rmap[nr++] = j;
            }
        }
        if (nr && !dev_->readMany(rops, nr))
            for (int r = 0; r < nr; r++) res[rmap[r]] = LOAD_ERROR;
    }
}

LoadResult WorldStore::fetchChunk(int cx, int cz, ChunkRecord& rec) {
    if (!chunkInRange(cx, cz)) return LOAD_ABSENT;
    ChunkHeaderInfo h[2];
    int order[2];
    int n = readHeaders(cx, cz, h, order);
    if (n < 0) return LOAD_ERROR;
    if (n == 0) return LOAD_ABSENT;
    int k = order[0];
    rec.bytes.clear();
    uint8_t* p = rec.bytes.append(h[k].stored);
    if (!p && h[k].stored) return LOAD_ERROR;
    if (h[k].stored && !dev_->read(chunkBase(cx, cz) + (uint64_t)k * slotSize_ + CHUNK_HEADER, p, h[k].stored))
        return LOAD_ERROR;
    rec.raw = h[k].raw;
    rec.crc = h[k].crc;
    rec.seq = h[k].seq;
    rec.slot = (int8_t)k;
    rec.flags = h[k].flags;
    return LOAD_OK;
}

bool WorldStore::decodeChunk(const ChunkRecord& rec, Chunk& c) const {
    if (decodeStored(rec.bytes.data(), (uint32_t)rec.bytes.size(), rec.raw, rec.crc, rec.flags, c) != DEC_OK)
        return false;
    c.storeSeq = rec.seq;
    c.storeSlot = rec.slot;
    return true;
}

bool WorldStore::encodeChunk(const Chunk& c, ChunkRecord& rec, uint8_t* deflateWs) const {
    rec.bytes.clear();
    CountSink rawCount;
    {
        Writer w(rawCount);
        writePayload(w, c);
    }
    if (compress_) {
        DeflateSink ds(rec.bytes, deflateWs);
        Writer w(ds);
        writePayload(w, c);
        ds.finish();
    } else {
        Writer w(rec.bytes);
        writePayload(w, c);
    }
    if (rec.bytes.failed()) return false;
    rec.raw = (uint32_t)rawCount.count;
    rec.crc = crc32(rec.bytes.data(), rec.bytes.size());
    rec.flags = (compress_ ? FLAG_ZLIB : 0) | FLAG_TICKS;
    return true;
}

bool WorldStore::writeChunk(Chunk& c, const ChunkRecord& rec) {
    if (!chunkInRange(c.cx, c.cz)) return false;
    if (rec.bytes.size() + CHUNK_HEADER > slotSize_) {
        MC_LOGE("storage: chunk %d,%d needs %u bytes, slot holds %u", c.cx, c.cz,
                (unsigned)(rec.bytes.size() + CHUNK_HEADER), (unsigned)slotSize_);
        return false;
    }
    ChunkHeader h;
    h.magic = CHUNK_MAGIC;
    h.seq = c.storeSeq + 1;
    h.cx = c.cx;
    h.cz = c.cz;
    h.stored = (uint32_t)rec.bytes.size();
    h.raw = rec.raw;
    h.crc = rec.crc;
    h.flags = rec.flags;
    h.version = RECORD_VERSION;
    int slot = c.storeSlot == 0 ? 1 : 0;
    uint8_t hb[CHUNK_HEADER];
    encodeHeader(hb, h);
    if (!dev_->beginWrite(chunkBase(c.cx, c.cz) + (uint64_t)slot * slotSize_, CHUNK_HEADER + h.stored)) return false;
    if (!dev_->writeData(hb, CHUNK_HEADER)) return false;
    if (h.stored && !dev_->writeData(rec.bytes.data(), h.stored)) return false;
    if (!dev_->endWrite()) return false;
    c.storeSeq = h.seq;
    c.storeSlot = (int8_t)slot;
    chunksWritten_++;
    return true;
}

bool WorldStore::saveChunk(Chunk& c) {
    if (!chunkInRange(c.cx, c.cz)) return false;
    // pass 1: sizes and checksum of what will be stored
    CountSink rawCount;
    {
        Writer w(rawCount);
        writePayload(w, c);
    }
    CrcCountSink cc;
    if (compress_) {
        DeflateSink ds(cc);
        Writer w(ds);
        writePayload(w, c);
        ds.finish();
    } else {
        Writer w(cc);
        writePayload(w, c);
    }
    if (cc.n + CHUNK_HEADER > slotSize_) {
        MC_LOGE("storage: chunk %d,%d needs %u bytes, slot holds %u", c.cx, c.cz, (unsigned)(cc.n + CHUNK_HEADER),
                (unsigned)slotSize_);
        return false;
    }
    ChunkHeader h;
    h.magic = CHUNK_MAGIC;
    h.seq = c.storeSeq + 1;
    h.cx = c.cx;
    h.cz = c.cz;
    h.stored = (uint32_t)cc.n;
    h.raw = (uint32_t)rawCount.count;
    h.crc = cc.crc;
    h.flags = (compress_ ? FLAG_ZLIB : 0) | FLAG_TICKS;
    h.version = RECORD_VERSION;
    int slot = c.storeSlot == 0 ? 1 : 0;
    uint64_t off = chunkBase(c.cx, c.cz) + (uint64_t)slot * slotSize_;
    uint8_t hb[CHUNK_HEADER];
    encodeHeader(hb, h);
    // pass 2: stream header + payload straight to the device
    if (!dev_->beginWrite(off, CHUNK_HEADER + h.stored)) return false;
    DeviceSink ds(dev_);
    ds.put(hb, CHUNK_HEADER);
    if (compress_) {
        DeflateSink z(ds);
        Writer w(z);
        writePayload(w, c);
        z.finish();
    } else {
        Writer w(ds);
        writePayload(w, c);
    }
    ds.drain();
    if (!ds.ok || !dev_->endWrite()) return false;
    c.storeSeq = h.seq;
    c.storeSlot = (int8_t)slot;
    chunksWritten_++;
    return true;
}

// ------------------------------------------------------------------ players
static uint32_t uuidHash(const uint8_t u[16]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; i++) { h ^= u[i]; h *= 16777619u; }
    return h;
}

static void encodePlayer(uint8_t* b, const PlayerData& p) {
    memset(b, 0, PLAYER_SLOT);
    BufSink s(b, PLAYER_SLOT);
    Writer w(s);
    w.u32(PLAYER_MAGIC);
    w.u32(1);
    w.uuid(p.uuid);
    w.bytes((const uint8_t*)p.name, 17);
    w.f64(p.x);
    w.f64(p.y);
    w.f64(p.z);
    w.f32(p.yaw);
    w.f32(p.pitch);
    w.u8(p.gamemode);
    w.f32(p.health);
    w.u8(p.food);
    w.f32(p.saturation);
    w.i32(p.xpLevel);
    w.f32(p.xpProgress);
    w.i32(p.xpTotal);
    w.u8(p.held);
    w.u8(p.hasSpawn ? 1 : 0);
    w.i32(p.spawnX);
    w.i32(p.spawnY);
    w.i32(p.spawnZ);
    for (int i = 0; i < 46; i++) writeStack(w, p.inv[i]);
    uint32_t c = crc32(b, PLAYER_SLOT - 4);
    b[508] = (uint8_t)(c >> 24);
    b[509] = (uint8_t)(c >> 16);
    b[510] = (uint8_t)(c >> 8);
    b[511] = (uint8_t)c;
}

static bool decodePlayer(const uint8_t* b, PlayerData& p) {
    uint32_t c = crc32(b, PLAYER_SLOT - 4);
    uint32_t stored = (uint32_t)b[508] << 24 | (uint32_t)b[509] << 16 | (uint32_t)b[510] << 8 | b[511];
    if (c != stored) return false;
    Reader r(b, PLAYER_SLOT - 4);
    if (r.u32() != PLAYER_MAGIC || r.u32() != 1) return false;
    r.bytes(p.uuid, 16);
    r.bytes((uint8_t*)p.name, 17);
    p.name[16] = 0;
    p.x = r.f64();
    p.y = r.f64();
    p.z = r.f64();
    p.yaw = r.f32();
    p.pitch = r.f32();
    p.gamemode = r.u8();
    p.health = r.f32();
    p.food = r.u8();
    p.saturation = r.f32();
    p.xpLevel = r.i32();
    p.xpProgress = r.f32();
    p.xpTotal = r.i32();
    p.held = r.u8();
    p.hasSpawn = r.u8() != 0;
    p.spawnX = r.i32();
    p.spawnY = r.i32();
    p.spawnZ = r.i32();
    for (int i = 0; i < 46; i++) readStack(r, p.inv[i]);
    return r.ok() && isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
}

// Finds the slot of a player (or the first free slot when `forWrite`). -1 if none.
static int findPlayerSlot(BlockDevice* dev, uint64_t tableOff, uint32_t slots, const uint8_t uuid[16], bool forWrite,
                          PlayerData* out, bool& ioError) {
    ioError = false;
    uint32_t start = uuidHash(uuid) % slots;
    const int BATCH = 4, MAX_PROBE = 32;
    uint8_t* buf = (uint8_t*)malloc(PLAYER_SLOT * BATCH);
    if (!buf) { ioError = true; return -1; }
    int result = -1;
    int freeSlot = -1;
    for (int base = 0; base < MAX_PROBE && result < 0; base += BATCH) {
        ReadOp ops[BATCH];
        for (int k = 0; k < BATCH; k++) {
            uint32_t slot = (start + base + k) % slots;
            ops[k] = {tableOff + (uint64_t)slot * PLAYER_SLOT, buf + k * PLAYER_SLOT, PLAYER_SLOT};
        }
        if (!dev->readMany(ops, BATCH)) { ioError = true; break; }
        bool end = false;
        for (int k = 0; k < BATCH; k++) {
            uint32_t slot = (start + base + k) % slots;
            const uint8_t* b = buf + k * PLAYER_SLOT;
            PlayerData pd;
            bool used = b[0] || b[1] || b[2] || b[3];
            if (!used) {
                if (freeSlot < 0) freeSlot = (int)slot;
                end = true;
                break;
            }
            if (decodePlayer(b, pd) && !memcmp(pd.uuid, uuid, 16)) {
                if (out) *out = pd;
                result = (int)slot;
                break;
            }
            if (!decodePlayer(b, pd) && freeSlot < 0 && forWrite) freeSlot = (int)slot;  // corrupt: reuse
        }
        if (end) break;
    }
    free(buf);
    if (result < 0 && forWrite) result = freeSlot;
    return result;
}

int WorldStore::cachedSlot(const uint8_t uuid[16]) const {
    for (int i = 0; i < slotCacheN_; i++)
        if (!memcmp(slotCache_[i].uuid, uuid, 16)) return slotCache_[i].slot;
    return -1;
}

void WorldStore::cacheSlot(const uint8_t uuid[16], int slot) {
    for (int i = 0; i < slotCacheN_; i++)
        if (!memcmp(slotCache_[i].uuid, uuid, 16)) { slotCache_[i].slot = slot; return; }
    int i = slotCacheN_ < SLOT_CACHE ? slotCacheN_++ : slotCacheNext_++ % SLOT_CACHE;
    memcpy(slotCache_[i].uuid, uuid, 16);
    slotCache_[i].slot = slot;
}

bool WorldStore::loadPlayer(const uint8_t uuid[16], PlayerData& out) {
    if (!open_) return false;
    bool ioError;
    int slot = findPlayerSlot(dev_, playerOff_, playerSlots_, uuid, false, &out, ioError);
    if (slot >= 0) cacheSlot(uuid, slot);
    return slot >= 0;
}

bool WorldStore::savePlayer(const PlayerData& p) {
    if (!open_) return false;
    // a player saved or loaded before keeps its slot: no lookup round trips
    int slot = cachedSlot(p.uuid);
    if (slot < 0) {
        bool ioError;
        slot = findPlayerSlot(dev_, playerOff_, playerSlots_, p.uuid, true, nullptr, ioError);
        if (slot < 0) {
            if (!ioError) MC_LOGE("storage: player table full");
            return false;
        }
        cacheSlot(p.uuid, slot);
    }
    uint8_t b[PLAYER_SLOT];
    encodePlayer(b, p);
    if (!dev_->write(playerOff_ + (uint64_t)slot * PLAYER_SLOT, b, PLAYER_SLOT)) return false;
    playersWritten_++;
    return true;
}

}  // namespace mc
