// The log lines kept for the dashboard's console: order, overwriting, long lines.
#include <string.h>
#include <string>
#include <vector>
#include "testing.h"
#include "mc/log_ring.h"
#include "mc/platform.h"

using namespace mc;

namespace {
struct Got {
    uint32_t seq, ms;
    uint8_t level;
    std::string text;
};
std::vector<Got> readAll(LogRing& r, uint32_t from, uint32_t* next = nullptr, size_t max = 100000) {
    std::vector<Got> v;
    uint32_t n = r.read(from, [&](uint32_t seq, uint32_t ms, uint8_t level, const char* text, size_t len) {
        if (v.size() == max) return false;
        CHECK_EQ(strlen(text), len);
        v.push_back(Got{seq, ms, level, std::string(text, len)});
        return true;
    });
    if (next) *next = n;
    return v;
}
}  // namespace

TEST(log_ring_keeps_lines_in_order_and_reads_from_a_sequence_number) {
    LogRing r;
    CHECK(r.init(4096));
    r.append(1, 100, "first");
    r.append(2, 200, "second");
    r.append(3, 300, "");
    uint32_t next = 0;
    std::vector<Got> v = readAll(r, 0, &next);
    CHECK_EQ(v.size(), (size_t)3);
    CHECK_EQ(next, 3u);
    CHECK(v[0].text == "first" && v[0].ms == 100 && v[0].level == 1 && v[0].seq == 0);
    CHECK(v[1].text == "second" && v[1].level == 2);
    CHECK(v[2].text.empty() && v[2].ms == 300);
    // from a later line; nothing new; a reader that stops early resumes where it stopped
    v = readAll(r, 1);
    CHECK(v.size() == 2 && v[0].text == "second");
    CHECK(readAll(r, 3).empty());
    v = readAll(r, 0, &next, 1);
    CHECK(v.size() == 1 && next == 1);
}

TEST(log_ring_overwrites_the_oldest_lines_and_cuts_long_ones) {
    LogRing r;
    CHECK(r.init(1000));   // not a multiple of the records: they wrap around the end
    char line[64];
    for (int i = 0; i < 500; i++) {
        snprintf(line, sizeof(line), "line %d %.*s", i, i % 30, "abcdefghijklmnopqrstuvwxyz0123456789");
        r.append((uint8_t)(i & 3), (uint32_t)i, line);
        CHECK(r.used() <= r.capacity());
    }
    CHECK_EQ(r.next(), 500u);
    CHECK(r.first() > 400u);   // only the last few dozen fit
    uint32_t next = 0;
    std::vector<Got> v = readAll(r, 0, &next);   // too old: from the oldest kept
    CHECK_EQ(next, 500u);
    CHECK_EQ(v.size(), (size_t)(500 - r.first()));
    for (size_t k = 0; k < v.size(); k++) {
        int i = (int)(r.first() + k);
        snprintf(line, sizeof(line), "line %d %.*s", i, i % 30, "abcdefghijklmnopqrstuvwxyz0123456789");
        CHECK(v[k].text == line);
        CHECK_EQ(v[k].seq, (uint32_t)i);
        CHECK_EQ(v[k].ms, (uint32_t)i);
    }
    std::string big(400, 'x');
    r.append(0, 1, big.c_str());
    v = readAll(r, 500);
    CHECK(v.size() == 1 && v[0].text.size() == LogRing::MAX_LINE);
}

TEST(log_ring_takes_what_logf_writes_once_installed) {
    LogRing r;
    CHECK(r.init(4096));
    MC_LOGI("before %d", 1);   // not installed yet
    setLogRing(&r);
    MC_LOGW("kept %d", 2);
    setLogRing(nullptr);
    std::vector<Got> v = readAll(r, 0);
    CHECK(v.size() == 1 && v[0].text == "kept 2" && v[0].level == LOG_WARN);
}

TEST(log_ring_cursor_resumes_where_it_stopped_and_survives_overwriting) {
    LogRing r;
    CHECK(r.init(600));
    LogRing::Cursor cur;
    char line[48];
    for (int round = 0; round < 60; round++) {
        for (int k = 0; k < round % 7; k++) {
            snprintf(line, sizeof(line), "r%d k%d %.*s", round, k, round % 20, "........................");
            r.append(1, (uint32_t)round, line);
        }
        if (round % 13 == 12) continue;   // a reader that falls behind: lines overwritten meanwhile
        uint32_t from = cur.seq;
        std::vector<Got> fresh = readAll(r, from);
        std::vector<std::string> got;
        r.read(cur, [&](uint32_t, uint32_t, uint8_t, const char* t, size_t) {
            if (got.size() == 3) return false;   // stops early now and then: this line stays unread
            got.push_back(t);
            return true;
        });
        CHECK_EQ(got.size(), std::min(fresh.size(), (size_t)3));
        for (size_t i = 0; i < got.size(); i++) CHECK(got[i] == fresh[i].text);
        CHECK_EQ(cur.seq, fresh.empty() ? r.next() : fresh[0].seq + (uint32_t)got.size());
    }
}
