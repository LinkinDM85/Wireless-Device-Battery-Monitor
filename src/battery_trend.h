#pragma once
// Estimates how long a device's battery will last from how fast its level has been dropping.
//
// Devices report coarse levels (often in 5 or 10 % steps), so a reading on its own says little about
// time. The estimate is built from level changes instead: the first drop that is seen fixes a start
// point (level and the moment it was first reported), every later drop moves the end point, and the
// time per percent between the two gives the rate. It needs a drop of at least kMinDropPct over at
// least kMinSpanSec before it says anything. Charging, or a level that has gone up (charged while
// the app was not running), starts over.
//
// Times are wall-clock seconds, so a device that keeps discharging while the PC is off is counted
// too. The state is small and stored per device (see Serialize / Parse).
#include <cstdint>
#include <string>

struct BatteryTrend {
    int seenPct = -1;       // last reading
    int startPct = -1;      // level right after the first drop seen
    int64_t startTime = 0;
    int curPct = -1;        // level right after the latest drop
    int64_t curTime = 0;

    static constexpr int kMinDropPct = 2;
    static constexpr int64_t kMinSpanSec = 20 * 60;
    static constexpr int64_t kForgetAfterSec = 21 * 24 * 3600;  // older than this: start over

    // Feeds one reading. Returns true when the stored part changed (worth saving).
    bool Update(int pct, bool charging, int64_t now);
    // Seconds until the battery is empty, or -1 while there is not enough data.
    int64_t RemainingSec(int64_t now) const;

    std::string Serialize() const;
    static BatteryTrend Parse(const std::string& s, int64_t now);
};

// "under 1 h", "~5 h", "~3 days"; empty for a negative value.
std::wstring FormatRemaining(int64_t sec);
