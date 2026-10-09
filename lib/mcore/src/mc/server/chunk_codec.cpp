// Encoding of the Chunk Data and Update Light packets (pure functions of the chunk).
#include "mc/server/chunk_codec.h"
#include <stdio.h>
#include <string.h>
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static constexpr int HEIGHTMAP_MOTION_BLOCKING = 4;

static void writeHeightmaps(Writer& w, const Chunk& c) {
    w.varint(1);
    w.varint(HEIGHTMAP_MOTION_BLOCKING);
    // 9 bits per entry (heights 0..256), 7 entries per long, entries never span longs
    w.varint(37);
    for (int l = 0; l < 37; l++) {
        uint64_t v = 0;
        for (int k = 0; k < 7; k++) {
            int idx = l * 7 + k;
            if (idx >= 256) break;
            v |= (uint64_t)(c.height(idx & 15, idx >> 4) & 0x1FF) << (k * 9);
        }
        w.u64(v);
    }
}

// The biomes of one section: 4x4x4 cells, here the same for every layer (the chunk keeps
// a 4x4 grid). A paletted container: one biome is bits 0 and the value; otherwise a
// palette of up to 8 biomes (1..3 bits), else the direct 7-bit ids.
struct BiomeContainer {
    uint8_t palette[16];
    uint8_t index[16];   // palette index of each 4x4 cell
    int count = 0;
    int bits = 0;
    explicit BiomeContainer(const uint8_t* cells) {
        for (int i = 0; i < 16; i++) {
            int k = 0;
            while (k < count && palette[k] != cells[i]) k++;
            if (k == count) palette[count++] = cells[i];
            index[i] = (uint8_t)k;
        }
        bits = count == 1 ? 0 : count <= 2 ? 1 : count <= 4 ? 2 : count <= 8 ? 3 : 7;
        if (bits == 7)
            for (int i = 0; i < 16; i++) index[i] = cells[i];
    }
    int longs() const { return bits ? (64 + 64 / bits - 1) / (64 / bits) : 0; }
    size_t size() const {
        if (!bits) return 1 + varintSize(palette[0]);
        size_t n = 1 + (size_t)longs() * 8;
        if (bits <= 3) {
            n += varintSize(count);
            for (int i = 0; i < count; i++) n += varintSize(palette[i]);
        }
        return n;
    }
    void write(Writer& w) const {
        w.u8((uint8_t)bits);
        if (!bits) { w.varint(palette[0]); return; }
        if (bits <= 3) {
            w.varint(count);
            for (int i = 0; i < count; i++) w.varint(palette[i]);
        }
        int per = 64 / bits;
        for (int l = 0; l < longs(); l++) {
            uint64_t v = 0;
            for (int k = 0; k < per; k++) {
                int e = l * per + k;   // (y * 4 + z) * 4 + x
                if (e >= 64) break;
                v |= (uint64_t)index[e & 15] << (k * bits);
            }
            w.u64(v);
        }
    }
};

static void nbtKey(Writer& w, uint8_t type, const char* k) {
    w.u8(type);
    w.u16((uint16_t)strlen(k));
    w.bytes((const uint8_t*)k, strlen(k));
}

static void signText(Writer& w, const char* key, const TileEntity* t) {
    NbtWriter n(w);
    n.beginCompound(key);
    n.listHeader("messages", NBT_STRING, 4);
    for (int i = 0; i < 4; i++) {
        const char* line = t ? t->text[i] : "";
        w.u16((uint16_t)strlen(line));
        w.bytes((const uint8_t*)line, strlen(line));
    }
    n.str("color", "black");
    n.i8("has_glowing_text", 0);
    n.end();
}

// Block entity NBT in a chunk (no id and position: they come with the entry).
void writeSignNbt(Writer& w, const TileEntity& t) {
    w.u8(NBT_COMPOUND);
    signText(w, "front_text", &t);
    signText(w, "back_text", nullptr);
    nbtKey(w, NBT_BYTE, "is_waxed");
    w.u8(0);
    w.u8(NBT_END);
}

static void writePistonNbt(Writer& w, const TileEntity& t) {
    NbtWriter n(w);
    w.u8(NBT_COMPOUND);
    n.i32("facing", t.pistonFace); n.f32("progress", t.pistonPrevious * .5f);
    n.i8("extending", t.pistonExtending); n.i8("source", t.pistonSource);
    n.beginCompound("blockState");
    const BlockDef& b = blockOf(t.movedState);
    char name[100]; snprintf(name, sizeof(name), "minecraft:%s", b.name); n.str("Name", name);
    if (b.propCount) {
        n.beginCompound("Properties");
        for (int i = 0; i < b.propCount; ++i) {
            const PropDef& p = PROPS[BLOCK_PROPS[b.propStart + i]];
            int v = propValue(t.movedState, i);
            char number[12];
            snprintf(number, sizeof(number), "%d", v);
            n.str(p.name, p.type == 0 ? (v == 0 ? "true" : "false") : p.values ? p.values[v] : number);
        }
        n.end();
    }
    n.end(); n.end();
}

// Light data as in Chunk Data and Update Light: four bit sets (sections -1..16 as bits
// 0..17), then the sky and block light arrays.
static void writeLightData(Writer& w, const ChunkLight& L, bool full) {
    int n = L.sections();
    int skySections = !L.hasSky() ? 0 : (full ? NUM_SECTIONS : n);   // no sky light in the Nether and the End
    uint64_t skyMask = 0, blockMask = 0, emptyBlock = 0;
    for (int s = 0; s < skySections; s++) skyMask |= 1ull << (s + 1);
    for (int s = 0; s < NUM_SECTIONS; s++) {
        if (s < n && !L.blockSectionEmpty(s)) blockMask |= 1ull << (s + 1);
        else emptyBlock |= 1ull << (s + 1);
    }
    auto bits = [&](uint64_t m) {
        if (!m) { w.varint(0); return; }
        w.varint(1);
        w.u64(m);
    };
    bits(skyMask);
    bits(blockMask);
    bits(0);
    bits(emptyBlock);
    // fully lit nibbles, written in pieces (no mutable static: this may run on a worker)
    uint8_t full15[256];
    memset(full15, 0xFF, sizeof(full15));
    w.varint(skySections);
    for (int s = 0; s < skySections; s++) {
        w.varint(2048);
        if (s < n) {
            w.bytes(L.sky(s), 2048);
        } else {
            for (int k = 0; k < 8; k++) w.bytes(full15, sizeof(full15));
        }
    }
    int blocks = 0;
    for (int s = 0; s < n; s++) blocks += (blockMask >> (s + 1)) & 1;
    w.varint(blocks);
    for (int s = 0; s < n; s++) {
        if (!(blockMask & (1ull << (s + 1)))) continue;
        w.varint(2048);
        w.bytes(L.block(s), 2048);
    }
}

void writeChunkPacket(Writer& w, const Chunk& c, const ChunkLight& L) {
    w.varint(pkt::s2c::MapChunk);
    w.i32(c.cx);
    w.i32(c.cz);
    writeHeightmaps(w, c);
    BiomeContainer biomes(c.biomeCells());
    size_t dataSize = 0;
    for (int s = 0; s < NUM_SECTIONS; s++) {
        const Section* sec = c.section(s);
        dataSize += (sec ? sec->wireSize() : 2 + 1 + 1) + biomes.size();
    }
    w.varint((int32_t)dataSize);
    for (int s = 0; s < NUM_SECTIONS; s++) {
        const Section* sec = c.section(s);
        if (sec) sec->writeWire(w);
        else {   // empty: no blocks, all air
            w.i16(0);
            w.u8(0);
            w.varint(0);
        }
        biomes.write(w);
    }
    int tiles = 0;
    for (TileEntity* t = c.tiles(); t; t = t->next)
        if (t->type == TILE_SIGN || t->type == TILE_PISTON) tiles++;
    w.varint(tiles);
    for (TileEntity* t = c.tiles(); t; t = t->next) {
        if (t->type != TILE_SIGN && t->type != TILE_PISTON) continue;
        w.u8((uint8_t)((t->lx << 4) | t->lz));
        w.i16((int16_t)t->y);
        w.varint(t->type == TILE_SIGN ? bet::Sign : bet::Piston);
        if (t->type == TILE_SIGN) writeSignNbt(w, *t);
        else writePistonNbt(w, *t);
    }
    writeLightData(w, L, false);
}

void writeLightPacket(Writer& w, const Chunk& c, const ChunkLight& L, bool full) {
    w.varint(pkt::s2c::UpdateLight);
    w.varint(c.cx);
    w.varint(c.cz);
    writeLightData(w, L, full);
}

}  // namespace mc
