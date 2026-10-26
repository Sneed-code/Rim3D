// The raid soak (task F7, docs/design/raid-soak.md): `simrun --raid-soak N`.
#pragma once

#include "ColonySim/Defs.h"

#include <cstdint>
#include <memory>

namespace raidsoak {

// Plays two scenarios on each of `seeds` seeds starting at `firstSeed`: a three against three
// firefight among scattered cover (checks the fighter rule of docs/design/fighters.md: under 10%
// of the time waiting in the open when cover is near), and a pirate raid on the Crashlanded
// colony (checks that it ends). Reports deaths, stuck fighters and fight length per seed and in
// total as text and one JSON line. Returns 0 if every rule held, 1 if one failed, 2 if the setup
// failed.
int run(uint64_t firstSeed, int seeds, std::shared_ptr<const colony::DefDatabase> defs, bool verbose);

}  // namespace raidsoak
