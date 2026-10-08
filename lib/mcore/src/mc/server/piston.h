// Java 1.16 piston structure planning. Runs on the game loop, before any edits.
#pragma once
#include <stdint.h>

namespace mc {
class Server;
struct Entity;
struct TileEntity;
struct PistonBox {
    double lo[3], hi[3];
    bool intersects(const PistonBox& b) const;
};
struct PistonPos {
    int x = 0, y = 0, z = 0;
    bool operator==(const PistonPos& b) const { return x == b.x && y == b.y && z == b.z; }
    PistonPos offset(int direction, int distance = 1) const;
};

class PistonPlan {
  public:
    // No allocations. A piston moves at most 12 blocks. Up to one breakable block
    // per branch can terminate the structure (six neighbours per moved block).
    PistonPos moved[12], broken[73];
    int moveCount = 0, breakCount = 0;
    bool resolve(Server& server, PistonPos base, int facing, bool extending);
    static bool pushable(Server& s, PistonPos pos, int direction, bool destroy, int interaction);

  private:
    Server* server_ = nullptr;
    PistonPos base_;
    int direction_ = 0;
    bool line(PistonPos pos, int interaction);
    bool branches(PistonPos pos);
    int index(PistonPos pos) const;
    void addBroken(PistonPos pos);
};
class Pistons {
  public:
    static void changed(Server& s, PistonPos pos, uint16_t state);
    static bool event(Server& s, PistonPos pos, uint16_t state, int type, int data);
    static void tick(Server& s, PistonPos pos);
    static void finish(Server& s, PistonPos pos, bool forced);
    static int collision(Server& s, PistonPos pos, PistonBox* out, int ignoredDirection = -1);

  private:
    static void pushEntities(Server& s, PistonPos pos, const TileEntity& tile);
    static void displace(Server& s, Entity& entity, int direction, double distance, int ignoredDirection);
    static bool powered(Server& s, PistonPos pos, int facing);
    static bool move(Server& s, PistonPos pos, int facing, bool extending, bool sticky);
    static bool moving(Server& s, PistonPos pos, uint16_t state, int facing, bool extending, bool source, bool sticky);
};
} // namespace mc
