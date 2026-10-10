// Game-loop-owned redstone. No worker reads live circuit state.
#pragma once
#include <stdint.h>

namespace mc {
class Server;
struct Entity;
struct TimerEvent;

class Redstone {
  public:
    Redstone() = default;
    ~Redstone();
    Redstone(const Redstone&) = delete;
    Redstone& operator=(const Redstone&) = delete;
    // Direction is from the receiver toward the queried source (Java getSignal).
    int signal(Server& s, int x, int y, int z, int direction, bool wires = true);
    int directSignal(Server& s, int x, int y, int z, int direction, bool wires = true);
    int bestSignal(Server& s, int x, int y, int z, bool wires = true);
    void changed(Server& s, uint8_t dim, int x, int y, int z, uint16_t oldState, uint16_t newState, uint8_t flags = 3);
    void neighbours(Server& s, int x, int y, int z);
    void updateAt(Server& s, int x, int y, int z);   // the block at x y z reacts to its neighbours
    void switchOutputChanged(Server& s, int x, int y, int z, uint16_t state);
    bool tick(Server& s, const TimerEvent& ev);
    uint16_t wireShape(Server& s, int x, int y, int z, uint16_t state);
    void analogChanged(Server& s, int x, int y, int z);
    int analog(Server& s, int x, int y, int z); // -1: not an analog provider
    void blockEvent(Server& s, int x, int y, int z, uint16_t block, uint8_t type, uint8_t data);
    void playNote(Server& s, int x, int y, int z);
    void runBlockEvents(Server& s);
    void entityInside(Server& s, const Entity& entity);
    void daylightDetector(Server& s, int x, int y, int z, uint16_t state);
    void pressurePlate(Server& s, int x, int y, int z, uint16_t state);
    void button(Server& s, int x, int y, int z, uint16_t state);
    void targetHit(Server& s, int x, int y, int z, int face, double hx, double hy, double hz, bool arrow);
    void tripwire(Server& s, int x, int y, int z, uint16_t state);
    void tripwireChanged(Server& s, int x, int y, int z, uint16_t state);
    void tripwireHook(Server& s, int x, int y, int z, uint16_t state, bool removed = false, int changedDistance = -1,
                      uint16_t changedState = 0);
    bool pinsChunk(uint8_t dim, int cx, int cz) const;
    // sculk sensors (sculk.cpp)
    static bool sculkSensor(uint16_t block);
    int sculkInput(Server& s, int x, int y, int z, uint16_t state);
    void sculkTick(Server& s, int x, int y, int z, uint16_t state);
    int pendingBlockEvents() const { return eventCount_ - eventHead_; }
    uint64_t daylightComputations = 0, daylightComputeUs = 0;
    uint32_t daylightPeakUs = 0;
    uint64_t blockEventsExecuted = 0;
    uint64_t updates = 0;
    uint32_t highWater = 0;
    uint32_t failures = 0;

  private:
    struct BlockEvent {
        int32_t x, z;
        int16_t y;
        uint16_t block;
        uint8_t dim, type, data;
    };
    BlockEvent* events_ = nullptr;
    int eventHead_ = 0, eventCount_ = 0, eventCapacity_ = 0;
    struct Update {
        int32_t x, z;
        int16_t y;
        uint8_t dim, kind;
    };
    struct Burn {
        int32_t x, z;
        uint32_t tick;
        int16_t y;
        uint8_t dim;
    };
    Update* work_ = nullptr;
    int size_ = 0, capacity_ = 0;
    Burn* burns_ = nullptr;
    int burnCount_ = 0, burnCapacity_ = 0;
    bool draining_ = false;
    bool failed_ = false;
    void fail(Server& s);
    void push(Server& s, uint8_t dim, int x, int y, int z, uint8_t kind = 0);
    void drain(Server& s);
    void neighbour(Server& s, int x, int y, int z);
    void output(Server& s, int x, int y, int z, uint16_t state);
    int input(Server& s, int x, int y, int z, uint16_t state);
    int sideInput(Server& s, int x, int y, int z, uint16_t state, bool diodesOnly);
    bool burnedOut(Server& s, int x, int y, int z, bool add);
};
} // namespace mc
