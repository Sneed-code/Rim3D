#include "ColonySim/Clock.h"

#include <doctest.h>

using namespace colony;

TEST_CASE("Clock: a day at 1x lasts 48 real minutes") {
    CHECK(kSecondsPerDay / kStepsPerRealSecondAt1x == 48 * 60);
}

TEST_CASE("Clock: calendar maths") {
    GameTime t;
    t.seconds = 3 * kSecondsPerDay + 5 * kSecondsPerHour + 10;
    CHECK(t.day() == 3);
    CHECK(t.hourOfDay() == 5);
    CHECK(t.season() == 0);

    t.seconds = (kDaysPerYear + 16) * kSecondsPerDay;
    CHECK(t.year() == 1);
    CHECK(t.dayOfYear() == 16);
    CHECK(t.season() == 1);
}

TEST_CASE("Clock: RimWorld ticks convert to game seconds") {
    CHECK(rimWorldTicksToSeconds(2500.0) == doctest::Approx(3600.0));
    CHECK(rimWorldTicksToSeconds(60000.0) == doctest::Approx(86400.0));
}
