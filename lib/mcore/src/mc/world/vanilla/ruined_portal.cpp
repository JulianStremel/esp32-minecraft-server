// The ruined portals: vanilla's RuinedPortalStructure (a setup by weight, an air pocket,
// one of the ten portals or at 5 % one of three giant ones, turned and mirrored around its
// middle, its height by its placement: on land, partly buried, on the ocean floor, in a
// mountain, underground or in the Nether, down to where three of its four corners stand on
// ground) and RuinedPortalPiece (gold blocks gone at 30 %, lava turned to magma or
// netherrack, netherrack to magma at 7 %, stone aged mossy and cracked and obsidian crying
// at 15 % as BlockAgeProcessor does, blackstone in the Nether, lava kept around blocks in it,
// netherrack spread on the ground around it with its drip columns, vines and leaves).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"
#include "mc/world/vanilla/structures.h"
#include "mc/world/vanilla/surface.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

enum : int16_t { RPF_AIR_POCKET = 1, RPF_OVERGROWN = 2, RPF_VINES = 4, RPF_BLACKSTONE = 8, RPF_COLD = 16 };

int templateNamed(const char* name) {
    for (int i = 0; i < TD_NUM_TEMPLATES; i++)
        if (!strcmp(TD_STRINGS[TD_TEMPLATES[i].name], name)) return i;
    return -1;
}
int between(JavaRandom& r, int lo, int hi) { return r.nextInt(hi - lo + 1) + lo; }   // Mth.randomBetweenInclusive

// the names the processors need, resolved once
struct Names {
    bool done = false;
    uint16_t gold, lava, magma, netherrack, obsidian, cryingObsidian, stoneBricks, stone, chiseledStoneBricks, crackedStoneBricks,
        mossyStoneBricks, stoneBrickStairs, mossyStoneBrickStairs, stoneSlab, stoneBrickSlab, mossyStoneBrickSlab, mossyStoneBrickWall,
        jungleLeaves, vine, air;
    uint16_t protectedIds[8];
    int nProtected = 0;
    // BlackstoneReplaceProcessor's map
    uint16_t from[24], to[24];
    int nMap = 0;
};
Names N;
uint16_t blockState(const char* n) {
    int b = findBlock(n);
    return b >= 0 ? BLOCKS[b].defState : 0;
}
uint16_t blockId(const char* n) {
    int b = findBlock(n);
    return b >= 0 ? (uint16_t)b : 0xFFFF;
}
void resolveNames() {
    if (N.done) return;
    N.gold = blockId("gold_block");
    N.lava = blockId("lava");
    N.magma = blockState("magma_block");
    N.netherrack = blockState("netherrack");
    N.obsidian = blockId("obsidian");
    N.cryingObsidian = blockState("crying_obsidian");
    N.stoneBricks = blockId("stone_bricks");
    N.stone = blockId("stone");
    N.chiseledStoneBricks = blockId("chiseled_stone_bricks");
    N.crackedStoneBricks = blockState("cracked_stone_bricks");
    N.mossyStoneBricks = blockState("mossy_stone_bricks");
    N.stoneBrickStairs = blockState("stone_brick_stairs");
    N.mossyStoneBrickStairs = blockState("mossy_stone_brick_stairs");
    N.stoneSlab = blockState("stone_slab");
    N.stoneBrickSlab = blockState("stone_brick_slab");
    N.mossyStoneBrickSlab = blockState("mossy_stone_brick_slab");
    N.mossyStoneBrickWall = blockState("mossy_stone_brick_wall");
    N.jungleLeaves = setBool(blockState("jungle_leaves"), "persistent", true);
    N.vine = blockState("vine");
    N.air = 0;
    static const char* const PROTECTED[] = {"bedrock", "spawner", "chest", "end_portal_frame", "reinforced_deepslate", "trial_spawner", "vault"};
    for (const char* p : PROTECTED) N.protectedIds[N.nProtected++] = blockId(p);
    static const char* const MAP[][2] = {
        {"cobblestone", "blackstone"}, {"mossy_cobblestone", "blackstone"}, {"stone", "polished_blackstone"},
        {"stone_bricks", "polished_blackstone_bricks"}, {"mossy_stone_bricks", "polished_blackstone_bricks"},
        {"cobblestone_stairs", "blackstone_stairs"}, {"mossy_cobblestone_stairs", "blackstone_stairs"},
        {"stone_stairs", "polished_blackstone_stairs"}, {"stone_brick_stairs", "polished_blackstone_brick_stairs"},
        {"mossy_stone_brick_stairs", "polished_blackstone_brick_stairs"}, {"cobblestone_slab", "blackstone_slab"},
        {"mossy_cobblestone_slab", "blackstone_slab"}, {"smooth_stone_slab", "polished_blackstone_slab"},
        {"stone_slab", "polished_blackstone_slab"}, {"stone_brick_slab", "polished_blackstone_brick_slab"},
        {"mossy_stone_brick_slab", "polished_blackstone_brick_slab"}, {"stone_brick_wall", "polished_blackstone_brick_wall"},
        {"mossy_stone_brick_wall", "polished_blackstone_brick_wall"}, {"cobblestone_wall", "blackstone_wall"},
        {"mossy_cobblestone_wall", "blackstone_wall"}, {"chiseled_stone_bricks", "chiseled_polished_blackstone"},
        {"cracked_stone_bricks", "cracked_polished_blackstone_bricks"}, {"iron_bars", "chain"},
    };
    for (const auto& m : MAP) {
        N.from[N.nMap] = blockId(m[0]);
        N.to[N.nMap++] = blockId(m[1]);
    }
    N.done = true;
}

// a state with another block's properties where they share them (stairs' facing and half, ...)
uint16_t withSameProps(uint16_t st, uint16_t toBlock) {
    if (toBlock == 0xFFFF) return st;
    uint16_t out = BLOCKS[toBlock].defState;
    static const char* const PROPS[] = {"facing", "half", "shape", "type", "waterlogged", "axis", "north", "east", "south", "west", "up"};
    uint16_t from = blockIdOf(st);
    for (const char* p : PROPS)
        if (propIndexOf(from, p) >= 0 && propIndexOf(toBlock, p) >= 0) out = setPropStr(out, p, getPropStr(st, p));
    return out;
}

struct PortalCtx { int16_t flags; uint8_t placement; float mossiness; };

// the piece's processors after the template's own: RuleProcessor, BlockAgeProcessor,
// ProtectedBlockProcessor, LavaSubmergedBlockProcessor, BlackstoneReplaceProcessor
uint16_t portalProcess(void* ctx, uint16_t st, uint16_t world, int X, int Y, int Z, bool& drop) {
    const PortalCtx& p = *(const PortalCtx*)ctx;
    uint16_t id = blockIdOf(st);
    {   // the rules: gold gone at 30 %, lava, netherrack to magma at 7 % (unless cold)
        JavaRandom r(positionSeed(X, Y, Z));
        bool done = false;
        if (id == N.gold && r.nextFloat() < 0.3f) {
            st = 0;
            done = true;
        }
        if (!done && id == N.lava) {
            if (p.placement == RP_OCEAN_FLOOR) { st = N.magma; done = true; }
            else if (p.flags & RPF_COLD) { st = N.netherrack; done = true; }
            else if (r.nextFloat() < 0.2f) { st = N.magma; done = true; }
        }
        if (!done && !(p.flags & RPF_COLD) && id == blockIdOf(N.netherrack) && r.nextFloat() < 0.07f) st = N.magma;
    }
    id = blockIdOf(st);
    {   // BlockAgeProcessor(mossiness)
        JavaRandom r(positionSeed(X, Y, Z));
        auto pick = [&](const uint16_t* nonMossy, int nn, const uint16_t* mossy, int nm) {
            return r.nextFloat() < p.mossiness ? mossy[r.nextInt(nm)] : nonMossy[r.nextInt(nn)];
        };
        auto randomStairs = [&](uint16_t base) {
            static const char* const D[4] = {"north", "east", "south", "west"};
            uint16_t s = setPropStr(base, "facing", D[r.nextInt(4)]);
            return setPropStr(s, "half", r.nextInt(2) ? "bottom" : "top");
        };
        if (id == N.stoneBricks || id == N.stone || id == N.chiseledStoneBricks) {
            if (r.nextFloat() < 0.5f) {
                uint16_t a[2] = {N.crackedStoneBricks, randomStairs(N.stoneBrickStairs)};
                uint16_t b[2] = {N.mossyStoneBricks, randomStairs(N.mossyStoneBrickStairs)};
                st = pick(a, 2, b, 2);
            }
        } else if (propIndexOf(id, "facing") >= 0 && propIndexOf(id, "half") >= 0 && propIndexOf(id, "shape") >= 0) {   // stairs
            if (r.nextFloat() < 0.5f) {
                uint16_t m = setPropStr(setPropStr(N.mossyStoneBrickStairs, "facing", getPropStr(st, "facing")), "half", getPropStr(st, "half"));
                uint16_t a[2] = {N.stoneSlab, N.stoneBrickSlab};
                uint16_t b[2] = {m, N.mossyStoneBrickSlab};
                st = pick(a, 2, b, 2);
            }
        } else if (propIndexOf(id, "type") >= 0 && strstr(BLOCKS[id].name, "_slab")) {
            if (r.nextFloat() < p.mossiness) st = N.mossyStoneBrickSlab;
        } else if (strstr(BLOCKS[id].name, "_wall") && propIndexOf(id, "up") >= 0) {
            if (r.nextFloat() < p.mossiness) st = N.mossyStoneBrickWall;
        } else if (id == N.obsidian) {
            if (r.nextFloat() < 0.15f) st = N.cryingObsidian;
        }
    }
    // ProtectedBlockProcessor(features_cannot_replace)
    uint16_t wid = blockIdOf(world);
    for (int i = 0; i < N.nProtected; i++)
        if (wid == N.protectedIds[i]) {
            drop = true;
            return st;
        }
    // LavaSubmergedBlockProcessor: in lava, lava stays where the block is not a full one
    if (wid == N.lava && !collisionFullBlock(st)) st = BLOCKS[N.lava].defState;
    if (p.flags & RPF_BLACKSTONE) {   // BlackstoneReplaceProcessor
        uint16_t sid = blockIdOf(st);
        for (int i = 0; i < N.nMap; i++)
            if (sid == N.from[i]) {
                st = withSameProps(st, N.to[i]);
                break;
            }
    }
    return st;
}

bool hasBiomeAt(const uint8_t* bits, uint8_t b) { return b < 128 && (bits[b >> 3] >> (b & 7) & 1); }

// a random number of the block's own for the spreading around (the same in every chunk)
float rnd(int64_t seed, int x, int y, int z, uint32_t salt) { return posRandom(seed, x, y, z, salt); }

}  // namespace

int ruinedPortalPieces(JavaRandom& r, int cx, int cz, const StructureDef& st, uint8_t dim, TerrainProbe& probe, Piece* out, int max) {
    resolveNames();
    if (max < 2 || !st.setupCount) return 0;
    // RuinedPortalStructure#findGenerationPoint: a setup by weight
    const PortalSetup* s = &SD_PORTAL_SETUPS[st.setupFirst];
    if (st.setupCount > 1) {
        float total = 0;
        for (int i = 0; i < st.setupCount; i++) total += SD_PORTAL_SETUPS[st.setupFirst + i].weight;
        float f = r.nextFloat();
        for (int i = 0; i < st.setupCount; i++) {
            f -= SD_PORTAL_SETUPS[st.setupFirst + i].weight / total;
            if (f < 0.0f) {
                s = &SD_PORTAL_SETUPS[st.setupFirst + i];
                break;
            }
        }
    }
    int16_t flags = 0;
    if (s->airPocket >= 1.0f || (s->airPocket > 0.0f && r.nextFloat() < s->airPocket)) flags |= RPF_AIR_POCKET;
    if (s->overgrown) flags |= RPF_OVERGROWN;
    if (s->vines) flags |= RPF_VINES;
    if (s->blackstone) flags |= RPF_BLACKSTONE;
    char name[48];
    if (r.nextFloat() < 0.05f) snprintf(name, sizeof(name), "ruined_portal/giant_portal_%d", (int)(r.nextInt(3) + 1));
    else snprintf(name, sizeof(name), "ruined_portal/portal_%d", (int)(r.nextInt(10) + 1));
    int t = templateNamed(name);
    if (t < 0) return 0;
    const TdTemplate& tp = TD_TEMPLATES[t];
    int rot = r.nextInt(4);   // Util.getRandom(Rotation.values())
    int mirror = r.nextFloat() < 0.5f ? 0 : 2;   // NONE or FRONT_BACK
    int px = tp.sx / 2, pz = tp.sz / 2;
    int x0 = cx * 16, z0 = cz * 16;
    SBox box = templateBox(t, x0, 0, z0, rot, mirror, px, pz);
    int midX = box.x0 + (box.x1 - box.x0 + 1) / 2, midZ = box.z0 + (box.z1 - box.z0 + 1) / 2;   // BoundingBox#getCenter
    const bool floor = s->placement == RP_OCEAN_FLOOR;
    int surface = probe.height(midX, midZ, floor) - 1;
    int ySpan = tp.sy;
    // findSuitableY
    const int minY = dim == DIM_NETHER ? 0 : -64, sea = dim == DIM_NETHER ? 32 : 63;
    int j = minY + 15, k;
    auto within = [&](int lo, int hi) { return lo < hi ? between(r, lo, hi) : hi; };
    switch (s->placement) {
        case RP_NETHER:
            if (flags & RPF_AIR_POCKET) k = between(r, 32, 100);
            else if (r.nextFloat() < 0.5f) k = between(r, 27, 29);
            else k = between(r, 29, 100);
            break;
        case RP_MOUNTAIN: k = within(70, surface - ySpan); break;
        case RP_UNDERGROUND: k = within(j, surface - ySpan); break;
        case RP_PARTLY_BURIED: k = surface - ySpan + between(r, 2, 8); break;
        default: k = surface; break;
    }
    const int corners[4][2] = {{box.x0, box.z0}, {box.x1, box.z0}, {box.x0, box.z1}, {box.x1, box.z1}};
    int y;
    for (y = k; y > j; y--) {
        int n = 0;
        for (const auto& c : corners) {
            // the base column: ground, or the sea's water and lava (not air: WORLD_SURFACE_WG)
            bool opaque = probe.solid(c[0], y, c[1]) || (!floor && y < sea);
            if (opaque && ++n == 3) break;
        }
        if (n >= 3) break;
    }
    if (!hasBiomeAt(st.biomes, probe.biome(x0, y, z0))) return 0;
    if (s->canBeCold) {   // Biome#coldEnoughToSnow at the start
        uint8_t b = probe.biome(x0, y, z0);
        float temp = b < 65 ? BIOME_TEMPERATURE[b] : 0.5f;
        if (y > sea + 17) temp -= (float)(y - (sea + 17)) * 0.05f / 40.0f;
        if (temp < 0.15f) flags |= RPF_COLD;
    }
    Piece& p = out[0];
    p = {};
    // its box with the netherrack spreading up to 14 blocks around and its drip columns
    p.box = templateBox(t, x0, y, z0, rot, mirror, px, pz);
    p.box = {p.box.x0 - 14, p.box.y0 - 12, p.box.z0 - 14, p.box.x1 + 14, p.box.y1 + 4, p.box.z1 + 14};
    p.kind = PK_TEMPLATE;
    p.orient = (uint8_t)rot;
    p.a = (int16_t)t;
    p.b = (int32_t)(((uint32_t)(x0 - p.box.x0) & 0xFFu) | ((uint32_t)(z0 - p.box.z0) & 0xFFu) << 8 | (uint32_t)rot << 16 | 5u << 18 |
                    (uint32_t)(mirror ? 1 : 0) << 22);
    // its properties in a second piece (no blocks of its own)
    Piece& q = out[1];
    q = {};
    q.box = p.box;
    q.kind = PK_PORTAL_PROPS;
    q.a = (int16_t)(flags | s->placement << 8);
    q.b = (int32_t)(s->mossiness * 100.0f + 0.5f);
    q.box.y0 = y;   // the template's own y
    return 2;
}

void buildRuinedPortal(Chunk& c, const SBox& clip, const Piece& p, const Piece& props, int64_t seed) {
    resolveNames();
    uint32_t b = (uint32_t)p.b;
    int x = p.box.x0 + (int8_t)(b & 0xFF), z = p.box.z0 + (int8_t)((b >> 8) & 0xFF), y = props.box.y0;
    int rot = (b >> 16) & 3, mirror = (b >> 22) & 1 ? 2 : 0;
    const TdTemplate& tp = TD_TEMPLATES[p.a];
    const SBox bx = templateBox(p.a, x, y, z, rot, mirror, tp.sx / 2, tp.sz / 2);   // the portal's own box
    PortalCtx ctx = {(int16_t)(props.a & 0xFF), (uint8_t)((props.a >> 8) & 0xFF), props.b / 100.0f};
    TemplatePlacement t = {p.a, x, y, z, (uint8_t)rot, (uint8_t)mirror, 0, !(ctx.flags & RPF_AIR_POCKET), false, false, false, false};
    t.pivotX = tp.sx / 2;
    t.pivotZ = tp.sz / 2;
    t.process = portalProcess;
    t.processCtx = &ctx;
    placeTemplate(c, clip, t, seed);
    const bool surface = ctx.placement == RP_LAND || ctx.placement == RP_OCEAN_FLOOR;
    auto inChunk = [&](int X, int Y, int Z) { return clip.inside(X, Y, Z) && c.hasY(Y); };
    auto placeNetherrack = [&](int X, int Y, int Z, uint32_t salt) {
        if (!inChunk(X, Y, Z)) return;
        bool magma = !(ctx.flags & RPF_COLD) && rnd(seed, X, Y, Z, salt) < 0.07f;
        c.set(X & 15, Y, Z & 15, magma ? N.magma : N.netherrack);
    };
    auto dripColumn = [&](int X, int Y, int Z) {   // addNetherrackDripColumn
        placeNetherrack(X, Y, Z, 1);
        for (int i = 8; i > 0 && rnd(seed, X, Y, Z, 20 + i) < 0.5f; i--) placeNetherrack(X, --Y, Z, 2);
    };
    auto leavesAbove = [&](int X, int Y, int Z) {   // maybeAddLeavesAbove
        if (!inChunk(X, Y + 1, Z) || rnd(seed, X, Y, Z, 3) >= 0.5f) return;
        if (blockIdOf(c.get(X & 15, Y, Z & 15)) == blockIdOf(N.netherrack) && stateIsAir(c.get(X & 15, Y + 1, Z & 15)))
            c.set(X & 15, Y + 1, Z & 15, N.jungleLeaves);
    };
    // spreadNetherrack: around the middle on the ground, thinning out
    static const float SPREAD[14] = {1, 1, 1, 1, 1, 1, 1, 0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
    int mx = bx.x0 + (bx.x1 - bx.x0 + 1) / 2, mz = bx.z0 + (bx.z1 - bx.z0 + 1) / 2;
    int l = (tp.sx + tp.sz) / 2;
    int bound = 8 - l / 2 > 1 ? 8 - l / 2 : 1;
    int i1 = (int)(rnd(seed, mx, 0, mz, 4) * bound);
    for (int X = mx - 14; X <= mx + 14; X++)
        for (int Z = mz - 14; Z <= mz + 14; Z++) {
            int d = abs(X - mx) + abs(Z - mz) + i1;
            if (d >= 14 || rnd(seed, X, 0, Z, 5) >= SPREAD[d]) continue;
            if (!clip.inside(X, clip.y0 + 1, Z)) continue;
            int top = c.maxY();   // the chunk's surface (WORLD_SURFACE_WG, or the ocean floor's)
            while (top > c.minY()) {
                uint16_t s = c.get(X & 15, top, Z & 15);
                if (!stateIsAir(s) && (ctx.placement != RP_OCEAN_FLOOR || stateCollides(s))) break;
                top--;
            }
            int Y = surface ? top : (top < bx.y0 ? top : bx.y0);
            if (abs(Y - bx.y0) > 3) continue;
            uint16_t at = c.get(X & 15, Y, Z & 15), aid = blockIdOf(at);
            bool can = !stateIsAir(at) && aid != N.obsidian && (ctx.placement == RP_NETHER || aid != N.lava);
            for (int k = 0; k < N.nProtected && can; k++) can = aid != N.protectedIds[k];
            if (!can) continue;
            placeNetherrack(X, Y, Z, 6);
            if (ctx.flags & RPF_OVERGROWN) leavesAbove(X, Y, Z);
            dripColumn(X, Y - 1, Z);
        }
    // addNetherrackDripColumnsBelowPortal
    for (int X = bx.x0 + 1; X < bx.x1; X++)
        for (int Z = bx.z0 + 1; Z < bx.z1; Z++)
            if (inChunk(X, bx.y0, Z) && blockIdOf(c.get(X & 15, bx.y0, Z & 15)) == blockIdOf(N.netherrack)) dripColumn(X, bx.y0 - 1, Z);
    // vines and leaves over its blocks
    if (ctx.flags & (RPF_VINES | RPF_OVERGROWN))
        for (int Y = bx.y0; Y <= bx.y1; Y++)
            for (int X = bx.x0; X <= bx.x1; X++)
                for (int Z = bx.z0; Z <= bx.z1; Z++) {
                    if (!inChunk(X, Y, Z)) continue;
                    uint16_t s = c.get(X & 15, Y, Z & 15);
                    if ((ctx.flags & RPF_VINES) && !stateIsAir(s) && blockIdOf(s) != blockIdOf(N.vine)) {   // maybeAddVines
                        static const int DX[4] = {0, 1, 0, -1}, DZ[4] = {-1, 0, 1, 0};
                        static const char* const FACE[4] = {"south", "west", "north", "east"};   // the side facing back
                        int d = (int)(rnd(seed, X, Y, Z, 7) * 4);
                        int nx = X + DX[d], nz = Z + DZ[d];
                        if (inChunk(nx, Y, nz) && stateIsAir(c.get(nx & 15, Y, nz & 15)) && collisionFullBlock(s))
                            c.set(nx & 15, Y, nz & 15, setBool(N.vine, FACE[d], true));
                    }
                    if (ctx.flags & RPF_OVERGROWN) leavesAbove(X, Y, Z);
                }
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
