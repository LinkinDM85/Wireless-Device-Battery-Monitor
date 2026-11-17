// test-sources: src/poll_scheduler.cpp
// The scheduler with fake drivers: parallel polling must give exactly what one-after-another polling
// gives, undeclared vendor ids must switch parallel polling off, and the adaptive intervals must
// ask again exactly when they should.
#include "../src/poll_scheduler.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::map<std::string, int> g_calls;  // driver name -> times it ran
std::map<std::wstring, int> g_level;  // device key -> percent the fake device reports
std::map<std::wstring, bool> g_asleep;
std::atomic<int> g_running{ 0 }, g_maxRunning{ 0 };
int g_sleepMs = 0;
std::mutex g_callsLock;

HidColl Coll(USHORT vid, USHORT pid) {
    HidColl c;
    c.vid = vid;
    c.pid = pid;
    c.path = L"fake";
    return c;
}

void Count(const char* name) {
    std::lock_guard<std::mutex> l(g_callsLock);
    g_calls[name]++;
}

void Busy() {
    int now = ++g_running;
    int prev = g_maxRunning.load();
    while (now > prev && !g_maxRunning.compare_exchange_weak(prev, now)) {}
    if (g_sleepMs) std::this_thread::sleep_for(std::chrono::milliseconds(g_sleepMs));
    --g_running;
}

// Reports every unclaimed product of `vid` as a device.
void ReportVendor(PollContext& ctx, USHORT vid, const wchar_t* prefix) {
    for (auto [v, pid] : ctx.Products(vid)) {
        DeviceStatus s;
        s.key = std::wstring(prefix) + std::to_wstring(pid);
        s.name = s.key;
        s.percent = g_level.count(s.key) ? g_level[s.key] : 50;
        s.asleep = g_asleep.count(s.key) && g_asleep[s.key];
        ctx.Add(s, v, pid);
    }
}

// A: vendor 1111. B: vendors 1111 (after A, so it only sees what A left) and 2222.
void DrvA(PollContext& ctx) { Count("A"); Busy(); for (auto [v, pid] : ctx.Products(0x1111)) if (pid < 0x10) { DeviceStatus s; s.key = L"a" + std::to_wstring(pid); s.name = s.key; s.percent = g_level.count(s.key) ? g_level[s.key] : 50; ctx.Add(s, v, pid); } }
void DrvB(PollContext& ctx) { Count("B"); Busy(); ReportVendor(ctx, 0x1111, L"b"); ReportVendor(ctx, 0x2222, L"b"); }
// C: vendor 3333, and it merges a duplicate into its own earlier entry (like the cable + receiver de-dup).
void DrvC(PollContext& ctx) {
    Count("C");
    Busy();
    bool first = true;
    for (auto [v, pid] : ctx.Products(0x3333)) {
        if (first) {
            DeviceStatus s;
            s.key = L"c";
            s.name = L"c";
            s.percent = g_level.count(L"c") ? g_level[L"c"] : 50;
            s.asleep = g_asleep.count(L"c") && g_asleep[L"c"];
            ctx.Add(s, v, pid);
            first = false;
        } else {
            for (auto& o : ctx.out)
                if (o.key == L"c") o.charging = true;
            ctx.Claim(v, pid);
        }
    }
}
// D: vendor 4444, unrelated to everything.
void DrvD(PollContext& ctx) { Count("D"); Busy(); ReportVendor(ctx, 0x4444, L"d"); }
// Fallback: reports anything nobody claimed.
void DrvFallback(PollContext& ctx) {
    Count("F");
    for (const auto& c : ctx.colls) {
        if (ctx.IsClaimed(c.vid, c.pid)) continue;
        DeviceStatus s;
        s.key = L"f" + std::to_wstring(c.vid) + L":" + std::to_wstring(c.pid);
        s.name = s.key;
        ctx.Add(s, c.vid, c.pid);
    }
}
// Bad: declares 5555 but also asks about 1111.
void DrvBad(PollContext& ctx) { Count("Bad"); ReportVendor(ctx, 0x5555, L"x"); ctx.Products(0x1111); }

std::vector<DriverInfo> Good() {
    return {
        { "A", DrvA, { 0x1111 } },
        { "B", DrvB, { 0x1111, 0x2222 } },
        { "C", DrvC, { 0x3333 } },
        { "D", DrvD, { 0x4444 } },
        { "F", DrvFallback, {} },
    };
}

std::vector<HidColl> Colls() {
    return { Coll(0x1111, 0x01), Coll(0x1111, 0x20), Coll(0x2222, 0x02), Coll(0x3333, 0x03),
             Coll(0x3333, 0x04), Coll(0x4444, 0x05), Coll(0x9999, 0x09), Coll(0x5555, 0x06) };
}

std::wstring Describe(const std::vector<DeviceStatus>& v) {
    std::wstring s;
    for (const auto& d : v)
        s += d.key + L"=" + std::to_wstring(d.percent) + (d.charging ? L"c" : L"") + (d.asleep ? L"z" : L"") + L" ";
    return s;
}

void Reset() {
    g_calls.clear();
    g_level.clear();
    g_asleep.clear();
    g_maxRunning = 0;
    g_sleepMs = 0;
}

void TestGroups() {
    auto groups = PollScheduler::Groups(Good());
    // A and B share 1111; C and D are on their own; the fallback is in no group.
    assert(groups.size() == 3);
    assert((groups[0] == std::vector<size_t>{ 0, 1 }));
    assert((groups[1] == std::vector<size_t>{ 2 }));
    assert((groups[2] == std::vector<size_t>{ 3 }));
    assert(PollScheduler::CheckOrder(Good()).empty());
    auto bad = Good();
    std::swap(bad[3], bad[4]);  // fallback before D
    assert(PollScheduler::CheckOrder(bad).size() == 1);
}

void TestParallelMatchesSequential() {
    Reset();
    PollOptions seq;
    seq.parallel = false;
    seq.force = true;
    PollScheduler s1(Good());
    auto a = s1.Poll(Colls(), seq);
    assert(!s1.Report().parallel);

    PollOptions par = seq;
    par.parallel = true;
    PollScheduler s2(Good());
    auto b = s2.Poll(Colls(), par);
    assert(s2.Report().parallel);
    assert(s2.Report().violations.empty());
    assert(Describe(a) == Describe(b));
    // A took 1111:01, B the rest of 1111 and 2222, C merged its duplicate, the fallback got the leftovers.
    assert(Describe(b) == L"a1=50 b32=50 b2=50 c=50c d5=50 f39321:9=-1 f21845:6=-1 ");
    assert(b[0].vid == 0x1111 && b[0].pid == 0x01);
}

void TestRunsAtTheSameTime() {
    Reset();
    g_sleepMs = 150;
    PollOptions par;
    par.force = true;
    PollScheduler s(Good());
    s.Poll(Colls(), par);
    assert(s.Report().threads == 3);
    assert(g_maxRunning >= 2);
    g_sleepMs = 0;
}

void TestUndeclaredVendorFallsBack() {
    Reset();
    auto drivers = Good();
    drivers.insert(drivers.begin() + 4, DriverInfo{ "Bad", DrvBad, { 0x5555 } });
    PollOptions par;
    par.force = true;
    PollScheduler s(drivers);
    auto out = s.Poll(Colls(), par);
    assert(!s.Report().parallel);
    assert(s.ParallelDisabled());
    assert(s.Report().violations.size() == 1);
    assert(s.Report().violations[0].find("Bad") != std::string::npos);
    assert(s.Report().violations[0].find("1111") != std::string::npos);
    PollOptions seq = par;
    seq.parallel = false;
    PollScheduler ref(drivers);
    assert(Describe(out) == Describe(ref.Poll(Colls(), seq)));
    // Stays off for the next polls.
    s.Poll(Colls(), par);
    assert(!s.Report().parallel);
}

void TestAdaptive() {
    Reset();
    PollScheduler s(Good());
    PollOptions o;
    o.baseIntervalMs = 60000;
    o.maxIntervalMs = 300000;
    auto at = [&](uint64_t sec, bool force = false) {
        o.nowMs = sec * 1000;
        o.force = force;
        return s.Poll(Colls(), o);
    };
    at(0, true);  // first poll: everything runs
    assert(g_calls["D"] == 1);
    at(60);  // unchanged once -> next in 120 s (at 180)
    assert(g_calls["D"] == 2);
    auto r = at(120);  // reused, same output
    assert(g_calls["D"] == 2);
    assert(Describe(r).find(L"d5=50") != std::wstring::npos);
    assert(s.Report().reused >= 1);
    at(180);  // due again; unchanged twice -> 240 s (at 420)
    assert(g_calls["D"] == 3);
    at(240); at(300); at(360);
    assert(g_calls["D"] == 3);
    g_level[L"d5"] = 40;
    at(420);  // due; the level changed -> back to every poll
    assert(g_calls["D"] == 4);
    at(480);
    assert(g_calls["D"] == 5);
    // A forced poll always asks.
    at(481, true);
    assert(g_calls["D"] == 6);
    // The fallback is never skipped.
    assert(g_calls["F"] == 10);
}

void TestAsleepGoesStraightToLongest() {
    Reset();
    PollScheduler s(Good());
    PollOptions o;
    g_asleep[L"c"] = true;
    auto at = [&](uint64_t sec) {
        o.nowMs = sec * 1000;
        o.force = sec == 0;
        s.Poll(Colls(), o);
    };
    at(0);
    assert(g_calls["C"] == 1);
    for (uint64_t t = 60; t < 300; t += 60) at(t);
    assert(g_calls["C"] == 1);  // not woken for 5 minutes
    at(300);
    assert(g_calls["C"] == 2);
}

void TestUnpluggedIsAskedAgain() {
    Reset();
    PollScheduler s(Good());
    PollOptions o;
    o.nowMs = 0;
    o.force = true;
    s.Poll(Colls(), o);
    o.force = false;
    o.nowMs = 60000;
    s.Poll(Colls(), o);  // unchanged once: D would be skipped at 120 s
    auto colls = Colls();
    colls.erase(colls.begin() + 5);  // 4444:05 is gone
    o.nowMs = 120000;
    auto out = s.Poll(colls, o);
    assert(g_calls["D"] == 3);
    assert(Describe(out).find(L"d5") == std::wstring::npos);
}

void TestAdaptiveOff() {
    Reset();
    PollScheduler s(Good());
    PollOptions o;
    o.adaptive = false;
    for (uint64_t t = 0; t < 10; t++) {
        o.nowMs = t * 60000;
        s.Poll(Colls(), o);
    }
    assert(g_calls["D"] == 10);
}

}  // namespace

int main() {
    TestGroups();
    TestParallelMatchesSequential();
    TestRunsAtTheSameTime();
    TestUndeclaredVendorFallsBack();
    TestAdaptive();
    TestAsleepGoesStraightToLongest();
    TestUnpluggedIsAskedAgain();
    TestAdaptiveOff();
    printf("Poll scheduler tests passed (groups, parallel == sequential, undeclared vendor ids, adaptive intervals)\n");
    return 0;
}
