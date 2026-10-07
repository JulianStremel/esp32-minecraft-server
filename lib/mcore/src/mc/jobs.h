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
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mc/bytebuf.h"
#include "mc/world/light.h"

namespace mc {

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
    virtual void run(WorkerScratch& ws) = 0;   // worker thread
    virtual void finish() = 0;                 // game loop thread; deleted afterwards
    virtual const char* kind() const { return "job"; }
    uint32_t runUs = 0;                        // time run() took

private:
    friend class JobQueue;
    Job* next_ = nullptr;
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

    void submit(Job* j);
    // Finishes up to maxJobs completed jobs (inline mode: runs them first).
    int poll(int maxJobs = 1 << 30);
    // Waits until every submitted job has finished.
    void drain();

    int inFlight() const { return inFlight_; }   // submitted, not yet finished
    int workers() const { return nWorkers_; }
    bool threaded() const { return nWorkers_ > 0; }
    uint32_t finishedJobs() const { return finished_; }
    // e.g. "2 workers, 3 jobs queued, busy 45% 30%" (busy share since the last call)
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
    Job* popTodo();

    void* mutex_ = nullptr;
    void* todoSem_ = nullptr;
    void* exitSem_ = nullptr;
    Job* todoHead_ = nullptr;
    Job* todoTail_ = nullptr;
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
