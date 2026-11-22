#pragma once

// Tests from before tree trunks stopped bullets, blows and the player (docs/design/structure-damage.md) set up their
// fights on generated maps that have trees; they clear the plants so a trunk doesn't stand in the line of fire.

#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"

#include <memory>

namespace colony {

inline std::unique_ptr<Sim> withoutPlants(std::unique_ptr<Sim> sim) {
    for (int y = 0; y < sim->map().height(); ++y) {
        for (int x = 0; x < sim->map().width(); ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    return sim;
}

}  // namespace colony
