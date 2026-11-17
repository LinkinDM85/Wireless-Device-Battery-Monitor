// Polls every driver (the list and its order are in drivers/registry.cpp, poll_scheduler.cpp runs
// it). Vendor drivers claim the devices they handle; the generic fallbacks only look at what is left.
#include "devices.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include "drivers/registry.h"
#include "hid.h"
#include "poll_scheduler.h"

// HKCU\Software\WirelessBattery\IgnoreDevices (REG_SZ): "VID:PID" pairs in hex, separated by spaces,
// commas or semicolons (e.g. "28BD:0947, 046D:C547"). No driver ever opens those devices (issue #232).
std::vector<std::pair<unsigned short, unsigned short>> IgnoredDeviceIds() {
    std::vector<std::pair<unsigned short, unsigned short>> out;
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"IgnoreDevices", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return out;
    for (const wchar_t* p = buf; *p;) {
        unsigned vid = 0, pid = 0;
        int used = 0;
        if (swscanf(p, L" %x:%x%n", &vid, &pid, &used) == 2 && used > 0) {
            out.push_back({ (USHORT)vid, (USHORT)pid });
            p += used;
        } else {
            p++;
        }
        while (*p == L',' || *p == L';' || *p == L' ') p++;
    }
    return out;
}

void SetIgnoredDeviceIds(const std::vector<std::pair<unsigned short, unsigned short>>& ids) {
    std::wstring v;
    for (auto [vid, pid] : ids) {
        wchar_t one[16];
        swprintf(one, 16, L"%04X:%04X", vid, pid);
        if (!v.empty()) v += L", ";
        v += one;
    }
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"IgnoreDevices", REG_SZ, v.c_str(),
                    (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

namespace {

std::vector<HidColl> Collections() {
    auto colls = EnumHid();
    auto ignored = IgnoredDeviceIds();
    if (!ignored.empty())
        colls.erase(std::remove_if(colls.begin(), colls.end(), [&](const HidColl& c) {
                        return std::find(ignored.begin(), ignored.end(), std::make_pair(c.vid, c.pid)) != ignored.end();
                    }),
                    colls.end());
    return colls;
}

}  // namespace

std::vector<DeviceStatus> PollAllDevices(PollScheduler& scheduler, const PollOptions& opt) {
    return scheduler.Poll(Collections(), opt);
}

std::vector<DeviceStatus> PollAllDevices() {
    PollScheduler scheduler(AllDrivers());
    PollOptions opt;
    opt.force = true;
    return scheduler.Poll(Collections(), opt);
}

std::wstring DumpHidCollections() {
    std::wstring out = L"VID:PID    IF  UsagePg:Usage  In  Out Feat  Product\n";
    for (const auto& c : EnumHid()) {
        wchar_t line[160];
        swprintf(line, 160, L"%04X:%04X  %2d  %04X:%04X    %3u %3u %3u   ", c.vid, c.pid, c.iface, c.page(), c.usage(), c.inLen(), c.outLen(),
                 c.featLen());
        out += line + HidProduct(c) + L"\n";
    }
    out += DumpXentaFrames();
    return out;
}
