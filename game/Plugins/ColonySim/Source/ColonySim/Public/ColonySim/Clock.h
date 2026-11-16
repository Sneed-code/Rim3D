#pragma once

#include <cstdint>

namespace colony {

// One sim step is one game second of action: walking, fighting and work. At 1x the sim runs
// kStepsPerRealSecondAt1x steps per real second. The calendar (needs, growth, rot, the sun and
// the seasons) moves one game second on most steps: on every step with the 48-minute day, the
// shortest, and on 48 of every `dayMinutes` steps with a longer one (docs/design/day-length.md).
inline constexpr int kStepsPerRealSecondAt1x = 30;

// Day length, in real minutes at 1x: the player's option, 48 (86,400 / 30 / 60) to 120.
inline constexpr int kBaseDayMinutes = 48;
inline constexpr int kMaxDayMinutes = 120;
inline constexpr bool dayMinutesValid(int minutes) { return minutes >= kBaseDayMinutes && minutes <= kMaxDayMinutes; }

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
    int64_t seconds = 0;  // calendar seconds: the clock, the day, needs and growth
    // Steps run, one per real 1/30 s at 1x: timers of things that happen in real time (aiming,
    // swings, dodges, fire). Equal to `seconds` with the 48-minute day.
    int64_t steps = 0;
    // This step moves the calendar on (`seconds` goes up at its end). Always true with the
    // 48-minute day. A system that runs every step does its per-second calendar work only then.
    bool tick = true;
    int dayMinutes = kBaseDayMinutes;  // the day length now: real minutes a game day lasts at 1x
    // Game seconds of calendar that pass in `stepCount` steps.
    double calendarSecondsIn(double stepCount) const { return stepCount * kBaseDayMinutes / dayMinutes; }

    int64_t day() const { return seconds / kSecondsPerDay; }
    int hourOfDay() const { return static_cast<int>((seconds % kSecondsPerDay) / kSecondsPerHour); }
    int64_t dayOfYear() const { return day() % kDaysPerYear; }
    int season() const { return static_cast<int>(dayOfYear() / kDaysPerSeason); }
    int64_t year() const { return day() / kDaysPerYear; }
};

}  // namespace colony
