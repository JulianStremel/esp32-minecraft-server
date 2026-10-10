#include "mc/bench.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mc/io.h"
#include "mc/jobs.h"
#include "mc/net/deflate.h"
#include "mc/platform.h"
#include "mc/server/chunk_codec.h"
#include "mc/storage/block_device.h"
#include "mc/storage/world_store.h"
#include "mc/world/light.h"
#include "mc/world/vanilla/aquifer.h"
#include "mc/world/vanilla/carvers.h"
#include "mc/world/vanilla/density.h"
#include "mc/world/vanilla/surface.h"
#include "mc/world/world.h"

namespace mc {

namespace {

void out(void (*print)(const char*), const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void out(void (*print)(const char*), const char* fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    print(line);
}

struct Stage {
    const char* name;
    uint64_t us = 0;
    uint64_t maxUs = 0;
    int n = 0;
    void add(uint64_t t) {
        us += t;
        if (t > maxUs) maxUs = t;
        n++;
    }
    double avgMs() const { return n ? us / 1000.0 / n : 0; }
};

void report(void (*print)(const char*), const Stage& s) {
    out(print, "[bench] %-34s %8.2f ms avg  %8.2f ms max  (%d)", s.name, s.avgMs(), s.maxUs / 1000.0, s.n);
}

void benchWorld(WorldType type, int R, void (*print)(const char*)) {
    Generator gen;
    gen.init(42, type);
    World world;
    int side = 2 * R + 1;
    Generator* gens[NUM_DIMS] = {&gen, nullptr, nullptr};
    world.init(gens, nullptr, side * side + 8, 64);

    Stage gen1{type == WORLD_FLAT ? "generate (flat)" : "generate (normal terrain)"};
    for (int cz = -R; cz <= R; cz++)
        for (int cx = -R; cx <= R; cx++) {
            uint64_t t = plat::micros();
            world.load(DIM_OVERWORLD, cx, cz);
            gen1.add(plat::micros() - t);
        }
    report(print, gen1);
    if (type == WORLD_NORMAL) {
        // generator versions side by side (version 1 is kept for worlds created with it)
        for (uint8_t v = 1; v <= GENERATOR_MAX; v++) {
            Generator g;
            g.init(42, type, v);
            char name[48];
            snprintf(name, sizeof(name), "generator v%d only (%s)", v,
                     v == 1 ? "old worlds" : v == 2 ? "new worlds" : "vanilla terrain");
            Stage st{name};
            Stage steps[5] = {{"  shape (router)"}, {"  sections written"}, {"  biomes"}, {"  surface"},
                              {"  ores, trees, plants"}};
            for (int cz = -R; cz <= R; cz++)
                for (int cx = -R; cx <= R; cx++) {
                    Chunk* c = new Chunk(cx, cz);
                    uint64_t t = plat::micros();
                    g.generate(*c);
                    st.add(plat::micros() - t);
                    const Generator::VanillaSteps& vs = g.vanillaSteps;
                    const uint32_t each[5] = {vs.shape, vs.write, vs.biomes, vs.surface, vs.features};
                    if (isVanillaGenerator(v))
                        for (int k = 0; k < 5; k++) steps[k].add(each[k]);
                    delete c;
                }
            report(print, st);
            if (isVanillaGenerator(v))
                for (int k = 0; k < 5; k++) report(print, steps[k]);
            if (v >= GENERATOR_VANILLA_SURFACE) {   // the surface rule's parts, one chunk
                Chunk* c = new Chunk(3, 5);
                g.generate(*c);
                delete c;
                const vanilla::SurfaceSteps& ss = vanilla::surfaceSteps;
                out(print, "[bench]     surface rule: preliminary surface %u us, columns %u us, rules %u us (%u blocks)",
                    (unsigned)ss.prelimUs, (unsigned)ss.columnsUs, (unsigned)ss.rulesUs, (unsigned)ss.evaluated);
                if (v >= GENERATOR_VANILLA_AQUIFERS) {
                    const vanilla::AquiferSteps& as = vanilla::aquiferSteps;
                    out(print, "[bench]     aquifer: %u us (%u cave blocks, %u fluid statuses in %u us, %u barriers), veins %u us (%u blocks)",
                        (unsigned)as.blocksUs, (unsigned)as.cave, (unsigned)as.statuses, (unsigned)as.statusUs, (unsigned)as.barriers,
                        (unsigned)as.veinsUs, (unsigned)as.veinBlocks);
                    out(print, "[bench]     veins: toggle %u us, %u cells, %u blocks tested, %u ridge corners", (unsigned)as.toggleUs,
                        (unsigned)as.veinCells, (unsigned)as.veinTested, (unsigned)as.ridgeCorners);
                    if (v >= GENERATOR_VANILLA_CARVERS) {
                        const vanilla::CarverSteps& cs = vanilla::carverSteps;
                        out(print, "[bench]     carvers: %u us (%u starts, %u tunnel steps, %u ellipsoids, %u cells, %u blocks)",
                            (unsigned)cs.us, (unsigned)cs.starts, (unsigned)cs.steps, (unsigned)cs.ellipsoids, (unsigned)cs.cells,
                            (unsigned)cs.carved);
                    }
                }
            }
        }
        // version 3's terrain shape alone (the router), its two passes
        vanilla::Router* r = new vanilla::Router();
        uint8_t* solid = (uint8_t*)plat::bigAlloc(16 * 16 * vanilla::DF_SHAPE.height);
        if (r && solid && r->init(42, 1.0f / 64, 0.1f, true, true)) {
            Stage shape{"v3 shape (router)"}, corners{"  corners (2D table, air bound)"}, bound{"  of it the 2D table and bound"},
                blocks{"  blocks"}, signs{"  of it the sign slot"}, fulls{"  of it the rest"};
            uint32_t evaluated = 0;
            for (int cz = -R; cz <= R; cz++)
                for (int cx = -R; cx <= R; cx++) {
                    r->fillChunk(cx, cz, solid);
                    shape.add(r->cornerUs + r->blockUs);
                    corners.add(r->cornerUs);
                    bound.add(r->boundUs);
                    signs.add(r->signUs);
                    fulls.add(r->fullUs);
                    blocks.add(r->blockUs);
                    evaluated += r->cornersEvaluated;
                }
            report(print, shape);
            report(print, corners);
            report(print, bound);
            report(print, signs);
            report(print, fulls);
            report(print, blocks);
            // the noise sampler alone: one octave sample, and the router's share of a corner
            {
                out(print, "[bench]   octave samples: %u (the last chunk)", (unsigned)r->octaveSamples);
                uint32_t before = r->octaveSamples;
                uint64_t t = plat::micros();
                float sink = 0;
                for (int i = 0; i < 2000; i++) sink += r->noiseValue(13, i * 0.37f, i * 0.11f, i * 0.53f);
                uint64_t us = plat::micros() - t;
                uint32_t n = r->octaveSamples - before;
                out(print, "[bench]   one octave sample: %.2f us (%u samples)%s", n ? (double)us / n : 0.0, (unsigned)n,
                    sink == 12345 ? "" : "");
                int bn = -1;
                for (int i = 0; i < vanilla::NUM_DF_NODES; i++)
                    if (vanilla::DF_NODES[i].op == vanilla::DF_OLD_BLENDED_NOISE) bn = i;
                if (bn >= 0) {
                    before = r->octaveSamples;
                    t = plat::micros();
                    for (int i = 0; i < 500; i++) sink += r->blended(vanilla::DF_NODES[bn], i * 4, (i % 40) * 8 - 64, i * 12);
                    us = plat::micros() - t;
                    n = r->octaveSamples - before;
                    out(print, "[bench]   blended noise: %.2f us a sample, %.1f octave samples (500)%s", us / 500.0, n / 500.0,
                        sink == 12345 ? "" : "");
                }
            }
            out(print, "[bench]   corners evaluated: %u a chunk (of %d)", (unsigned)(evaluated / (side * side)),
                5 * 5 * (vanilla::DF_SHAPE.height / vanilla::DF_SHAPE.cellH + 1));
        }
        plat::bigFree(solid);
        delete r;
    }
    if (type != WORLD_NORMAL) return;

    const size_t CAP = 96 * 1024;
    uint8_t* raw = (uint8_t*)plat::bigAlloc(CAP);
    uint8_t* comp = (uint8_t*)plat::bigAlloc(CAP);
    // the store's layout (world_store.cpp): superblocks and players in the first MiB, the
    // region directory (256 KiB at least), then a unit (two copies) per saved chunk and
    // per region map; the chunks here span up to 4 regions
    const size_t units = (size_t)(2 * R - 1) * (2 * R - 1) + 4;
    MemDevice dev((1u << 20) + (256u << 10) + units * 2 * 16384 + 65536);
    WorldStore store(&dev);
    StoreParams sp;
    sp.radius = R;
    sp.chunkSlotSize = 16384;
    sp.playerSlots = 16;
    bool haveStore = raw && comp && dev.ok() && store.open(sp, true);
    if (!haveStore) {
        out(print, "[bench] not enough memory for the encode/store stages");
        plat::bigFree(raw);
        plat::bigFree(comp);
        return;
    }

    Stage light{"light (sky + block, with neighbours)"};
    Stage lightRegion{"light region (exact, 3x3 chunks)"};
    Stage phC[ChunkLight::PHASES] = {{"  per chunk: fill grid"}, {"  per chunk: sky"}, {"  per chunk: block"},
                                     {"  per chunk: output"}, {"  per chunk: sky, direct part"}};
    Stage phR[ChunkLight::PHASES] = {{"  region: fill grid"}, {"  region: sky"}, {"  region: block"},
                                     {"  region: output"}, {"  region: sky, direct part"}};
    Stage encChunk{"encode chunk packet (with light)"};
    Stage encLight{"encode light packet"};
    Stage defChunk{"deflate chunk packet"};
    Stage defLight{"deflate light packet"};
    Stage save{"store: save (2x deflate + CRC)"};
    Stage load{"store: load (inflate + CRC + decode)"};
    size_t rawChunk = 0, compChunk = 0, rawLight = 0, compLight = 0;
    ChunkLight L, LR;
    uint64_t pushC = 0, pushR = 0;
    for (int cz = -R + 1; cz < R; cz++)
        for (int cx = -R + 1; cx < R; cx++) {
            Chunk* c = world.get(DIM_OVERWORLD, cx, cz);
            uint64_t t = plat::micros();
            L.compute(*c, &world);
            light.add(plat::micros() - t);
            for (int k = 0; k < ChunkLight::PHASES; k++) phC[k].add(L.phaseUs[k]);
            pushC += L.skyPushes;
            {
                const Chunk* nine[9];
                bool all = true;
                for (int k = 0; k < 9 && all; k++) all = (nine[k] = world.peek(DIM_OVERWORLD, cx + k % 3 - 1, cz + k / 3 - 1)) != nullptr;
                if (all) {   // LR keeps its scratch buffers between chunks, as a worker does
                    t = plat::micros();
                    LR.computeRegion(nine);
                    lightRegion.add(plat::micros() - t);
                    for (int k = 0; k < ChunkLight::PHASES; k++) phR[k].add(LR.phaseUs[k]);
                    pushR += LR.skyPushes;
                }
            }

            BufSink bs(raw, CAP);
            {
                Writer w(bs);
                t = plat::micros();
                writeChunkPacket(w, *c, L);   // light included (1.21.8)
                encChunk.add(plat::micros() - t);
            }
            BufSink cs(comp, CAP);
            t = plat::micros();
            {
                DeflateSink d(cs);
                d.put(raw, bs.size());
                d.finish();
            }
            defChunk.add(plat::micros() - t);
            rawChunk += bs.size();
            compChunk += cs.size();

            bs.reset();
            {
                Writer w(bs);
                t = plat::micros();
                writeLightPacket(w, *c, L, false);
                encLight.add(plat::micros() - t);
            }
            cs.reset();
            t = plat::micros();
            {
                DeflateSink d(cs);
                d.put(raw, bs.size());
                d.finish();
            }
            defLight.add(plat::micros() - t);
            rawLight += bs.size();
            compLight += cs.size();

            t = plat::micros();
            bool ok = store.saveChunk(*c);
            save.add(plat::micros() - t);
            Chunk* back = new Chunk(cx, cz);
            t = plat::micros();
            LoadResult lr = store.loadChunk(*back);
            load.add(plat::micros() - t);
            if (!ok || lr != LOAD_OK) out(print, "[bench] store round trip failed for %d,%d", cx, cz);
            delete back;
        }
    report(print, light);
    for (const Stage& st : phC) report(print, st);
    report(print, lightRegion);
    for (const Stage& st : phR) report(print, st);
    if (light.n && lightRegion.n)
        out(print, "[bench]   sky flood pushes: per chunk %u, region %u (avg)", (unsigned)(pushC / light.n),
            (unsigned)(pushR / lightRegion.n));
    report(print, encChunk);
    report(print, defChunk);
    report(print, encLight);
    report(print, defLight);
    report(print, save);
    report(print, load);
    int n = light.n;
    out(print, "[bench] chunk packet %u -> %u bytes, light packet %u -> %u bytes (avg)",
        (unsigned)(rawChunk / n), (unsigned)(compChunk / n), (unsigned)(rawLight / n), (unsigned)(compLight / n));
    // what Player::sendChunk costs today: light + (count pass + deflate pass) per packet
    double send = light.avgMs() + 2 * encChunk.avgMs() + defChunk.avgMs() + 2 * encLight.avgMs() + defLight.avgMs();
    double first = gen1.avgMs() + send;
    out(print, "[bench] => sending a resident chunk: %.2f ms; a new chunk (generate + send): %.2f ms", send, first);
    out(print, "[bench] => one core streams about %.0f new chunks/s (a 50 ms tick fits %.1f)", 1000.0 / first,
        50.0 / first);
    plat::bigFree(raw);
    plat::bigFree(comp);
}

struct GenJob : Job {
    const Generator* gen;
    int cx, cz;
    GenJob(const Generator* g, int x, int z) : gen(g), cx(x), cz(z) {}
    void run(WorkerScratch&) override {
        Chunk* c = new Chunk(cx, cz);
        if (c) gen->generate(*c);
        delete c;
    }
    void finish() override {}
};

// Registers only, no memory traffic: shows how much parallelism the machine (or the
// emulator) offers at all, as a reference for the generation numbers.
struct AluJob : Job {
    uint32_t x = 1;
    void run(WorkerScratch&) override {
        uint32_t v = x;
        for (int i = 0; i < 400000; i++) {
            v ^= v << 13;
            v ^= v >> 17;
            v ^= v << 5;
        }
        x = v;
    }
    void finish() override {}
};

// Throughput through the job queue: 0 workers (on the calling thread), 1 worker,
// one per core.
void benchWorkers(void (*print)(const char*)) {
    Generator gen;
    gen.init(42, WORLD_NORMAL);
    const int N = 24;
    int cores = plat::cpuCores();
    for (int kind = 0; kind < 2; kind++) {
        double base = 0;
        for (int workers = 0; workers <= cores && workers <= 2; workers++) {
            JobQueue q;
            q.start(workers);
            uint64_t t0 = plat::micros();
            for (int i = 0; i < N; i++) {
                if (kind == 0) q.submit(new GenJob(&gen, 100 + i % 6, 100 + i / 6));
                else q.submit(new AluJob());
            }
            q.drain();
            double ms = (plat::micros() - t0) / 1000.0;
            if (workers == 0) base = ms;
            out(print, "[bench] %-17s x%d, %d worker%s %s: %8.1f ms  (%5.1f jobs/s, x%.2f)",
                kind == 0 ? "generate chunk" : "ALU-only (ref.)", N, workers, workers == 1 ? " " : "s",
                workers == 0 ? "(game loop)" : "           ", ms, N * 1000.0 / ms, base / ms);
            q.stop();
        }
    }
}

}  // namespace

// The generator must produce the same blocks here as on the PC: compare with the
// fingerprints the PC build computed (GENERATOR_GOLDEN). tools/emulator/run.sh --bench
// fails on "generator check FAILED".
static void checkGenerator(void (*print)(const char*)) {
    int bad = 0;
    if (!generatorArithmeticIsPortable()) {
        bad++;
        out(print, "[bench] generator: compiled with fused multiply-add (build with -ffp-contract=off)");
    }
    for (int i = 0; i < NUM_GENERATOR_GOLDEN; i++) {
        const GeneratorGolden& g = GENERATOR_GOLDEN[i];
        uint64_t t0 = plat::micros();
        uint32_t blocks = generatorFingerprint(g.seed, g.version);
        uint32_t floats = generatorFloatFingerprint(g.seed, g.version);
        bool ok = blocks == g.blocks && floats == g.floats;
        if (!ok) bad++;
        out(print, "[bench] generator v%d seed %llu: fingerprints %08x %08x, PC %08x %08x: %s (%.0f ms)", g.version,
            (unsigned long long)g.seed, (unsigned)blocks, (unsigned)floats, (unsigned)g.blocks, (unsigned)g.floats,
            ok ? "same" : "DIFFERENT", (plat::micros() - t0) / 1000.0);
    }
    for (int i = 0; i < NUM_GENERATOR_DIM_GOLDEN; i++) {
        const GeneratorDimGolden& g = GENERATOR_DIM_GOLDEN[i];
        uint64_t t0 = plat::micros();
        uint32_t nether = generatorDimFingerprint(g.seed, DIM_NETHER), end = generatorDimFingerprint(g.seed, DIM_END);
        bool ok = nether == g.nether && end == g.end;
        if (!ok) bad++;
        out(print, "[bench] nether/end seed %llu: fingerprints %08x %08x, PC %08x %08x: %s (%.0f ms)",
            (unsigned long long)g.seed, (unsigned)nether, (unsigned)end, (unsigned)g.nether, (unsigned)g.end,
            ok ? "same" : "DIFFERENT", (plat::micros() - t0) / 1000.0);
    }
    out(print, "[bench] generator: %s", bad ? "DIFFERENT from the PC build, generator check FAILED"
                                            : "bit-identical to the PC build");
}

void runChunkBench(int radius, void (*print)(const char*)) {
    if (radius < 1) radius = 1;
    checkGenerator(print);
    out(print, "[bench] chunk pipeline, %d chunks generated, %d fully processed", (2 * radius + 1) * (2 * radius + 1),
        (2 * radius - 1) * (2 * radius - 1));
    uint64_t t0 = plat::micros();
    benchWorld(WORLD_NORMAL, radius, print);
    benchWorld(WORLD_FLAT, radius, print);
    benchWorkers(print);
    out(print, "[bench] total %.1f ms, free heap %u KB", (plat::micros() - t0) / 1000.0, (unsigned)(plat::freeHeap() / 1024));
}

}  // namespace mc
