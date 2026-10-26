#pragma once

// A new building that burns fuel starts empty (task C11); tests about something else fill it.

#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

namespace colony {

inline Entity testFueled(Sim& sim, Entity building) {
    if (building != kNoEntity && sim.things().registry().all_of<Fuel>(building)) {
        sim.jobs().fuel().add(building, static_cast<int>(sim.jobs().fuel().specOf(building).capacity));
    }
    return building;
}

}  // namespace colony
