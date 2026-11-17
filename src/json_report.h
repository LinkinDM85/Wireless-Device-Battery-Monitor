#pragma once
// `WirelessBattery.exe --list --json`: the poll result as JSON (UTF-8), for scripts, Rainmeter,
// OBS overlays and the like.
//
//   { "devices": [ { "key": "...", "name": "...", "via": "...", "kind": "mouse", "percent": 93,
//                    "level": null, "detail": null, "charging": false, "asleep": false,
//                    "connected": true, "millivolts": null, "vid": "258A", "pid": "2022",
//                    "estimatedSecondsLeft": 270000 }, ... ] }
//
// Fields that are unknown are null. estimatedSecondsLeft comes from the tray app's discharge
// history (battery_trend.h) and is null until it has enough data.
#include <cstdint>
#include <string>
#include <vector>

#include "devices.h"

// `remainingSec` is empty or has one entry per device (-1 = unknown).
std::string DevicesToJson(const std::vector<DeviceStatus>& devices, const std::vector<int64_t>& remainingSec);
std::string JsonString(const std::string& utf8);
