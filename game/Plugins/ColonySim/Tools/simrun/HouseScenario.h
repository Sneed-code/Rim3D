// The two-storey house check (task M4, docs/design/house-scenario.md): `simrun --scenario house`.
#pragma once

#include "ColonySim/Defs.h"

#include <cstdint>
#include <memory>
#include <string>

namespace housescenario {

// Plays Crashlanded's six colonists for `days` game days with a script of commands: a 9 by 7 ring of
// walls with a door, stairs inside, an upper floor over all of it (planned once the walls stand),
// walls and six beds upstairs (planned once the floor is laid), then a stockpile on each level and,
// every few game hours, cloth to haul up and stone blocks to haul down. Prints one JSON line and checks:
// nobody died or starved, nobody stood stuck for a game hour, nothing planned is left unbuilt, the
// upstairs is an enclosed roofed room with six beds, every colonist slept in a bed up there, cloth
// went up and blocks came down the stairs, and no command or job failed. With `pngBase` (e.g. "house")
// it writes `<pngBase>-level0.png` and `<pngBase>-level1.png`, the house seen from above, one per level.
// Returns 0 if all checks hold, 1 if one failed (each is said on stderr), 2 if the scenario could not
// be set up.
int run(uint64_t seed, int mapSize, double days, std::shared_ptr<const colony::DefDatabase> defs, bool verbose,
        const std::string& pngBase);

}  // namespace housescenario
