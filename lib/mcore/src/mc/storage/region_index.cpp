#include "mc/storage/region_index.h"
#include <stdlib.h>
#include <string.h>
#include "mc/io.h"
#include "mc/platform.h"

namespace mc {

static const uint32_t DIR_MAGIC = 0x44495231;   // "DIR1"
static const uint32_t MAP_MAGIC = 0x4D415031;   // "MAP1"
static const uint32_t DIR_ENTRY = 32;
static const uint32_t MAP_HEADER = 24;
static const uint32_t MAP_BYTES = MAP_HEADER + 1024 * 4;
static const uint32_t MAP_COPY_STRIDE = 8192;    // copy B follows copy A in the map's unit
enum : uint8_t { DIR_REGION = 1, DIR_WATERMARK = 2 };

static inline int32_t regionOf(int32_t c) { return c >> 5; }   // floor division by 32
static inline int mapIndex(int32_t cx, int32_t cz) { return (cz & 31) * 32 + (cx & 31); }

RegionIndex::~RegionIndex() {
    plat::bigFree(dir_);
    plat::bigFree(maps_);
}

// ------------------------------------------------------------------ directory in RAM
static uint32_t dirHash(uint8_t dim, int32_t rx, int32_t rz) {
    uint32_t h = (uint32_t)rx * 73856093u ^ (uint32_t)rz * 19349663u ^ (uint32_t)dim * 83492791u;
    return h ^ (h >> 15);
}

RegionIndex::Dir* RegionIndex::findDir(uint8_t dim, int32_t rx, int32_t rz) {
    if (!dirCap_) return nullptr;
    for (uint32_t i = dirHash(dim, rx, rz) & (dirCap_ - 1);; i = (i + 1) & (dirCap_ - 1)) {
        Dir& d = dir_[i];
        if (!d.unit) return nullptr;
        if (d.rx == rx && d.rz == rz && d.dim == dim) return &d;
    }
}

bool RegionIndex::addDir(uint8_t dim, int32_t rx, int32_t rz, uint32_t unit) {
    if ((dirCount_ + 1) * 2 > dirCap_) {   // keep the table at most half full
        uint32_t cap = dirCap_ ? dirCap_ * 2 : 1024;
        Dir* t = (Dir*)plat::bigAlloc(sizeof(Dir) * cap);
        if (!t) return false;
        memset(t, 0, sizeof(Dir) * cap);
        Dir* old = dir_;
        uint32_t oldCap = dirCap_;
        dir_ = t;
        dirCap_ = cap;
        dirCount_ = 0;
        for (uint32_t i = 0; i < oldCap; i++)
            if (old[i].unit) addDir(old[i].dim, old[i].rx, old[i].rz, old[i].unit - 1);
        plat::bigFree(old);
    }
    Dir* have = findDir(dim, rx, rz);
    if (have) {   // a later entry for the same region wins
        have->unit = unit + 1;
        return true;
    }
    for (uint32_t i = dirHash(dim, rx, rz) & (dirCap_ - 1);; i = (i + 1) & (dirCap_ - 1)) {
        if (dir_[i].unit) continue;
        dir_[i] = Dir{rx, rz, unit + 1, dim};
        dirCount_++;
        stats_.regions = dirCount_;
        return true;
    }
}

// ------------------------------------------------------------------ directory log
bool RegionIndex::appendDir(uint8_t type, uint8_t dim, int32_t rx, int32_t rz, uint64_t value) {
    if (dirLen_ + DIR_ENTRY > layout_.dirCap) {
        stats_.full = true;
        MC_LOGE("storage: the region directory is full");
        return false;
    }
    uint8_t b[DIR_ENTRY];
    BufSink s(b, DIR_ENTRY);
    Writer w(s);
    w.u32(DIR_MAGIC);
    w.u8(type);
    w.u8(dim);
    w.u16(0);
    w.i32(rx);
    w.i32(rz);
    w.u64(value);
    w.u32(++dirSeq_);
    uint32_t c = crc32(b, DIR_ENTRY - 4);
    w.u32(c);
    if (!dev_->write(layout_.dirOff + dirLen_, b, DIR_ENTRY)) return false;
    dirLen_ += DIR_ENTRY;
    stats_.dirEntries++;
    return true;
}

bool RegionIndex::format(BlockDevice* dev, const Layout& layout) {
    dev_ = dev;
    layout_ = layout;
    // an all-zero first entry ends the log
    uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    if (!dev->write(layout.dirOff, zero, sizeof(zero))) return false;
    return open(dev, layout, 0);
}

bool RegionIndex::open(BlockDevice* dev, const Layout& layout, uint64_t allocHint) {
    dev_ = dev;
    layout_ = layout;
    plat::bigFree(dir_);
    dir_ = nullptr;
    dirCap_ = dirCount_ = 0;
    dirLen_ = 0;
    dirSeq_ = 0;
    stats_ = Stats();
    if (!maps_) {
        maps_ = (Map*)plat::bigAlloc(sizeof(Map) * CACHE);
        if (!maps_) return false;
    }
    for (int i = 0; i < CACHE; i++) maps_[i].valid = false;
    mark_ = allocHint;
    // replay the log until the first entry that is not valid (never written, or torn)
    const uint32_t CHUNK = 64 * 1024;
    uint8_t* buf = (uint8_t*)plat::bigAlloc(CHUNK);
    if (!buf) return false;
    bool done = false;
    for (uint64_t pos = 0; pos < layout.dirCap && !done; pos += CHUNK) {
        uint32_t n = layout.dirCap - pos < CHUNK ? (uint32_t)(layout.dirCap - pos) : CHUNK;
        if (!dev->read(layout.dirOff + pos, buf, n)) {
            plat::bigFree(buf);
            return false;
        }
        for (uint32_t o = 0; o + DIR_ENTRY <= n; o += DIR_ENTRY) {
            const uint8_t* e = buf + o;
            Reader r(e, DIR_ENTRY);
            uint32_t magic = r.u32();
            uint8_t type = r.u8(), dim = r.u8();
            r.u16();
            int32_t rx = r.i32(), rz = r.i32();
            uint64_t value = r.u64();
            uint32_t seq = r.u32(), crc = r.u32();
            if (magic != DIR_MAGIC || crc != crc32(e, DIR_ENTRY - 4)) {
                done = true;
                break;
            }
            if (type == DIR_REGION) {
                if (!addDir(dim, rx, rz, (uint32_t)value)) {
                    plat::bigFree(buf);
                    return false;
                }
                if (value + 1 > mark_) mark_ = value + 1;   // a map is in use: never reuse it
            } else if (type == DIR_WATERMARK && value > mark_) {
                mark_ = value;
            }
            dirSeq_ = seq;
            dirLen_ = pos + o + DIR_ENTRY;
            stats_.dirEntries++;
        }
    }
    plat::bigFree(buf);
    // whatever was allocated before but not yet recorded lies below the watermark:
    // continue above it (a little space may be skipped, never reused)
    next_ = mark_;
    stats_.unitsUsed = next_;
    return true;
}

// ------------------------------------------------------------------ allocation
bool RegionIndex::alloc(uint32_t& unit) {
    if (layout_.dataOff + (next_ + 1) * layout_.unit > dev_->size()) {
        if (!stats_.full) MC_LOGE("storage: the export is full (%llu MiB); grow it to save more chunks",
                                  (unsigned long long)(dev_->size() >> 20));
        stats_.full = true;
        return false;
    }
    if (next_ >= mark_) {
        // record the new watermark, durably, before any unit above the old one is used
        uint64_t m = next_ + MARK_STEP;
        if (!appendDir(DIR_WATERMARK, 0, 0, 0, m) || !dev_->flush()) return false;
        mark_ = m;
    }
    unit = (uint32_t)next_++;
    stats_.unitsUsed = next_;
    return true;
}

// ------------------------------------------------------------------ slot maps
void RegionIndex::encodeMap(uint8_t* b, const Map& m, uint32_t seq) const {
    BufSink s(b, MAP_BYTES);
    Writer w(s);
    w.u32(MAP_MAGIC);
    w.u32(seq);
    w.u32(m.dim);
    w.i32(m.rx);
    w.i32(m.rz);
    w.u32(0);   // CRC, filled in below
    for (int i = 0; i < 1024; i++) w.u32(m.entries[i]);
    uint32_t c = crc32(b, 20);
    c = crc32(b + MAP_HEADER, MAP_BYTES - MAP_HEADER, c);
    b[20] = (uint8_t)(c >> 24);
    b[21] = (uint8_t)(c >> 16);
    b[22] = (uint8_t)(c >> 8);
    b[23] = (uint8_t)c;
}

// one copy: valid if its magic, region and CRC match (m's key is set by the caller)
bool RegionIndex::decodeMap(const uint8_t* b, Map& m) const {
    Reader r(b, MAP_HEADER);
    if (r.u32() != MAP_MAGIC) return false;
    uint32_t seq = r.u32();
    uint32_t dim = r.u32();
    int32_t rx = r.i32(), rz = r.i32();
    uint32_t stored = r.u32();
    if (dim != m.dim || rx != m.rx || rz != m.rz) return false;
    uint32_t c = crc32(b, 20);
    c = crc32(b + MAP_HEADER, MAP_BYTES - MAP_HEADER, c);
    if (c != stored) return false;
    Reader e(b + MAP_HEADER, MAP_BYTES - MAP_HEADER);
    for (int i = 0; i < 1024; i++) m.entries[i] = e.u32();
    m.seq = seq;
    return true;
}

RegionIndex::Map* RegionIndex::cached(uint8_t dim, int32_t rx, int32_t rz) {
    for (int i = 0; i < CACHE; i++) {
        Map& m = maps_[i];
        if (m.valid && m.rx == rx && m.rz == rz && m.dim == dim) {
            m.lastUse = ++clock_;
            return &m;
        }
    }
    return nullptr;
}

RegionIndex::Map* RegionIndex::slotFor(uint8_t dim, int32_t rx, int32_t rz) {
    Map* best = nullptr;
    for (int i = 0; i < CACHE; i++) {
        Map& m = maps_[i];
        if (m.valid && (m.fresh || m.dirty)) continue;   // not written yet: keep it
        if (!m.valid) { best = &m; break; }
        if (!best || m.lastUse < best->lastUse) best = &m;
    }
    if (!best) return nullptr;
    best->valid = false;
    best->fresh = false;
    best->dirty = false;
    best->dim = dim;
    best->rx = rx;
    best->rz = rz;
    best->lastUse = ++clock_;
    return best;
}

bool RegionIndex::prefetch(int n, const uint8_t* dims, const int32_t* cx, const int32_t* cz) {
    const int MAXR = 8;   // distinct regions per round trip (a batch of chunks spans few)
    struct Want { uint8_t dim; int32_t rx, rz; uint32_t unit; uint8_t* buf; };
    Want want[MAXR];
    int nw = 0;
    for (int i = 0; i < n; i++) {
        uint8_t dim = dims ? dims[i] : 0;
        int32_t rx = regionOf(cx[i]), rz = regionOf(cz[i]);
        if (cached(dim, rx, rz)) { stats_.mapHits++; continue; }
        Dir* d = findDir(dim, rx, rz);
        if (!d) continue;   // never saved: nothing to read
        bool dup = false;
        for (int k = 0; k < nw && !dup; k++) dup = want[k].dim == dim && want[k].rx == rx && want[k].rz == rz;
        if (dup || nw == MAXR) continue;
        want[nw++] = Want{dim, rx, rz, d->unit - 1, nullptr};
    }
    if (!nw) return true;
    uint8_t* buf = (uint8_t*)plat::bigAlloc((size_t)nw * 2 * MAP_BYTES);
    if (!buf) return false;
    ReadOp ops[2 * MAXR];
    for (int k = 0; k < nw; k++) {
        uint64_t base = layout_.dataOff + (uint64_t)want[k].unit * layout_.unit;
        want[k].buf = buf + (size_t)k * 2 * MAP_BYTES;
        ops[2 * k] = {base, want[k].buf, MAP_BYTES};
        ops[2 * k + 1] = {base + MAP_COPY_STRIDE, want[k].buf + MAP_BYTES, MAP_BYTES};
    }
    if (!dev_->readMany(ops, 2 * nw)) {
        plat::bigFree(buf);
        return false;
    }
    for (int k = 0; k < nw; k++) {
        stats_.mapMisses++;
        Map* m = slotFor(want[k].dim, want[k].rx, want[k].rz);
        if (!m) continue;
        m->unit = want[k].unit;
        // the newest valid copy; none valid (never written, or both torn): empty
        Map other = *m;
        bool a = decodeMap(want[k].buf, *m);
        uint32_t seqA = m->seq;
        bool b = decodeMap(want[k].buf + MAP_BYTES, other);
        if (a && (!b || seqA >= other.seq)) {
            m->copy = 0;
        } else if (b) {
            memcpy(m->entries, other.entries, sizeof(m->entries));
            m->seq = other.seq;
            m->copy = 1;
        } else {
            memset(m->entries, 0, sizeof(m->entries));
            m->seq = 0;
            m->copy = 1;   // the next write goes to copy A
            MC_LOGW("storage: region %d,%d (dim %u) has no valid slot map", (int)want[k].rx, (int)want[k].rz,
                    (unsigned)want[k].dim);
        }
        m->valid = true;
    }
    plat::bigFree(buf);
    return true;
}

bool RegionIndex::lookup(uint8_t dim, int32_t cx, int32_t cz, uint64_t& off) {
    off = 0;
    int32_t rx = regionOf(cx), rz = regionOf(cz);
    Map* m = cached(dim, rx, rz);
    if (!m) {
        if (!findDir(dim, rx, rz)) return true;   // never saved
        if (!prefetch(1, &dim, &cx, &cz)) return false;
        m = cached(dim, rx, rz);
        if (!m) return false;
    }
    uint32_t e = m->entries[mapIndex(cx, cz)];
    if (e) off = layout_.dataOff + (uint64_t)(e - 1) * layout_.unit;
    return true;
}

bool RegionIndex::unitForWrite(uint8_t dim, int32_t cx, int32_t cz, uint64_t& off, bool& isNew) {
    isNew = false;
    if (!lookup(dim, cx, cz, off)) return false;
    int32_t rx = regionOf(cx), rz = regionOf(cz);
    Map* m = cached(dim, rx, rz);
    if (off) {
        isNew = m && (m->dirty || m->fresh);   // its map (or directory entry) still has to be written
        return true;
    }
    if (!m) {
        // a region without a map yet: allocate one (written by commit())
        uint32_t unit;
        if (!alloc(unit)) return false;
        m = slotFor(dim, rx, rz);
        if (!m) return false;
        m->unit = unit;
        m->seq = 0;
        m->copy = 1;
        memset(m->entries, 0, sizeof(m->entries));
        m->valid = true;
        m->fresh = true;
    }
    uint32_t unit;
    if (!alloc(unit)) return false;
    m->entries[mapIndex(cx, cz)] = unit + 1;
    m->dirty = true;
    off = layout_.dataOff + (uint64_t)unit * layout_.unit;
    isNew = true;
    return true;
}

bool RegionIndex::commit(uint8_t dim, int32_t cx, int32_t cz) {
    int32_t rx = regionOf(cx), rz = regionOf(cz);
    Map* m = cached(dim, rx, rz);
    if (!m) return false;
    uint8_t* b = (uint8_t*)plat::bigAlloc(MAP_BYTES);
    if (!b) return false;
    uint32_t seq = m->seq + 1;
    encodeMap(b, *m, seq);
    int copy = m->copy == 0 ? 1 : 0;   // the older copy: a torn write keeps the newest
    bool ok = dev_->write(layout_.dataOff + (uint64_t)m->unit * layout_.unit + (uint64_t)copy * MAP_COPY_STRIDE, b,
                          MAP_BYTES);
    plat::bigFree(b);
    if (!ok) return false;
    m->seq = seq;
    m->copy = (uint8_t)copy;
    m->dirty = false;
    if (m->fresh) {
        // the map exists now: make it findable
        if (!appendDir(DIR_REGION, dim, rx, rz, m->unit) || !addDir(dim, rx, rz, m->unit)) return false;
        m->fresh = false;
    }
    return true;
}

}  // namespace mc
