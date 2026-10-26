#pragma once

#include "ColonySim/Colonist.h"
#include "ColonySim/Entity.h"

#include <entt/entity/registry.hpp>

namespace colony {

// The one rule for "the player's mind is never taken from them" (docs/design/protagonist.md, "Mind rules").
// The protagonist is the player's own character: mood is tracked and shown, but nothing in the sim may
// take the character out of the player's hands (mental breaks, social fights, panic when burning, and
// any later refusal of orders or AI override). Every such system asks this, so changing the rule later
// (say, only while the player is not steering) is a change to this one function.
inline bool mindIsPlayersOwn(const entt::registry& reg, Entity pawn) {
    if (!reg.valid(pawn)) {
        return false;
    }
    const Colonist* colonist = reg.try_get<Colonist>(pawn);
    return colonist != nullptr && colonist->protagonist;
}

}  // namespace colony
