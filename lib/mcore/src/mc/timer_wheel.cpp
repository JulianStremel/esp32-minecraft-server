#include "mc/timer_wheel.h"
#include <algorithm>
#include "mc/platform.h"

namespace mc {

uint32_t TimerKey::hash() const {
    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        h ^= v;
        h *= 16777619u;
        h ^= h >> 15;
    };
    mix(kind);
    mix((uint32_t)x);
    mix((uint32_t)(uint16_t)y | (uint32_t)data << 16);
    mix((uint32_t)z);
    return h;
}

TimerWheel::~TimerWheel() {
    plat::bigFree(nodes_);
    plat::bigFree(hash_);
    plat::bigFree(order_);
}

bool TimerWheel::init(int capacity) {
    plat::bigFree(nodes_);
    plat::bigFree(hash_);
    plat::bigFree(order_);
    nodes_ = nullptr;
    hash_ = nullptr;
    order_ = nullptr;
    cap_ = 0;
    if (capacity < 1) capacity = 1;
    uint32_t hsize = 16;
    while (hsize < (uint32_t)capacity * 2) hsize <<= 1;
    nodes_ = (Node*)plat::bigAlloc(sizeof(Node) * (size_t)capacity);
    hash_ = (int32_t*)plat::bigAlloc(sizeof(int32_t) * hsize);
    order_ = (int32_t*)plat::bigAlloc(sizeof(int32_t) * (size_t)capacity);
    if (!nodes_ || !hash_ || !order_) return false;
    cap_ = capacity;
    hashMask_ = hsize - 1;
    reset(0);
    return true;
}

void TimerWheel::reset(uint32_t tick) {
    for (int b = 0; b < BUCKETS; b++) head_[b] = tail_[b] = -1;
    for (uint32_t i = 0; i <= hashMask_ && hash_; i++) hash_[i] = -1;
    free_ = -1;
    for (int i = cap_ - 1; i >= 0; i--) {
        nodes_[i].bucket = -1;
        nodes_[i].next = free_;
        free_ = i;
    }
    used_ = 0;
    cur_ = tick;
}

int TimerWheel::bucketFor(uint32_t due) const {
    uint32_t d = due - cur_;   // due >= cur_ (schedule() clamps)
    if (d < (1u << 8)) return (int)(due & 255);
    if (d < (1u << 14)) return B_L1 + (int)((due >> 8) & 63);
    if (d < (1u << 20)) return B_L2 + (int)((due >> 14) & 63);
    if (d < (1u << 26)) return B_L3 + (int)((due >> 20) & 63);
    return B_OVERFLOW;
}

void TimerWheel::link(int i, int b) {
    Node& n = nodes_[i];
    n.bucket = (int16_t)b;
    n.next = -1;
    n.prev = tail_[b];
    if (tail_[b] >= 0) nodes_[tail_[b]].next = i;
    else head_[b] = i;
    tail_[b] = i;
}

void TimerWheel::unlink(int i) {
    Node& n = nodes_[i];
    int b = n.bucket;
    if (n.prev >= 0) nodes_[n.prev].next = n.next;
    else head_[b] = n.next;
    if (n.next >= 0) nodes_[n.next].prev = n.prev;
    else tail_[b] = n.prev;
    n.prev = n.next = -1;
}

void TimerWheel::place(int i) { link(i, bucketFor(nodes_[i].ev.due)); }

void TimerWheel::cascade(int b) {
    int i = head_[b];
    head_[b] = tail_[b] = -1;
    while (i >= 0) {
        int next = nodes_[i].next;
        place(i);   // lands in a finer level now
        i = next;
    }
}

int TimerWheel::findNode(const TimerKey& k) const {
    if (!hash_ || !cap_) return -1;
    for (uint32_t h = k.hash() & hashMask_;; h = (h + 1) & hashMask_) {
        int32_t i = hash_[h];
        if (i < 0) return -1;
        if (nodes_[i].ev.key == k) return i;
    }
}

const TimerEvent* TimerWheel::find(const TimerKey& k) const {
    int i = findNode(k);
    return i < 0 ? nullptr : &nodes_[i].ev;
}

void TimerWheel::hashInsert(int i) {
    uint32_t h = nodes_[i].ev.key.hash() & hashMask_;
    while (hash_[h] >= 0) h = (h + 1) & hashMask_;
    hash_[h] = i;
}

void TimerWheel::hashRemove(int i) {
    uint32_t h = nodes_[i].ev.key.hash() & hashMask_;
    while (hash_[h] != i) h = (h + 1) & hashMask_;
    // backward-shift deletion keeps every probe chain unbroken
    uint32_t hole = h;
    hash_[hole] = -1;
    for (uint32_t j = (hole + 1) & hashMask_; hash_[j] >= 0; j = (j + 1) & hashMask_) {
        uint32_t home = nodes_[hash_[j]].ev.key.hash() & hashMask_;
        // move j back unless its home is cyclically in (hole, j]
        bool inRange = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
        if (!inRange) {
            hash_[hole] = hash_[j];
            hash_[j] = -1;
            hole = j;
        }
    }
}

void TimerWheel::release(int i) {
    unlink(i);
    hashRemove(i);
    nodes_[i].bucket = -1;
    nodes_[i].next = free_;
    free_ = i;
    used_--;
}

bool TimerWheel::schedule(const TimerKey& k, uint32_t due, int8_t prio) {
    if (findNode(k) >= 0) return false;
    if (free_ < 0) {
        dropped_++;
        return false;
    }
    if ((int32_t)(due - cur_) < 0) due = cur_;
    int i = free_;
    free_ = nodes_[i].next;
    Node& n = nodes_[i];
    n.ev.key = k;
    n.ev.due = due;
    n.ev.prio = prio;
    n.ev.seq = seq_++;
    n.prev = n.next = -1;
    hashInsert(i);
    place(i);
    used_++;
    return true;
}

bool TimerWheel::cancel(const TimerKey& k) {
    int i = findNode(k);
    if (i < 0) return false;
    release(i);
    return true;
}

int TimerWheel::advance(TimerEvent* out, int max) {
    if (!cap_) {
        cur_++;
        return 0;
    }
    // spread the coarser bucket that starts now over the finer levels (coarsest first)
    if ((cur_ & 255) == 0) {
        if (((cur_ >> 8) & 63) == 0) {
            if (((cur_ >> 14) & 63) == 0) {
                if (((cur_ >> 20) & 63) == 0) cascade(B_OVERFLOW);
                cascade(B_L3 + (int)((cur_ >> 20) & 63));
            }
            cascade(B_L2 + (int)((cur_ >> 14) & 63));
        }
        cascade(B_L1 + (int)((cur_ >> 8) & 63));
    }
    // this tick's events, by priority and then scheduling order
    int slot = (int)(cur_ & 255);
    int m = 0;
    for (int i = head_[slot]; i >= 0; i = nodes_[i].next) order_[m++] = i;
    std::sort(order_, order_ + m, [this](int32_t a, int32_t b) {
        const TimerEvent& x = nodes_[a].ev;
        const TimerEvent& y = nodes_[b].ev;
        if (x.prio != y.prio) return x.prio < y.prio;
        return (int32_t)(x.seq - y.seq) < 0;
    });
    int n = 0;
    // events left over from earlier ticks come first, in their order
    while (n < max && head_[B_CARRY] >= 0) {
        int i = head_[B_CARRY];
        out[n++] = nodes_[i].ev;
        release(i);
    }
    for (int k = 0; k < m; k++) {
        int i = order_[k];
        if (n < max) {
            out[n++] = nodes_[i].ev;
            release(i);
        } else {
            unlink(i);
            link(i, B_CARRY);   // keeps its key pending; runs next time, in order
        }
    }
    cur_++;
    return n;
}

}  // namespace mc
