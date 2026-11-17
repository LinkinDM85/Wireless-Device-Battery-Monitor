#include "json_report.h"

#include <cstdio>

#include "platform_compat.h"

std::string JsonString(const std::string& utf8) {
    std::string o = "\"";
    for (unsigned char c : utf8) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;  // UTF-8 bytes pass through unchanged
                }
        }
    }
    return o + "\"";
}

namespace {

std::string Str(const std::wstring& w) { return w.empty() ? "null" : JsonString(WideToUtf8(w)); }

std::string Hex(unsigned short v) {
    if (!v) return "null";
    char b[8];
    snprintf(b, sizeof(b), "\"%04X\"", v);
    return b;
}

const char* KindName(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse:    return "mouse";
        case DeviceKind::Headset:  return "headset";
        case DeviceKind::Keyboard: return "keyboard";
        case DeviceKind::Gamepad:  return "gamepad";
        case DeviceKind::Phone:    return "phone";
        default:                   return "other";
    }
}

}  // namespace

std::string DevicesToJson(const std::vector<DeviceStatus>& devices, const std::vector<int64_t>& remainingSec) {
    std::string o = "{\n  \"devices\": [";
    for (size_t i = 0; i < devices.size(); i++) {
        const DeviceStatus& s = devices[i];
        int64_t left = i < remainingSec.size() ? remainingSec[i] : -1;
        o += i ? ",\n    {" : "\n    {";
        o += "\"key\": " + Str(s.key);
        o += ", \"name\": " + Str(s.name);
        o += ", \"via\": " + Str(s.via);
        o += std::string(", \"kind\": \"") + KindName(s.kind) + "\"";
        o += ", \"percent\": " + (s.percent >= 0 ? std::to_string(s.percent) : std::string("null"));
        o += ", \"level\": " + Str(s.levelText);
        o += ", \"detail\": " + Str(s.detail);
        o += std::string(", \"charging\": ") + (s.charging ? "true" : "false");
        o += std::string(", \"asleep\": ") + (s.asleep ? "true" : "false");
        o += std::string(", \"connected\": ") + (s.connected ? "true" : "false");
        o += ", \"millivolts\": " + (s.millivolts ? std::to_string(s.millivolts) : std::string("null"));
        o += ", \"vid\": " + Hex(s.vid);
        o += ", \"pid\": " + Hex(s.pid);
        o += ", \"estimatedSecondsLeft\": " + (left >= 0 ? std::to_string(left) : std::string("null"));
        o += "}";
    }
    o += devices.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}
