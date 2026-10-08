// Path finding for mobs: A* over the blocks of a 3 x 3 block of chunk snapshots, after
// vanilla's walk node evaluator (WalkNodeEvaluator) in a simplified form. Pure: runs on
// worker threads.
#pragma once
#include <stdint.h>
#include "mc/world/chunk.h"

namespace mc {

struct PathPoint {
    int32_t x, z;
    int16_t y;
};

struct PathRequest {
    const Chunk* nine[9];   // nine[(dz + 1) * 3 + (dx + 1)] around chunk (cx, cz); may be nullptr
    int cx = 0, cz = 0;
    PathPoint start{0, 0, 0}, goal{0, 0, 0};   // feet positions
    int height = 2;          // blocks of head room the mob needs
    int maxDrop = 3;         // the highest drop it walks off
    int maxNodes = 800;      // search budget
};

struct PathResult {
    static const int MAX = 32;
    PathPoint points[MAX];   // waypoints after the start, nearest first
    int n = 0;
    bool reached = false;    // ends at the goal (else: the closest reachable point)
    int nodes = 0;           // nodes expanded
};

// Finds a path; false only when the start itself is not a place to stand.
bool findPath(const PathRequest& req, PathResult& out);

}  // namespace mc
