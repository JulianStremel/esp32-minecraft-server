#include "mc/tick_pacer.h"
#include "mc/platform.h"

namespace mc {

ClockTickSource::ClockTickSource(uint32_t periodMs, uint32_t (*clock)()) : period_(periodMs ? periodMs : 1), clock_(clock) {
    restart();
}

uint32_t ClockTickSource::now() const { return clock_ ? clock_() : plat::millis(); }

void ClockTickSource::restart() { next_ = now(); }

uint32_t ClockTickSource::take() {
    uint32_t t = now();
    if ((int32_t)(t - next_) < 0) return 0;
    uint32_t n = (t - next_) / period_ + 1;
    next_ += n * period_;
    return n;
}

uint32_t ClockTickSource::msUntilNext() {
    int32_t d = (int32_t)(next_ - now());
    return d > 0 ? (uint32_t)d : 0;
}

uint32_t TickPacer::add(uint32_t signals) {
    skippedNow_ = 0;
    if (signals > 1) win_.overruns++;
    backlog_ += signals;
    if (backlog_ > MAX_BACKLOG) {
        skippedNow_ = backlog_ - 1;
        win_.skipped += skippedNow_;
        backlog_ = 1;
    }
    return backlog_ < MAX_CATCH_UP ? backlog_ : MAX_CATCH_UP;
}

void TickPacer::started() {
    if (backlog_ == 0) return;
    if (backlog_ > 1) win_.late++;
    backlog_--;
}

TickPacer::Counts TickPacer::takeWindow() {
    Counts c = win_;
    win_ = Counts();
    return c;
}

}  // namespace mc
