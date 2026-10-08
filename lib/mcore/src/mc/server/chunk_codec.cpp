// Encoding of the Chunk Data and Update Light packets (pure functions of the chunk).
#include "mc/server/chunk_codec.h"
#include <stdio.h>
#include <string.h>
#include "mc/nbt.h"
#include "mc/registry.h"
#include "mc/server/server.h"

namespace mc {

static void writeHeightmap(Writer& w, const Chunk& c) {
    NbtWriter n(w);
    n.beginRoot();
    n.longArrayHeader("MOTION_BLOCKING", 37);
    // 9 bits per entry, 7 entries per long, entries never span longs
    for (int l = 0; l < 37; l++) {
        uint64_t v = 0;
        for (int k = 0; k < 7; k++) {
            int idx = l * 7 + k;
            if (idx >= 256) break;
            v |= (uint64_t)(c.height(idx & 15, idx >> 4) & 0x1FF) << (k * 9);
        }
        w.u64(v);
    }
    n.end();
}

static void writeSignNbt(Writer& w, const TileEntity& t, int x, int z) {
    NbtWriter n(w);
    n.beginRoot();
    n.str("id", "minecraft:sign");
    n.i32("x", x);
    n.i32("y", t.y);
    n.i32("z", z);
    char key[8], json[200];
    for (int i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "Text%d", i + 1);
        textJson(json, sizeof(json), t.text[i], nullptr);
        n.str(key, json);
    }
    n.str("Color", "black");
    n.end();
}

void writeChunkPacket(Writer& w, const Chunk& c) {
    w.varint(pkt::s2c::MapChunk);
    w.i32(c.cx);
    w.i32(c.cz);
    w.boolean(true);
    int mask = 0;
    size_t dataSize = 0;
    for (int s = 0; s < NUM_SECTIONS; s++) {
        const Section* sec = c.section(s);
        if (sec && sec->nonAirCount() > 0) {
            mask |= 1 << s;
            dataSize += sec->wireSize();
        }
    }
    w.varint(mask);
    writeHeightmap(w, c);
    w.varint(1024);
    const uint8_t* cells = c.biomeCells();
    for (int i = 0; i < 1024; i++) w.varint(cells[i & 15]);
    w.varint((int32_t)dataSize);
    for (int s = 0; s < NUM_SECTIONS; s++)
        if (mask & (1 << s)) c.section(s)->writeWire(w);
    int signs = 0;
    for (TileEntity* t = c.tiles(); t; t = t->next)
        if (t->type == TILE_SIGN) signs++;
    w.varint(signs);
    for (TileEntity* t = c.tiles(); t; t = t->next)
        if (t->type == TILE_SIGN) writeSignNbt(w, *t, c.cx * 16 + t->lx, c.cz * 16 + t->lz);
}

void writeLightPacket(Writer& w, const Chunk& c, const ChunkLight& L, bool full) {
    int n = L.sections();
    int skySections = !L.hasSky() ? 0 : (full ? NUM_SECTIONS : n);   // no sky light in the Nether and the End
    uint32_t skyMask = 0, blockMask = 0, emptyBlock = 0;
    for (int s = 0; s < skySections; s++) skyMask |= 1u << (s + 1);
    for (int s = 0; s < NUM_SECTIONS; s++) {
        if (s < n && !L.blockSectionEmpty(s)) blockMask |= 1u << (s + 1);
        else emptyBlock |= 1u << (s + 1);
    }
    w.varint(pkt::s2c::UpdateLight);
    w.varint(c.cx);
    w.varint(c.cz);
    w.boolean(true);
    w.varint((int32_t)skyMask);
    w.varint((int32_t)blockMask);
    w.varint(0);
    w.varint((int32_t)emptyBlock);
    // fully lit nibbles, written in pieces (no mutable static: this may run on a worker)
    uint8_t full15[256];
    memset(full15, 0xFF, sizeof(full15));
    for (int s = 0; s < skySections; s++) {
        w.varint(2048);
        if (s < n) {
            w.bytes(L.sky(s), 2048);
        } else {
            for (int k = 0; k < 8; k++) w.bytes(full15, sizeof(full15));
        }
    }
    for (int s = 0; s < n; s++) {
        if (!(blockMask & (1u << (s + 1)))) continue;
        w.varint(2048);
        w.bytes(L.block(s), 2048);
    }
}

}  // namespace mc
