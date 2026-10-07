// Events scheduled for a game tick: block ticks (fluids, buttons, fire), furnaces and
// mob timers. A hierarchical timing wheel keyed by absolute tick:
//
//   level 0: 256 buckets of 1 tick       (events due within 256 ticks)
//   level 1:  64 buckets of 256 ticks    (within 16384 ticks, 13.6 min)
//   level 2:  64 buckets of 16384 ticks  (within 2^20 ticks, 14.5 h)
//   level 3:  64 buckets of 2^20 ticks   (within 2^26 ticks, 39 days)
//   overflow: everything later
//
// Scheduling and cancelling are O(1). Each tick only takes its own level-0 bucket; when
// the low bits of the tick wrap, the matching bucket of the next level is spread over
// the finer levels. Within a tick, events run by priority (lower first, like vanilla's
// TickPriority) and then in the order they were scheduled. A key can be pending only
// once: scheduling it again is ignored (vanilla's duplicate check), in O(1) through a
// hash of the keys.
#pragma once
#include <stdint.h>

namespace mc {

enum TimerKind : uint8_t {
    TK_NONE = 0,
    TK_BLOCK = 1,     // scheduled block tick: (x, y, z), data = block id
    TK_FURNACE = 2,   // furnace at (x, y, z) finishes an item or runs out of fuel
    TK_ENTITY = 3,    // mob timer: x = entity id, data = timer type
};

struct TimerKey {
    uint8_t kind = TK_NONE;
    int16_t y = 0;
    uint16_t data = 0;
    int32_t x = 0, z = 0;

    static TimerKey block(int x, int y, int z, uint16_t blockId) { return make(TK_BLOCK, x, y, z, blockId); }
    static TimerKey furnace(int x, int y, int z) { return make(TK_FURNACE, x, y, z, 0); }
    static TimerKey entity(int32_t id, uint16_t timer) { return make(TK_ENTITY, id, 0, 0, timer); }
    static TimerKey make(uint8_t kind, int32_t x, int y, int32_t z, uint16_t data) {
        TimerKey k;
        k.kind = kind;
        k.x = x;
        k.y = (int16_t)y;
        k.z = z;
        k.data = data;
        return k;
    }
    bool operator==(const TimerKey& o) const {
        return kind == o.kind && x == o.x && y == o.y && z == o.z && data == o.data;
    }
    uint32_t hash() const;
};

struct TimerEvent {
    TimerKey key;
    uint32_t due = 0;     // absolute tick
    uint32_t seq = 0;     // scheduling order
    int8_t prio = 0;      // lower runs first within a tick
};

class TimerWheel {
public:
    TimerWheel() {}
    ~TimerWheel();
    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    // Allocates room for `capacity` pending events (PSRAM on the ESP32).
    bool init(int capacity);
    // Drops everything; the next tick advance() runs is `tick`.
    void reset(uint32_t tick);
    uint32_t now() const { return cur_; }   // the tick the next advance() runs

    // Schedules `k` for tick `due` (earlier ticks than now() mean now()). false when the
    // key is already pending (that one stays) or the wheel is full.
    bool schedule(const TimerKey& k, uint32_t due, int8_t prio = 0);
    bool cancel(const TimerKey& k);
    bool pending(const TimerKey& k) const { return findNode(k) >= 0; }
    const TimerEvent* find(const TimerKey& k) const;

    // Runs tick now(): copies the events due at it (and any left over from earlier
    // ticks) into `out` in execution order, at most `max`, removes them and moves on to
    // the next tick. Events beyond `max` stay pending and come first next time.
    int advance(TimerEvent* out, int max);

    int size() const { return used_; }
    int capacity() const { return cap_; }
    uint32_t dropped() const { return dropped_; }   // schedule() calls refused because full

    // Visits every pending event (in no particular order).
    template <class F>
    void forEach(F f) const {
        for (int i = 0; i < cap_; i++)
            if (nodes_[i].bucket >= 0) f(nodes_[i].ev);
    }
    // Removes the pending events for which pred(event) is true; returns how many.
    template <class P>
    int removeIf(P pred) {
        int n = 0;
        for (int i = 0; i < cap_; i++)
            if (nodes_[i].bucket >= 0 && pred(nodes_[i].ev)) {
                release(i);
                n++;
            }
        return n;
    }

private:
    static constexpr int L0 = 256, LN = 64;
    static constexpr int B_L1 = L0, B_L2 = L0 + LN, B_L3 = L0 + 2 * LN;
    static constexpr int B_OVERFLOW = L0 + 3 * LN;   // 448
    static constexpr int B_CARRY = B_OVERFLOW + 1;   // left over from earlier ticks
    static constexpr int BUCKETS = B_CARRY + 1;

    struct Node {
        TimerEvent ev;
        int32_t prev, next;   // bucket list
        int16_t bucket;       // -1: free
    };

    int bucketFor(uint32_t due) const;
    void link(int i, int b);           // append to bucket b
    void unlink(int i);
    void place(int i);                 // into the bucket for its due tick
    void cascade(int b);
    int findNode(const TimerKey& k) const;
    void hashInsert(int i);
    void hashRemove(int i);
    void release(int i);               // unlink, unhash, free

    Node* nodes_ = nullptr;
    int32_t* hash_ = nullptr;          // open addressing: node index or -1
    uint32_t hashMask_ = 0;
    int32_t* order_ = nullptr;         // scratch for sorting a tick's events
    int32_t head_[BUCKETS];
    int32_t tail_[BUCKETS];
    int32_t free_ = -1;
    int cap_ = 0, used_ = 0;
    uint32_t cur_ = 0;
    uint32_t seq_ = 0;
    uint32_t dropped_ = 0;
};

}  // namespace mc
