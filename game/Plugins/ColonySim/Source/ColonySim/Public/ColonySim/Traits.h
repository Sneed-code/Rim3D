#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <entt/entt.hpp>
#include <unordered_map>

namespace colony {

// What a colonist's traits do to the numbers (docs/design/traits.md): the stats of every trait degree
// (`degreeDatas.N.statOffsets` and `statFactors` in Traits defs) folded into one small record, so the
// systems that use them (work, walking, shooting, damage, pain, learning) ask once per pawn and not by
// def path each step. Moods, break thresholds, melee, needs and opinions read their own trait numbers.
struct COLONY_API TraitStats {
    float workSpeed = 1.0f;          // times every work speed (Industriousness, Neurotic): 1 + workSpeedGlobal
    float moveSpeedStep = 0.0f;      // metres per step added to walking (SpeedOffset)
    float shootingSkill = 0.0f;      // shooting skill levels added to the aim (ShootingAccuracy)
    float aimDelayFactor = 1.0f;     // times the time spent aiming (ShootingAccuracy)
    float damageFactor = 1.0f;       // times the damage a part takes (Tough)
    float painShockOffset = 0.0f;    // added to the pain level that downs the pawn (Wimp)
    float learningFactor = 1.0f;     // times the experience learned: 1 + globalLearningFactor (learners)
    float skillLossFactor = 1.0f;    // times the experience lost a day by high skills (Great memory)
};

// The TraitStats of colonists, worked out on first ask and kept (a colonist's traits don't change).
// Not saved; the same defs and traits always give the same record.
class COLONY_API TraitBook {
public:
    TraitBook(const entt::registry& reg, const DefDatabase& defs) : reg_(reg), defs_(defs) {}
    // The record of `pawn`: all neutral for anyone without traits (animals, raiders without a Colonist).
    const TraitStats& of(Entity pawn) const;
    bool has(Entity pawn, const char* trait) const;

private:
    const entt::registry& reg_;
    const DefDatabase& defs_;
    mutable std::unordered_map<Entity, TraitStats> cache_;
};

}  // namespace colony
