// test-sources: src/json_report.cpp
#include "../src/json_report.h"

#include <cassert>
#include <cstdio>

int main() {
    assert(JsonString("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    assert(DevicesToJson({}, {}) == "{\n  \"devices\": []\n}\n");

    DeviceStatus m;
    m.key = L"glorious:2022";
    m.name = L"Model O \"Wireless\"";
    m.via = L"2.4 GHz receiver";
    m.kind = DeviceKind::Mouse;
    m.percent = 93;
    m.vid = 0x258A;
    m.pid = 0x2022;
    DeviceStatus h;
    h.key = L"hyperx:05B7";
    h.name = L"Cloud III é";  // non-ASCII name: written as UTF-8
    h.kind = DeviceKind::Headset;
    h.charging = true;
    h.millivolts = 3900;
    std::string j = DevicesToJson({ m, h }, { 270000, -1 });
    const char* want =
        "{\n  \"devices\": [\n"
        "    {\"key\": \"glorious:2022\", \"name\": \"Model O \\\"Wireless\\\"\", \"via\": \"2.4 GHz receiver\", \"kind\": \"mouse\", "
        "\"percent\": 93, \"level\": null, \"detail\": null, \"charging\": false, \"asleep\": false, \"connected\": true, "
        "\"millivolts\": null, \"vid\": \"258A\", \"pid\": \"2022\", \"estimatedSecondsLeft\": 270000},\n"
        "    {\"key\": \"hyperx:05B7\", \"name\": \"Cloud III \xc3\xa9\", \"via\": null, \"kind\": \"headset\", "
        "\"percent\": null, \"level\": null, \"detail\": null, \"charging\": true, \"asleep\": false, \"connected\": true, "
        "\"millivolts\": 3900, \"vid\": null, \"pid\": null, \"estimatedSecondsLeft\": null}\n"
        "  ]\n}\n";
    if (j != want) {
        printf("got:\n%s\nwant:\n%s\n", j.c_str(), want);
        return 1;
    }
    printf("JSON report tests passed\n");
    return 0;
}
