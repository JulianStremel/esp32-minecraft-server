// Game tick timing for the event-driven game loop.
//
// A TickSource signals elapsed 50 ms periods. The firmware uses a periodic hardware
// timer that notifies the game task (src/platform_esp32.cpp); everything else derives
// the periods from a millisecond clock. The TickPacer turns those signals into ticks to
// run and counts how often the loop fell behind.
#pragma once
#include <stdint.h>

namespace mc {

class TickSource {
public:
    virtual ~TickSource() {}
    // Tick periods that elapsed since the last call (0 if none). More than one means the
    // game loop did not get to run for a whole period: an overrun.
    virtual uint32_t take() = 0;
    // Milliseconds until the next period ends (how long the loop may sleep).
    virtual uint32_t msUntilNext() = 0;
};

// Periods measured with a millisecond clock (plat::millis unless another one is given).
class ClockTickSource : public TickSource {
public:
    explicit ClockTickSource(uint32_t periodMs = 50, uint32_t (*clock)() = nullptr);
    void restart();             // the first period is due right away
    uint32_t take() override;
    uint32_t msUntilNext() override;

private:
    uint32_t now() const;
    uint32_t period_;
    uint32_t (*clock_)();
    uint32_t next_ = 0;         // when the next period is due
};

// Decides how many ticks run now. Late ticks are caught up a few per loop() call; a
// backlog of more than a second is dropped (vanilla: "Can't keep up!").
class TickPacer {
public:
    static constexpr uint32_t MAX_CATCH_UP = 5;   // ticks run back to back per loop() call
    static constexpr uint32_t MAX_BACKLOG = 20;   // more ticks pending (1 s): skip them

    struct Counts {
        uint32_t overruns = 0;  // wakeups that found more than one tick due
        uint32_t late = 0;      // ticks that ran while another one was already due
        uint32_t skipped = 0;   // ticks dropped
    };

    // New tick signals; returns how many ticks to run now (at most MAX_CATCH_UP).
    uint32_t add(uint32_t signals);
    // Before each tick runs.
    void started();
    uint32_t backlog() const { return backlog_; }        // ticks due, not yet run
    uint32_t skippedNow() const { return skippedNow_; }  // dropped by the last add()
    // Counts since the last call.
    Counts takeWindow();

private:
    uint32_t backlog_ = 0;
    uint32_t skippedNow_ = 0;
    Counts win_;
};

}  // namespace mc
