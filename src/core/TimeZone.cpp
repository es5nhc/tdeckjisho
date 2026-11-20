#include "core/TimeZone.hpp"

#include <algorithm>   // std::clamp
#include <cstdio>      // std::snprintf

namespace tz {

namespace {

// "9", "5:30", "12:45" – hours, and minutes only when there are some.
std::string hoursMinutes(int absMin) {
    char buf[16];
    if (absMin % 60) std::snprintf(buf, sizeof buf, "%d:%02d", absMin / 60, absMin % 60);
    else std::snprintf(buf, sizeof buf, "%d", absMin / 60);
    return buf;
}

}  // namespace

const std::vector<int>& choices() {
    static const std::vector<int> kChoices = {
        -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60,
        0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570,
        600, 630, 660, 720, 765, 780, 840,
    };
    return kChoices;
}

std::string posix(int offsetMin) {
    offsetMin = std::clamp(offsetMin, -14 * 60, 14 * 60);
    // Opposite sign, see the header: UTC+9 → "LOC-9", UTC-5 → "LOC5".
    return std::string("LOC") + (offsetMin > 0 ? "-" : "") +
           hoursMinutes(offsetMin < 0 ? -offsetMin : offsetMin);
}

std::string label(int offsetMin) {
    if (offsetMin == 0) return "UTC";
    offsetMin = std::clamp(offsetMin, -14 * 60, 14 * 60);
    return std::string("UTC") + (offsetMin > 0 ? "+" : "-") +
           hoursMinutes(offsetMin < 0 ? -offsetMin : offsetMin);
}

}  // namespace tz
