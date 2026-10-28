// The performance colony (tasks S9 and G3, docs/design/sim-cost.md): the Stage 2 sized colony that `simrun --perf`
// times and the game's `perf` smoke test draws.
#pragma once

#include "ColonySim/Api.h"

#include <string>

namespace colony {

class Sim;

struct PerfColonyCounts {
    int colonists = 0;
    int tameAnimals = 0;
    int wildAnimals = 0;
};

// Sets up a sim started with the PerfColony scenario (30 colonists): plans two walled rooms with beds, a stove and a
// wood floor, a 24 by 24 rice field, mine, fell and pick marks round the landing, and puts 300 tame, grown animals
// (chickens, cows, muffalo, alpacas) on a grid nearby. Changes the sim directly (a dev setup, not commands), so call it
// before the first step. Returns an empty string, or what went wrong.
COLONY_API std::string setUpPerfColony(Sim& sim, PerfColonyCounts& counts);

// Feeds every tame animal, so the pen needs no hay. The perf runs call it at the start of every game hour.
COLONY_API void feedPerfColonyAnimals(Sim& sim);

// Puts an assault rifle in every colonist's hands (a dev setup, not a command), for the raid in the game's perf test.
// Returns how many were armed.
COLONY_API int armPerfColonists(Sim& sim);

}  // namespace colony
