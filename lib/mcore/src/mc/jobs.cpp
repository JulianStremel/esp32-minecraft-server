#include "mc/jobs.h"
#include <stdio.h>
#include "mc/net/deflate.h"
#include "mc/platform.h"

namespace mc {

// deadlines are millisecond timestamps that may wrap
static inline bool before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static uint32_t defaultClock() { return plat::millis(); }

const char* JobQueue::priorityName(int prio) {
    static const char* names[PRIO_COUNT] = {"urgent", "high", "normal", "background"};
    return prio >= 0 && prio < PRIO_COUNT ? names[prio] : "?";
}

JobQueue::~JobQueue() {
    stop();
    plat::bigFree(inline_.deflateWs);
}

bool JobQueue::start(int workers, size_t stackBytes) {
    if (mutex_) return true;
    if (!clock_) clock_ = defaultClock;
    mutex_ = plat::mutexCreate();
    todoSem_ = plat::semCreate();
    exitSem_ = plat::semCreate();
    if (!mutex_ || !todoSem_ || !exitSem_) return false;
    if (workers <= 0) return true;
    workers_ = new Worker[workers];
    int cores = plat::cpuCores();
    for (int i = 0; i < workers; i++) {
        Worker& w = workers_[i];
        w.q = this;
        w.scratch.index = i;
        w.scratch.deflateWs = (uint8_t*)plat::bigAlloc(DeflateSink::WORKSPACE);
        char name[16];
        snprintf(name, sizeof(name), "mcworker%d", i);
        if (!w.scratch.deflateWs || !plat::startThread(name, i % cores, 0, stackBytes, workerMain, &w)) {
            MC_LOGE("jobs: could not start worker %d, running jobs on the game loop", i);
            plat::bigFree(w.scratch.deflateWs);
            w.scratch.deflateWs = nullptr;
            stop();  // stops the workers started so far
            return false;
        }
        nWorkers_ = i + 1;
    }
    MC_LOGI("jobs: %d worker threads (%d cores)", nWorkers_, cores);
    return true;
}

void JobQueue::stop() {
    if (!mutex_) return;
    drain();
    if (nWorkers_ > 0) {
        {
            LockGuard g(mutex_);
            stopping_ = true;
        }
        for (int i = 0; i < nWorkers_; i++) plat::semGive(todoSem_);
        for (int i = 0; i < nWorkers_; i++)
            if (!plat::semTake(exitSem_, 10000)) MC_LOGW("jobs: a worker did not stop");
    }
    // workers that started share the array; free their buffers after they exited
    for (int i = 0; workers_ && i < nWorkers_; i++) plat::bigFree(workers_[i].scratch.deflateWs);
    delete[] workers_;
    workers_ = nullptr;
    nWorkers_ = 0;
    stopping_ = false;
    plat::semDestroy(todoSem_);
    plat::semDestroy(exitSem_);
    plat::mutexDestroy(mutex_);
    todoSem_ = exitSem_ = mutex_ = nullptr;
}

Job* JobQueue::popTodo() {
    LockGuard g(mutex_);
    // earliest deadline first; on a tie the more urgent class (lower index) wins
    int best = -1;
    for (int p = 0; p < PRIO_COUNT; p++) {
        Job* h = todoHead_[p];
        if (h && (best < 0 || before(h->deadline_, todoHead_[best]->deadline_))) best = p;
    }
    if (best < 0) return nullptr;
    Job* j = todoHead_[best];
    todoHead_[best] = j->next_;
    if (!todoHead_[best]) todoTail_[best] = nullptr;
    j->next_ = nullptr;
    j->state_ = Job::RUNNING;
    queued_[best]--;
    uint32_t now = clock_();
    j->waitMs = now - j->queuedAt_;
    // a promoted job's earlier wait belongs to its old class, not to this one
    uint32_t inClass = now - j->classSince_;
    if (inClass > waitMax_[best]) waitMax_[best] = inClass;
    return j;
}

void JobQueue::pushDone(Job* j) {
    j->state_ = Job::DONE;
    j->next_ = nullptr;
    if (doneTail_) doneTail_->next_ = j;
    else doneHead_ = j;
    doneTail_ = j;
}

bool JobQueue::unlinkQueued(Job* j) {
    if (j->state_ != Job::QUEUED) return false;
    int p = j->prio_;
    Job* prev = nullptr;
    for (Job* k = todoHead_[p]; k; prev = k, k = k->next_) {
        if (k != j) continue;
        if (prev) prev->next_ = j->next_;
        else todoHead_[p] = j->next_;
        if (todoTail_[p] == j) todoTail_[p] = prev;
        j->next_ = nullptr;
        queued_[p]--;
        return true;
    }
    return false;
}

bool JobQueue::promote(Job* j, JobPriority prio) {
    if (!mutex_ || prio >= PRIO_COUNT) return false;
    LockGuard g(mutex_);
    if (j->state_ != Job::QUEUED || prio >= j->prio_) return false;
    uint32_t deadline = clock_() + maxWait_[prio];
    if (!before(deadline, j->deadline_)) return false;  // already due sooner than that
    unlinkQueued(j);
    // the newest job of the class has the latest deadline: the FIFO stays in deadline order
    j->prio_ = prio;
    j->deadline_ = deadline;
    j->classSince_ = deadline - maxWait_[prio];
    j->state_ = Job::QUEUED;
    if (todoTail_[prio]) todoTail_[prio]->next_ = j;
    else todoHead_[prio] = j;
    todoTail_[prio] = j;
    queued_[prio]++;
    return true;
}

bool JobQueue::cancel(Job* j) {
    if (!mutex_) return false;
    LockGuard g(mutex_);
    if (!unlinkQueued(j)) return false;
    j->cancelled_ = true;
    pushDone(j);  // finished by the next poll(), without running
    return true;
}

int JobQueue::queued(JobPriority prio) {
    if (!mutex_ || prio >= PRIO_COUNT) return 0;
    LockGuard g(mutex_);
    return queued_[prio];
}

void JobQueue::workerMain(void* arg) {
    Worker* w = (Worker*)arg;
    JobQueue* q = w->q;
    uint32_t lastPause = plat::millis();
    for (;;) {
        if (!plat::semTake(q->todoSem_, 1000)) continue;
        Job* j = q->popTodo();
        if (!j) {
            LockGuard g(q->mutex_);
            if (q->stopping_) break;
            continue;
        }
        uint64_t t0 = plat::micros();
        j->run(w->scratch);
        j->runUs = (uint32_t)(plat::micros() - t0);
        bool urgent = false;
        {
            LockGuard g(q->mutex_);
            w->busyUs += j->runUs;
            urgent = j->prio_ == PRIO_URGENT;
            q->pushDone(j);
        }
        if (urgent && q->urgentHook_) q->urgentHook_();
        // under sustained load, let lower-priority tasks run now and then (on the
        // ESP32 the idle task feeds the task watchdog)
        if (plat::millis() - lastPause > 100) {
            plat::yield();
            lastPause = plat::millis();
        }
    }
    plat::semGive(q->exitSem_);
}

void JobQueue::submit(Job* j, JobPriority prio) {
    if (!mutex_) start(0);
    if (prio >= PRIO_COUNT) prio = PRIO_NORMAL;
    if (!clock_) clock_ = defaultClock;
    j->next_ = nullptr;
    j->prio_ = prio;
    j->cancelled_ = false;
    inFlight_++;
    {
        LockGuard g(mutex_);
        j->queuedAt_ = clock_();
        j->classSince_ = j->queuedAt_;
        j->deadline_ = j->queuedAt_ + maxWait_[prio];
        j->state_ = Job::QUEUED;
        if (todoTail_[prio]) todoTail_[prio]->next_ = j;
        else todoHead_[prio] = j;
        todoTail_[prio] = j;
        queued_[prio]++;
    }
    if (nWorkers_ > 0) plat::semGive(todoSem_);
}

int JobQueue::poll(int maxJobs) {
    if (!mutex_) return 0;
    if (nWorkers_ == 0) {
        // inline mode: finish cancelled jobs (cheap, not counted against maxJobs), then
        // run up to maxJobs queued ones here
        if (!inline_.deflateWs) inline_.deflateWs = (uint8_t*)plat::bigAlloc(DeflateSink::WORKSPACE);
        int n = 0, cancelled = 0;
        for (;;) {
            Job* j;
            {
                LockGuard g(mutex_);
                j = doneHead_;
                if (j) {
                    doneHead_ = j->next_;
                    if (!doneHead_) doneTail_ = nullptr;
                }
            }
            if (!j) break;
            finishOne(j);
            cancelled++;
        }
        while (n < maxJobs) {
            Job* j = popTodo();
            if (!j) break;
            uint64_t t0 = plat::micros();
            j->run(inline_);
            j->runUs = (uint32_t)(plat::micros() - t0);
            {
                LockGuard g(mutex_);
                j->state_ = Job::DONE;
            }
            finishOne(j);
            n++;
        }
        return n + cancelled;
    }
    Job* list;
    {
        uint64_t t0 = plat::micros();
        LockGuard g(mutex_);
        uint32_t waited = (uint32_t)(plat::micros() - t0);
        if (waited > lockMaxUs_) lockMaxUs_ = waited;
        list = doneHead_;
        doneHead_ = doneTail_ = nullptr;
    }
    int n = 0;
    while (list) {
        if (n >= maxJobs) {
            // put the rest back (in order) for the next poll
            LockGuard g(mutex_);
            Job* last = list;
            while (last->next_) last = last->next_;
            last->next_ = doneHead_;
            doneHead_ = list;
            if (!doneTail_) doneTail_ = last;
            break;
        }
        Job* j = list;
        list = j->next_;
        finishOne(j);
        n++;
    }
    return n;
}

void JobQueue::finishOne(Job* j) {
    uint64_t t0 = plat::micros();
    inFlight_--;
    finished_++;
    j->finish();
    uint32_t us = (uint32_t)(plat::micros() - t0);
    if (us > finishMaxUs_) {
        finishMaxUs_ = us;
        finishMaxKind_ = j->kind();   // string literals: valid after the job is gone
    }
    delete j;
}

void JobQueue::takeLoopCost(uint32_t& finishUs, const char*& finishKind, uint32_t& lockUs) {
    finishUs = finishMaxUs_;
    finishKind = finishMaxKind_;
    lockUs = lockMaxUs_;
    finishMaxUs_ = lockMaxUs_ = 0;
    finishMaxKind_ = "-";
}

void JobQueue::drain() {
    while (inFlight_ > 0) {
        poll();
        if (inFlight_ > 0) plat::delayMs(1);
    }
}

void JobQueue::statusLine(char* buf, size_t cap) {
    if (!mutex_) {
        snprintf(buf, cap, "jobs stopped");
        return;
    }
    int q[PRIO_COUNT];
    uint32_t w[PRIO_COUNT];
    {
        LockGuard g(mutex_);
        for (int p = 0; p < PRIO_COUNT; p++) {
            q[p] = queued_[p];
            w[p] = waitMax_[p];
            waitMax_[p] = 0;
        }
    }
    int n;
    if (nWorkers_ == 0) {
        n = snprintf(buf, cap, "jobs inline");
    } else {
        uint64_t now = plat::micros();
        uint64_t span = now - lastStatusUs_;
        lastStatusUs_ = now;
        n = snprintf(buf, cap, "%d workers busy", nWorkers_);
        for (int i = 0; i < nWorkers_ && n > 0 && (size_t)n < cap; i++) {
            uint64_t busy;
            {
                LockGuard g(mutex_);
                busy = workers_[i].busyUs;
            }
            uint64_t d = busy - workers_[i].reportedUs;
            workers_[i].reportedUs = busy;
            int pct = span ? (int)(d * 100 / span) : 0;
            n += snprintf(buf + n, cap - n, " %d%%", pct > 100 ? 100 : pct);
        }
    }
    if (n > 0 && (size_t)n < cap)
        snprintf(buf + n, cap - n, ", queued %d/%d/%d/%d, max wait %u/%u/%u/%u ms", q[0], q[1], q[2], q[3],
                 (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3]);
}

}  // namespace mc
