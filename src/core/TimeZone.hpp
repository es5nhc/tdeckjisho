#pragma once
// ============================================================================
//  TimeZone.hpp – the clock's time zone, as a UTC offset
// ============================================================================
//
//  The setting `tz_min` is the offset from UTC in MINUTES: 540 = UTC+9 (Japan),
//  -300 = UTC-5, 330 = UTC+5:30 (India). Minutes, because a few real zones are
//  not whole hours. A plain offset knows nothing about summer time: in a country
//  that switches, you change the setting twice a year. The default is 0 (UTC).
//
//  The C library learns the zone from a POSIX "TZ" string (see hal::setTimeZone).
//  Its sign is the OPPOSITE of the usual one: "how much to ADD to local time to get
//  UTC". So UTC+9 is written "LOC-9". (The name "LOC" is required by the format,
//  3+ letters, and is never shown.) Python's time.tzset() reads the same strings.
// ============================================================================

#include <string>
#include <vector>

namespace tz {

// Every UTC offset in use somewhere in the world, west to east.
const std::vector<int>& choices();

// The POSIX TZ string for a `tz_min` value. Out-of-range values clamp to ±14 h.
std::string posix(int offsetMin);

// "UTC+9", "UTC-3:30", "UTC" – for the Settings screen.
std::string label(int offsetMin);

}  // namespace tz
