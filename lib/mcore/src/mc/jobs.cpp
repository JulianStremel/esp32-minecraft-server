#include "mc/jobs.h"
#include <stdio.h>
#include "mc/net/deflate.h"
#include "mc/platform.h"

namespace mc {

JobQueue::~JobQueue() {
    stop();
    plat::bigFree(inline_.deflateWs);
}

bool JobQueue::start(int workers, size_t stackBytes) {
    if (mutex_) return true;
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
    Job* j = todoHead_;
    if (j) {
        todoHead_ = j->next_;
        if (!todoHead_) todoTail_ = nullptr;
        j->next_ = nullptr;
    }
    return j;
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
        {
            LockGuard g(q->mutex_);
            w->busyUs += j->runUs;
            if (q->doneTail_) q->doneTail_->next_ = j;
            else q->doneHead_ = j;
            q->doneTail_ = j;
        }
        // under sustained load, let lower-priority tasks run now and then (on the
        // ESP32 the idle task feeds the task watchdog)
        if (plat::millis() - lastPause > 100) {
            plat::yield();
            lastPause = plat::millis();
        }
    }
    plat::semGive(q->exitSem_);
}

void JobQueue::submit(Job* j) {
    if (!mutex_) start(0);
    j->next_ = nullptr;
    inFlight_++;
    {
        LockGuard g(mutex_);
        if (todoTail_) todoTail_->next_ = j;
        else todoHead_ = j;
        todoTail_ = j;
    }
    if (nWorkers_ > 0) plat::semGive(todoSem_);
}

int JobQueue::poll(int maxJobs) {
    if (!mutex_) return 0;
    if (nWorkers_ == 0) {
        // inline mode: run the queued jobs here
        if (!inline_.deflateWs) inline_.deflateWs = (uint8_t*)plat::bigAlloc(DeflateSink::WORKSPACE);
        int n = 0;
        while (n < maxJobs) {
            Job* j = popTodo();
            if (!j) break;
            uint64_t t0 = plat::micros();
            j->run(inline_);
            j->runUs = (uint32_t)(plat::micros() - t0);
            finishOne(j);
            n++;
        }
        return n;
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
    if (nWorkers_ == 0) {
        snprintf(buf, cap, "jobs inline, %d queued", inFlight_);
        return;
    }
    uint64_t now = plat::micros();
    uint64_t span = now - lastStatusUs_;
    lastStatusUs_ = now;
    int n = snprintf(buf, cap, "%d workers, %d jobs queued, busy", nWorkers_, inFlight_);
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

}  // namespace mc
