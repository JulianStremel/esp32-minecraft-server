// Worker threads for compute-heavy work, so the game loop never stalls on it:
// terrain generation, lighting, packet compression and chunk record (de)compression.
//
// The game loop submit()s jobs and collects finished ones with poll(). Job::run()
// executes on a worker thread and may only use the job's own data (snapshots the game
// loop made when it submitted the job) and the worker's scratch buffers -- never the
// live world, players or connections. Job::finish() then runs on the game loop thread
// and applies the result. With zero workers, poll() runs the jobs itself (same results,
// no threads).
//
// On the ESP32 there is one worker per core: the one on core 1 shares the core with
// the game loop but has a lower priority, so it only uses the time the loop sleeps;
// the one on core 0 runs beside the WiFi stack, which preempts it.
//
// Scheduling: every job has a priority class, and every class a maximum wait. A job's
// deadline is its submit time plus that wait, and workers always take the queued job
// with the earliest deadline (on a tie, the more urgent class). Urgent work therefore
// jumps ahead of a backlog, but a waiting job can only be overtaken by jobs submitted
// before its deadline: a constant stream of urgent jobs delays a background job by at
// most its class's wait plus the work that was already due -- it never starves.
// Queued jobs can be promoted (deadline moves up) or cancelled (never run).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/bytebuf.h"
#include "mc/world/light.h"

namespace mc {

enum JobPriority : uint8_t {
    PRIO_URGENT = 0,       // a player is waiting for it right now (the ground under them)
    PRIO_HIGH = 1,         // visible soon (near chunks, light after an edit)
    PRIO_NORMAL = 2,       // the rest of the view
    PRIO_BACKGROUND = 3,   // nobody waits for it (saves)
    PRIO_COUNT = 4
};

// Per-worker buffers, reused across jobs.
struct WorkerScratch {
    int index = -1;                 // worker number (-1: the game loop, inline mode)
    uint8_t* deflateWs = nullptr;   // DeflateSink::WORKSPACE bytes
    ChunkLight light;
    ByteBuf tmp;
};

class Job {
public:
    virtual ~Job() {}
    virtual void run(WorkerScratch& ws) = 0;   // worker thread (skipped when cancelled)
    virtual void finish() = 0;                 // game loop thread; deleted afterwards
    virtual const char* kind() const { return "job"; }
    // true in finish() when the job was cancelled before it started (run() never ran)
    bool cancelled() const { return cancelled_; }
    JobPriority priority() const { return (JobPriority)prio_; }
    uint32_t runUs = 0;                        // time run() took
    uint32_t waitMs = 0;                       // time it spent queued

private:
    friend class JobQueue;
    enum State : uint8_t { NEW, QUEUED, RUNNING, DONE };
    Job* next_ = nullptr;
    uint32_t queuedAt_ = 0, deadline_ = 0;
    uint32_t classSince_ = 0;                  // when it entered its current class (promote)
    uint8_t prio_ = PRIO_NORMAL;
    uint8_t state_ = NEW;                      // guarded by the queue mutex
    bool cancelled_ = false;
};

class JobQueue {
public:
    JobQueue() {}
    ~JobQueue();
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Starts `workers` threads, worker i pinned to core i % cores (0 = inline mode).
    // Returns false if a thread could not be started (the queue then runs inline).
    bool start(int workers, size_t stackBytes = 20480);
    // Runs everything still queued, finishes it and stops the threads.
    void stop();

    void submit(Job* j, JobPriority prio = PRIO_NORMAL);
    // Moves a queued job to a more urgent class (its deadline only ever moves up).
    // false if it already started or is not more urgent.
    bool promote(Job* j, JobPriority prio);
    // Removes a job that has not started yet: finish() still runs (in the next poll),
    // with cancelled() == true. false if it already started; it then completes normally.
    // Only for jobs that have not been finished yet.
    bool cancel(Job* j);
    // Finishes up to maxJobs completed jobs (inline mode: runs them first).
    int poll(int maxJobs = 1 << 30);
    // Waits until every submitted job has finished.
    void drain();

    // Maximum wait of a class (default 0 / 100 / 500 / 3000 ms). Set before submitting.
    void setMaxWait(JobPriority prio, uint32_t ms) { maxWait_[prio] = ms; }
    uint32_t maxWait(JobPriority prio) const { return maxWait_[prio]; }
    // Millisecond clock used for deadlines (tests drive it; default plat::millis).
    void setClock(uint32_t (*clock)()) { clock_ = clock; }
    // Called (on the worker's thread) when an urgent job has finished, so the game loop
    // can wake up and apply it right away; other results wait for its next pass.
    void setUrgentHook(void (*hook)()) { urgentHook_ = hook; }
    int queued(JobPriority prio);                // waiting, not started
    // Time worker i has spent running jobs since it started (statusLine() reports the
    // share since its last call; this leaves that alone).
    uint64_t workerBusyUs(int i);
    static const char* priorityName(int prio);

    int inFlight() const { return inFlight_; }   // submitted, not yet finished
    int workers() const { return nWorkers_; }
    bool threaded() const { return nWorkers_ > 0; }
    uint32_t finishedJobs() const { return finished_; }
    // e.g. "2 workers busy 45% 30%, queued 0/3/8/2, max wait 4/60/420/2100 ms"
    // (busy share and longest wait per class since the last call)
    void statusLine(char* buf, size_t cap);
    // Game-loop cost of the queue since the last call: the slowest finish() (and its job
    // kind) and the longest wait for the queue lock, in microseconds.
    void takeLoopCost(uint32_t& finishUs, const char*& finishKind, uint32_t& lockUs);

private:
    struct Worker {
        JobQueue* q = nullptr;
        WorkerScratch scratch;
        uint64_t busyUs = 0;
        uint64_t reportedUs = 0;
    };
    static void workerMain(void* arg);
    Job* popTodo();                 // earliest deadline first; caller holds no lock
    void pushDone(Job* j);          // caller holds the lock
    bool unlinkQueued(Job* j);      // caller holds the lock

    void* mutex_ = nullptr;
    void* todoSem_ = nullptr;
    void* exitSem_ = nullptr;
    // one FIFO per class: all jobs of a class wait the same, so FIFO order is deadline order
    Job* todoHead_[PRIO_COUNT] = {};
    Job* todoTail_[PRIO_COUNT] = {};
    int queued_[PRIO_COUNT] = {};
    uint32_t waitMax_[PRIO_COUNT] = {};   // longest wait in each class since statusLine()
    uint32_t maxWait_[PRIO_COUNT] = {0, 100, 500, 3000};
    uint32_t (*clock_)() = nullptr;
    void (*urgentHook_)() = nullptr;
    Job* doneHead_ = nullptr;
    Job* doneTail_ = nullptr;
    Worker* workers_ = nullptr;
    int nWorkers_ = 0;
    bool stopping_ = false;      // guarded by mutex_
    int inFlight_ = 0;           // game loop only
    uint32_t finished_ = 0;      // game loop only
    WorkerScratch inline_;       // inline mode
    uint64_t lastStatusUs_ = 0;
    uint32_t finishMaxUs_ = 0, lockMaxUs_ = 0;
    const char* finishMaxKind_ = "-";
    void finishOne(Job* j);
};

}  // namespace mc
