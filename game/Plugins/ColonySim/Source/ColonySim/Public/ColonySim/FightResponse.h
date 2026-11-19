#pragma once

#include "ColonySim/Api.h"

#include <cstdint>
#include <string>

namespace colony {

// What a pawn does about enemies (sneed, 2026-11-17; docs/design/focus.md "Fight response"), set per pawn by
// SetFightResponseCommand. RimWorld's hostility response, with a fists option and fleeing on sight.
enum class FightResponse : uint8_t {
    Attack,       // fights as its side does (the default, as before)
    Fists,        // attacked, it fights back, with its fists if it has no weapon
    Flee,         // attacked, it runs from the attacker for a while and doesn't fight
    FleeOnSight,  // it runs as soon as it sees an enemy, and doesn't fight
};

COLONY_API const char* fightResponseName(FightResponse response);
COLONY_API bool fightResponseFromName(const std::string& name, FightResponse& out);

// Lock-on focus (FocusCommand): set a target, cycle to the next nearest (Tab), or let go (F1).
enum class FocusMode : uint8_t { Set, Next, Clear, Left, Right };

}  // namespace colony
