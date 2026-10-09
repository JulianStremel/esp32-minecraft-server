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
// The only format this build reads and writes: chunks found through a region index
// (unbounded worlds), player records with item NBT, block states and item ids of
// Minecraft 1.21.8. A world in an older format (1 and 2 dense, 3 without item NBT,
// 4 with 1.16.5 ids) is not converted: opening it starts a new world.
static const int FORMAT_ITEM_TAGS = 5;
static const uint32_t PLAYER_EXTENDED_SLOT = 1024;
static const uint32_t PLAYER_SLOT = 512;
static const uint32_t CHUNK_HEADER = 32;
static const uint32_t FLAG_ZLIB = 1;
static const uint32_t FLAG_TICKS = 2;   // the payload ends with the chunk's scheduled ticks (v2)
static const uint32_t FLAG_ITEM_TAGS = 4;
static const uint16_t RECORD_VERSION = 3;
static const uint32_t SUPER_HAS_WORLD = 1;
// region directory: 1/256 of the export, 256 KiB (8192 entries) to 16 MiB (524288)
static uint64_t dirBytes(uint64_t devSize) {
    uint64_t d = (devSize / 256 + 65535) & ~(uint64_t)65535;
    return d < (256u << 10) ? (256u << 10) : (d > (16ull << 20) ? (16ull << 20) : d);
}
static const int MAX_RADIUS = 1874999;
// The world's extra state (WorldMeta::extra): two copies in the unused 3 KiB between the
// superblock copies and the player table (every format leaves it free; older builds
// never read it). Each: magic, sequence number, length, data, CRC over all of that.
static const uint64_t EXTRA_OFF = 1024;
static const uint32_t EXTRA_COPY = 1536;
static const uint32_t EXTRA_MAGIC = 0x57444154;   // "WDAT"
          // vanilla's border: 29 999 984 blocks

static uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

WorldStore::WorldStore(BlockDevice* dev) : dev_(dev) {}

uint64_t WorldStore::requiredSize() const { return index_.end(); }

bool WorldStore::chunkInRange(int cx, int cz) const {
    return open_ && cx >= -radius_ && cx < radius_ && cz >= -radius_ && cz < radius_;
}

bool WorldStore::findChunk(uint8_t dim, int cx, int cz, uint64_t& base) {
    base = 0;
    return index_.lookup(dim, cx, cz, base);
}

bool WorldStore::chunkForWrite(uint8_t dim, int cx, int cz, uint64_t& base, bool& commit) {
    commit = false;
    return index_.unitForWrite(dim, cx, cz, base, commit);
}

// ------------------------------------------------------------------ superblock
bool WorldStore::writeSuper() {
    uint8_t b[512];
    superSeq_++;
    memset(b, 0, 512);
    BufSink s(b, 512);
    Writer w(s);
    w.bytes((const uint8_t*)SUPER_MAGIC, 8);
    w.u32(FORMAT_ITEM_TAGS);
    w.u32(superSeq_);
    w.i32(radius_);
    w.u32(slotSize_);
    w.u32(playerSlots_);
    w.u32(playerStride_);
    w.u64(playerOff_);
    w.u64(0);   // formats 1 and 2: the dense chunk area
    w.u64(meta_.seed);
    w.u8(meta_.worldType);
    w.u8(meta_.raining);
    w.u16(meta_.generatorVersion);   // was reserved (0): older worlds read as version 0 = 1
    w.i32(meta_.spawnX);
    w.i32(meta_.spawnY);
    w.i32(meta_.spawnZ);
    w.i64(meta_.worldAge);
    w.i64(meta_.timeOfDay);
    w.i32(meta_.weatherTimer);
    w.u32(haveWorld_ ? SUPER_HAS_WORLD : 0);
    w.u64(layout_.dirOff);
    w.u64(layout_.dirCap);
    w.u64(layout_.dataOff);
    w.u64(index_.watermark());   // a hint: the directory log has the authoritative one
    w.i32(0);                    // format 3: the radius of a dense area kept from formats 1, 2
    uint32_t c = crc32(b, 508);
    b[508] = (uint8_t)(c >> 24);
    b[509] = (uint8_t)(c >> 16);
    b[510] = (uint8_t)(c >> 8);
    b[511] = (uint8_t)c;
    return dev_->write((uint64_t)(superSeq_ & 1) * 512, b, 512);
}

// Reads the newest valid superblock and the region layout; allocHint receives the
// watermark it knew. A valid superblock of another format: false, with oldFormat set.
bool WorldStore::readSuper(uint64_t& allocHint, int& oldFormat) {
    oldFormat = 0;
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
    if (version != (uint32_t)FORMAT_ITEM_TAGS) {
        oldFormat = (int)version;
        free(buf);
        return false;
    }
    format_ = FORMAT_ITEM_TAGS;
    superSeq_ = r.u32();
    radius_ = r.i32();
    slotSize_ = r.u32();
    playerSlots_ = r.u32();
    playerStride_ = r.u32();
    if (playerStride_ != PLAYER_EXTENDED_SLOT) { free(buf); return false; }
    playerOff_ = r.u64();
    r.u64();   // the dense chunk area of formats 1 and 2
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
    layout_.dirOff = r.u64();
    layout_.dirCap = r.u64();
    layout_.dataOff = r.u64();
    allocHint = r.u64();
    r.i32();   // format 3's dense area
    layout_.unit = 2ull * slotSize_;
    meta_.radius = radius_;
    free(buf);
    return r.ok();
}

// Format 4 on a blank device (the world border is the setting, not the device size).
bool WorldStore::formatRegions(const StoreParams& params) {
    playerSlots_ = params.playerSlots;
    slotSize_ = params.chunkSlotSize;
    playerOff_ = 4096;
    playerStride_ = PLAYER_EXTENDED_SLOT;
    layout_.unit = 2ull * slotSize_;
    layout_.dirOff = alignUp(playerOff_ + (uint64_t)playerSlots_ * playerStride_, 1 << 20);
    layout_.dirCap = dirBytes(dev_->size());
    layout_.dataOff = layout_.dirOff + layout_.dirCap;
    if (dev_->size() < layout_.dataOff + layout_.unit) {
        MC_LOGE("storage: device too small (%llu bytes, needs at least %llu)", (unsigned long long)dev_->size(),
                (unsigned long long)(layout_.dataOff + layout_.unit));
        return false;
    }
    format_ = FORMAT_ITEM_TAGS;
    if (!index_.format(dev_, layout_)) return false;
    return true;
}

bool WorldStore::open(const StoreParams& params, bool allowFormat) {
    params_ = params;
    open_ = false;
    compress_ = params.compress;
    if (!dev_->available()) return false;
    uint64_t allocHint = 0;
    int oldFormat = 0;
    if (readSuper(allocHint, oldFormat)) {
        if (!index_.open(dev_, layout_, allocHint)) {
            MC_LOGE("storage: cannot read the region directory");
            return false;
        }
        int r = params.radius > MAX_RADIUS ? MAX_RADIUS : params.radius;
        if (r != radius_) {   // the world border is a setting
            MC_LOGI("storage: world border %d -> %d chunks", radius_, r);
            radius_ = r;
            if (!writeSuper()) return false;
        }
        open_ = true;
        MC_LOGI("storage: opened %s (format %d, border %d chunks, %u regions, %s)", dev_->describe(), format_, radius_,
                (unsigned)index_.stats().regions, haveWorld_ ? "world present" : "empty");
        return true;
    }
    if (oldFormat) {
        // our world, from an older build: not converted, a new world takes its place
        MC_LOGW("storage: the world on %s has format %d from an older build; starting a new world in format %d "
                "(the old one is discarded)", dev_->describe(), oldFormat, FORMAT_ITEM_TAGS);
    } else {
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
    }
    radius_ = params.radius > MAX_RADIUS ? MAX_RADIUS : params.radius;
    // the player table is zeroed: a device used before may hold an old world's players
    if (!formatRegions(params) || !zeroPlayerTable()) return false;
    haveWorld_ = false;
    meta_.reset();
    meta_.radius = radius_;
    superSeq_ = 0;
    extraSeq_ = 0;
    extraCrc_ = 0;
    {   // a reused device may hold an old world's extra state
        uint8_t* z = (uint8_t*)calloc(1, 2 * EXTRA_COPY);
        if (!z || !dev_->write(EXTRA_OFF, z, 2 * EXTRA_COPY)) {
            free(z);
            return false;
        }
        free(z);
    }
    // wipe both superblock copies + player table header region is assumed zero (sparse)
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    if (!dev_->write(512, zero, 512)) return false;
    if (!writeSuper() || !dev_->flush()) return false;
    open_ = true;
    MC_LOGI("storage: formatted %s (format %d): border %d chunks, %u player slots, %u KiB per chunk copy",
            dev_->describe(), format_, radius_, (unsigned)playerSlots_, (unsigned)(slotSize_ / 1024));
    return true;
}

bool WorldStore::zeroPlayerTable() {
    const uint32_t BLOCK = 64 * 1024;
    uint8_t* z = (uint8_t*)plat::bigAlloc(BLOCK);
    if (!z) return false;
    memset(z, 0, BLOCK);
    uint64_t end = playerOff_ + (uint64_t)playerSlots_ * playerStride_;
    bool ok = true;
    for (uint64_t off = playerOff_; off < end && ok; off += BLOCK) {
        uint64_t n = end - off < BLOCK ? end - off : BLOCK;
        ok = dev_->write(off, z, (size_t)n);
    }
    plat::bigFree(z);
    return ok;
}

bool WorldStore::resetWorld(const WorldMeta& fresh) {
    if (!open_) return false;
    // a new format (the old chunks are no longer reachable, their space is reused), the
    // player table zeroed, then the new world's metadata
    StoreParams p = params_;
    p.radius = radius_;
    if (!formatRegions(p) || !zeroPlayerTable()) return false;
    haveWorld_ = false;
    meta_.reset();
    superSeq_ = 0;
    extraSeq_ = 0;
    extraCrc_ = 0;
    {
        uint8_t* e = (uint8_t*)calloc(1, 2 * EXTRA_COPY);
        if (!e || !dev_->write(EXTRA_OFF, e, 2 * EXTRA_COPY)) {
            free(e);
            return false;
        }
        free(e);
    }
    open_ = true;
    // both superblock copies: the old ones have higher sequence numbers and would win
    {
        uint8_t zero[512];
        memset(zero, 0, sizeof(zero));
        if (!dev_->write(0, zero, 512) || !dev_->write(512, zero, 512)) return false;
    }
    if (!saveMeta(fresh) || !saveMeta(fresh) || !dev_->flush()) return false;   // twice: both copies
    MC_LOGW("storage: the world was reset (seed %lld)", (long long)fresh.seed);
    return true;
}

bool WorldStore::loadMeta(WorldMeta& m) {
    m.radius = radius_;
    if (!open_ || !haveWorld_) return false;
    m = meta_;
    m.radius = radius_;
    readExtra(m);
    return true;
}

bool WorldStore::saveMeta(const WorldMeta& m) {
    if (!open_) return false;
    meta_ = m;
    meta_.radius = radius_;
    haveWorld_ = true;
    return writeSuper() && writeExtra(m);
}

bool WorldStore::readExtra(WorldMeta& m) {
    m.extraLen = 0;
    uint8_t* b = (uint8_t*)malloc(EXTRA_COPY);
    if (!b) return false;
    uint32_t bestSeq = 0;
    bool found = false;
    for (uint32_t k = 0; k < 2; k++) {
        if (!dev_->read(EXTRA_OFF + k * EXTRA_COPY, b, EXTRA_COPY)) continue;
        Reader r(b, EXTRA_COPY);
        uint32_t magic = r.u32(), seq = r.u32();
        uint16_t len = r.u16();
        if (magic != EXTRA_MAGIC || len > WorldMeta::EXTRA_CAP) continue;
        uint32_t c = crc32(b, 10 + len);
        uint32_t stored = (uint32_t)b[10 + len] << 24 | (uint32_t)b[11 + len] << 16 | (uint32_t)b[12 + len] << 8 | b[13 + len];
        if (c != stored || (found && seq <= bestSeq)) continue;
        found = true;
        bestSeq = seq;
        memcpy(m.extra, b + 10, len);
        m.extraLen = len;
        extraCrc_ = crc32(m.extra, len) ^ len;
    }
    free(b);
    extraSeq_ = bestSeq;
    return found;
}

bool WorldStore::writeExtra(const WorldMeta& m) {
    if (m.extraLen > WorldMeta::EXTRA_CAP) return false;
    uint8_t* b = (uint8_t*)calloc(1, EXTRA_COPY);
    if (!b) return false;
    // unchanged since the last write: nothing to do (the CRC covers the sequence number,
    // so compare the data's own checksum)
    uint32_t dataCrc = crc32(m.extra, m.extraLen) ^ m.extraLen;
    if (extraSeq_ && dataCrc == extraCrc_) {
        free(b);
        return true;
    }
    BufSink s(b, EXTRA_COPY);
    Writer w(s);
    w.u32(EXTRA_MAGIC);
    w.u32(extraSeq_ + 1);
    w.u16(m.extraLen);
    w.bytes(m.extra, m.extraLen);
    uint32_t c = crc32(b, 10 + m.extraLen);
    b[10 + m.extraLen] = (uint8_t)(c >> 24);
    b[11 + m.extraLen] = (uint8_t)(c >> 16);
    b[12 + m.extraLen] = (uint8_t)(c >> 8);
    b[13 + m.extraLen] = (uint8_t)c;
    // the older copy is overwritten: the newer one survives a torn write
    bool ok = dev_->write(EXTRA_OFF + (uint64_t)((extraSeq_ + 1) & 1) * EXTRA_COPY, b, EXTRA_COPY);
    free(b);
    if (ok) {
        extraSeq_++;
        extraCrc_ = dataCrc;
    }
    return ok;
}

bool WorldStore::flush() { return open_ && dev_->flush(); }

void WorldStore::statusLine(char* buf, size_t cap) {
    const DeviceStats& s = dev_->stats();
    const RegionIndex::Stats& x = index_.stats();
    int n = snprintf(buf, cap, "%s | %u KB read, %u KB written, %u chunks saved, %u loaded, %u ms latency, %u errors%s",
                     dev_->describe(), (unsigned)(s.bytesRead / 1024), (unsigned)(s.bytesWritten / 1024),
                     (unsigned)chunksWritten_, (unsigned)chunksRead_, (unsigned)s.lastLatencyMs, (unsigned)s.errors,
                     s.reconnects ? " (reconnected)" : "");
    if (n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - (size_t)n, " | %u regions, %llu MiB allocated, maps %u hit %u read%s", (unsigned)x.regions,
                      (unsigned long long)((x.unitsUsed * layout_.unit) >> 20), (unsigned)x.mapHits, (unsigned)x.mapMisses,
                      x.full ? ", EXPORT FULL" : "");
    if (BlockDevice* d = dev_->backing())   // what reached the device behind the cache
        if (n > 0 && (size_t)n < cap) {
            const DeviceStats& b = d->stats();
            snprintf(buf + n, cap - (size_t)n, " | device: %u reads (%u KB), %u writes (%u KB), %u flushes, last %u ms",
                     (unsigned)b.reads, (unsigned)(b.bytesRead / 1024), (unsigned)b.writes, (unsigned)(b.bytesWritten / 1024),
                     (unsigned)b.flushes, (unsigned)b.lastLatencyMs);
        }
}

// ------------------------------------------------------------------ chunk payload
static void writeStack(Writer& w, const ItemStack& s, bool tags = false) {
    w.u16(s.empty() ? 0 : s.id);
    w.u8(s.empty() ? 0 : s.count);
    w.u16(s.damage);
    if (tags) {
        if (s.empty() || !s.tagSize()) w.u8(0);
        else w.bytes(s.tagData(), s.tagSize());
    }
}

static bool readStack(Reader& r, ItemStack& s, bool tags = false) {
    uint16_t id = r.u16();
    uint8_t count = r.u8();
    uint16_t dmg = r.u16();
    s.clear();
    if (tags && !s.readTag(r)) return false;
    if (id && count && id < NUM_ITEMS) {
        s.id = id;
        s.count = count;
        s.damage = dmg;
    } else s.clear();
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
            for (int i = 0; i < 27; i++) writeStack(w, t->items[i], true);
        } else if (t->type == TILE_HOPPER || t->type == TILE_DROPPER || t->type == TILE_DISPENSER) {
            for (int i = 0; i < t->slotCount(); ++i) writeStack(w, t->items[i], true);
            if (t->type == TILE_HOPPER) w.i16(t->transferCooldown);
        } else if (t->type == TILE_FURNACE) {
            for (int i = 0; i < 3; i++) writeStack(w, t->items[i], true);
            w.i16(t->burnTime);
            w.i16(t->burnTotal);
            w.i16(t->cookTime);
        } else if (t->type == TILE_LECTERN) {
            writeStack(w, t->items[0], true);
            w.i32(t->bookPage);
        } else if (t->type == TILE_BOOKSHELF) {
            for (int i = 0; i < 6; i++) writeStack(w, t->items[i], true);
            w.i8(t->lastSlot);
        } else if (t->type == TILE_CRAFTER) {
            for (int i = 0; i < 9; i++) writeStack(w, t->items[i], true);
            w.u16(t->disabledSlots);
        } else if (t->type == TILE_SCULK) {
            w.u8(t->frequency);
        } else if (t->type == TILE_COMPARATOR) {
            w.u8(t->signal);
        } else if (t->type == TILE_PISTON) {
            w.u16(t->movedState);
            w.u8(t->pistonFace);
            w.u8(t->pistonPrevious); // vanilla saves progressO, not progress
            w.u8((t->pistonExtending ? 1 : 0) | (t->pistonSource ? 2 : 0));
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
        if (!t) return false;
        if (type == TILE_CHEST || type == TILE_BARREL) {
            for (int k = 0; k < 27; k++) readStack(r, t->items[k], flags & FLAG_ITEM_TAGS);
        } else if (type == TILE_HOPPER || type == TILE_DROPPER || type == TILE_DISPENSER) {
            for (int k = 0; k < t->slotCount(); ++k) readStack(r, t->items[k], flags & FLAG_ITEM_TAGS);
            if (type == TILE_HOPPER) t->transferCooldown = r.i16();
        } else if (type == TILE_FURNACE) {
            for (int k = 0; k < 3; k++) readStack(r, t->items[k], flags & FLAG_ITEM_TAGS);
            t->burnTime = r.i16();
            t->burnTotal = r.i16();
            t->cookTime = r.i16();
        } else if (type == TILE_LECTERN) {
            if (!readStack(r, t->items[0], true)) return false;
            t->bookPage = r.i32();
            if (t->bookPage < -1 || t->bookPage > 32767) return false;
        } else if (type == TILE_BOOKSHELF) {
            for (int k = 0; k < 6; k++) readStack(r, t->items[k], true);
            t->lastSlot = r.i8();
            if (t->lastSlot < -1 || t->lastSlot > 5) return false;
        } else if (type == TILE_CRAFTER) {
            for (int k = 0; k < 9; k++) readStack(r, t->items[k], true);
            t->disabledSlots = r.u16() & 0x1FF;
        } else if (type == TILE_SCULK) {
            t->frequency = r.u8();
            if (t->frequency > 15) return false;
        } else if (type == TILE_COMPARATOR) {
            t->signal = r.u8();
            if (t->signal > 15) return false;
        } else if (type == TILE_PISTON) {
            t->movedState = r.u16();
            t->pistonFace = r.u8();
            t->pistonProgress = t->pistonPrevious = r.u8();
            uint8_t bits = r.u8();
            if (t->movedState >= NUM_STATES || t->pistonFace > 5 || t->pistonProgress > 2 || bits > 3) return false;
            t->pistonExtending = bits & 1; t->pistonSource = bits & 2;
        } else if (type == TILE_DAYLIGHT) {
            // Only the block state persists; cached light is recomputed after loading.
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
int WorldStore::readHeaders(int cx, int cz, uint64_t base, ChunkHeaderInfo h[2], int order[2]) {
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
    uint64_t base;
    if (!findChunk(c.dim, c.cx, c.cz, base)) return LOAD_ERROR;
    if (!base) return LOAD_ABSENT;
    ChunkHeaderInfo h[2];
    int order[2];
    int n = readHeaders(c.cx, c.cz, base, h, order);
    if (n < 0) return LOAD_ERROR;
    if (n == 0) return LOAD_ABSENT;
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
                MC_LOGW("storage: chunk %d,%d copy %d fails its checksum", (int)c.cx, (int)c.cz, k);
                continue;
            case DEC_BAD_ZLIB:
                MC_LOGW("storage: chunk %d,%d copy %d does not decompress", (int)c.cx, (int)c.cz, k);
                continue;
            case DEC_MALFORMED:
                MC_LOGW("storage: chunk %d,%d copy %d is malformed", (int)c.cx, (int)c.cz, k);
                return LOAD_ERROR;  // chunk object partly filled: let the caller start over
            case DEC_NO_MEMORY:
                return LOAD_ERROR;
        }
    }
    return LOAD_ERROR;
}

void WorldStore::fetchChunks(int n, const uint8_t* dims, const int32_t* cx, const int32_t* cz,
                             ChunkRecord* const* recs, LoadResult* res) {
    // round trip 1: both header copies of every chunk; round trip 2: the newest valid
    // record of the chunks that have one (never-stored chunks cost only the first)
    // (format 3: before that, one round trip for the slot maps not cached)
    const int MAX = 16;
    for (int start = 0; start < n; start += MAX) {
        int m = n - start < MAX ? n - start : MAX;
        uint8_t hb[MAX][2][CHUNK_HEADER];
        uint64_t bases[MAX];
        ReadOp ops[2 * MAX];
        int nops = 0;
        if (!index_.prefetch(m, dims + start, cx + start, cz + start)) {
            for (int i = 0; i < m; i++) res[start + i] = LOAD_ERROR;
            continue;
        }
        for (int i = 0; i < m; i++) {
            int j = start + i;
            res[j] = LOAD_ABSENT;
            bases[i] = 0;
            if (!chunkInRange(cx[j], cz[j])) continue;
            uint64_t base;
            if (!findChunk(dims[j], cx[j], cz[j], base)) {
                res[j] = LOAD_ERROR;
                continue;
            }
            if (!base) continue;   // never saved
            bases[i] = base;
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
                rops[nr] = {bases[i] + (uint64_t)k * slotSize_ + CHUNK_HEADER, p, h[k].stored};
                rmap[nr++] = j;
            }
        }
        if (nr && !dev_->readMany(rops, nr))
            for (int r = 0; r < nr; r++) res[rmap[r]] = LOAD_ERROR;
    }
}

LoadResult WorldStore::fetchChunk(uint8_t dim, int cx, int cz, ChunkRecord& rec) {
    if (!chunkInRange(cx, cz)) return LOAD_ABSENT;
    uint64_t base;
    if (!findChunk(dim, cx, cz, base)) return LOAD_ERROR;
    if (!base) return LOAD_ABSENT;
    ChunkHeaderInfo h[2];
    int order[2];
    int n = readHeaders(cx, cz, base, h, order);
    if (n < 0) return LOAD_ERROR;
    if (n == 0) return LOAD_ABSENT;
    int k = order[0];
    rec.bytes.clear();
    uint8_t* p = rec.bytes.append(h[k].stored);
    if (!p && h[k].stored) return LOAD_ERROR;
    if (h[k].stored && !dev_->read(base + (uint64_t)k * slotSize_ + CHUNK_HEADER, p, h[k].stored))
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
    rec.flags = (compress_ ? FLAG_ZLIB : 0) | FLAG_TICKS | FLAG_ITEM_TAGS;
    return true;
}

bool WorldStore::writeChunk(Chunk& c, const ChunkRecord& rec) {
    if (!chunkInRange(c.cx, c.cz)) return false;
    if (rec.bytes.size() + CHUNK_HEADER > slotSize_) {
        MC_LOGE("storage: chunk %d,%d needs %u bytes, slot holds %u", (int)c.cx, (int)c.cz,
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
    uint64_t base;
    bool commit;
    if (!chunkForWrite(c.dim, c.cx, c.cz, base, commit)) return false;
    if (!dev_->beginWrite(base + (uint64_t)slot * slotSize_, CHUNK_HEADER + h.stored)) return false;
    if (!dev_->writeData(hb, CHUNK_HEADER)) return false;
    if (h.stored && !dev_->writeData(rec.bytes.data(), h.stored)) return false;
    if (!dev_->endWrite()) return false;
    // a chunk's first record in format 3: its region's map points to it from now on
    if (commit && !index_.commit(c.dim, c.cx, c.cz)) return false;
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
        MC_LOGE("storage: chunk %d,%d needs %u bytes, slot holds %u", (int)c.cx, (int)c.cz, (unsigned)(cc.n + CHUNK_HEADER),
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
    h.flags = (compress_ ? FLAG_ZLIB : 0) | FLAG_TICKS | FLAG_ITEM_TAGS;
    h.version = RECORD_VERSION;
    int slot = c.storeSlot == 0 ? 1 : 0;
    uint64_t base;
    bool commit;
    if (!chunkForWrite(c.dim, c.cx, c.cz, base, commit)) return false;
    uint64_t off = base + (uint64_t)slot * slotSize_;
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
    if (commit && !index_.commit(c.dim, c.cx, c.cz)) return false;
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
    w.u32(2);   // 2: the dimension follows the inventory
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
    w.u8(p.dim);
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
    if (r.u32() != PLAYER_MAGIC) return false;
    uint32_t version = r.u32();
    if (version < 1 || version > 2) return false;
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
    p.dim = version >= 2 ? r.u8() : DIM_OVERWORLD;   // version 1 predates the other dimensions
    if (p.dim >= NUM_DIMS) p.dim = DIM_OVERWORLD;
    return r.ok() && isfinite(p.x) && isfinite(p.y) && isfinite(p.z);
}

// Format 4 retains each legacy 512-byte record, followed by two independent
// 256-byte descriptors. Variable-size payloads use the region allocator. A save
// writes the older payload, flushes it, then publishes only that descriptor.
struct PlayerRecord {
    bool valid = false;
    uint32_t sequence = 0, size = 0, capacity = 0, crc = 0;
    uint64_t offset = 0;
    uint8_t uuid[16] = {};
};
static const uint32_t PLAYER_DESCRIPTOR_MAGIC = 0x504C5933; // PLY3
static const uint32_t MAX_PLAYER_BYTES = PLAYER_SLOT + 46 * ItemStack::MAX_TAG_BYTES;
static PlayerRecord playerRecord(const uint8_t* b, uint64_t deviceSize) {
    PlayerRecord rec;
    Reader r(b, 256);
    if (r.u32() != PLAYER_DESCRIPTOR_MAGIC) return rec;
    rec.sequence = r.u32(); r.bytes(rec.uuid, 16);
    rec.offset = r.u64(); rec.size = r.u32(); rec.capacity = r.u32(); rec.crc = r.u32();
    r.skip(252 - 44);
    if (r.u32() != crc32(b, 252) || rec.size < PLAYER_SLOT || rec.size > MAX_PLAYER_BYTES ||
        rec.capacity < rec.size || rec.offset < 4096 || rec.offset > deviceSize ||
        rec.capacity > deviceSize - rec.offset) return rec;
    rec.valid = true;
    return rec;
}
static void encodePlayerRecord(uint8_t* b, const PlayerRecord& rec) {
    memset(b, 0, 256);
    BufSink sink(b, 256); Writer w(sink);
    w.u32(PLAYER_DESCRIPTOR_MAGIC); w.u32(rec.sequence); w.uuid(rec.uuid);
    w.u64(rec.offset); w.u32(rec.size); w.u32(rec.capacity); w.u32(rec.crc);
    w.zeros(252 - 44); w.u32(crc32(b, 252));
}
static LoadResult readExtendedPlayer(BlockDevice* dev, const uint8_t* entry, const uint8_t uuid[16], PlayerData& out) {
    PlayerRecord records[2] = {playerRecord(entry + 512, dev->size()), playerRecord(entry + 768, dev->size())};
    int first = records[1].valid && (!records[0].valid || records[1].sequence > records[0].sequence) ? 1 : 0;
    for (int j = 0; j < 2; ++j) {
        const PlayerRecord& rec = records[first ^ j];
        if (!rec.valid || memcmp(rec.uuid, uuid, 16)) continue;
        ByteBuf payload;
        uint8_t* bytes = payload.append(rec.size);
        if (!bytes || !dev->read(rec.offset, bytes, rec.size)) return LOAD_ERROR;
        if (crc32(bytes, rec.size) != rec.crc) continue;
        PlayerData data;
        if (!decodePlayer(bytes, data) || memcmp(data.uuid, uuid, 16)) continue;
        Reader r(bytes + PLAYER_SLOT, rec.size - PLAYER_SLOT);
        for (ItemStack& stack : data.inv) {
            uint16_t damage = stack.damage;
            if (!stack.readTag(r)) break;
            stack.damage = damage;
        }
        if (!r.ok() || r.remaining()) continue;
        out = data;
        return LOAD_OK;
    }
    PlayerData legacy;
    if (decodePlayer(entry, legacy) && !memcmp(legacy.uuid, uuid, 16)) { out = legacy; return LOAD_OK; }
    return LOAD_ERROR;
}

static int findExtendedPlayerSlot(BlockDevice* dev, uint64_t tableOff, uint32_t slots, const uint8_t uuid[16],
                                  bool forWrite, PlayerData* out, bool& ioError) {
    ioError = false;
    const int BATCH = 4, MAX_PROBE = 32;
    uint32_t start = uuidHash(uuid) % slots;
    ByteBuf buffer;
    uint8_t* bytes = buffer.append(PLAYER_EXTENDED_SLOT * BATCH);
    if (!bytes) { ioError = true; return -1; }
    for (int base = 0; base < MAX_PROBE; base += BATCH) {
        ReadOp ops[BATCH];
        for (int k = 0; k < BATCH; ++k)
            ops[k] = {tableOff + (uint64_t)((start + base + k) % slots) * PLAYER_EXTENDED_SLOT,
                      bytes + k * PLAYER_EXTENDED_SLOT, PLAYER_EXTENDED_SLOT};
        if (!dev->readMany(ops, BATCH)) { ioError = true; return -1; }
        for (int k = 0; k < BATCH; ++k) {
            const uint8_t* b = bytes + k * PLAYER_EXTENDED_SLOT;
            bool empty = true;
            for (int i = 0; i < (int)PLAYER_EXTENDED_SLOT; ++i) empty &= b[i] == 0;
            int slot = (start + base + k) % slots;
            if (empty) return forWrite ? slot : -1;
            // A damaged matching record is an error, never a fresh inventory.
            Reader legacyMagic(b, 4), firstMagic(b + 512, 4), secondMagic(b + 768, 4);
            bool matches = (legacyMagic.u32() == PLAYER_MAGIC && !memcmp(b + 8, uuid, 16)) ||
                (firstMagic.u32() == PLAYER_DESCRIPTOR_MAGIC && !memcmp(b + 520, uuid, 16)) ||
                (secondMagic.u32() == PLAYER_DESCRIPTOR_MAGIC && !memcmp(b + 776, uuid, 16));
            if (!matches) continue;
            if (out && readExtendedPlayer(dev, b, uuid, *out) != LOAD_OK) { ioError = true; return -1; }
            return slot;
        }
    }
    return -1;
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
    return fetchPlayer(uuid, out) == LOAD_OK;
}

LoadResult WorldStore::fetchPlayer(const uint8_t uuid[16], PlayerData& out) {
    if (!open_) return LOAD_ERROR;
    bool ioError;
    int slot = findExtendedPlayerSlot(dev_, playerOff_, playerSlots_, uuid, false, &out, ioError);
    if (slot >= 0) cacheSlot(uuid, slot);
    return ioError ? LOAD_ERROR : (slot >= 0 ? LOAD_OK : LOAD_ABSENT);
}

bool WorldStore::savePlayer(const PlayerData& p) {
    if (!open_) return false;
    // a player saved or loaded before keeps its slot: no lookup round trips
    int slot = cachedSlot(p.uuid);
    if (slot < 0) {
        bool ioError;
        slot = findExtendedPlayerSlot(dev_, playerOff_, playerSlots_, p.uuid, true, nullptr, ioError);
        if (slot < 0) {
            if (!ioError) MC_LOGE("storage: player table full");
            return false;
        }
        cacheSlot(p.uuid, slot);
    }
    uint8_t b[PLAYER_SLOT];
    encodePlayer(b, p);
    {
        uint8_t entry[PLAYER_EXTENDED_SLOT];
        uint64_t entryOffset = playerOff_ + (uint64_t)slot * PLAYER_EXTENDED_SLOT;
        if (!dev_->read(entryOffset, entry, sizeof(entry))) return false;
        PlayerRecord records[2] = {playerRecord(entry + 512, dev_->size()), playerRecord(entry + 768, dev_->size())};
        uint32_t newestSequence = 0;
        for (PlayerRecord& saved : records) {
            if (!saved.valid) continue;
            if (memcmp(saved.uuid, p.uuid, 16)) return false;
            if (saved.sequence > newestSequence) newestSequence = saved.sequence;
            ByteBuf previous;
            uint8_t* bytes = previous.append(saved.size);
            if (!bytes || !dev_->read(saved.offset, bytes, saved.size)) return false;
            // A load can recover the older copy after a torn/corrupt payload.
            // Preserve that same good copy during the next save.
            if (crc32(bytes, saved.size) != saved.crc) saved.valid = false;
        }
        int latest = records[1].valid && (!records[0].valid || records[1].sequence > records[0].sequence) ? 1 : 0;
        int target = records[latest].valid ? latest ^ 1 : 0;
        ByteBuf payload;
        payload.put(b, sizeof(b)); Writer w(payload);
        for (const ItemStack& stack : p.inv) {
            if (stack.empty() || !stack.tagSize()) w.u8(0);
            else w.bytes(stack.tagData(), stack.tagSize());
        }
        if (payload.failed() || payload.size() > MAX_PLAYER_BYTES) return false;
        PlayerRecord rec = records[target];
        if (!rec.valid || rec.capacity < payload.size()) {
            rec.capacity = (uint32_t)alignUp(payload.size(), layout_.unit);
            if (!index_.allocateBytes(rec.capacity, rec.offset)) return false;
        }
        rec.sequence = newestSequence + 1;
        rec.size = (uint32_t)payload.size(); rec.crc = crc32(payload.data(), payload.size());
        memcpy(rec.uuid, p.uuid, 16);
        if (!dev_->write(rec.offset, payload.data(), rec.size) || !dev_->flush()) return false;
        uint8_t descriptor[256]; encodePlayerRecord(descriptor, rec);
        if (!dev_->write(entryOffset + 512 + target * 256, descriptor, sizeof(descriptor))) return false;
    }
    playersWritten_++;
    return true;
}

}  // namespace mc
