// Rearming traps (docs/design/defense.md): the RearmTrap job walks next to a sprung trap and sets it again.
#include "ColonySim/Defense.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

bool JobSystem::failCheckRearm(Entity pawn, Job& job, std::string& why) {
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (defense_ == nullptr || !defense_->needsRearm(job.thing)) {
        why = "the trap doesn't need setting";
        return false;
    }
    return true;
}

// Each game second adds the pawn's Manipulation to the work; done at the trap's `rearmSeconds`.
JobSystem::StepResult JobSystem::rearmWork(Entity pawn, Job& job, std::string& why) {
    if (defense_ == nullptr || !defense_->needsRearm(job.thing)) {
        why = "the trap doesn't need setting";
        return StepResult::Failed;
    }
    double manipulation = 1.0;
    if (const Health* health = things_.registry().try_get<Health>(pawn);
        health != nullptr && manipulationIndex_ >= 0 && static_cast<size_t>(manipulationIndex_) < health->capacities.size()) {
        manipulation = std::max(0.0, static_cast<double>(health->capacities[static_cast<size_t>(manipulationIndex_)]));
    }
    if (!(manipulation > 0.0)) {
        why = "it can't use its hands";
        return StepResult::Failed;
    }
    if (job.progress == 0.0f) {
        gatherEvent(pawn, PawnAction::Rearm, job.cell);
    }
    job.progress += std::max(static_cast<float>(manipulation), 0.001f);
    if (job.progress < defense_->rearmSeconds(job.thing)) {
        return StepResult::Running;
    }
    return defense_->rearm(job.thing, &why) ? StepResult::Next : StepResult::Failed;
}

}  // namespace colony
