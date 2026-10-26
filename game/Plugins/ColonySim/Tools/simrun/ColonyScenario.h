// The colony scenario (task C10, docs/design/colony-scenario.md): `simrun --scenario colony`.
#pragma once

#include "ColonySim/Defs.h"

#include <cstdint>
#include <memory>

namespace colonyscenario {

// Plays Crashlanded's six colonists on a map for `days` game days with a script of commands
// (a walled room with a door, six beds, a table and a stove; mining, chopping and harvesting marks;
// a cooking bill on the stove once it stands), prints one JSON line with what happened and checks:
// nobody died or went hungry to malnutrition, nobody stood stuck for a game hour, every mark was
// worked off, everything planned was built, and the stove made meals that were eaten.
// Returns 0 if all checks hold, 1 if one failed (each is said on stderr), 2 if the scenario could
// not be set up (no room site or rock near the landing).
int run(uint64_t seed, int mapSize, double days, std::shared_ptr<const colony::DefDatabase> defs, bool verbose, bool year = false);
// `year`: also wild animals, a rice field and a stockpile, a bigger meal bill, and the slowest step is reported.

}  // namespace colonyscenario
