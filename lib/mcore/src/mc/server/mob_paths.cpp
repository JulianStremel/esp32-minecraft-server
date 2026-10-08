// Path finding for mobs on the worker threads: a PathJob runs A* (path.cpp) over shared
// snapshots of the 3 x 3 chunks around the mob; the mob walks the waypoints. A path is
// asked for again when the target moved away from its end, when a chunk on the way
// changed, or when the mob got stuck -- not every tick.
#include <math.h>
#include <stdlib.h>
#include "mc/server/path.h"
#include "mc/server/server.h"

namespace mc {

class PathJob : public Job {
public:
    Server* srv = nullptr;
    int32_t entityId = -1;
    ChunkSnap* nine[9] = {};
    PathRequest req;
    PathResult res;
    uint32_t versions = 0;

    ~PathJob() override {
        for (ChunkSnap* s : nine)
            if (s) s->release();
    }
    void run(WorkerScratch&) override {
        for (int k = 0; k < 9; k++) req.nine[k] = nine[k] ? nine[k]->chunk : nullptr;
        findPath(req, res);
    }
    void finish() override { srv->pathFinished(*this); }
    const char* kind() const override { return "path"; }
};

static const int MAX_PATH_JOBS = 2;   // in flight at once (chunks under players come first)

// Changes when a block in one of the 3 x 3 chunks around (cx, cz) changes.
uint32_t Server::pathVersionsAround(int cx, int cz) {
    uint32_t sum = 0;
    for (int k = 0; k < 9; k++) {
        const Chunk* c = world.peek(curDim, cx + k % 3 - 1, cz + k / 3 - 1);
        sum += c ? c->version * 2654435761u + (uint32_t)k : 0;
    }
    return sum;
}

bool Server::pathDirection(Entity& e, double tx, double ty, double tz, double& mx, double& mz, bool& jump) {
    int ex = (int)floor(e.x), ey = (int)floor(e.y + 0.01), ez = (int)floor(e.z);
    // stuck: hardly moved while trying to walk
    double moved = fabs(e.x - e.lastX) + fabs(e.z - e.lastZ);
    e.lastX = (float)e.x;
    e.lastZ = (float)e.z;
    e.stuckTicks = moved < 0.02 ? (int16_t)(e.stuckTicks + 1) : 0;
    int gx = (int)floor(tx), gy = (int)floor(ty + 0.01), gz = (int)floor(tz);
    bool need = e.pathIdx >= e.pathLen || abs(gx - e.pathGoal.x) + abs(gz - e.pathGoal.z) > 2 ||
                abs(gy - e.pathGoal.y) > 1 || e.stuckTicks > 20 ||
                ((ticks + (uint32_t)e.id) % 20 == 0 && pathVersionsAround(ex >> 4, ez >> 4) != e.pathVersions);
    if (need && !e.pathPending && pathStats.inFlight < MAX_PATH_JOBS) {
        PathJob* j = new PathJob();
        int cx = ex >> 4, cz = ez >> 4;
        bool ok = true;
        for (int k = 0; k < 9 && ok; k++)   // a chunk that is not resident counts as solid
            if (world.peek(curDim, cx + k % 3 - 1, cz + k / 3 - 1) && !(j->nine[k] = world.snapshot(curDim, cx + k % 3 - 1, cz + k / 3 - 1)))
                ok = false;
        if (ok) {
            j->srv = this;
            j->entityId = e.id;
            j->req.cx = cx;
            j->req.cz = cz;
            j->req.start = PathPoint{ex, ez, (int16_t)ey};
            j->req.goal = PathPoint{gx, gz, (int16_t)gy};
            j->req.height = e.height > 1.0f ? 2 : 1;
            j->versions = pathVersionsAround(cx, cz);
            e.pathPending = true;
            e.pathGoal = j->req.goal;
            e.stuckTicks = 0;
            pathStats.inFlight++;
            chunkJobs.queue().submit(j, PRIO_NORMAL);
        } else {
            delete j;
        }
    }
    // the next waypoint not reached yet
    while (e.pathIdx < e.pathLen) {
        const PathPoint& w = e.path[e.pathIdx];
        double dx = w.x + 0.5 - e.x, dz = w.z + 0.5 - e.z;
        if (dx * dx + dz * dz < 0.35 * 0.35 && abs(w.y - ey) <= 1) {
            e.pathIdx++;
            continue;
        }
        double d = sqrt(dx * dx + dz * dz) + 1e-6;
        mx = dx / d;
        mz = dz / d;
        jump = w.y > ey;   // a step up
        return true;
    }
    return false;
}

void Server::pathFinished(PathJob& j) {
    pathStats.inFlight--;
    pathStats.jobs++;
    pathStats.us += j.runUs;
    pathStats.nodes += (uint32_t)j.res.nodes;
    if (j.res.reached) pathStats.reached++;
    Entity* e = findEntity(j.entityId);
    if (!e || e->kind != EK_MOB) return;
    e->pathPending = false;
    if (j.cancelled()) return;
    int n = j.res.n < Entity::PATH_POINTS ? j.res.n : Entity::PATH_POINTS;
    for (int i = 0; i < n; i++) e->path[i] = j.res.points[i];
    e->pathLen = (uint8_t)n;
    e->pathIdx = 0;
    e->pathVersions = j.versions;
}

}  // namespace mc
