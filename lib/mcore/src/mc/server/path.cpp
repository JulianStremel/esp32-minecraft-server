#include "mc/server/path.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "mc/platform.h"
#include "mc/registry.h"

namespace mc {

namespace {

struct Area {
    const PathRequest& r;
    int ox, oz;   // world coordinates of the area's (0, 0)
    explicit Area(const PathRequest& req) : r(req), ox((req.cx - 1) * 16), oz((req.cz - 1) * 16) {}
    // outside the 3 x 3 chunks, or in a missing chunk: solid
    uint16_t get(int x, int y, int z) const {
        if (y < 0 || y >= WORLD_HEIGHT) return y < 0 ? bs::Bedrock : 0;
        int ax = x - ox, az = z - oz;
        if (ax < 0 || ax >= 48 || az < 0 || az >= 48) return bs::Stone;
        const Chunk* c = r.nine[(az >> 4) * 3 + (ax >> 4)];
        return c ? c->get(ax & 15, y, az & 15) : bs::Stone;
    }
    static bool burns(uint16_t st) {
        uint16_t id = blockIdOf(st);
        return id == blk::Lava || id == blk::Fire || id == blk::SoulFire;
    }
    // can a mob's body be in this block? Not above a fence, wall or closed gate either:
    // their collision box is 1.5 blocks high, so they cannot be stood or stepped on
    bool open(int x, int y, int z) const {
        uint16_t st = get(x, y, z);
        return !stateCollides(st) && !burns(st) && collisionTop32(get(x, y - 1, z)) <= 32;
    }
    bool body(int x, int y, int z) const {
        for (int h = 0; h < r.height; h++)
            if (!open(x, y + h, z)) return false;
        return true;
    }
    // a place to stand: room for the body, and something to stand on (or water)
    bool stand(int x, int y, int z) const {
        if (!body(x, y, z)) return false;
        uint16_t below = get(x, y - 1, z);
        return (stateCollides(below) && !burns(below)) || blockIdOf(get(x, y, z)) == blk::Water;
    }
    // extra cost: next to a cactus or a fire, or wading
    float penalty(int x, int y, int z) const {
        float p = 0;
        static const int8_t D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : D) {
            uint16_t id = blockIdOf(get(x + d[0], y, z + d[1]));
            if (id == blk::Cactus || id == blk::Fire || id == blk::Lava) p += 8;
        }
        if (blockIdOf(get(x, y, z)) == blk::Water) p += 2;
        return p;
    }
};

struct Node {
    int16_t x, z;   // relative to the area
    int16_t y;
    int16_t parent;
    float g, f;
    bool closed;
};

}  // namespace

bool findPath(const PathRequest& req, PathResult& out) {
    out.n = 0;
    out.reached = false;
    out.nodes = 0;
    Area a(req);
    const PathPoint& s = req.start;
    if (!a.body(s.x, s.y, s.z)) return false;
    const int maxNodes = req.maxNodes > 32000 ? 32000 : req.maxNodes;
    const int HASH = 4096;   // power of two, > 2 * maxNodes for the default budget
    // PSRAM: about 22 KB per search would otherwise come out of the internal RAM that
    // WiFi and lwIP need (allocations up to 16 KB go there on the ESP32)
    Node* nodes = (Node*)plat::bigAlloc(sizeof(Node) * (size_t)maxNodes);
    int16_t* hash = (int16_t*)plat::bigAlloc(sizeof(int16_t) * HASH);
    int16_t* heap = (int16_t*)plat::bigAlloc(sizeof(int16_t) * (size_t)maxNodes);
    if (!nodes || !hash || !heap) {
        plat::bigFree(nodes);
        plat::bigFree(hash);
        plat::bigFree(heap);
        return false;
    }
    memset(hash, 0xFF, sizeof(int16_t) * HASH);
    int count = 0, heapN = 0;

    auto key = [](int x, int y, int z) { return (uint32_t)((y * 48 + z) * 48 + x); };
    auto slotOf = [&](int x, int y, int z) {
        uint32_t k = key(x, y, z);
        uint32_t h = (k * 2654435761u) >> 20 & (HASH - 1);
        while (hash[h] >= 0) {
            const Node& n = nodes[hash[h]];
            if (n.x == x && n.y == y && n.z == z) break;
            h = (h + 1) & (HASH - 1);
        }
        return h;
    };
    auto heur = [&](int x, int y, int z) {
        int dx = abs(x + a.ox - req.goal.x), dz = abs(z + a.oz - req.goal.z), dy = abs(y - req.goal.y);
        int lo = dx < dz ? dx : dz, hi = dx < dz ? dz : dx;
        return (float)(hi - lo) + 1.41421356f * (float)lo + (float)dy;
    };
    auto less = [&](int i, int j) { return nodes[heap[i]].f < nodes[heap[j]].f; };
    auto heapPush = [&](int16_t idx) {
        int i = heapN++;
        heap[i] = idx;
        while (i > 0) {
            int p = (i - 1) / 2;
            if (!less(i, p)) break;
            int16_t t = heap[i]; heap[i] = heap[p]; heap[p] = t;
            i = p;
        }
    };
    auto heapPop = [&]() {
        int16_t top = heap[0];
        heap[0] = heap[--heapN];
        int i = 0;
        for (;;) {
            int l = 2 * i + 1, r = l + 1, m = i;
            if (l < heapN && less(l, m)) m = l;
            if (r < heapN && less(r, m)) m = r;
            if (m == i) break;
            int16_t t = heap[i]; heap[i] = heap[m]; heap[m] = t;
            i = m;
        }
        return top;
    };
    // a node reached at cost g from parent; adds or improves it
    auto visit = [&](int x, int y, int z, float g, int16_t parent) {
        if (count >= maxNodes) return;
        uint32_t h = slotOf(x, y, z);
        if (hash[h] >= 0) {
            Node& n = nodes[hash[h]];
            if (n.closed || g >= n.g) return;
            n.g = g;
            n.f = g + heur(x, y, z);
            n.parent = parent;
            heapPush(hash[h]);   // stale entries are skipped when popped
            return;
        }
        int16_t idx = (int16_t)count++;
        nodes[idx] = Node{(int16_t)x, (int16_t)z, (int16_t)y, parent, g, g + heur(x, y, z), false};
        hash[h] = idx;
        heapPush(idx);
    };

    visit(s.x - a.ox, s.y, s.z - a.oz, 0, -1);
    int best = 0;
    float bestH = 1e30f;
    int end = -1;
    static const int8_t DIRS[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    while (heapN > 0) {
        int16_t ci = heapPop();
        Node cur = nodes[ci];
        if (cur.closed) continue;
        nodes[ci].closed = true;
        out.nodes++;
        int wx = cur.x + a.ox, wz = cur.z + a.oz;
        float h = heur(cur.x, cur.y, cur.z);
        if (h < bestH) { bestH = h; best = ci; }
        if (abs(wx - req.goal.x) <= 1 && abs(wz - req.goal.z) <= 1 && abs(cur.y - req.goal.y) <= 1) {
            end = ci;
            out.reached = true;
            break;
        }
        for (int d = 0; d < 8; d++) {
            int dx = DIRS[d][0], dz = DIRS[d][1];
            int nx = wx + dx, nz = wz + dz;
            bool diag = dx && dz;
            int ny;
            if (diag) {
                // no cutting corners: both sides must be open at this level
                if (!a.body(wx + dx, cur.y, wz) || !a.body(wx, cur.y, wz + dz) || !a.stand(nx, cur.y, nz)) continue;
                ny = cur.y;
            } else if (a.stand(nx, cur.y, nz)) {
                ny = cur.y;
            } else if (a.stand(nx, cur.y + 1, nz) && a.open(wx, cur.y + req.height, wz)) {
                ny = cur.y + 1;   // step (or jump) up one block
            } else if (a.body(nx, cur.y, nz)) {
                // walk off the edge: the first place to stand below, within the drop limit
                ny = -1;
                for (int y = cur.y - 1; y >= cur.y - req.maxDrop && y > 0; y--) {
                    if (a.stand(nx, y, nz)) { ny = y; break; }
                    if (!a.open(nx, y, nz)) break;
                }
                if (ny < 0) continue;
            } else {
                continue;
            }
            float step = diag ? 1.41421356f : 1.0f;
            if (ny > cur.y) step += 0.5f;
            visit(nx - a.ox, ny, nz - a.oz, cur.g + step + a.penalty(nx, ny, nz), ci);
        }
    }
    if (end < 0) end = best;
    // waypoints from the start to `end`, nearest first
    int len = 0;
    for (int i = end; i >= 0 && nodes[i].parent >= 0; i = nodes[i].parent) len++;
    int skip = len > PathResult::MAX ? len - PathResult::MAX : 0;   // keep the first MAX steps
    int k = len - skip;
    out.n = k;
    for (int i = end, j = len; i >= 0 && nodes[i].parent >= 0; i = nodes[i].parent) {
        j--;
        if (j < k) out.points[j] = PathPoint{nodes[i].x + a.ox, nodes[i].z + a.oz, nodes[i].y};
    }
    plat::bigFree(nodes);
    plat::bigFree(hash);
    plat::bigFree(heap);
    return true;
}

}  // namespace mc
