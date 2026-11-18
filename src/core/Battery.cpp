// core/Battery.cpp – see Battery.hpp
#include "core/Battery.hpp"

namespace battery {

namespace {
// Voltage at 100 %, 90 %, 80 % ... 0 %, in steps of 10 %.
// Source: Meshtastic firmware, src/power.h, OCV_ARRAY (full citation in Battery.hpp).
constexpr int kCurve[] = {4190, 4050, 3990, 3890, 3800, 3720, 3630, 3530, 3420, 3300, 3100};
constexpr int kSteps = sizeof kCurve / sizeof kCurve[0] - 1;   // 10 gaps of 10 %
}  // namespace

int percentFromMillivolts(int mv) {
    if (mv >= kCurve[0]) return 100;
    if (mv <= kCurve[kSteps]) return 0;
    // Find the gap mv falls into: kCurve[i] > mv >= kCurve[i + 1].
    int i = 0;
    while (mv < kCurve[i + 1]) ++i;
    const int hi = kCurve[i], lo = kCurve[i + 1];
    const int pctHi = 100 - i * 10;                       // the percentage at `hi`
    // Linear inside the gap, rounded to the nearest whole percent.
    return pctHi - ((hi - mv) * 10 + (hi - lo) / 2) / (hi - lo);
}

}  // namespace battery
