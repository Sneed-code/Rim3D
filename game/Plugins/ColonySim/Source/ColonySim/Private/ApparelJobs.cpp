// Changing clothes and stripping, as jobs (docs/design/apparel.md).
#include "ColonySim/Apparel.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

// The think node: take off what the outfit doesn't allow, else put on the clothes that gain most.
// Once nothing is found a pawn doesn't look again for ApparelRules.checkEverySteps (it keeps looking
// while it keeps finding something, so it dresses in one go).
bool JobSystem::thinkApparel(Entity pawn, Job& out) {
    if (apparel_ == nullptr) {
        return false;
    }
    entt::registry& reg = things_.registry();
    if (!reg.all_of<PawnOutfit>(pawn)) {
        reg.emplace<PawnOutfit>(pawn, PawnOutfit{apparel_->outfitOf(pawn), 0});
    }
    PawnOutfit& own = reg.get<PawnOutfit>(pawn);
    if (now() < own.nextCheck) {
        return false;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    const Entity remove = apparel_->wornToRemove(pawn);
    if (remove != kNoEntity) {
        out = Job{};
        out.type = JobType::WearApparel;
        out.thing = remove;
        out.count = 1;
        out.cell = at;
        return true;
    }
    const Entity best = apparel_->bestToWear(pawn, [&](Entity item) {
        const JobTarget target{item, CellPos{}};
        return !reservedByOther(target, pawn) && !avoiding(pawn, target) && canReach(at, cellOf(reg.get<Position>(item)));
    });
    if (best != kNoEntity) {
        out = Job{};
        out.type = JobType::WearApparel;
        out.thing = best;
        out.count = 0;
        out.cell = cellOf(reg.get<Position>(best));
        return true;
    }
    own.nextCheck = now() + apparel_->rules().checkEverySteps;
    return false;
}

bool JobSystem::failCheckApparel(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (apparel_ == nullptr) {
        why = "there is no clothing system";
        return false;
    }
    if (job.type == JobType::WearApparel) {
        if (job.count != 0) {
            const std::vector<Entity>& worn = apparel_->worn(pawn);
            if (std::find(worn.begin(), worn.end(), job.thing) == worn.end()) {
                why = "it isn't wearing that any more";
                return false;
            }
        } else if (!things_.spawned(job.thing)) {
            why = "the clothes are gone";
            return false;
        }
        return true;
    }
    // StripPawn
    if (!reg.valid(job.thing) || !reg.all_of<Position>(job.thing) || !apparel_->stripMarked(job.thing)) {
        why = "the strip order is gone";
        return false;
    }
    if (!health_.incapacitated(job.thing) || !apparel_->hasGear(job.thing)) {
        why = "there is nothing to take any more";
        return false;
    }
    return true;
}

JobSystem::StepResult JobSystem::apparelGoto(Entity pawn, Job& job, std::string& why) {
    if (job.count != 0) {
        return StepResult::Next;  // a worn piece comes off where the pawn stands
    }
    return walkTo(pawn, job, cellOf(things_.registry().get<Position>(job.thing)), why);
}

JobSystem::StepResult JobSystem::apparelWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (job.count == 0 && cellOf(reg.get<Position>(pawn)) != cellOf(reg.get<Position>(job.thing))) {
        why = "the pawn isn't where the clothes lie";
        return StepResult::Failed;
    }
    job.progress += 1.0f;
    if (job.progress < static_cast<float>(apparel_->rules().wearSteps)) {
        return StepResult::Running;
    }
    std::string error;
    const bool ok = job.count != 0 ? apparel_->takeOff(pawn, job.thing, error) : apparel_->putOn(pawn, job.thing, error);
    if (!ok) {
        why = error;
        return StepResult::Failed;
    }
    return StepResult::Next;
}

JobSystem::StepResult JobSystem::stripWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const Position& there = reg.get<Position>(job.thing);
    const float dx = at.x - there.x;
    const float dy = at.y - there.y;
    if (dx * dx + dy * dy > 4.0f) {
        why = "the pawn isn't beside the one it strips";
        return StepResult::Failed;
    }
    const Gear* gear = reg.try_get<Gear>(job.thing);
    const size_t pieces = gear == nullptr ? 0 : gear->apparel.size() + gear->stowed.size() + (gear->weapon != kNoEntity ? 1 : 0);
    job.progress += 1.0f;
    if (job.progress < static_cast<float>(pieces) * static_cast<float>(apparel_->rules().stripStepsPerPiece)) {
        return StepResult::Running;
    }
    apparel_->strip(job.thing);
    std::string error;
    apparel_->markStrip(job.thing, false, error);
    return StepResult::Next;
}

}  // namespace colony
