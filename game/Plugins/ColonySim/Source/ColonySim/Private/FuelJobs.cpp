// The Refuel job: fetch fuel, carry it to a building and put it in (task C11, docs/design/fuel.md).
// Part of the job system.
#include "ColonySim/Fuel.h"
#include "ColonySim/Jobs.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Things.h"

namespace colony {

bool JobSystem::failCheckRefuel(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (!things_.spawned(job.site) || !reg.all_of<Fuel>(job.site) || reg.all_of<ConstructionSite>(job.site)) {
        why = "the building is gone";
        return false;
    }
    if (fuel_->roomFor(job.site) < 1) {
        why = "the building is full of fuel";
        return false;
    }
    if (job.step <= kRefuelPickUpStep) {
        if (!things_.spawned(job.thing)) {
            why = "the fuel is gone";
            return false;
        }
    } else {
        const Carrying* carrying = reg.try_get<Carrying>(pawn);
        if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
            why = "it lost what it carried";
            return false;
        }
    }
    if (reg.get<Thing>(job.thing).def != fuel_->specOf(job.site).item) {
        why = "that isn't what the building burns";
        return false;
    }
    return true;
}

// Stands beside the building for kRefuelWorkSeconds, then puts the carried fuel in. What does not fit
// stays in the pawn's hands and is put down when the job ends.
JobSystem::StepResult JobSystem::refuelWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the building";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Refuel, job.site);
    }
    job.progress += 1.0f;
    if (job.progress < FuelSystem::kRefuelWorkSeconds) {
        return StepResult::Running;
    }
    const Entity carried = job.thing;
    const int count = reg.get<StackCount>(carried).count;
    const int taken = fuel_->add(job.site, count);
    if (taken <= 0) {
        why = "the building is full of fuel";
        return StepResult::Failed;
    }
    if (taken == count) {
        reg.remove<Carrying>(pawn);
        things_.destroy(carried);
    } else {
        reg.get<StackCount>(carried).count -= taken;
    }
    return StepResult::Next;
}

}  // namespace colony
