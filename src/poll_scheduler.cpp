#include "poll_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#else
#include <thread>
#endif

namespace {

// A minimal thread (the Windows build uses plain Win32 threads, like the rest of the app).
class Worker {
public:
    explicit Worker(std::function<void()> fn) : fn_(std::move(fn)) {
#ifdef _WIN32
        h_ = CreateThread(nullptr, 0, Entry, this, 0, nullptr);
        if (!h_) fn_();  // could not start a thread: just run it here
#else
        t_ = std::thread(fn_);
#endif
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    void Join() {
#ifdef _WIN32
        if (h_) { WaitForSingleObject(h_, INFINITE); CloseHandle(h_); h_ = nullptr; }
#else
        if (t_.joinable()) t_.join();
#endif
    }
    ~Worker() { Join(); }

private:
    std::function<void()> fn_;
#ifdef _WIN32
    static DWORD WINAPI Entry(LPVOID p) {
        static_cast<Worker*>(p)->fn_();
        return 0;
    }
    HANDLE h_ = nullptr;
#else
    std::thread t_;
#endif
};

double MsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// What a driver reported, minus things that wobble from poll to poll (battery voltage).
bool SameResult(const std::vector<DeviceStatus>& a, const std::vector<DeviceStatus>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        const DeviceStatus &x = a[i], &y = b[i];
        if (x.key != y.key || x.percent != y.percent || x.charging != y.charging || x.asleep != y.asleep ||
            x.connected != y.connected || x.levelText != y.levelText || x.detail != y.detail)
            return false;
    }
    return true;
}

bool AllResting(const std::vector<DeviceStatus>& out) {
    for (const auto& s : out)
        if (s.connected && !s.asleep) return false;
    return !out.empty();
}

std::string HexId(USHORT v) {
    char b[8];
    snprintf(b, sizeof(b), "%04X", v);
    return b;
}

}  // namespace

PollScheduler::PollScheduler(std::vector<DriverInfo> drivers) : drivers_(std::move(drivers)) {
    groups_ = Groups(drivers_);
    orderOk_ = CheckOrder(drivers_).empty();
    state_.resize(drivers_.size());
}

std::vector<std::string> PollScheduler::CheckOrder(const std::vector<DriverInfo>& drivers) {
    std::vector<std::string> problems;
    const DriverInfo* firstUnrestricted = nullptr;
    for (const auto& d : drivers) {
        if (d.vids.empty()) {
            if (!firstUnrestricted) firstUnrestricted = &d;
        } else if (firstUnrestricted) {
            problems.push_back(std::string(d.name) + " has vendor ids but comes after the unrestricted " + firstUnrestricted->name);
        }
    }
    return problems;
}

std::vector<std::vector<size_t>> PollScheduler::Groups(const std::vector<DriverInfo>& drivers) {
    // Union-find over drivers, joined through every vendor id they share.
    std::vector<size_t> parent(drivers.size());
    for (size_t i = 0; i < parent.size(); i++) parent[i] = i;
    auto root = [&](size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    std::map<USHORT, size_t> owner;
    for (size_t i = 0; i < drivers.size(); i++)
        for (USHORT v : drivers[i].vids) {
            auto it = owner.find(v);
            if (it == owner.end()) owner[v] = i;
            else parent[root(i)] = root(it->second);
        }
    std::map<size_t, std::vector<size_t>> byRoot;
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < drivers.size(); i++)
        if (!drivers[i].vids.empty()) byRoot[root(i)].push_back(i);
    for (auto& [r, members] : byRoot) groups.push_back(members);  // members stay in list order
    std::sort(groups.begin(), groups.end());
    return groups;
}

bool PollScheduler::CanReuse(size_t i, const PollContext& ctx, const PollOptions& opt) const {
    const State& st = state_[i];
    if (!opt.adaptive || opt.force || !st.have || drivers_[i].vids.empty()) return false;
    if (st.last.out.empty() || st.last.claimed.empty()) return false;  // nothing found: always look again
    if (opt.nowMs + 2000 >= st.nextDueMs) return false;
    for (const auto& p : st.last.claimed) {
        if (ctx.claimed.count(p)) return false;  // another driver took it meanwhile
        bool present = false;
        for (const auto& c : ctx.colls)
            if (c.vid == p.first && c.pid == p.second) { present = true; break; }
        if (!present) return false;  // unplugged: ask again so the device disappears now
    }
    return true;
}

void PollScheduler::RunOne(size_t i, PollContext& ctx, const PollOptions& opt, Run& run) const {
    run = Run{};
    run.begin = ctx.out.size();
    if (CanReuse(i, ctx, opt)) {
        const Run& last = state_[i].last;
        ctx.out.insert(ctx.out.end(), last.out.begin(), last.out.end());
        for (const auto& p : last.claimed) ctx.claimed.insert(p);
        for (const auto& b : last.bt) ctx.btContainers.insert(b);
        run.claimed = last.claimed;
        run.bt = last.bt;
        run.reused = true;
        run.end = ctx.out.size();
        return;
    }
    auto claimedBefore = ctx.claimed;
    auto btBefore = ctx.btContainers;
    ctx.requested.clear();
    auto t0 = std::chrono::steady_clock::now();
    try {
        drivers_[i].poll(ctx);
    } catch (...) {
        // A driver must never take the whole poll down.
    }
    run.ms = MsSince(t0);
    run.requested = ctx.requested;
    for (const auto& p : ctx.claimed)
        if (!claimedBefore.count(p)) run.claimed.push_back(p);
    for (const auto& b : ctx.btContainers)
        if (!btBefore.count(b)) run.bt.push_back(b);
    run.end = ctx.out.size();
}

std::vector<DeviceStatus> PollScheduler::PollSequential(const std::vector<HidColl>& colls, const PollOptions& opt,
                                                        std::vector<Run>& runs) {
    PollContext ctx;
    ctx.colls = colls;
    for (size_t i = 0; i < drivers_.size(); i++) RunOne(i, ctx, opt, runs[i]);
    // Taken after the whole poll: a later driver may merge a duplicate into an earlier entry.
    for (auto& r : runs) r.out.assign(ctx.out.begin() + r.begin, ctx.out.begin() + r.end);
    return ctx.out;
}

bool PollScheduler::PollParallel(const std::vector<HidColl>& colls, const PollOptions& opt, std::vector<Run>& runs,
                                 std::vector<DeviceStatus>& result) {
    struct GroupJob {
        const std::vector<size_t>* members;
        PollContext ctx;
    };
    std::vector<GroupJob> jobs(groups_.size());
    std::vector<std::unique_ptr<Worker>> threads;
    std::vector<size_t> inline_;
    for (size_t g = 0; g < groups_.size(); g++) {
        jobs[g].members = &groups_[g];
        std::set<USHORT> vids;
        for (size_t i : groups_[g]) vids.insert(drivers_[i].vids.begin(), drivers_[i].vids.end());
        for (const auto& c : colls)
            if (vids.count(c.vid)) jobs[g].ctx.colls.push_back(c);
    }
    auto runGroup = [&](GroupJob& job) {
        for (size_t i : *job.members) RunOne(i, job.ctx, opt, runs[i]);
        for (size_t i : *job.members) runs[i].out.assign(job.ctx.out.begin() + runs[i].begin, job.ctx.out.begin() + runs[i].end);
    };
    // Groups with none of their devices plugged in finish at once; only the others get a thread.
    for (size_t g = 0; g < jobs.size(); g++) {
        if (jobs[g].ctx.colls.empty()) inline_.push_back(g);
        else threads.push_back(std::make_unique<Worker>([&runGroup, &jobs, g] { runGroup(jobs[g]); }));
    }
    for (size_t g : inline_) runGroup(jobs[g]);
    for (auto& t : threads) t->Join();
    report_.threads = (int)threads.size();

    std::vector<std::string> violations;
    CollectViolations(runs, violations);
    if (!violations.empty()) {
        report_.violations = violations;
        return false;
    }

    // Merge in list order, then let the unrestricted fallbacks look at everything.
    PollContext ctx;
    ctx.colls = colls;
    for (size_t i = 0; i < drivers_.size(); i++) {
        if (drivers_[i].vids.empty()) continue;
        Run& r = runs[i];
        size_t begin = ctx.out.size();
        ctx.out.insert(ctx.out.end(), r.out.begin(), r.out.end());
        for (const auto& p : r.claimed) ctx.claimed.insert(p);
        for (const auto& b : r.bt) ctx.btContainers.insert(b);
        r.begin = begin;
        r.end = ctx.out.size();
    }
    for (size_t i = 0; i < drivers_.size(); i++)
        if (drivers_[i].vids.empty()) RunOne(i, ctx, opt, runs[i]);
    for (size_t i = 0; i < drivers_.size(); i++) runs[i].out.assign(ctx.out.begin() + runs[i].begin, ctx.out.begin() + runs[i].end);
    result = ctx.out;
    return true;
}

void PollScheduler::CollectViolations(const std::vector<Run>& runs, std::vector<std::string>& into) const {
    for (size_t i = 0; i < drivers_.size(); i++) {
        const auto& vids = drivers_[i].vids;
        if (vids.empty() || runs[i].reused) continue;
        for (USHORT v : runs[i].requested)
            if (std::find(vids.begin(), vids.end(), v) == vids.end())
                into.push_back(std::string(drivers_[i].name) + " asked about vendor id " + HexId(v) + ", which it does not declare");
    }
}

void PollScheduler::Finish(const PollOptions& opt, const std::vector<Run>& runs) {
    for (size_t i = 0; i < drivers_.size(); i++) {
        const Run& r = runs[i];
        PollReport::Timing t;
        t.name = drivers_[i].name;
        t.ms = r.ms;
        t.reused = r.reused;
        t.devices = (int)r.out.size();
        report_.timings.push_back(t);
        if (r.reused) {
            report_.reused++;
            continue;
        }
        State& st = state_[i];
        st.unchanged = st.have && SameResult(st.last.out, r.out) ? st.unchanged + 1 : 0;
        uint64_t interval = 0;
        if (r.out.empty()) interval = 0;
        else if (AllResting(r.out)) interval = opt.maxIntervalMs;
        else interval = std::min(opt.maxIntervalMs, opt.baseIntervalMs << std::min(st.unchanged, 3));
        st.nextDueMs = opt.nowMs + interval;
        st.last = r;
        st.have = true;
    }
}

std::vector<DeviceStatus> PollScheduler::Poll(const std::vector<HidColl>& colls, const PollOptions& opt) {
    report_ = PollReport{};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<Run> runs(drivers_.size());
    std::vector<DeviceStatus> out;
    bool done = false;
    if (opt.parallel && orderOk_ && !sequentialOnly_) {
        done = PollParallel(colls, opt, runs, out);
        report_.parallel = done;
        if (!done) {
            sequentialOnly_ = true;  // the list is wrong somewhere: stay safe from now on
            runs.assign(drivers_.size(), Run{});
        }
    }
    if (!done) {
        report_.threads = 0;
        out = PollSequential(colls, opt, runs);
        CollectViolations(runs, report_.violations);
        if (!report_.violations.empty()) sequentialOnly_ = true;
        // keep only unique messages (the parallel attempt and the redo report the same driver)
        std::sort(report_.violations.begin(), report_.violations.end());
        report_.violations.erase(std::unique(report_.violations.begin(), report_.violations.end()), report_.violations.end());
    }
    Finish(opt, runs);
    report_.totalMs = MsSince(t0);
    return out;
}
