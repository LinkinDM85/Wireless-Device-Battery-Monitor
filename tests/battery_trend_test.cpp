// test-sources: src/battery_trend.cpp
#include "../src/battery_trend.h"

#include <cassert>
#include <cstdio>

namespace {

constexpr int64_t H = 3600;

void TestNeedsTwoKnownPoints() {
    BatteryTrend t;
    int64_t now = 1'000'000;
    assert(!t.Update(57, false, now));  // first reading: somewhere inside the 57 % level
    assert(t.RemainingSec(now) == -1);
    assert(t.Update(56, false, now + 1 * H));  // first drop: start point
    assert(t.RemainingSec(now + 1 * H) == -1);
    assert(!t.Update(56, false, now + 2 * H));
    assert(t.Update(55, false, now + 3 * H));  // 1 % in 2 h: not enough drop yet
    assert(t.RemainingSec(now + 3 * H) == -1);
    assert(t.Update(54, false, now + 5 * H));  // 2 % in 4 h -> 2 h per percent
    int64_t left = t.RemainingSec(now + 5 * H);
    assert(left == 54 * 2 * H);
    // An hour later it is still at 54 %: one hour less.
    assert(t.RemainingSec(now + 6 * H) == 54 * 2 * H - 1 * H);
}

void TestOverdueDropMeansSlower() {
    BatteryTrend t;
    t.Update(50, false, 0);
    t.Update(40, false, 0);       // start 40 at t=0
    t.Update(30, false, 10 * H);  // 10 % in 10 h -> 1 h per percent, 30 h left
    assert(t.RemainingSec(10 * H) == 30 * H);
    // 30 hours later still at 30 %: it cannot have used more than 11 % in 40 h.
    int64_t left = t.RemainingSec(40 * H);
    double perPct = 40.0 * H / 11;
    assert(left == (int64_t)(30 * perPct - perPct));
}

void TestChargingAndRiseStartOver() {
    BatteryTrend t;
    t.Update(80, false, 0);
    t.Update(70, false, 1 * H);
    t.Update(60, false, 3 * H);
    assert(t.RemainingSec(3 * H) > 0);
    assert(t.Update(61, true, 4 * H));  // charging
    assert(t.RemainingSec(4 * H) == -1);
    assert(t.startPct == -1 && t.seenPct == -1);

    BatteryTrend u;
    u.Update(80, false, 0);
    u.Update(70, false, 1 * H);
    u.Update(60, false, 3 * H);
    assert(!u.Update(62, false, 4 * H));  // a small wobble upwards is ignored
    assert(u.RemainingSec(4 * H) > 0);
    assert(u.Update(95, false, 5 * H));  // charged while the app was not running
    assert(u.RemainingSec(5 * H) == -1);
    assert(u.seenPct == 95);
}

void TestWobbleDoesNotMoveThePoints() {
    BatteryTrend t;
    t.Update(41, false, 0);
    t.Update(40, false, 1 * H);
    t.Update(41, false, 2 * H);
    t.Update(40, false, 3 * H);  // back to 40: not a new level
    assert(t.curTime == 1 * H);
}

void TestSerialize() {
    BatteryTrend t;
    t.Update(80, false, 1000);
    t.Update(70, false, 2000);
    t.Update(60, false, 2000 + 5 * H);
    std::string s = t.Serialize();
    BatteryTrend back = BatteryTrend::Parse(s, 2000 + 6 * H);
    assert(back.startPct == 70 && back.curPct == 60 && back.startTime == 2000 && back.curTime == 2000 + 5 * H);
    assert(back.RemainingSec(2000 + 6 * H) == t.RemainingSec(2000 + 6 * H));
    // After a restart a higher level means it was charged.
    assert(back.Update(100, false, 2000 + 7 * H));
    assert(back.RemainingSec(2000 + 7 * H) == -1);
    // Garbage and stale data are ignored.
    assert(BatteryTrend::Parse("nonsense", 0).startPct == -1);
    assert(BatteryTrend::Parse("1,50,0,60,10", 100).startPct == -1);  // cur above start
    assert(BatteryTrend::Parse(s, 2000 + 30 * 24 * H).startPct == -1);
    assert(BatteryTrend().Serialize().empty());
}

void TestFormat() {
    assert(FormatRemaining(-1).empty());
    assert(FormatRemaining(1200) == L"under 1 h");
    assert(FormatRemaining(5 * H) == L"~5 h");
    assert(FormatRemaining(47 * H) == L"~47 h");
    assert(FormatRemaining(3 * 24 * H) == L"~3 days");
}

}  // namespace

int main() {
    TestNeedsTwoKnownPoints();
    TestOverdueDropMeansSlower();
    TestChargingAndRiseStartOver();
    TestWobbleDoesNotMoveThePoints();
    TestSerialize();
    TestFormat();
    printf("Battery trend tests passed (known points, overdue drops, charging, wobble, storage, formatting)\n");
    return 0;
}
