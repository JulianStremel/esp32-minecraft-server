// Nether portals: frames, breaking, linking; and the world state they are saved in.
#include <string.h>
#include <unistd.h>
#include "testing.h"
#include "mc/registry.h"
#include "mc/server/server.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"

using namespace mc;

static Server* portalServer() {
    static Server* s = nullptr;
    if (s) return s;
    s = new Server();
    ServerConfig cfg;
    cfg.port = 0;
    cfg.worldType = WORLD_FLAT;
    cfg.spawnMobs = false;
    cfg.seed = 3;
    if (!s->begin(cfg, nullptr)) return nullptr;
    return s;
}

// An obsidian frame with its inside's lower corner at (x, y, z): w x h inside, axis 0
// along x. `gap`: leave one frame block out.
static void buildFrame(Server& s, uint8_t dim, int x, int y, int z, uint8_t axis, int w, int h, bool gap = false) {
    int ax = axis ? 0 : 1, az = axis ? 1 : 0;
    for (int i = -1; i <= w; i++)
        for (int j = -1; j <= h; j++) {
            bool edge = i == -1 || i == w || j == -1 || j == h;
            uint16_t st = edge ? bs::Obsidian : 0;
            if (gap && i == w && j == 1) st = 0;
            s.world.setBlock(dim, x + ax * i, y + j, z + az * i, st);
        }
}

static int countPortal(Server& s, uint8_t dim, int x0, int y0, int z0, int x1, int y1, int z1) {
    int n = 0;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) n += blockIdOf(s.world.getBlock(dim, x, y, z)) == blk::NetherPortal;
    return n;
}

TEST(world_state_roundtrip) {
    WorldState a;
    for (int i = 0; i < 5; i++) {
        PortalRef r;
        r.dim = (uint8_t)(i % 2);
        r.axis = (uint8_t)(i % 2);
        r.x = -1000000 + i * 12345;
        r.y = (int16_t)(30 + i);
        r.z = 29999000 - i;
        a.addPortal(r);
    }
    a.addPortal(a.portals[2]);   // already known: not added again
    CHECK_EQ(a.nPortals, 5);
    a.dragon.state = DragonFight::KILLED;
    a.dragon.previouslyKilled = true;
    a.dragon.dragonHealth = 123.5f;
    uint8_t buf[WorldMeta::EXTRA_CAP];
    size_t n = a.encode(buf, sizeof(buf));
    CHECK(n > 0);
    WorldState b;
    CHECK(b.decode(buf, n));
    CHECK_EQ(b.nPortals, 5);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(b.portals[i].x, a.portals[i].x);
        CHECK_EQ(b.portals[i].y, a.portals[i].y);
        CHECK_EQ(b.portals[i].z, a.portals[i].z);
        CHECK_EQ(b.portals[i].dim, a.portals[i].dim);
        CHECK_EQ(b.portals[i].axis, a.portals[i].axis);
    }
    CHECK_EQ(b.dragon.state, DragonFight::KILLED);
    CHECK(b.dragon.previouslyKilled);
    CHECK(b.dragon.dragonHealth == 123.5f);
    // full: the oldest goes
    WorldState c;
    for (int i = 0; i < WorldState::MAX_PORTALS + 3; i++) {
        PortalRef r;
        r.x = i;
        c.addPortal(r);
    }
    CHECK_EQ(c.nPortals, WorldState::MAX_PORTALS);
    CHECK_EQ(c.portals[0].x, 3);
    CHECK(c.encode(buf, sizeof(buf)) > 0);   // the largest state fits the record
}

TEST(world_state_survives_reopening_and_torn_writes) {
    MemDevice dev(64u << 20);
    StoreParams sp;
    sp.radius = 64;
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp, true));
        WorldMeta m;
        m.seed = 77;
        m.extraLen = 3;
        m.extra[0] = 1; m.extra[1] = 2; m.extra[2] = 3;
        CHECK(ws.saveMeta(m));
        m.extraLen = 4;
        m.extra[3] = 4;
        CHECK(ws.saveMeta(m));   // the second copy
        CHECK(ws.flush());
    }
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp, false));
        WorldMeta m;
        CHECK(ws.loadMeta(m));
        CHECK_EQ(m.seed, 77u);
        CHECK_EQ(m.extraLen, 4);
        CHECK_EQ(m.extra[3], 4);
    }
    // tear the newer copy: the older one is read
    uint8_t junk[64];
    memset(junk, 0xAB, sizeof(junk));
    bool torn = false;
    for (int k = 0; k < 2 && !torn; k++) {
        uint8_t h[10];
        dev.read(1024 + k * 1536, h, sizeof(h));
        if (h[7] == 2) {   // sequence number 2 (big-endian, low byte)
            dev.write(1024 + k * 1536 + 8, junk, sizeof(junk));
            torn = true;
        }
    }
    CHECK(torn);
    {
        WorldStore ws(&dev);
        CHECK(ws.open(sp, false));
        WorldMeta m;
        CHECK(ws.loadMeta(m));
        CHECK_EQ(m.extraLen, 3);
        CHECK_EQ(m.extra[2], 3);
    }
    // a new world on the same device starts without it
    MemDevice dev2(64u << 20);
    {
        WorldStore ws(&dev2);
        CHECK(ws.open(sp, true));
        WorldMeta m;
        m.extraLen = 2;
        m.extra[0] = 9;
        CHECK(ws.saveMeta(m));
        uint8_t zero[512];
        memset(zero, 0, sizeof(zero));
        dev2.write(0, zero, 512);    // the superblocks gone: "unknown data"
        dev2.write(512, zero, 512);
    }
    {
        WorldStore ws(&dev2);
        CHECK(ws.open(sp, true));    // formats it
        WorldMeta m;
        m.seed = 5;
        CHECK(ws.saveMeta(m));
        WorldStore again(&dev2);
        CHECK(again.open(sp, false));
        WorldMeta r;
        CHECK(again.loadMeta(r));
        CHECK_EQ(r.extraLen, 0);
    }
}

TEST(portal_lights_only_in_a_complete_frame) {
    Server* s = portalServer();
    CHECK(s);
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    for (int cx = 0; cx <= 4; cx++)
        for (int cz = -1; cz <= 1; cz++) s->world.load(DIM_OVERWORLD, cx, cz);
    // the smallest frame, along x
    buildFrame(*s, DIM_OVERWORLD, 10, 10, 0, 0, 2, 3);
    CHECK(s->lightPortal(10, 10, 0));
    CHECK_EQ(countPortal(*s, DIM_OVERWORLD, 9, 9, 0, 12, 13, 0), 6);
    CHECK(!strcmp(getPropStr(s->blockAt(11, 12, 0), "axis"), "x"));
    // a big one along z, lit from its middle
    buildFrame(*s, DIM_OVERWORLD, 30, 10, -5, 1, 7, 9);
    CHECK(s->lightPortal(30, 14, -2));
    CHECK_EQ(countPortal(*s, DIM_OVERWORLD, 30, 9, -6, 30, 20, 3), 63);
    CHECK(!strcmp(getPropStr(s->blockAt(30, 12, 0), "axis"), "z"));
    // a frame with a gap, too small, too large: nothing
    buildFrame(*s, DIM_OVERWORLD, 50, 10, 0, 0, 2, 3, true);
    CHECK(!s->lightPortal(50, 10, 0));
    buildFrame(*s, DIM_OVERWORLD, 60, 10, 0, 0, 1, 3);
    CHECK(!s->lightPortal(60, 10, 0));
    buildFrame(*s, DIM_OVERWORLD, 62, 10, 5, 0, 2, 2);
    CHECK(!s->lightPortal(62, 10, 5));
    CHECK_EQ(countPortal(*s, DIM_OVERWORLD, 49, 9, -1, 70, 14, 6), 0);
    // both lit ones are known
    int known = 0;
    for (int i = 0; i < s->wstate.nPortals; i++)
        known += s->wstate.portals[i].dim == DIM_OVERWORLD && (s->wstate.portals[i].x == 10 || s->wstate.portals[i].x == 30);
    CHECK_EQ(known, 2);
}

TEST(portal_breaks_with_its_frame_but_not_from_the_side) {
    Server* s = portalServer();
    if (!s) return;
    Server::InDim in(*s, DIM_OVERWORLD);
    buildFrame(*s, DIM_OVERWORLD, 10, 30, 8, 0, 3, 4);
    CHECK(s->lightPortal(11, 31, 8));
    CHECK_EQ(countPortal(*s, DIM_OVERWORLD, 9, 29, 8, 13, 34, 8), 12);
    // blocks placed in front of and behind it: across its plane, nothing happens
    s->setBlock(11, 31, 9, bs::Stone);
    s->setBlock(11, 31, 9, 0);
    s->setBlock(12, 32, 7, bs::Dirt);
    CHECK_EQ(countPortal(*s, DIM_OVERWORLD, 9, 29, 8, 13, 34, 8), 12);
    // a frame block broken: all of it goes
    s->setBlock(13, 32, 8, 0);
    CHECK_EQ(countPortal(*s, DIM_OVERWORLD, 9, 29, 8, 13, 34, 8), 0);
}

TEST(portals_link_both_ways) {
    Server* s = portalServer();
    if (!s) return;
    Player& p = s->players[0];
    // an overworld portal at (400, 10, 400) (flat world: the ground is at y 4)
    {
        Server::InDim in(*s, DIM_OVERWORLD);
        s->world.load(DIM_OVERWORLD, 400 >> 4, 400 >> 4);
        buildFrame(*s, DIM_OVERWORLD, 400, 10, 400, 0, 2, 3);
        CHECK(s->lightPortal(400, 10, 400));
    }
    p.e.dim = DIM_OVERWORLD;
    p.e.x = 401.0; p.e.y = 10; p.e.z = 400.5; p.e.yaw = 0;
    double x, y, z;
    float yaw;
    // to the Nether: nothing there yet, so a portal is built near (50, 50)
    int before = s->wstate.nPortals;
    CHECK(s->portalArrival(p, DIM_NETHER, x, y, z, yaw));
    CHECK_EQ(s->wstate.nPortals, before + 1);
    printf("    built in the Nether at %.1f %.1f %.1f\n", x, y, z);
    CHECK(fabs(x - 50) <= 17 && fabs(z - 50) <= 17);
    CHECK_EQ(blockIdOf(s->world.getBlock(DIM_NETHER, (int)floor(x), (int)floor(y), (int)floor(z))), blk::NetherPortal);
    CHECK_EQ(blockIdOf(s->world.getBlock(DIM_NETHER, (int)floor(x), (int)floor(y) - 1, (int)floor(z))), blk::Obsidian);
    // again: the same one
    double x2, y2, z2;
    CHECK(s->portalArrival(p, DIM_NETHER, x2, y2, z2, yaw));
    CHECK(x2 == x && y2 == y && z2 == z);
    CHECK_EQ(s->wstate.nPortals, before + 1);
    // back from it: the lit overworld portal (x 8 = within 128 blocks of it)
    p.e.dim = DIM_NETHER;
    p.e.x = x; p.e.y = y; p.e.z = z;
    CHECK(s->portalArrival(p, DIM_OVERWORLD, x2, y2, z2, yaw));
    printf("    back in the overworld at %.1f %.1f %.1f\n", x2, y2, z2);
    CHECK(x2 == 401.0 && y2 == 10 && z2 == 400.5);
    // a broken portal is forgotten and replaced
    {
        Server::InDim in(*s, DIM_OVERWORLD);
        s->setBlock(399, 11, 400, 0);
        CHECK_EQ(blockIdOf(s->blockAt(400, 10, 400)), 0);
    }
    CHECK(s->portalArrival(p, DIM_OVERWORLD, x2, y2, z2, yaw));
    CHECK(!(x2 == 401.0 && z2 == 400.5));
    CHECK_EQ(blockIdOf(s->world.getBlock(DIM_OVERWORLD, (int)floor(x2), (int)floor(y2), (int)floor(z2))), blk::NetherPortal);
    printf("    rebuilt at %.1f %.1f %.1f\n", x2, y2, z2);
    p.e.dim = DIM_OVERWORLD;
}
