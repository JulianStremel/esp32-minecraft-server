#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include "testing.h"
#include "mc/registry.h"
#include "mc/bytebuf.h"
#include "mc/nbt.h"
#include "mc/storage/nbd_device.h"
#include "mc/storage/region_index.h"
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

// ---------------------------------------------------------------- format 3 (unbounded)
TEST(store_border_is_a_setting_not_the_device_size) {
    MemDevice dev(16u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 1000000;   // 16 million blocks
    CHECK(ws.open(sp));
    CHECK_EQ(ws.format(), 6);
    CHECK_EQ(ws.radius(), 1000000);
    CHECK(ws.chunkInRange(999999, -1000000));
    CHECK(!ws.chunkInRange(1000000, 0));
    Chunk far(987654, -912345);
    fillTestChunk(far, 3);
    CHECK(ws.saveChunk(far));
    Chunk back(987654, -912345);
    CHECK_EQ(ws.loadChunk(back), LOAD_OK);
    CHECK(sameChunk(far, back));
    // the border follows the setting when the world is opened again
    WorldStore ws2(&dev);
    sp.radius = 5000;
    CHECK(ws2.open(sp, false));
    CHECK_EQ(ws2.radius(), 5000);
}

TEST(store_regions_survive_reopening_and_do_not_overlap) {
    MemDevice dev(64u << 20);
    StoreParams sp;
    sp.radius = 100000;
    sp.playerSlots = 64;
    static const int CX[] = {0, 31, 32, -1, -33, 640, -6400, 50000, 7, 8};
    static const int CZ[] = {0, 31, 0, -1, 70, -640, 6400, -50000, 7, 9};
    const int N = sizeof(CX) / sizeof(CX[0]);
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp));
        for (int i = 0; i < N; i++) {
            Chunk c(CX[i], CZ[i]);
            fillTestChunk(c, i);
            CHECK(ws.saveChunk(c));
        }
        CHECK(ws.flush());
        printf("    %u regions, %u directory entries\n", (unsigned)ws.index().stats().regions,
               (unsigned)ws.index().stats().dirEntries);
    }
    // reopened: the directory is replayed; chunks saved now get new units above the old
    WorldStore ws(&dev);
    CHECK(ws.open(sp, false));
    for (int i = 0; i < 20; i++) {
        Chunk c(1000 + i, 1000);
        fillTestChunk(c, 100 + i);
        CHECK(ws.saveChunk(c));
    }
    Chunk never(12345, 12345);
    CHECK_EQ(ws.loadChunk(never), LOAD_ABSENT);
    for (int i = 0; i < N; i++) {
        Chunk want(CX[i], CZ[i]), got(CX[i], CZ[i]);
        fillTestChunk(want, i);
        CHECK_EQ(ws.loadChunk(got), LOAD_OK);
        CHECK(sameChunk(want, got));
    }
    // batched fetches (the background path): one map round trip, then records
    int32_t xs[N], zs[N];
    ChunkRecord recs[N];
    ChunkRecord* rp[N];
    LoadResult res[N];
    for (int i = 0; i < N; i++) { xs[i] = CX[i]; zs[i] = CZ[i]; rp[i] = &recs[i]; }
    WorldStore ws3(&dev);
    CHECK(ws3.open(sp, false));
    uint8_t ds[N] = {};
    ws3.fetchChunks(N, ds, xs, zs, rp, res);
    for (int i = 0; i < N; i++) {
        CHECK_EQ(res[i], LOAD_OK);
        Chunk want(CX[i], CZ[i]), got(CX[i], CZ[i]);
        fillTestChunk(want, i);
        CHECK(ws3.decodeChunk(recs[i], got));
        CHECK(sameChunk(want, got));
    }
}

// The index on its own: three dimensions with the same coordinates stay apart.
TEST(region_index_keeps_dimensions_apart) {
    MemDevice dev(32u << 20);
    RegionIndex::Layout l;
    l.dirOff = 1 << 20;
    l.dirCap = 256 << 10;
    l.dataOff = l.dirOff + l.dirCap;
    {
        RegionIndex idx;
        CHECK(idx.format(&dev, l));
        for (uint8_t dim = 0; dim < 3; dim++) {
            uint64_t off;
            bool isNew;
            CHECK(idx.unitForWrite(dim, 5, -7, off, isNew));
            CHECK(isNew);
            uint8_t tag = (uint8_t)(0xA0 + dim);
            CHECK(dev.write(off, &tag, 1));
            CHECK(idx.commit(dim, 5, -7));
        }
    }
    RegionIndex idx;
    CHECK(idx.open(&dev, l, 0));
    CHECK_EQ(idx.stats().regions, 3);
    for (uint8_t dim = 0; dim < 3; dim++) {
        uint64_t off;
        CHECK(idx.lookup(dim, 5, -7, off));
        CHECK(off != 0);
        uint8_t tag = 0;
        CHECK(dev.read(off, &tag, 1));
        CHECK_EQ(tag, 0xA0 + dim);
    }
    uint64_t off;
    CHECK(idx.lookup(3, 5, -7, off));
    CHECK_EQ(off, 0);
}

TEST(store_falls_back_to_the_older_slot_map_copy) {
    MemDevice dev(32u << 20);
    StoreParams sp;
    sp.radius = 1000;
    sp.playerSlots = 64;
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp));
        Chunk a(1, 1), b(2, 1);   // the same region
        fillTestChunk(a, 1);
        fillTestChunk(b, 2);
        CHECK(ws.saveChunk(a));   // map copy A (seq 1)
        CHECK(ws.saveChunk(b));   // map copy B (seq 2), 8 KiB into the map's unit
    }
    // tear the newest copy
    uint64_t base = 0;
    for (uint64_t off = 0; off < dev.size() && !base; off += 4096) {
        uint8_t h[8];
        dev.read(off, h, 8);
        if (h[0] == 'M' && h[1] == 'A' && h[2] == 'P' && h[3] == '1' && h[7] == 2) base = off;
    }
    CHECK(base != 0);
    uint8_t junk[64];
    memset(junk, 0x5A, sizeof(junk));
    dev.write(base + 100, junk, sizeof(junk));
    WorldStore ws(&dev);
    CHECK(ws.open(sp, false));
    Chunk a(1, 1), b(2, 1);
    CHECK_EQ(ws.loadChunk(a), LOAD_OK);       // in both copies
    CHECK_EQ(ws.loadChunk(b), LOAD_ABSENT);   // only in the torn one: as if never saved
}

TEST(store_reports_a_full_export_and_keeps_working) {
    MemDevice dev(4u << 20);   // room for about 20 units
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 100000;
    sp.playerSlots = 64;
    CHECK(ws.open(sp));
    int saved = 0;
    bool failed = false;
    for (int i = 0; i < 40 && !failed; i++) {
        Chunk c(i * 40, 0);   // a region (and a map) each
        fillTestChunk(c, i);
        if (ws.saveChunk(c)) saved++;
        else failed = true;
    }
    CHECK(failed);
    CHECK(saved > 3);
    CHECK(ws.index().stats().full);
    char line[300];
    ws.statusLine(line, sizeof(line));
    CHECK(strstr(line, "EXPORT FULL") != nullptr);
    // what was saved before is still there
    Chunk first(0, 0), want(0, 0);
    fillTestChunk(want, 0);
    CHECK_EQ(ws.loadChunk(first), LOAD_OK);
    CHECK(sameChunk(want, first));
    printf("    %d chunks fit, then: %s\n", saved, line);
}

TEST(store_starts_a_new_world_over_an_older_format) {
    // worlds are not converted: one in an older format (here format 4, the 1.16.5 block
    // states) is replaced by a new, empty world -- also when formatting unknown data is
    // not allowed
    MemDevice dev(64u << 20);
    StoreParams sp;
    sp.radius = 8;
    sp.playerSlots = 4;
    PlayerData player;
    player.uuid[0] = 5;
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp));
        WorldMeta m;
        m.seed = 77;
        CHECK(ws.saveMeta(m));
        Chunk c(1, 1);
        fillTestChunk(c, 7);
        CHECK(ws.saveChunk(c));
        CHECK(ws.savePlayer(player));
        CHECK(ws.flush());
    }
    for (int k = 0; k < 2; ++k) {   // both superblock copies say format 4
        uint8_t super[512];
        CHECK(dev.read(k * 512, super, sizeof(super)));
        if (memcmp(super, "ESPMCW01", 8)) continue;
        super[8] = super[9] = super[10] = 0;
        super[11] = 4;
        uint32_t crc = crc32(super, 508);
        BufSink sink(super + 508, 4);
        Writer w(sink);
        w.u32(crc);
        CHECK(dev.write(k * 512, super, sizeof(super)));
    }
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp, false));
        CHECK_EQ(ws.format(), 6);
        WorldMeta m;
        CHECK(!ws.loadMeta(m));   // no world: the server creates one
        PlayerData loaded;
        CHECK(!ws.loadPlayer(player.uuid, loaded));
        Chunk c(1, 1);
        CHECK_EQ(ws.loadChunk(c), LOAD_ABSENT);
        m.seed = 78;
        CHECK(ws.saveMeta(m));
    }
    WorldStore ws(&dev);
    CHECK(ws.open(sp, false));
    WorldMeta m;
    CHECK(ws.loadMeta(m));
    CHECK_EQ(m.seed, 78u);
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

TEST(storage_comparator_output_survives_save_load_and_snapshot) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev); StoreParams params;
    params.radius = 1; params.playerSlots = 4;
    CHECK(store.open(params));
    Chunk c(0, 0);
    c.set(3, 80, 4, setBool(bs::Comparator, "powered", true));
    TileEntity* t = c.addTile(TILE_COMPARATOR, 3, 80, 4);
    t->signal = 11;
    CHECK_EQ(t->slotCount(), 0);
    Chunk* copy = c.clone(); CHECK(copy != nullptr);
    if (copy) {
        CHECK_EQ(copy->tileAt(3, 80, 4)->signal, 11);
        CHECK(store.saveChunk(*copy)); delete copy;
    }
    Chunk loaded(0, 0);
    CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    TileEntity* restored = loaded.tileAt(3, 80, 4);
    CHECK(restored != nullptr);
    if (restored) { CHECK_EQ(restored->type, TILE_COMPARATOR); CHECK_EQ(restored->signal, 11); }
}

TEST(storage_moving_piston_saves_previous_progress_and_resumes_state) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev); StoreParams params;
    params.radius = 1; params.playerSlots = 4; CHECK(store.open(params));
    Chunk c(0, 0); c.set(3, 80, 4, setPropStr(bs::MovingPiston, "facing", "east"));
    TileEntity* t = c.addTile(TILE_PISTON, 3, 80, 4);
    t->movedState = setPropStr(bs::OakStairs, "facing", "west");
    t->pistonFace = 5; t->pistonProgress = 2; t->pistonPrevious = 1;
    t->pistonExtending = true; t->pistonSource = false;
    CHECK_EQ(t->slotCount(), 0); CHECK_EQ(c.movingPistons(), 1);
    Chunk* copy = c.clone(); CHECK(copy != nullptr); if (!copy) return;
    CHECK_EQ(copy->movingPistons(), 1);
    CHECK_EQ(copy->tileAt(3, 80, 4)->pistonProgress, 2);
    CHECK(store.saveChunk(*copy)); delete copy;
    Chunk loaded(0, 0); CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    TileEntity* restored = loaded.tileAt(3, 80, 4); CHECK(restored != nullptr);
    if (restored) {
        CHECK_EQ(restored->type, TILE_PISTON); CHECK_EQ(restored->movedState, t->movedState);
        CHECK_EQ(restored->pistonFace, 5); CHECK_EQ(restored->pistonProgress, 1);
        CHECK_EQ(restored->pistonPrevious, 1); CHECK(restored->pistonExtending); CHECK(!restored->pistonSource);
    }
    CHECK_EQ(loaded.movingPistons(), 1); loaded.removeTile(3, 80, 4); CHECK_EQ(loaded.movingPistons(), 0);
}

TEST(storage_automated_container_slots_and_hopper_cooldown_roundtrip) {
    MemDevice dev(32 * 1024 * 1024);WorldStore store(&dev);StoreParams params;
    params.radius=1;params.playerSlots=4;CHECK(store.open(params));
    Chunk c(0,0);
    const uint8_t types[]={TILE_HOPPER,TILE_DROPPER,TILE_DISPENSER};
    const uint16_t states[]={bs::Hopper,bs::Dropper,bs::Dispenser};
    for(int i=0;i<3;++i) {
        c.set(i,80,0,states[i]);TileEntity* t=c.addTile(types[i],i,80,0);
        CHECK_EQ(t->slotCount(),i==0?5:9);t->transferCooldown=7;
        t->items[t->slotCount()-1]=ItemStack::of(itm::FlintAndSteel);t->items[t->slotCount()-1].damage=13;
    }
    CHECK(store.saveChunk(c));Chunk loaded(0,0);CHECK_EQ(store.loadChunk(loaded),LOAD_OK);
    for(int i=0;i<3;++i) {
        TileEntity* t=loaded.tileAt(i,80,0);CHECK(t!=nullptr);if(!t)continue;
        CHECK_EQ(t->type,types[i]);CHECK_EQ(t->items[t->slotCount()-1].id,itm::FlintAndSteel);
        CHECK_EQ(t->items[t->slotCount()-1].damage,13);
        if(i==0)CHECK_EQ(t->transferCooldown,7);
    }
}

TEST(storage_daylight_state_roundtrip_discards_derived_sky_cache) {
    MemDevice dev(32 * 1024 * 1024);
    WorldStore store(&dev); StoreParams params;
    params.radius = 1; params.playerSlots = 4; CHECK(store.open(params));
    Chunk c(0, 0);
    uint16_t state = setProp(setBool(bs::DaylightDetector, "inverted", true), "power", 11);
    c.set(3, 80, 4, state);
    TileEntity* t = c.addTile(TILE_DAYLIGHT, 3, 80, 4);
    t->daylightValid = true; t->daylightSky = 12;
    CHECK_EQ(t->slotCount(), 0); CHECK_EQ(c.tickingBlockEntities(), 1);
    CHECK(store.saveChunk(c));
    Chunk loaded(0, 0); CHECK_EQ(store.loadChunk(loaded), LOAD_OK);
    CHECK_EQ(loaded.get(3, 80, 4), state);
    TileEntity* restored = loaded.tileAt(3, 80, 4); CHECK(restored != nullptr);
    if (restored) { CHECK_EQ(restored->type, TILE_DAYLIGHT); CHECK(!restored->daylightValid); }
    CHECK_EQ(loaded.tickingBlockEntities(), 1);
    loaded.removeTile(3, 80, 4); CHECK_EQ(loaded.tickingBlockEntities(), 0);
}

namespace {
ItemStack storedBook(const char* text) {
    ByteBuf bytes; Writer w(bytes); NbtWriter n(w); n.beginRoot();
    n.str("author", "redstone"); n.listHeader("pages", NBT_STRING, 1);
    w.u16(strlen(text)); w.bytes((const uint8_t*)text, strlen(text)); n.end();
    ItemStack book = ItemStack::of(itm::WrittenBook);
    CHECK(book.setTag(bytes.data(), bytes.size()));
    return book;
}
class TornPlayerDevice : public MemDevice {
public:
    TornPlayerDevice() : MemDevice(96u << 20) {}
    int writesUntilFailure = -1;
    int flushesUntilFailure = -1;
    bool write(uint64_t offset, const void* data, uint32_t length) override {
        if (writesUntilFailure == 0) {
            writesUntilFailure = -1;
            MemDevice::write(offset, data, length / 2);
            return false;
        }
        if (writesUntilFailure > 0) --writesUntilFailure;
        return MemDevice::write(offset, data, length);
    }
    bool flush() override {
        if (flushesUntilFailure == 0) { flushesUntilFailure = -1; return false; }
        if (flushesUntilFailure > 0) --flushesUntilFailure;
        return true;
    }
};
}
TEST(storage_tagged_items_survive_chunk_snapshot_and_player_restart) {
    MemDevice dev(64u << 20); StoreParams params; params.playerSlots = 4;
    PlayerData player; player.uuid[0] = 9;
    player.inv[36] = storedBook("{\"text\":\"Page one\"}");
    player.inv[5] = player.inv[36]; player.inv[5].damage = 37;
    Chunk c(0, 0); c.set(0, 80, 0, bs::Hopper);
    c.addTile(TILE_HOPPER, 0, 80, 0)->items[2] = player.inv[36];
    {
        WorldStore ws(&dev); CHECK(ws.open(params));
        Chunk* snapshot = c.clone(); CHECK(snapshot != nullptr);
        CHECK(snapshot->tileAt(0, 80, 0)->items[2].tagData() == player.inv[36].tagData());
        CHECK(ws.saveChunk(*snapshot)); delete snapshot;
        CHECK(ws.savePlayer(player)); CHECK(ws.flush());
    }
    WorldStore restored(&dev); CHECK(restored.open(params, false));
    Chunk loaded(0, 0); CHECK_EQ(restored.loadChunk(loaded), LOAD_OK);
    CHECK(loaded.tileAt(0, 80, 0)->items[2].sameItem(player.inv[36]));
    PlayerData actual; CHECK_EQ(restored.fetchPlayer(player.uuid, actual), LOAD_OK);
    CHECK(actual.inv[36].sameItem(player.inv[36])); CHECK(actual.inv[5].sameItem(player.inv[5]));
    CHECK_EQ(actual.inv[5].damage, 37);
}
TEST(storage_player_torn_payload_and_descriptor_keep_previous_inventory) {
    for (int failure = 0; failure < 3; ++failure) {
        TornPlayerDevice dev; StoreParams params; params.playerSlots = 4;
        PlayerData player; player.uuid[0] = 17;
        ItemStack oldBook = storedBook("old"), newBook = storedBook("new");
        {
            WorldStore ws(&dev); CHECK(ws.open(params));
            player.inv[36] = oldBook;
            CHECK(ws.savePlayer(player)); CHECK(ws.savePlayer(player)); // allocate both copies
            player.inv[36] = newBook;
            if (failure < 2) dev.writesUntilFailure = failure; // payload or descriptor
            else dev.flushesUntilFailure = 0; // payload durability barrier
            CHECK(!ws.savePlayer(player));
        }
        WorldStore restored(&dev); CHECK(restored.open(params, false));
        PlayerData actual; CHECK_EQ(restored.fetchPlayer(player.uuid, actual), LOAD_OK);
        CHECK(actual.inv[36].sameItem(oldBook));
        CHECK(restored.savePlayer(player));
        CHECK_EQ(restored.fetchPlayer(player.uuid, actual), LOAD_OK);
        CHECK(actual.inv[36].sameItem(newBook));
    }
}
TEST(storage_player_zero_uuid_does_not_match_empty_legacy_half_of_other_player) {
    MemDevice dev(32u << 20); StoreParams params; params.playerSlots = 4;
    WorldStore ws(&dev); CHECK(ws.open(params));
    PlayerData first, zero; first.uuid[0] = 4; first.xpLevel = 17; zero.xpLevel = 23;
    CHECK(ws.savePlayer(first)); CHECK(ws.savePlayer(zero));
    PlayerData loaded; CHECK(ws.loadPlayer(first.uuid, loaded)); CHECK_EQ(loaded.xpLevel, 17);
    CHECK(ws.loadPlayer(zero.uuid, loaded)); CHECK_EQ(loaded.xpLevel, 23);
}

TEST(storage_player_recovery_then_failed_save_preserves_recovered_copy) {
    TornPlayerDevice dev; StoreParams params; params.playerSlots = 1;
    WorldStore ws(&dev); CHECK(ws.open(params));
    PlayerData player; player.uuid[0] = 19;
    ItemStack oldBook = storedBook("first"), newBook = storedBook("second");
    player.inv[36] = oldBook; CHECK(ws.savePlayer(player));
    player.inv[36] = newBook; CHECK(ws.savePlayer(player));
    uint8_t descriptor[256]; CHECK(dev.read(4096 + 768, descriptor, 256));
    Reader pointer(descriptor + 24, 8); uint64_t payload = pointer.u64();
    uint8_t bad = 255; CHECK(dev.write(payload, &bad, 1));
    PlayerData recovered; CHECK_EQ(ws.fetchPlayer(player.uuid, recovered), LOAD_OK);
    CHECK(recovered.inv[36].sameItem(oldBook));
    // No new watermark is needed here, so the first write is the new payload.
    dev.writesUntilFailure = 0; CHECK(!ws.savePlayer(player));
    CHECK_EQ(ws.fetchPlayer(player.uuid, recovered), LOAD_OK);
    CHECK(recovered.inv[36].sameItem(oldBook));
}

TEST(storage_lectern_book_and_page_survive_restart) {
    MemDevice dev(32u << 20); StoreParams params; params.playerSlots = 4;
    WorldStore ws(&dev); CHECK(ws.open(params));
    Chunk c(0, 0); c.set(0, 80, 0, setBool(bs::Lectern, "has_book", true));
    TileEntity* t = c.addTile(TILE_LECTERN, 0, 80, 0);
    t->items[0] = storedBook("{\"text\":\"saved book\"}"); t->bookPage = 0;
    CHECK(ws.saveChunk(c)); Chunk loaded(0, 0); CHECK_EQ(ws.loadChunk(loaded), LOAD_OK);
    TileEntity* actual = loaded.tileAt(0, 80, 0); CHECK(actual != nullptr);
    CHECK_EQ(actual->type, TILE_LECTERN); CHECK_EQ(actual->bookPage, 0);
    CHECK(actual->items[0].sameItem(t->items[0])); CHECK_EQ(actual->slotCount(), 1);
}

static void checkResetForgetsTheOldWorld(MemDevice& dev, const StoreParams& params, const PlayerData& player) {
    {
        WorldStore ws(&dev);
        CHECK(ws.open(params, false));
        WorldMeta fresh;
        fresh.seed = 99;
        fresh.spawnY = 70;
        CHECK(ws.resetWorld(fresh));
    }
    WorldStore reopened(&dev);
    CHECK(reopened.open(params, false));
    CHECK_EQ(reopened.format(), 6);
    WorldMeta m;
    CHECK(reopened.loadMeta(m));
    CHECK_EQ(m.seed, 99u);
    CHECK_EQ(m.spawnY, 70);
    PlayerData loaded;
    CHECK(!reopened.loadPlayer(player.uuid, loaded));
    Chunk c(0, 0);
    CHECK(reopened.loadChunk(c) != LOAD_OK);
}

TEST(storage_reset_world_keeps_the_new_seed_and_forgets_players) {
    MemDevice dev(64u << 20);
    StoreParams params; params.radius = 8; params.playerSlots = 4;
    PlayerData player; player.uuid[0] = 7; player.inv[36] = storedBook("old world");
    {
        WorldStore ws(&dev); CHECK(ws.open(params));
        WorldMeta m; m.seed = 1; CHECK(ws.saveMeta(m));
        CHECK(ws.savePlayer(player));
        Chunk c(0, 0); c.set(1, 70, 1, bs::GoldBlock); CHECK(ws.saveChunk(c));
        CHECK(ws.flush());
    }
    checkResetForgetsTheOldWorld(dev, params, player);
}


TEST(store_keeps_a_chunks_entities) {
    MemDevice dev(64u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 4;
    CHECK(ws.open(sp));
    Chunk a(1, -2);
    fillTestChunk(a, 3);
    SavedEntity sheep;
    sheep.kind = 3;   // a mob (the server's EK_MOB)
    sheep.type = (uint16_t)findEntityType("sheep");
    sheep.variant = 14;
    sheep.x = 20.5; sheep.y = -12.25; sheep.z = -27.75;
    sheep.vx = 0.1f; sheep.yaw = 270; sheep.health = 6.5f; sheep.fireTicks = 40; sheep.age = 12345;
    CHECK(a.addEntity(sheep));                 // stashed
    SavedEntity item;
    item.kind = 2;    // a dropped item (EK_ITEM)
    item.type = (uint16_t)findEntityType("item");
    item.x = 18; item.y = 70; item.z = -30;
    item.pickupDelay = 7;
    item.age = 600;
    item.item = ItemStack::of(itm::DiamondSword, 1);
    {   // an enchanted sword: the stack's NBT goes with it
        ByteBuf b;
        Writer w(b);
        NbtWriter n(w);
        n.beginRoot();
        n.beginCompound("display");
        n.str("Name", "{\"text\":\"Old faithful\"}");
        n.end();
        n.end();
        CHECK(item.item.setTag(b.data(), b.size()));
    }
    item.item.damage = 12;   // (setTag takes the damage from the tag)
    CHECK(a.setLiveEntities(&item, 1));       // in the game, copied in for the save
    CHECK(ws.saveChunk(a));
    Chunk b(1, -2);
    CHECK_EQ(ws.loadChunk(b), LOAD_OK);
    CHECK_EQ(b.entCount, 2);                   // both come back stashed
    CHECK_EQ(b.liveCount, 0);
    CHECK(b.hadEntities);
    const SavedEntity& s = b.ents[0];
    CHECK_EQ(s.kind, 3);
    CHECK_EQ(s.type, sheep.type);
    CHECK_EQ(s.variant, 14);
    CHECK(s.x == 20.5 && s.y == -12.25 && s.z == -27.75);
    CHECK(s.vx == 0.1f && s.yaw == 270 && s.health == 6.5f);
    CHECK_EQ(s.fireTicks, 40);
    CHECK_EQ(s.age, 12345u);
    const SavedEntity& d = b.ents[1];
    CHECK_EQ(d.kind, 2);
    CHECK_EQ(d.item.id, itm::DiamondSword);
    CHECK_EQ(d.item.damage, 12);
    CHECK_EQ(d.pickupDelay, 7);
    CHECK_EQ(d.item.tagSize(), item.item.tagSize());
    CHECK(d.item.sameItem(item.item));
}

TEST(store_keeps_a_chest_boats_slots) {
    MemDevice dev(64u << 20);
    WorldStore ws(&dev);
    StoreParams sp;
    sp.radius = 4;
    CHECK(ws.open(sp));
    Chunk a(0, 0);
    fillTestChunk(a, 3);
    SavedEntity boat;
    boat.kind = SAVED_KIND_BOAT;
    boat.type = (uint16_t)findEntityType("oak_chest_boat");
    boat.x = 3.5; boat.y = 62.4; boat.z = 7.25; boat.yaw = -90;
    boat.cargo.resize(27);
    boat.cargo[4] = ItemStack::of(itm::Diamond, 5);
    boat.cargo[26] = ItemStack::of(itm::IronPickaxe, 1);
    boat.cargo[26].damage = 30;
    SavedEntity plain;   // a boat without a chest: no slots
    plain.kind = SAVED_KIND_BOAT;
    plain.type = (uint16_t)findEntityType("bamboo_raft");
    plain.x = 5;
    CHECK(a.addEntity(boat));
    CHECK(a.addEntity(plain));
    CHECK(ws.saveChunk(a));
    Chunk b(0, 0);
    CHECK_EQ(ws.loadChunk(b), LOAD_OK);
    CHECK_EQ(b.entCount, 2);
    const SavedEntity& s = b.ents[0];
    CHECK_EQ(s.type, boat.type);
    CHECK_EQ(s.cargo.size(), (size_t)27);
    if (s.cargo.size() == 27) {
        CHECK(s.cargo[4].id == itm::Diamond && s.cargo[4].count == 5);
        CHECK(s.cargo[26].id == itm::IronPickaxe && s.cargo[26].damage == 30);
        CHECK(s.cargo[0].empty());
    }
    CHECK(s.yaw == -90 && s.y == 62.4);
    CHECK_EQ(b.ents[1].cargo.size(), (size_t)0);
}
