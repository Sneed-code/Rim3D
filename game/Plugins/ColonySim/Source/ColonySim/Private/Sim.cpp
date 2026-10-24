#include "ColonySim/Sim.h"

#include "ColonySim/Random.h"

namespace colony {

Sim::Sim(const SimSettings& settings)
    : settings_(settings),
      map_(MapSettings{settings.mapSize, settings.mapSize, splitMix64(settings.seed ^ 0x6D6170ULL)}),
      time_() {}

void Sim::step() { time_.seconds += 1; }

void Sim::stepMany(int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        step();
    }
}

}  // namespace colony
