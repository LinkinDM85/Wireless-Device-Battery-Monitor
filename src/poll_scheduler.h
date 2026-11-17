#pragma once
// Runs the drivers from registry.cpp.
//
// Parallel: drivers are grouped by shared vendor ids. Inside a group they run one after another in
// list order, exactly as before; different groups have nothing in common, so they run at the same
// time, each on its own thread with only its own vendors' collections in view. The unrestricted
// fallbacks then run on the merged result. Should a driver ask about a vendor id it did not declare,
// the poll is redone one driver after another and parallel polling stays off from then on, so a
// mistake in the list can delay a device by one poll but never send it another driver's commands.
//
// Adaptive: a driver whose devices report the same thing poll after poll is queried less often
// (twice the interval, then four times, up to maxIntervalMs); its last result is shown in between.
// Devices that are all asleep or switched off go straight to the longest interval so the poll does
// not keep waking them. A forced poll (start, "Refresh now", a device plugged in or removed, resume
// from sleep) queries every driver.
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "drivers/registry.h"

struct PollOptions {
    bool parallel = true;
    bool adaptive = true;
    bool force = false;
    uint64_t nowMs = 0;  // a monotonic clock, used only for the adaptive intervals
    uint64_t baseIntervalMs = 60 * 1000;
    uint64_t maxIntervalMs = 5 * 60 * 1000;
};

struct PollReport {
    bool parallel = false;  // groups ran at the same time
    int threads = 0;        // groups that had devices and got their own thread
    int reused = 0;         // drivers whose previous result was shown instead of asking again
    std::vector<std::string> violations;  // drivers that asked about a vendor id they did not declare
    struct Timing {
        std::string name;
        double ms = 0;
        bool reused = false;
        int devices = 0;
    };
    std::vector<Timing> timings;  // one per driver, in list order
    double totalMs = 0;
};

class PollScheduler {
public:
    explicit PollScheduler(std::vector<DriverInfo> drivers);

    std::vector<DeviceStatus> Poll(const std::vector<HidColl>& colls, const PollOptions& opt);
    const PollReport& Report() const { return report_; }
    bool ParallelDisabled() const { return sequentialOnly_; }

    // Problems with the order of the list (an unrestricted driver before a restricted one); empty when fine.
    static std::vector<std::string> CheckOrder(const std::vector<DriverInfo>& drivers);
    // Indices of restricted drivers that share vendor ids, directly or through other drivers.
    static std::vector<std::vector<size_t>> Groups(const std::vector<DriverInfo>& drivers);

private:
    struct Run {
        size_t begin = 0, end = 0;  // this driver's entries in its context's `out`
        std::vector<DeviceStatus> out;
        std::vector<std::pair<USHORT, USHORT>> claimed;
        std::vector<std::wstring> bt;
        std::set<USHORT> requested;
        double ms = 0;
        bool reused = false;
    };
    struct State {
        bool have = false;
        Run last;
        uint64_t nextDueMs = 0;
        int unchanged = 0;
    };

    bool CanReuse(size_t i, const PollContext& ctx, const PollOptions& opt) const;
    void RunOne(size_t i, PollContext& ctx, const PollOptions& opt, Run& run) const;
    std::vector<DeviceStatus> PollSequential(const std::vector<HidColl>& colls, const PollOptions& opt, std::vector<Run>& runs);
    bool PollParallel(const std::vector<HidColl>& colls, const PollOptions& opt, std::vector<Run>& runs,
                      std::vector<DeviceStatus>& out);
    void Finish(const PollOptions& opt, const std::vector<Run>& runs);
    void CollectViolations(const std::vector<Run>& runs, std::vector<std::string>& into) const;

    std::vector<DriverInfo> drivers_;
    std::vector<std::vector<size_t>> groups_;
    bool orderOk_ = true;
    bool sequentialOnly_ = false;
    std::vector<State> state_;
    PollReport report_;
};
