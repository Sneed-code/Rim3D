// The sim cost check (task S9, docs/design/sim-cost.md): `simrun --perf`.
#pragma once

#include "ColonySim/Defs.h"

#include <cstdint>
#include <memory>

namespace perfscenario {

// Plays a Stage 2 sized colony on a map for `days` game days: 30 colonists working (building two
// walled rooms, sowing and harvesting a growing zone, hauling, mining, felling), 300 tame animals
// (chickens, cows, muffalo and alpacas) on top of the wild ones, weather and room temperatures.
// Times every system of every step, prints a table of what each costs, and one JSON line. If
// `budgetMs` is above 0, returns 1 when the average step costs more than that (the nightly check).
// Returns 2 if the scenario could not be set up.
int run(uint64_t seed, int mapSize, double days, double budgetMs, std::shared_ptr<const colony::DefDatabase> defs);

}  // namespace perfscenario
