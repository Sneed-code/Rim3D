// Firefighting (task E4, docs/design/fire.md): the ExtinguishFire job walks next to a burning cell and beats the
// fire down. Nothing is reserved, so several colonists fight one fire together.
#include "ColonySim/Fire.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

bool JobSystem::failCheckExtinguish(Entity pawn, Job& job, std::string& why) {
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (fire_ == nullptr) {
        why = "there is no fire layer";
        return false;
    }
    if (!fire_->burning(job.cell)) {
        // Out before the pawn got there (or while it beat): that is the job done, not a failure. Skip to the last step, which
        // ends the job at once.
        job.step = static_cast<uint8_t>(jobStepCount(JobType::ExtinguishFire) - 1);
    }
    return true;
}

// Each game second the pawn beats `beatPerSecond` of fire size off, times its Manipulation. The job is done when the
// fire is out.
JobSystem::StepResult JobSystem::extinguishWork(Entity pawn, Job& job, std::string& why) {
    if (fire_ == nullptr || !fire_->burning(job.cell)) {
        return StepResult::Next;
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
        gatherEvent(pawn, PawnAction::Extinguish, job.cell);
        job.progress = 1.0f;
    }
    return fire_->extinguish(job.cell, fire_->beatPerSecond() * static_cast<float>(manipulation)) ? StepResult::Next
                                                                                                 : StepResult::Running;
}

}  // namespace colony
