#pragma once
// The list of drivers, in the order they run.
//
// Each driver declares the USB vendor ids it may look at. That makes the order explicit: two drivers
// only depend on each other's order when they share a vendor id, and the poll scheduler runs drivers
// with unrelated vendor ids at the same time (see poll_scheduler.h). A driver with no vendor ids is
// unrestricted: it sees every collection and runs after all the others, in list order (the
// Bluetooth, Phone Link, HeadsetControl and generic fallbacks).
#include <vector>

#include "drivers.h"

struct DriverInfo {
    const char* name;
    void (*poll)(PollContext&);
    std::vector<USHORT> vids;  // empty = unrestricted
};

const std::vector<DriverInfo>& AllDrivers();
