#pragma once

#include "ColonySim/Api.h"

#include <cstdint>

namespace colony {

class DefDatabase;

// How doors work, from the DoorRules def (with these values when the def is missing). Opening and
// closing are part of walking, so they run at RimWorld's walking speed: a RimWorld tick is 1/60 of
// a real second, a sim step 1/30, so a tick is half a step. See docs/design/rooms.md.
struct COLONY_API DoorNumbers {
    int openSteps = 23;        // a pawn waits this long at a closed door: 45 ticks
    int closeDelaySteps = 55;  // the door stays open this long after the last pawn needed it: 110 ticks
    float pathCost = 3.5f;     // metres of walking a path counts a door as: 45 ticks of walking at 4.6 m/s

    static DoorNumbers read(const DefDatabase& defs);
};

// On every door building. A pawn can't walk into a closed door's cell: it stops at the edge and
// the door starts to open; once open, any pawn passes. The door closes `closeDelaySteps` after the
// last step a pawn was in it or waiting at it. The movement system drives it (MovementSystem.h).
struct Door {
    uint16_t progress = 0;  // steps spent opening, up to DoorNumbers::openSteps
    uint16_t hold = 0;      // steps left before an open door closes
    bool open = false;

    // 0 closed to 1 open, for the door's animation.
    float openFraction(const DoorNumbers& n) const {
        if (open) {
            return 1.0f;
        }
        return n.openSteps > 0 ? static_cast<float>(progress) / static_cast<float>(n.openSteps) : 0.0f;
    }
};

}  // namespace colony
