// The power check (task E5, docs/design/power.md): `simrun --power`.
#pragma once

#include "ColonySim/Defs.h"

#include <memory>

namespace powerscenario {

// A solar panel, a wood-fired generator that is switched off, two batteries, ten lamps and a heater
// on one net, run for `days` game days. Prints an hourly table (sun, generation, demand, stored
// energy, who has power) when `verbose`, and a summary line. Checks: after the first noon the lamps
// never go dark (the batteries carry the night), the batteries charge in the day and fall at night,
// and flicking the switch splits the net. Returns 0 if all hold, 1 if one failed, 2 if the setup failed.
int run(double days, std::shared_ptr<const colony::DefDatabase> defs, bool verbose);

}  // namespace powerscenario
