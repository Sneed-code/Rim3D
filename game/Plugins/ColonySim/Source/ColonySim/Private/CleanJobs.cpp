// Cleaning filth (task C17, docs/design/room-stats.md): the CleanFilth job walks onto a cell with
// filth and takes it off a layer at a time.
#include "ColonySim/Jobs.h"

#include "ColonySim/Filth.h"
#include "ColonySim/Health.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

double JobSystem::cleaningSpeed(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    double manipulation = 1.0;
    if (const Health* health = reg.try_get<Health>(pawn);
        health != nullptr && manipulationIndex_ >= 0 && static_cast<size_t>(manipulationIndex_) < health->capacities.size()) {
        manipulation = std::max(0.0, static_cast<double>(health->capacities[static_cast<size_t>(manipulationIndex_)]));
    }
    const double base = filth_ != nullptr ? static_cast<double>(filth_->numbers().cleaningSpeed) : 1.0;
    return base * manipulation;
}

bool JobSystem::failCheckClean(Entity pawn, Job& job, std::string& why) {
    if (!(cleaningSpeed(pawn) > 0.0)) {
        why = "it can't use its hands";
        return false;
    }
    if (filth_ == nullptr || !filth_->has(job.cell)) {
        why = "the filth is gone";
        return false;
    }
    return true;
}

// One layer comes off when the work for it (the filth def's `cleanTicks`, in game seconds at 100%
// speed) is done; the pawn goes on to the next layer, or the next pile, on the cell.
JobSystem::StepResult JobSystem::cleanWork(Entity pawn, Job& job, std::string& why) {
    const double speed = cleaningSpeed(pawn);
    if (!(speed > 0.0)) {
        why = "it can't use its hands";
        return StepResult::Failed;
    }
    if (job.progress == 0.0f) {
        gatherEvent(pawn, PawnAction::Clean, job.cell);
    }
    job.progress += static_cast<float>(speed);
    const float need = filth_->cleanSeconds(job.cell);
    if (job.progress < need) {
        return StepResult::Running;
    }
    job.progress = 0.0f;
    filth_->clean(job.cell);
    return filth_->has(job.cell) ? StepResult::Running : StepResult::Next;
}

}  // namespace colony
