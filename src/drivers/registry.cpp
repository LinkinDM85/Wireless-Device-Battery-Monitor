// Every driver, in the order it runs, with the vendor ids it may touch.
//
// Rules (checked by tests/driver_registry_test.cpp):
//  * a driver lists every vendor id it passes to ctx.Products / Find / IsClaimed / Claim / Add, and
//    every vendor id it matches itself when it walks ctx.colls;
//  * when two drivers share a vendor id, the one that must look first comes first (the reason is
//    noted next to it);
//  * unrestricted drivers (no vendor ids) come last.
// A driver that asks about a vendor id it did not declare makes the scheduler fall back to running
// everything one after another, and `--list` names it.
#include "registry.h"

const std::vector<DriverInfo>& AllDrivers() {
    static const std::vector<DriverInfo> drivers = {
        { "Glorious", PollGlorious, { 0x258A, 0x3770, 0x33E4, 0x36A7 } },  // also Hitscan, G-Wolves, WLMouse YING
        { "AULA", PollAula, { 0x3554, 0x41E4, 0x372E } },  // before Kysona, which shares the 3554 vendor id
        { "DAREU", PollDareu, { 0x260D } },
        { "Darmoshark", PollDarmoshark, { 0x248A } },
        { "ATK serial", PollAtkSerial, { 0x373B } },  // before Kysona, which shares the ATK vendor id
        { "Pulsar", PollPulsar, { 0x3710 } },  // before Kysona, which also scans 3710
        { "ATK BITMOUSE", PollAtkBitmouse, { 0x373B } },  // before Kysona, which would probe the NK dongle with the wrong protocol
        { "Kysona", PollKysona, { 0x3554, 0x373B, 0x3770, 0x33E4, 0x3710, 0x35BB } },
        { "Keychron", PollKeychron, { 0x3434 } },
        { "LAMZU", PollLamzu, { 0x373E, 0x37B0 } },
        { "Endgame Gear", PollEndgame, { 0x3367 } },
        { "VAXEE", PollVaxee, { 0x3057 } },
        { "WLMouse", PollWlmouse, { 0x36A7 } },
        { "Finalmouse", PollFinalmouse, { 0x361D } },
        { "Ninjutso", PollNinjutso, { 0x1915, 0x093A } },
        { "Sprime", PollSprime, { 0x1915 } },
        { "Rongyuan", PollRongyuan, RongyuanVendorIds() },
        { "SONiX", PollSonix, { 0x05AC, 0x0C45 } },
        { "Sony INZONE", PollSonyInzone, { 0x054C } },
        { "Fractal", PollFractal, { 0x36BC } },
        { "Jabra", PollJabra, { 0x0B0E } },
        { "JBL", PollJbl, { 0x0ECB } },
        { "ASUS ROG", PollRog, { 0x0B05 } },
        { "ZOWIE", PollZowie, { 0x04A5 } },
        { "Royal Kludge", PollRoyalKludge, { 0x2E3C } },
        { "MCHOSE", PollMchose, { 0x5253, 0x3837, 0x41E4 } },
        { "Rapoo", PollRapoo, { 0x24AE } },
        { "Areson", PollAreson, { 0x25A7 } },
        { "MCHOSE G3", PollMchoseG3, { 0x3837 } },
        { "MSI", PollMsi, { 0x0DB0 } },
        { "Machenike", PollMachenike, { 0x248A } },  // before Ajazz in MiscMice, which shares the L7's 248A vendor id
        { "MCHOSE modern", PollMchoseModern, { 0x3837 } },
        { "Misc mice", PollMiscMice, { 0x093A, 0x0BDA, 0x1D57, 0x248A, 0x249A, 0x3151, 0xA8A4, 0xA8A5 } },
        { "Fantech", PollFantech, { 0x25A7 } },
        { "Razer headsets", PollRazerHeadsets, { 0x1532 } },  // before the generic Razer driver, which shares the vendor id
        { "HyperX", PollHyperX, { 0x03F0, 0x0951 } },
        { "Logitech G435", PollLogitechG435, { 0x046D } },  // before the HID++ driver, which shares the vendor id
        { "ASTRO", PollAstro, { 0x046D } },
        { "Logitech", PollLogitech, { 0x046D } },
        { "Razer", PollRazer, { 0x1532 } },
        { "SteelSeries", PollSteelSeries, { 0x1038 } },
        { "Corsair", PollCorsair, { 0x1B1C } },
        { "Cooler Master", PollCoolerMaster, { 0x2516 } },
        { "PlayStation", PollPlayStation, { 0x054C } },
        { "Steam Controller", PollSteamController, { 0x28DE } },
        { "GameSir", PollGameSir, { 0x3537 } },
        { "Flydigi", PollFlydigi, { 0x37D7 } },
        { "GameSir Cyclone", PollGameSirCyclone, { 0x3537 } },
        { "8BitDo", Poll8BitDo, { 0x2DC8 } },
        { "MOONDROP", PollMoondrop, { 0x35D8 } },
        // Unrestricted fallbacks, in this order.
        { "AirPods", PollAirPods, {} },         // before the Bluetooth fallback, which has no level for AirPods
        { "Phone Link", PollPhoneLink, {} },
        { "Galaxy Buds", PollGalaxyBuds, {} },  // before the Bluetooth fallback, which skips devices already listed by name
        { "HeadsetControl", PollHeadsetControl, {} },
        { "Bluetooth", PollBluetooth, {} },     // before the generic HID check: Windows' own level wins for the same device
        { "Generic HID", PollGenericHid, {} },
        { "XInput", PollXInput, {} },
    };
    return drivers;
}
