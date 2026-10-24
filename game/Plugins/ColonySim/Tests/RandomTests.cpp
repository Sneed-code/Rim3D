#include "ColonySim/Random.h"

#include <doctest.h>

using colony::Rng;

TEST_CASE("Rng: same seed gives the same sequence") {
    Rng a(42);
    Rng b(42);
    for (int i = 0; i < 1000; ++i) {
        CHECK(a.next() == b.next());
    }
}

TEST_CASE("Rng: different seeds and streams differ") {
    Rng a(42);
    Rng b(43);
    Rng c(42, 7);
    const uint32_t first = a.next();
    CHECK(first != b.next());
    CHECK(first != c.next());
}

TEST_CASE("Rng: below stays in range and unit stays in [0, 1)") {
    Rng rng(7);
    for (int i = 0; i < 10000; ++i) {
        CHECK(rng.below(10) < 10u);
        const float u = rng.unit();
        CHECK(u >= 0.0f);
        CHECK(u < 1.0f);
    }
    CHECK(rng.below(0) == 0u);
}
