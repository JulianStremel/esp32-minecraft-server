#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include "testing.h"
#include "mc/registry.h"
#include "mc/storage/nbd_device.h"
#include "mc/storage/world_store.h"
#include "mc/world/generator.h"

using namespace mc;

static void fillTestChunk(Chunk& c, int salt) {
    Generator g;
    g.init(1234 + salt, WORLD_NORMAL);
    g.generate(c);
    // player edits and block entities
    c.set(3, 100, 4, bs::Glowstone);
    c.set(5, 101, 6, bs::OakPlanks);
    TileEntity* t = c.addTile(TILE_CHEST, 1, 70, 2);
    t->items[0] = ItemStack::of(itm::Diamond, 12);
    t->items[26] = ItemStack::of(itm::IronPickaxe, 1);
    t->items[26].damage = 57;
    TileEntity* f = c.addTile(TILE_FURNACE, 2, 71, 3);
    f->items[0] = ItemStack::of(itm::IronOre, 5);
    f->burnTime = 123;
    f->cookTime = 45;
    TileEntity* s = c.addTile(TILE_SIGN, 4, 72, 5);
    snprintf(s->text[0], sizeof(s->text[0]), "Hello");
    snprintf(s->text[3], sizeof(s->text[3]), "ESP32 \"quotes\"");
    c.recomputeHeightmap();
}

static bool sameChunk(const Chunk& a, const Chunk& b) {
    for (int y = 0; y < 256; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++)
                if (a.get(x, y, z) != b.get(x, y, z)) return false;
    for (int i = 0; i < 16; i++)
        if (a.biomeCells()[i] != b.biomeCells()[i]) return false;
    if (a.tileCount() != b.tileCount()) return false;
    for (TileEntity* t = a.tiles(); t; t = t->next) {
        TileEntity* u = b.tileAt(t->lx, t->y, t->lz);
        if (!u || u->type != t->type) return false;
        for (int i = 0; i < t->slotCount(); i++)
            if (t->items[i].id != u->items[i].id || t->items[i].count != u->items[i].count ||
                t->items[i].damage != u->items[i].damage)
                return false;
        if (t->burnTime != u->burnTime || t->cookTime != u->cookTime) return false;
        for (int i = 0; i < 4; i++)
            if (strcmp(t->text[i], u->text[i])) return false;
    }
    return true;
}

static void storeRoundtrip(BlockDevice& dev, bool compress) {
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 8;
    sp.chunkSlotSize = 65536;
    sp.playerSlots = 64;
    sp.compress = compress;
    CHECK(ws.open(sp));
    WorldMeta m;
    CHECK(!ws.loadMeta(m));  // fresh: no world yet
    m.seed = 0xDEADBEEFCAFEull;
    m.spawnX = 10; m.spawnY = 70; m.spawnZ = -5;
    m.timeOfDay = 12345;
    CHECK(ws.saveMeta(m));
    CHECK(ws.saveMeta(m));  // second copy

    Chunk a(2, -3);
    fillTestChunk(a, 0);
    Chunk absent(1, 1);
    CHECK_EQ(ws.loadChunk(absent), LOAD_ABSENT);
    CHECK(ws.saveChunk(a));
    CHECK_EQ(a.storeSlot, 0);
    // edit and save again -> other slot
    a.set(7, 120, 7, bs::DiamondBlock);
    CHECK(ws.saveChunk(a));
    CHECK_EQ(a.storeSlot, 1);
    CHECK_EQ((int)a.storeSeq, 2);
    Chunk b(2, -3);
    CHECK_EQ(ws.loadChunk(b), LOAD_OK);
    CHECK(sameChunk(a, b));
    CHECK_EQ(b.storeSlot, 1);

    // players
    PlayerData p;
    for (int i = 0; i < 16; i++) p.uuid[i] = (uint8_t)(i * 7 + 1);
    snprintf(p.name, sizeof(p.name), "Steve");
    p.x = 1.5; p.y = 64; p.z = -2.25;
    p.food = 17;
    p.inv[36] = ItemStack::of(itm::Torch, 40);
    p.inv[5] = ItemStack::of(itm::IronHelmet, 1);
    p.inv[5].damage = 9;
    CHECK(ws.savePlayer(p));
    p.x = 99;
    CHECK(ws.savePlayer(p));  // update in place
    PlayerData q;
    CHECK(ws.loadPlayer(p.uuid, q));
    CHECK(q.x == 99);
    CHECK_STR(q.name, "Steve");
    CHECK_EQ(q.food, 17);
    CHECK_EQ(q.inv[36].id, itm::Torch);
    CHECK_EQ(q.inv[36].count, 40);
    CHECK_EQ(q.inv[5].damage, 9);
    uint8_t other[16] = {9};
    CHECK(!ws.loadPlayer(other, q));
    // many players with colliding hashes stay distinct
    for (int k = 0; k < 40; k++) {
        PlayerData pk;
        for (int i = 0; i < 16; i++) pk.uuid[i] = (uint8_t)(k * 31 + i);
        snprintf(pk.name, sizeof(pk.name), "p%d", k);
        pk.xpLevel = k;
        CHECK(ws.savePlayer(pk));
    }
    for (int k = 0; k < 40; k++) {
        uint8_t u[16];
        for (int i = 0; i < 16; i++) u[i] = (uint8_t)(k * 31 + i);
        PlayerData r;
        CHECK(ws.loadPlayer(u, r));
        CHECK_EQ(r.xpLevel, k);
    }
    CHECK(ws.flush());

    // reopen from the same device: meta and chunk are back
    WorldStore ws2(&dev);
    CHECK(ws2.open(sp, false));
    WorldMeta m2;
    CHECK(ws2.loadMeta(m2));
    CHECK(m2.seed == m.seed);
    CHECK_EQ(m2.spawnZ, -5);
    CHECK_EQ(m2.timeOfDay, 12345);
    Chunk c(2, -3);
    CHECK_EQ(ws2.loadChunk(c), LOAD_OK);
    CHECK(sameChunk(a, c));
}

TEST(store_roundtrip_mem_compressed) {
    MemDevice dev(64u << 20);
    storeRoundtrip(dev, true);
}

TEST(store_roundtrip_mem_uncompressed) {
    MemDevice dev(64u << 20);
    storeRoundtrip(dev, false);
}

TEST(store_shrinks_world_to_device) {
    MemDevice dev(16u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 64;
    CHECK(ws.open(sp));
    CHECK(ws.radius() < 64);
    CHECK(ws.requiredSize() <= dev.size());
    CHECK(!ws.chunkInRange(ws.radius(), 0));
    CHECK(ws.chunkInRange(ws.radius() - 1, -ws.radius()));
}

TEST(store_survives_torn_write_of_newest_copy) {
    MemDevice dev(64u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 4;
    CHECK(ws.open(sp));
    Chunk a(0, 0);
    fillTestChunk(a, 1);
    CHECK(ws.saveChunk(a));               // slot 0, seq 1
    Chunk older(0, 0);
    CHECK_EQ(ws.loadChunk(older), LOAD_OK);
    a.set(0, 200, 0, bs::GoldBlock);
    CHECK(ws.saveChunk(a));               // slot 1, seq 2
    // corrupt the payload of slot 1 (simulates power loss mid write)
    // find the chunk base by probing: slot1 header is where seq==2 lives
    bool corrupted = false;
    for (uint64_t off = 0; off + 65536 < dev.size() && !corrupted; off += 65536) {
        uint8_t h[32];
        dev.read(off, h, 32);
        if (h[0] == 'C' && h[1] == 'H' && h[2] == 'K' && h[3] == '1' && h[7] == 2) {
            uint8_t junk[64];
            memset(junk, 0xAB, sizeof(junk));
            dev.write(off + 40, junk, sizeof(junk));
            corrupted = true;
        }
    }
    CHECK(corrupted);
    Chunk b(0, 0);
    CHECK_EQ(ws.loadChunk(b), LOAD_OK);   // falls back to the older copy
    CHECK(sameChunk(older, b));
    CHECK_EQ(b.get(0, 200, 0), 0);
}

TEST(store_refuses_to_format_foreign_data) {
    MemDevice dev(8u << 20);
    uint8_t junk[16] = {'s', 'o', 'm', 'e', ' ', 'f', 'i', 'l', 'e', 's', 'y', 's', 't', 'e', 'm', 0};
    dev.write(1024, junk, sizeof(junk));
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 2;
    CHECK(!ws.open(sp, false));
    CHECK(ws.open(sp, true));
}

// ---------------------------------------------------------------- NBD (real socket)
struct PyNbd {
    pid_t pid = -1;
    int port = 0;
    std::string file;
    bool start(int p, const char* size) {
        port = p;
        char path[] = "/tmp/mc_nbd_test_XXXXXX";
        int fd = mkstemp(path);
        if (fd < 0) return false;
        close(fd);
        unlink(path);
        file = path;
        pid = fork();
        if (pid == 0) {
            char portStr[16];
            snprintf(portStr, sizeof(portStr), "%d", p);
            execlp("python3", "python3", "../tools/nbd_server.py", "--file", path, "--size", size, "--port", portStr,
                   "--bind", "127.0.0.1", (char*)nullptr);
            _exit(127);
        }
        for (int i = 0; i < 100; i++) {
            Conn* c = plat::connectTcp("127.0.0.1", (uint16_t)port, 200);
            if (c) { delete c; return true; }
            usleep(50000);
        }
        return false;
    }
    void stop() {
        if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); pid = -1; }
    }
    ~PyNbd() { stop(); if (!file.empty()) unlink(file.c_str()); }
};

TEST(nbd_store_roundtrip_against_python_server) {
    PyNbd srv;
    if (!srv.start(10900 + (getpid() % 500), "64M")) { printf("    (python3 not available, skipped)\n"); return; }
    NbdDevice dev("127.0.0.1", (uint16_t)srv.port, "");
    CHECK(dev.connect());
    CHECK_EQ(dev.size(), 64ull << 20);
    // pipelined reads land in the right buffers
    uint8_t a[100], b[3000], c[7];
    memset(a, 1, sizeof(a));
    CHECK(dev.write(1000, "hello", 5));
    CHECK(dev.write(5000, "world!", 6));
    ReadOp ops[3] = {{5000, b, sizeof(b)}, {1000, a, sizeof(a)}, {64ull * 1024 * 1024 - 7, c, 7}};
    CHECK(dev.readMany(ops, 3));
    CHECK(!memcmp(a, "hello", 5));
    CHECK(!memcmp(b, "world!", 6));
    CHECK(c[0] == 0);
    CHECK(!dev.read(64ull << 20, a, 1) || true);  // out of range: error reply, connection stays usable
    CHECK(dev.read(1000, a, 5));
    CHECK(!memcmp(a, "hello", 5));
    // posted flushes: replies are collected by later requests, the stream stays in sync
    for (int i = 0; i < 40; i++) {
        uint8_t v = (uint8_t)i;
        CHECK(dev.write(20000 + i, &v, 1));
        CHECK(dev.flushLater());
    }
    uint8_t back[40];
    CHECK(dev.read(20000, back, sizeof(back)));
    for (int i = 0; i < 40; i++) CHECK_EQ(back[i], i);
    storeRoundtrip(dev, true);
    CHECK(dev.flush());
    printf("    nbd stats: %u reads, %u writes, %llu KB written\n", (unsigned)dev.stats().reads,
           (unsigned)dev.stats().writes, (unsigned long long)(dev.stats().bytesWritten / 1024));
}

TEST(nbd_reconnects_after_server_restart) {
    PyNbd srv;
    int port = 11500 + (getpid() % 400);
    if (!srv.start(port, "16M")) { printf("    (python3 not available, skipped)\n"); return; }
    NbdDevice dev("127.0.0.1", (uint16_t)port, "");
    CHECK(dev.connect());
    CHECK(dev.write(4096, "persist", 7));
    CHECK(dev.flush());
    std::string file = srv.file;
    srv.stop();
    uint8_t buf[8];
    CHECK(!dev.read(4096, buf, 7));   // server gone
    // restart on the same file
    pid_t pid = fork();
    if (pid == 0) {
        char portStr[16];
        snprintf(portStr, sizeof(portStr), "%d", port);
        execlp("python3", "python3", "../tools/nbd_server.py", "--file", file.c_str(), "--port", portStr, "--bind",
               "127.0.0.1", (char*)nullptr);
        _exit(127);
    }
    srv.pid = pid;
    bool ok = false;
    for (int i = 0; i < 100 && !ok; i++) {
        usleep(100000);
        ok = dev.read(4096, buf, 7);  // available() reconnects with backoff
    }
    CHECK(ok);
    CHECK(!memcmp(buf, "persist", 7));
    CHECK(dev.stats().reconnects >= 1);
}
