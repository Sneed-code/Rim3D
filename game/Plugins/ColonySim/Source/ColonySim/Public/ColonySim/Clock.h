#pragma once

#include <cstdint>

namespace colony {

// One sim step is one game second. At 1x the sim runs kStepsPerRealSecondAt1x
// steps per real second, so a game day lasts 86,400 / 30 s = 48 real minutes.
// Change this one number to change the day length.
inline constexpr int kStepsPerRealSecondAt1x = 30;

inline constexpr int64_t kSecondsPerMinute = 60;
inline constexpr int64_t kSecondsPerHour = 3600;
inline constexpr int64_t kSecondsPerDay = 86400;
inline constexpr int64_t kDaysPerSeason = 15;
inline constexpr int64_t kSeasonsPerYear = 4;
inline constexpr int64_t kDaysPerYear = kDaysPerSeason * kSeasonsPerYear;

// RimWorld counts 2,500 ticks per game hour. The importer uses this to turn
// RimWorld's tick-based numbers into game seconds.
inline constexpr double kRimWorldTicksPerHour = 2500.0;

inline constexpr double rimWorldTicksToSeconds(double ticks) {
    return ticks * (static_cast<double>(kSecondsPerHour) / kRimWorldTicksPerHour);
}

struct GameTime {
    int64_t seconds = 0;

    int64_t day() const { return seconds / kSecondsPerDay; }
    int hourOfDay() const { return static_cast<int>((seconds % kSecondsPerDay) / kSecondsPerHour); }
    int64_t dayOfYear() const { return day() % kDaysPerYear; }
    int season() const { return static_cast<int>(dayOfYear() / kDaysPerSeason); }
    int64_t year() const { return day() / kDaysPerYear; }
};

}  // namespace colony
