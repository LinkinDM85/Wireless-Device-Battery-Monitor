// test-sources: src/drivers/*.cpp src/hid.cpp src/poll_scheduler.cpp
// Checks the driver list in registry.cpp against the real drivers (Windows only, no devices needed):
//  * every driver runs exactly once, names are unique, unrestricted drivers come last;
//  * with no devices plugged in, every vendor id a restricted driver asks about is one it declared.
// Drivers are run with an empty collection list, so nothing is opened or sent anywhere.
#include "../src/drivers/registry.h"
#include "../src/poll_scheduler.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
#include <string>

int main() {
    const auto& drivers = AllDrivers();
    int failures = 0;

    std::set<std::string> names;
    std::set<void (*)(PollContext&)> fns;
    for (const auto& d : drivers) {
        if (!names.insert(d.name).second) { printf("duplicate driver name: %s\n", d.name); failures++; }
        if (!fns.insert(d.poll).second) { printf("driver listed twice: %s\n", d.name); failures++; }
    }
    for (const auto& p : PollScheduler::CheckOrder(drivers)) { printf("order: %s\n", p.c_str()); failures++; }

    for (const auto& d : drivers) {
        if (d.vids.empty()) continue;  // fallbacks look at everything by design (and some start background work)
        PollContext ctx;
        d.poll(ctx);
        std::string missing, asked;
        for (USHORT v : ctx.requested) {
            char hex[8];
            snprintf(hex, sizeof(hex), "%04X ", v);
            asked += hex;
            if (std::find(d.vids.begin(), d.vids.end(), v) == d.vids.end()) missing += hex;
        }
        printf("%-18s asks about: %s\n", d.name, asked.empty() ? "(nothing without devices)" : asked.c_str());
        if (!missing.empty()) {
            printf("  NOT DECLARED in registry.cpp: %s\n", missing.c_str());
            failures++;
        }
        assert(ctx.out.empty());
    }

    printf("%zu drivers, %zu groups that can run at the same time\n", drivers.size(), PollScheduler::Groups(drivers).size());
    if (failures) {
        printf("Driver registry test FAILED (%d problems)\n", failures);
        return 1;
    }
    printf("Driver registry tests passed\n");
    return 0;
}
