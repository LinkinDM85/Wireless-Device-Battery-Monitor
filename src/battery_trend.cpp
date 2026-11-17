#include "battery_trend.h"

#include <algorithm>
#include <cstdio>

namespace {
constexpr int kRisePct = 5;  // a level this much above the lowest one means it was charged meanwhile
}

bool BatteryTrend::Update(int pct, bool charging, int64_t now) {
    if (pct < 0 || pct > 100) return false;
    bool stored = startPct >= 0;
    if (charging) {
        *this = BatteryTrend{};
        return stored;
    }
    if (stored && now - curTime > kForgetAfterSec) {  // not seen for weeks: old data says nothing
        *this = BatteryTrend{};
        seenPct = pct;
        return true;
    }
    if (seenPct < 0) {  // first reading since start or since charging
        seenPct = pct;
        return false;
    }
    if (pct >= seenPct + kRisePct) {  // charged while we were not looking
        *this = BatteryTrend{};
        seenPct = pct;
        return stored;
    }
    if (pct < seenPct) {  // a new lowest level: the moment it was first reported is a known point
        if (startPct < 0) {
            startPct = pct;
            startTime = now;
        }
        curPct = pct;
        curTime = now;
        seenPct = pct;
        return true;
    }
    return false;  // same level, or a small wobble upwards
}

int64_t BatteryTrend::RemainingSec(int64_t now) const {
    if (startPct < 0 || curPct < 0) return -1;
    int drop = startPct - curPct;
    int64_t span = curTime - startTime;
    if (drop < kMinDropPct || span < kMinSpanSec) return -1;
    if (curPct == 0) return 0;
    double perPct = (double)span / drop;
    // The level has not gone below curPct yet, so less than drop + 1 percent was used since the
    // start: if the next drop is overdue, the device is draining more slowly now.
    double since = (double)std::max<int64_t>(0, now - curTime);
    perPct = std::max(perPct, (double)(now - startTime) / (drop + 1));
    double left = curPct * perPct - std::min(since, perPct);
    return (int64_t)std::max(0.0, left);
}

std::string BatteryTrend::Serialize() const {
    if (startPct < 0) return {};
    char b[96];
    snprintf(b, sizeof(b), "1,%d,%lld,%d,%lld", startPct, (long long)startTime, curPct, (long long)curTime);
    return b;
}

BatteryTrend BatteryTrend::Parse(const std::string& s, int64_t now) {
    BatteryTrend t;
    int v = 0, sp = -1, cp = -1;
    long long st = 0, ct = 0;
    if (sscanf(s.c_str(), "%d,%d,%lld,%d,%lld", &v, &sp, &st, &cp, &ct) != 5 || v != 1) return t;
    if (sp < 0 || sp > 100 || cp < 0 || cp > sp || ct < st || now - ct > kForgetAfterSec || ct > now + 3600) return t;
    t.startPct = sp;
    t.startTime = st;
    t.curPct = cp;
    t.curTime = ct;
    t.seenPct = cp;
    return t;
}

std::wstring FormatRemaining(int64_t sec) {
    if (sec < 0) return {};
    if (sec < 3600) return L"under 1 h";
    int64_t hours = (sec + 1800) / 3600;
    if (hours < 48) return L"~" + std::to_wstring(hours) + L" h";
    int64_t days = (sec + 43200) / 86400;
    return L"~" + std::to_wstring(days) + L" days";
}
