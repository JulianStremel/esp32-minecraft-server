// The woodland mansions: vanilla's WoodlandMansionStructure and WoodlandMansionPieces
// (1.21.8): the 11 x 11 grid of the ground floors (the entrance, corridors grown at random,
// edges cleaned), its rooms (1x1, 1x2, 2x2, secret rooms), the third floor over a stairway
// room; then the pieces in vanilla's order of random numbers: the entrance, the outer walls
// walked around each floor, the roofs, corridors with their carpets, inner walls and doors,
// the rooms. Each piece a template of woodland_mansion/, turned and mirrored.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "mc/platform.h"
#include "mc/world/vanilla/structure_build.h"
#include "mc/world/vanilla/structures.h"
#include "mc/world/vanilla/templates.h"

namespace mc {
namespace vanilla {
namespace sb {

namespace {

// directions: Direction.Plane.HORIZONTAL's order (clockwise), UP for a room entered from above
enum { N = 0, E = 1, S = 2, W = 3, UP = 4, NONE = -1 };
const int SX[4] = {0, 1, 0, -1}, SZ[4] = {-1, 0, 1, 0};
inline int cw(int d) { return (d + 1) & 3; }
inline int ccw(int d) { return (d + 3) & 3; }
inline int opposite(int d) { return (d + 2) & 3; }
const int FROM_2D[4] = {S, W, N, E};   // Direction.from2DDataValue

// SimpleGrid
struct Grid {
    int v[11][11];
    int outside;
    explicit Grid(int out) : outside(out) { memset(v, 0, sizeof(v)); }
    void set(int x, int y, int val) {
        if (x >= 0 && x < 11 && y >= 0 && y < 11) v[x][y] = val;
    }
    void set(int x0, int y0, int x1, int y1, int val) {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) set(x, y, val);
    }
    int get(int x, int y) const { return x >= 0 && x < 11 && y >= 0 && y < 11 ? v[x][y] : outside; }
    void setif(int x, int y, int is, int val) {
        if (get(x, y) == is) set(x, y, val);
    }
    bool edgesTo(int x, int y, int val) const { return get(x - 1, y) == val || get(x + 1, y) == val || get(x, y + 1) == val || get(x, y - 1) == val; }
};

bool isHouse(const Grid& g, int x, int y) {
    int v = g.get(x, y);
    return v == 1 || v == 2 || v == 3 || v == 4;
}

// MansionGrid
struct MansionGrid {
    JavaRandom& r;
    Grid base{5}, third{5};
    Grid rooms[3] = {Grid(5), Grid(5), Grid(5)};
    static const int EX = 7, EY = 4;   // the entrance

    explicit MansionGrid(JavaRandom& rnd) : r(rnd) {
        base.set(EX, EY, EX + 1, EY + 1, 3);
        base.set(EX - 1, EY, EX - 1, EY + 1, 2);
        base.set(EX + 2, EY - 2, EX + 3, EY + 3, 5);
        base.set(EX + 1, EY - 2, EX + 1, EY - 1, 1);
        base.set(EX + 1, EY + 2, EX + 1, EY + 3, 1);
        base.set(EX - 1, EY - 1, 1);
        base.set(EX - 1, EY + 2, 1);
        base.set(0, 0, 11, 1, 5);
        base.set(0, 9, 11, 11, 5);
        corridor(base, EX, EY - 2, W, 6);
        corridor(base, EX, EY + 3, W, 6);
        corridor(base, EX - 2, EY - 1, W, 3);
        corridor(base, EX - 2, EY + 2, W, 3);
        while (cleanEdges(base)) {
        }
        identifyRooms(base, rooms[0]);
        identifyRooms(base, rooms[1]);
        rooms[0].set(EX + 1, EY, EX + 1, EY + 1, 0x800000);
        rooms[1].set(EX + 1, EY, EX + 1, EY + 1, 0x800000);
        setupThirdFloor();
        identifyRooms(third, rooms[2]);
    }

    bool isRoomId(int x, int y, int floor, int id) const { return (rooms[floor].get(x, y) & 0xFFFF) == id; }
    int roomDirection(int x, int y, int floor, int id) const {   // get1x2RoomDirection
        for (int d = 0; d < 4; d++)
            if (isRoomId(x + SX[d], y + SZ[d], floor, id)) return d;
        return NONE;
    }

    void corridor(Grid& g, int x, int y, int dir, int length) {   // recursiveCorridor
        if (length <= 0) return;
        g.set(x, y, 1);
        g.setif(x + SX[dir], y + SZ[dir], 0, 1);
        for (int k = 0; k < 8; k++) {
            int d2 = FROM_2D[r.nextInt(4)];
            if (d2 == opposite(dir)) continue;
            if (d2 == E && r.nextBoolean()) continue;
            int l = x + SX[dir], m = y + SZ[dir];
            if (g.get(l + SX[d2], m + SZ[d2]) == 0 && g.get(l + SX[d2] * 2, m + SZ[d2] * 2) == 0) {
                corridor(g, x + SX[dir] + SX[d2], y + SZ[dir] + SZ[d2], d2, length - 1);
                break;
            }
        }
        int c1 = cw(dir), c2 = ccw(dir);
        g.setif(x + SX[c1], y + SZ[c1], 0, 2);
        g.setif(x + SX[c2], y + SZ[c2], 0, 2);
        g.setif(x + SX[dir] + SX[c1], y + SZ[dir] + SZ[c1], 0, 2);
        g.setif(x + SX[dir] + SX[c2], y + SZ[dir] + SZ[c2], 0, 2);
        g.setif(x + SX[dir] * 2, y + SZ[dir] * 2, 0, 2);
        g.setif(x + SX[c1] * 2, y + SZ[c1] * 2, 0, 2);
        g.setif(x + SX[c2] * 2, y + SZ[c2] * 2, 0, 2);
    }

    static bool cleanEdges(Grid& g) {
        bool changed = false;
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++) {
                if (g.get(x, y) != 0) continue;
                int n = isHouse(g, x + 1, y) + isHouse(g, x - 1, y) + isHouse(g, x, y + 1) + isHouse(g, x, y - 1);
                if (n >= 3) {
                    g.set(x, y, 2);
                    changed = true;
                } else if (n == 2) {
                    int d = isHouse(g, x + 1, y + 1) + isHouse(g, x - 1, y + 1) + isHouse(g, x + 1, y - 1) + isHouse(g, x - 1, y - 1);
                    if (d <= 1) {
                        g.set(x, y, 2);
                        changed = true;
                    }
                }
            }
        return changed;
    }

    void setupThirdFloor() {
        Grid& second = rooms[1];
        int cand[121][2], nc = 0;
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++) {
                int v = second.get(x, y);
                if ((v & 0xF0000) == 131072 && (v & 0x200000) == 0x200000) {
                    cand[nc][0] = x;
                    cand[nc++][1] = y;
                }
            }
        if (!nc) {
            third.set(0, 0, 11, 11, 5);
            return;
        }
        int pick = r.nextInt(nc);
        int ax = cand[pick][0], ay = cand[pick][1];
        int v = second.get(ax, ay);
        second.set(ax, ay, v | 0x400000);
        int dir = roomDirection(ax, ay, 1, v & 0xFFFF);
        if (dir < 0) dir = N;   // (vanilla: never; a 1x2 room has its other half)
        int bx = ax + SX[dir], by = ay + SZ[dir];
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++) {
                if (!isHouse(base, x, y)) {
                    third.set(x, y, 5);
                } else if (x == ax && y == ay) {
                    third.set(x, y, 3);
                } else if (x == bx && y == by) {
                    third.set(x, y, 3);
                    rooms[2].set(x, y, 0x800000);
                }
            }
        int dirs[4], nd = 0;
        for (int d = 0; d < 4; d++)
            if (third.get(bx + SX[d], by + SZ[d]) == 0) dirs[nd++] = d;
        if (!nd) {
            third.set(0, 0, 11, 11, 5);
            second.set(ax, ay, v);
            return;
        }
        int d = dirs[r.nextInt(nd)];
        corridor(third, bx + SX[d], by + SZ[d], d, 4);
        while (cleanEdges(third)) {
        }
    }

    void identifyRooms(const Grid& g, Grid& out) {
        int list[121][2], n = 0;
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++)
                if (g.get(x, y) == 2) {
                    list[n][0] = x;
                    list[n++][1] = y;
                }
        for (int i = n; i > 1; i--) {   // Collections.shuffle
            int j = r.nextInt(i);
            int tx = list[i - 1][0], ty = list[i - 1][1];
            list[i - 1][0] = list[j][0];
            list[i - 1][1] = list[j][1];
            list[j][0] = tx;
            list[j][1] = ty;
        }
        int id = 10;
        for (int i = 0; i < n; i++) {
            int x = list[i][0], y = list[i][1];
            if (out.get(x, y) != 0) continue;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            int size = 65536;
            if (out.get(x + 1, y) == 0 && out.get(x, y + 1) == 0 && out.get(x + 1, y + 1) == 0 && g.get(x + 1, y) == 2 && g.get(x, y + 1) == 2 &&
                g.get(x + 1, y + 1) == 2) {
                x1++;
                y1++;
                size = 262144;
            } else if (out.get(x - 1, y) == 0 && out.get(x, y + 1) == 0 && out.get(x - 1, y + 1) == 0 && g.get(x - 1, y) == 2 &&
                       g.get(x, y + 1) == 2 && g.get(x - 1, y + 1) == 2) {
                x0--;
                y1++;
                size = 262144;
            } else if (out.get(x - 1, y) == 0 && out.get(x, y - 1) == 0 && out.get(x - 1, y - 1) == 0 && g.get(x - 1, y) == 2 &&
                       g.get(x, y - 1) == 2 && g.get(x - 1, y - 1) == 2) {
                x0--;
                y0--;
                size = 262144;
            } else if (out.get(x + 1, y) == 0 && g.get(x + 1, y) == 2) {
                x1++;
                size = 131072;
            } else if (out.get(x, y + 1) == 0 && g.get(x, y + 1) == 2) {
                y1++;
                size = 131072;
            } else if (out.get(x - 1, y) == 0 && g.get(x - 1, y) == 2) {
                x0--;
                size = 131072;
            } else if (out.get(x, y - 1) == 0 && g.get(x, y - 1) == 2) {
                y0--;
                size = 131072;
            }
            int dx = r.nextBoolean() ? x0 : x1;
            int dy = r.nextBoolean() ? y0 : y1;
            int door = 0x200000;
            if (!g.edgesTo(dx, dy, 1)) {
                dx = dx == x0 ? x1 : x0;
                dy = dy == y0 ? y1 : y0;
                if (!g.edgesTo(dx, dy, 1)) {
                    dy = dy == y0 ? y1 : y0;
                    if (!g.edgesTo(dx, dy, 1)) {
                        dx = dx == x0 ? x1 : x0;
                        dy = dy == y0 ? y1 : y0;
                        if (!g.edgesTo(dx, dy, 1)) {
                            door = 0;
                            dx = x0;
                            dy = y0;
                        }
                    }
                }
            }
            for (int yy = y0; yy <= y1; yy++)
                for (int xx = x0; xx <= x1; xx++) {
                    if (xx == dx && yy == dy) out.set(xx, yy, 0x100000 | door | size | id);
                    else out.set(xx, yy, size | id);
                }
            id++;
        }
    }
};

struct Pos { int x, y, z; };
inline Pos rel(Pos p, int dir, int n) { return {p.x + SX[dir] * n, p.y, p.z + SZ[dir] * n}; }
inline Pos above(Pos p, int n) { return {p.x, p.y + n, p.z}; }

// the woodland_mansion templates, found once
struct Names {
    char name[64][24];
    int tmpl[64];
    int n = 0;
    int find(const char* s) {
        for (int i = 0; i < n; i++)
            if (!strcmp(name[i], s)) return tmpl[i];
        char full[64];
        snprintf(full, sizeof(full), "woodland_mansion/%s", s);
        int t = -1;
        for (int i = 0; i < TD_NUM_TEMPLATES && t < 0; i++)
            if (!strcmp(TD_STRINGS[TD_TEMPLATES[i].name], full)) t = i;
        if (n < 64) {
            snprintf(name[n], sizeof(name[n]), "%s", s);
            tmpl[n++] = t;
        }
        return t;
    }
};

// the rooms' templates per floor (FloorRoomCollection)
struct Rooms {
    int floor;   // 0 first, 1 second, 2 third (as the second)
    void name(char* out, size_t sz, const char* fmt, JavaRandom& r, int bound) const { snprintf(out, sz, fmt, r.nextInt(bound) + 1); }
    void get1x1(char* o, size_t s, JavaRandom& r) const { floor == 0 ? name(o, s, "1x1_a%d", r, 5) : name(o, s, "1x1_b%d", r, 5); }
    void get1x1Secret(char* o, size_t s, JavaRandom& r) const { name(o, s, "1x1_as%d", r, 4); }
    void get1x2Side(char* o, size_t s, JavaRandom& r, bool stairs) const {
        if (floor == 0) name(o, s, "1x2_a%d", r, 9);
        else if (stairs) snprintf(o, s, "1x2_c_stairs");
        else name(o, s, "1x2_c%d", r, 4);
    }
    void get1x2Front(char* o, size_t s, JavaRandom& r, bool stairs) const {
        if (floor == 0) name(o, s, "1x2_b%d", r, 5);
        else if (stairs) snprintf(o, s, "1x2_d_stairs");
        else name(o, s, "1x2_d%d", r, 5);
    }
    void get1x2Secret(char* o, size_t s, JavaRandom& r) const { floor == 0 ? name(o, s, "1x2_s%d", r, 2) : name(o, s, "1x2_se%d", r, 1); }
    void get2x2(char* o, size_t s, JavaRandom& r) const { floor == 0 ? name(o, s, "2x2_a%d", r, 4) : name(o, s, "2x2_b%d", r, 5); }
    void get2x2Secret(char* o, size_t s) const { snprintf(o, s, "2x2_s1"); }
};

enum { M_NONE = 0, M_LEFT_RIGHT = 1, M_FRONT_BACK = 2 };

struct Placer {
    JavaRandom& r;
    Names& names;
    Piece* out;
    int n = 0, max;
    int startX = 0, startY = 0;
    Placer(JavaRandom& rnd, Names& nm, Piece* o, int m) : r(rnd), names(nm), out(o), max(m) {}

    void add(const char* name, Pos p, int rot, int mirror = M_NONE) {
        int t = names.find(name);
        if (t < 0 || n >= max) return;
        rot &= 3;
        Piece& q = out[n++];
        q = {};
        q.box = templateBox(t, p.x, p.y, p.z, rot, mirror);
        q.kind = PK_TEMPLATE;
        q.orient = (uint8_t)rot;
        q.a = (int16_t)t;
        q.b = (int32_t)(((uint32_t)(p.x - q.box.x0) & 0xFFu) | ((uint32_t)(p.z - q.box.z0) & 0xFFu) << 8 | (uint32_t)rot << 16 | 7u << 18 |
                        (uint32_t)mirror << 30);
    }

    struct Placement { int rot; Pos pos; const char* wall; };
    static int rotate(int rot, int dir) { return (dir + rot) & 3; }

    void wallPiece(Placement& d) {   // traverseWallPiece
        add(d.wall, rel(d.pos, rotate(d.rot, E), 7), d.rot);
        d.pos = rel(d.pos, rotate(d.rot, S), 8);
    }
    void turn(Placement& d) {   // traverseTurn
        d.pos = rel(d.pos, rotate(d.rot, S), -1);
        add("wall_corner", d.pos, d.rot);
        d.pos = rel(d.pos, rotate(d.rot, S), -7);
        d.pos = rel(d.pos, rotate(d.rot, W), -6);
        d.rot = (d.rot + 1) & 3;
    }
    void innerTurn(Placement& d) {   // traverseInnerTurn
        d.pos = rel(d.pos, rotate(d.rot, S), 6);
        d.pos = rel(d.pos, rotate(d.rot, E), 8);
        d.rot = (d.rot + 3) & 3;
    }
    void outerWalls(Placement& d, const Grid& g, int dir, int sx, int sy, int ex, int ey) {   // traverseOuterWalls
        int i = sx, j = sy, start = dir;
        do {
            if (!isHouse(g, i + SX[dir], j + SZ[dir])) {
                turn(d);
                dir = cw(dir);
                if (i != ex || j != ey || start != dir) wallPiece(d);
            } else if (isHouse(g, i + SX[dir] + SX[ccw(dir)], j + SZ[dir] + SZ[ccw(dir)])) {
                innerTurn(d);
                i += SX[dir];
                j += SZ[dir];
                dir = ccw(dir);
            } else {
                i += SX[dir];
                j += SZ[dir];
                if (i != ex || j != ey || start != dir) wallPiece(d);
            }
        } while (i != ex || j != ey || start != dir);
    }

    Pos cell(Pos base, int rot, int x, int y, int dx = 0) const {
        Pos p = rel(base, rotate(rot, S), 8 + (y - startY) * 8);
        return rel(p, rotate(rot, E), dx + (x - startX) * 8);
    }

    void roof(Pos base, int rot, const Grid& g, const Grid* g3) {   // createRoof
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++) {
                Pos p = cell(base, rot, x, y);
                bool upper = g3 && isHouse(*g3, x, y);
                if (!isHouse(g, x, y) || upper) continue;
                add("roof", above(p, 3), rot);
                if (!isHouse(g, x + 1, y)) add("roof_front", rel(p, rotate(rot, E), 6), rot);
                if (!isHouse(g, x - 1, y)) add("roof_front", rel(rel(p, rotate(rot, E), 0), rotate(rot, S), 7), rot + 2);
                if (!isHouse(g, x, y - 1)) add("roof_front", rel(p, rotate(rot, W), 1), rot + 3);
                if (!isHouse(g, x, y + 1)) add("roof_front", rel(rel(p, rotate(rot, E), 6), rotate(rot, S), 6), rot + 1);
            }
        if (g3) {
            for (int y = 0; y < 11; y++)
                for (int x = 0; x < 11; x++) {
                    Pos p = cell(base, rot, x, y);
                    if (!isHouse(g, x, y) || !isHouse(*g3, x, y)) continue;
                    if (!isHouse(g, x + 1, y)) add("small_wall", rel(p, rotate(rot, E), 7), rot);
                    if (!isHouse(g, x - 1, y)) add("small_wall", rel(rel(p, rotate(rot, W), 1), rotate(rot, S), 6), rot + 2);
                    if (!isHouse(g, x, y - 1)) add("small_wall", rel(rel(p, rotate(rot, W), 0), rotate(rot, N), 1), rot + 3);
                    if (!isHouse(g, x, y + 1)) add("small_wall", rel(rel(p, rotate(rot, E), 6), rotate(rot, S), 7), rot + 1);
                    if (!isHouse(g, x + 1, y)) {
                        if (!isHouse(g, x, y - 1)) add("small_wall_corner", rel(rel(p, rotate(rot, E), 7), rotate(rot, N), 2), rot);
                        if (!isHouse(g, x, y + 1)) add("small_wall_corner", rel(rel(p, rotate(rot, E), 8), rotate(rot, S), 7), rot + 1);
                    }
                    if (isHouse(g, x - 1, y)) continue;
                    if (!isHouse(g, x, y - 1)) add("small_wall_corner", rel(rel(p, rotate(rot, W), 2), rotate(rot, N), 1), rot + 3);
                    if (isHouse(g, x, y + 1)) continue;
                    add("small_wall_corner", rel(rel(p, rotate(rot, W), 1), rotate(rot, S), 8), rot + 2);
                }
        }
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 11; x++) {
                Pos p = cell(base, rot, x, y);
                bool upper = g3 && isHouse(*g3, x, y);
                if (!isHouse(g, x, y) || upper) continue;
                if (!isHouse(g, x + 1, y)) {
                    Pos q = rel(p, rotate(rot, E), 6);
                    if (!isHouse(g, x, y + 1)) add("roof_corner", rel(q, rotate(rot, S), 6), rot);
                    else if (isHouse(g, x + 1, y + 1)) add("roof_inner_corner", rel(q, rotate(rot, S), 5), rot);
                    if (!isHouse(g, x, y - 1)) add("roof_corner", q, rot + 3);
                    else if (isHouse(g, x + 1, y - 1)) add("roof_inner_corner", rel(rel(p, rotate(rot, E), 9), rotate(rot, N), 2), rot + 1);
                }
                if (isHouse(g, x - 1, y)) continue;
                Pos q = rel(rel(p, rotate(rot, E), 0), rotate(rot, S), 0);
                if (!isHouse(g, x, y + 1)) add("roof_corner", rel(q, rotate(rot, S), 6), rot + 1);
                else if (isHouse(g, x - 1, y + 1)) add("roof_inner_corner", rel(rel(q, rotate(rot, S), 8), rotate(rot, W), 3), rot + 3);
                if (!isHouse(g, x, y - 1)) add("roof_corner", q, rot + 2);
                else if (isHouse(g, x - 1, y - 1)) add("roof_inner_corner", rel(q, rotate(rot, S), 1), rot + 2);
            }
    }

    void room1x1(Pos p, int rot, int door, const Rooms& rc) {   // addRoom1x1
        char name[24];
        int r2 = 0;
        rc.get1x1(name, sizeof(name), r);
        if (door != E) {
            if (door == N) r2 = 3;
            else if (door == W) r2 = 2;
            else if (door == S) r2 = 1;
            else rc.get1x1Secret(name, sizeof(name), r);
        }
        // StructureTemplate.getZeroPositionWithTransform((1, 0, 0), NONE, r2, 7, 7), turned by rot
        int zx = 1, zz = 0;
        if (r2 == 3) zz += 6;
        else if (r2 == 1) zx += 6;
        else if (r2 == 2) { zx += 6; zz += 6; }
        int ox = zx, oz = zz;   // BlockPos#rotate
        if (rot == 1) { ox = -zz; oz = zx; }
        else if (rot == 2) { ox = -zx; oz = -zz; }
        else if (rot == 3) { ox = zz; oz = -zx; }
        add(name, {p.x + ox, p.y, p.z + oz}, r2 + rot);
    }

    void room1x2(Pos p, int rot, int rdir, int door, const Rooms& rc, bool stairs) {   // addRoom1x2
        char name[24];
        auto side = [&]() { rc.get1x2Side(name, sizeof(name), r, stairs); return name; };
        auto front = [&]() { rc.get1x2Front(name, sizeof(name), r, stairs); return name; };
        const int e = rotate(rot, E), s = rotate(rot, S), w = rotate(rot, W), nn = rotate(rot, N);
        if (door == E && rdir == S) add(side(), rel(p, e, 1), rot);
        else if (door == E && rdir == N) add(side(), rel(rel(p, e, 1), s, 6), rot, M_LEFT_RIGHT);
        else if (door == W && rdir == N) add(side(), rel(rel(p, e, 7), s, 6), rot + 2);
        else if (door == W && rdir == S) add(side(), rel(p, e, 7), rot, M_FRONT_BACK);
        else if (door == S && rdir == E) add(side(), rel(p, e, 1), rot + 1, M_LEFT_RIGHT);
        else if (door == S && rdir == W) add(side(), rel(p, e, 7), rot + 1);
        else if (door == N && rdir == W) add(side(), rel(rel(p, e, 7), s, 6), rot + 1, M_FRONT_BACK);
        else if (door == N && rdir == E) add(side(), rel(rel(p, e, 1), s, 6), rot + 3);
        else if (door == S && rdir == N) add(front(), rel(rel(p, e, 1), nn, 8), rot);
        else if (door == N && rdir == S) add(front(), rel(rel(p, e, 7), s, 14), rot + 2);
        else if (door == W && rdir == E) add(front(), rel(p, e, 15), rot + 1);
        else if (door == E && rdir == W) add(front(), rel(rel(p, w, 7), s, 6), rot + 3);
        else if (door == UP && rdir == E) { rc.get1x2Secret(name, sizeof(name), r); add(name, rel(p, e, 15), rot + 1); }
        else if (door == UP && rdir == S) { rc.get1x2Secret(name, sizeof(name), r); add(name, rel(rel(p, e, 1), nn, 0), rot); }
    }

    void room2x2(Pos p, int rot, int rdir, int door, const Rooms& rc) {   // addRoom2x2
        int dx = 0, dz = 0, r2 = rot, m = M_NONE;
        if (door == E && rdir == S) { dx = -7; }
        else if (door == E && rdir == N) { dx = -7; dz = 6; m = M_LEFT_RIGHT; }
        else if (door == N && rdir == E) { dx = 1; dz = 14; r2 = rot + 3; }
        else if (door == N && rdir == W) { dx = 7; dz = 14; r2 = rot + 3; m = M_LEFT_RIGHT; }
        else if (door == S && rdir == W) { dx = 7; dz = -8; r2 = rot + 1; }
        else if (door == S && rdir == E) { dx = 1; dz = -8; r2 = rot + 1; m = M_LEFT_RIGHT; }
        else if (door == W && rdir == N) { dx = 15; dz = 6; r2 = rot + 2; }
        else if (door == W && rdir == S) { dx = 15; m = M_FRONT_BACK; }
        Pos q = rel(rel(p, rotate(rot, E), dx), rotate(rot, S), dz);
        char name[24];
        rc.get2x2(name, sizeof(name), r);
        add(name, q, r2, m);
    }

    // MansionPiecePlacer#createMansion
    void create(Pos pos, int rot, MansionGrid& g) {
        Placement d1 = {rot, pos, "wall_flat"};
        add("entrance", rel(d1.pos, rotate(rot, W), 9), rot);   // entrance
        d1.pos = rel(d1.pos, rotate(rot, S), 16);
        Placement d2 = {rot, above(d1.pos, 8), "wall_window"};
        startX = MansionGrid::EX + 1;
        startY = MansionGrid::EY + 1;
        int ex = MansionGrid::EX + 1, ey = MansionGrid::EY;
        outerWalls(d1, g.base, S, startX, startY, ex, ey);
        outerWalls(d2, g.base, S, startX, startY, ex, ey);
        Placement d3 = {rot, above(d1.pos, 19), "wall_window"};
        bool found = false;
        for (int y = 0; y < 11 && !found; y++)
            for (int x = 10; x >= 0 && !found; x--) {
                if (!isHouse(g.third, x, y)) continue;
                d3.pos = rel(d3.pos, rotate(rot, S), 8 + (y - startY) * 8);
                d3.pos = rel(d3.pos, rotate(rot, E), (x - startX) * 8);
                wallPiece(d3);
                outerWalls(d3, g.third, S, x, y, x, y);
                found = true;
            }
        roof(above(pos, 16), rot, g.base, &g.third);
        roof(above(pos, 27), rot, g.third, nullptr);
        for (int f = 0; f < 3; f++) {
            Rooms rc = {f};
            Pos fp = above(pos, 8 * f + (f == 2 ? 3 : 0));
            const Grid& rooms = g.rooms[f];
            const Grid& grid = f == 2 ? g.third : g.base;
            const char* carpetS = f == 0 ? "carpet_south_1" : "carpet_south_2";
            const char* carpetW = f == 0 ? "carpet_west_1" : "carpet_west_2";
            for (int y = 0; y < 11; y++)
                for (int x = 0; x < 11; x++) {
                    if (grid.get(x, y) != 1) continue;
                    Pos p = cell(fp, rot, x, y);
                    add("corridor_floor", p, rot);
                    if (grid.get(x, y - 1) == 1 || (rooms.get(x, y - 1) & 0x800000) == 0x800000)
                        add("carpet_north", above(rel(p, rotate(rot, E), 1), 1), rot);
                    if (grid.get(x + 1, y) == 1 || (rooms.get(x + 1, y) & 0x800000) == 0x800000)
                        add("carpet_east", above(rel(rel(p, rotate(rot, S), 1), rotate(rot, E), 5), 1), rot);
                    if (grid.get(x, y + 1) == 1 || (rooms.get(x, y + 1) & 0x800000) == 0x800000)
                        add(carpetS, rel(rel(p, rotate(rot, S), 5), rotate(rot, W), 1), rot);
                    if (grid.get(x - 1, y) == 1 || (rooms.get(x - 1, y) & 0x800000) == 0x800000)
                        add(carpetW, rel(rel(p, rotate(rot, W), 1), rotate(rot, N), 1), rot);
                }
            const char* wall = f == 0 ? "indoors_wall_1" : "indoors_wall_2";
            const char* doorName = f == 0 ? "indoors_door_1" : "indoors_door_2";
            for (int y = 0; y < 11; y++)
                for (int x = 0; x < 11; x++) {
                    bool stair = f == 2 && grid.get(x, y) == 3;
                    if (grid.get(x, y) != 2 && !stair) continue;
                    int q = rooms.get(x, y);
                    int size = q & 0xF0000, id = q & 0xFFFF;
                    stair = stair && (q & 0x800000) == 0x800000;
                    int doors[4], nd = 0;
                    if ((q & 0x200000) == 0x200000)
                        for (int d = 0; d < 4; d++)
                            if (grid.get(x + SX[d], y + SZ[d]) == 1) doors[nd++] = d;
                    int door = NONE;
                    if (nd) door = doors[r.nextInt(nd)];
                    else if ((q & 0x100000) == 0x100000) door = UP;
                    Pos p = cell(fp, rot, x, y, -1);
                    if (isHouse(grid, x - 1, y) && !g.isRoomId(x - 1, y, f, id)) add(door == W ? doorName : wall, p, rot);
                    if (grid.get(x + 1, y) == 1 && !stair) add(door == E ? doorName : wall, rel(p, rotate(rot, E), 8), rot);
                    if (isHouse(grid, x, y + 1) && !g.isRoomId(x, y + 1, f, id))
                        add(door == S ? doorName : wall, rel(rel(p, rotate(rot, S), 7), rotate(rot, E), 7), rot + 1);
                    if (grid.get(x, y - 1) == 1 && !stair)
                        add(door == N ? doorName : wall, rel(rel(p, rotate(rot, N), 1), rotate(rot, E), 7), rot + 1);
                    if (size == 65536) {
                        room1x1(p, rot, door, rc);
                    } else if (size == 131072 && door != NONE) {
                        int rdir = g.roomDirection(x, y, f, id);
                        room1x2(p, rot, rdir, door, rc, (q & 0x400000) == 0x400000);
                    } else if (size == 262144 && door != NONE && door != UP) {
                        int rdir = cw(door);
                        if (!g.isRoomId(x + SX[rdir], y + SZ[rdir], f, id)) rdir = opposite(rdir);
                        room2x2(p, rot, rdir, door, rc);
                    } else if (size == 262144 && door == UP) {
                        char name[24];
                        rc.get2x2Secret(name, sizeof(name));
                        add(name, rel(p, rotate(rot, E), 1), rot);
                    }
                }
        }
    }
};

}  // namespace

int mansionPieces(JavaRandom& r, int cx, int cz, TerrainProbe& probe, const uint8_t* biomes, Piece* out, int max) {
    // WoodlandMansionStructure#findGenerationPoint: the lowest of four corners 5 blocks out
    // (by the rotation) from the chunk's (7, 7); none below y 60
    int rot = r.nextInt(4);
    int w = 5, d = 5;
    if (rot == 1) w = -5;
    else if (rot == 2) { w = -5; d = -5; }
    else if (rot == 3) d = -5;
    int x = cx * 16 + 7, z = cz * 16 + 7;
    int y = probe.height(x, z, false);
    int h;
    if ((h = probe.height(x, z + d, false)) < y) y = h;
    if ((h = probe.height(x + w, z, false)) < y) y = h;
    if ((h = probe.height(x + w, z + d, false)) < y) y = h;
    y -= 1;   // getFirstOccupiedHeight
    if (y < 60) return 0;
    uint8_t b = probe.biome(x, y, z);
    if (b >= 128 || !(biomes[b >> 3] >> (b & 7) & 1)) return 0;
    MansionGrid* grid = new MansionGrid(r);
    Names* names = new Names();
    Placer pl(r, *names, out, max);
    pl.create({x, y, z}, rot, *grid);
    delete grid;
    delete names;
    return pl.n;
}

// WoodlandMansionStructure#afterPlace: cobblestone under the mansion's floor down to the
// ground (through air and fluids), in this chunk
void mansionFoundation(Chunk& c, const Piece* pieces, int n, const SBox& box) {
    int cobble = findBlock("cobblestone");
    if (cobble < 0) return;
    uint16_t st = BLOCKS[cobble].defState;
    int y0 = box.y0;
    if (!c.hasY(y0)) return;
    for (int lz = 0; lz < 16; lz++)
        for (int lx = 0; lx < 16; lx++) {
            int X = c.cx * 16 + lx, Z = c.cz * 16 + lz;
            if (stateIsAir(c.get(lx, y0, lz)) || !box.inside(X, y0, Z)) continue;
            bool in = false;
            for (int i = 0; i < n && !in; i++) in = pieces[i].box.inside(X, y0, Z);
            if (!in) continue;
            for (int y = y0 - 1; y > c.minY(); y--) {
                uint16_t at = c.get(lx, y, lz);
                if (!stateIsAir(at) && !stateIsFluid(at)) break;
                c.set(lx, y, lz, st);
            }
        }
}

}  // namespace sb
}  // namespace vanilla
}  // namespace mc
