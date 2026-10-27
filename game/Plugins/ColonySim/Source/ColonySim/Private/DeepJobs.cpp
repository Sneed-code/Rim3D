// The DeepWork job: walk to a ground-penetrating scanner or a deep drill, sit beside it and work it
// (docs/design/deep-mining.md). Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Construction.h"
#include "ColonySim/DeepResources.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

DeepWorkKind JobSystem::deepKindOf(Entity building) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(building) || !reg.all_of<Thing>(building)) {
        return DeepWorkKind::None;
    }
    const std::string kind = defs_.get(reg.get<Thing>(building).def).text("deepWork.kind");
    if (kind == "scan") {
        return DeepWorkKind::Scan;
    }
    return kind == "drill" ? DeepWorkKind::Drill : DeepWorkKind::None;
}

// A scanner does not work under a roof (RimWorld).
bool JobSystem::deepOpenSky(Entity building) const {
    const Footprint& footprint = things_.registry().get<Footprint>(building);
    const CellRect r = footprint.rect();
    for (int y = r.minY; y < r.minY + r.sizeY; ++y) {
        for (int x = r.minX; x < r.minX + r.sizeX; ++x) {
            if (map_.roof(CellPos{x, y, r.level}) != Roof::None) {
                return false;
            }
        }
    }
    return true;
}

double JobSystem::deepDrillRadius(Entity building) const {
    return defs_.get(things_.registry().get<Thing>(building).def).number("deepWork.radius", 2.6);
}

// Whether the building has something for a pawn to do now: a scanner always (when it has sky and power), a drill
// only with deep resources in reach. Used by the work givers and the fail check.
bool JobSystem::deepWorkable(Entity building, std::string& why) const {
    const entt::registry& reg = things_.registry();
    const DeepWorkKind kind = deepKindOf(building);
    if (deep_ == nullptr || kind == DeepWorkKind::None || !things_.spawned(building) || reg.all_of<ConstructionSite>(building)) {
        why = "the machine is gone";
        return false;
    }
    if (reg.all_of<MarkedForDeconstruct>(building)) {
        why = "the machine is being taken down";
        return false;
    }
    if (!buildingRuns(building)) {
        why = "the machine has no power";
        return false;
    }
    if (kind == DeepWorkKind::Scan && !deepOpenSky(building)) {
        why = "the scanner is under a roof";
        return false;
    }
    if (kind == DeepWorkKind::Drill &&
        !deep_->hasResourceNear(reg.get<Footprint>(building).origin, deepDrillRadius(building))) {
        why = "there is nothing under the drill";
        return false;
    }
    return true;
}

bool JobSystem::failCheckDeepWork(Entity pawn, Job& job, std::string& why) {
    if (!deepWorkable(job.site, why)) {
        return false;
    }
    if (deepKindOf(job.site) == DeepWorkKind::Drill ? !canUseHands(pawn) : health_.capacityValue(pawn, "Consciousness") <= 0.0f) {
        why = deepKindOf(job.site) == DeepWorkKind::Drill ? "it can't use its hands" : "it can't think";
        return false;
    }
    return true;
}

// One game second of work. The job ends (done) when a scanner finds a deposit or a drill finishes a portion.
JobSystem::StepResult JobSystem::deepWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the machine";
        return StepResult::Failed;
    }
    const CellPos origin = reg.get<Footprint>(job.site).origin;
    const bool scan = deepKindOf(job.site) == DeepWorkKind::Scan;
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, scan ? PawnAction::Scan : PawnAction::Drill, job.site);
    }
    if (scan) {
        const int level = skillLevel(pawn, "Intellectual");
        const double skill = std::max(0.0, deep_->scanSpeedBase() + deep_->scanSpeedPerLevel() * level);
        const double speed = skill * health_.capacityValue(pawn, "Consciousness") * lightWorkFactor(pawn);
        learnSkill(pawn, "Intellectual", deep_->scanXpPerSecond());
        DeepFind find;
        if (!deep_->scanStep(origin, speed, find)) {
            return StepResult::Running;
        }
        Event event;
        event.type = EventType::DeepDepositFound;
        event.thing = job.site;
        event.def = find.def;
        event.cell = find.center;
        event.count = find.cells;
        events_.push_back(std::move(event));
        return StepResult::Next;
    }
    const double speed = miningSpeed(pawn);
    if (!(speed > 0.0)) {
        why = "it can't use its hands";
        return StepResult::Failed;
    }
    learnSkill(pawn, "Mining", deep_->drillXpPerSecond());
    if (!deep_->drillStep(origin, speed)) {
        return StepResult::Running;
    }
    const DeepPortion portion = deep_->takePortion(origin, deepDrillRadius(job.site));
    DefId def = portion.def;
    double amount = static_cast<double>(portion.count);
    if (portion.chunk) {
        def = mineables_.chunkOf(job.cell);
        if (!def.valid()) {
            return StepResult::Next;
        }
    } else {
        amount *= yieldFactor(skillLevel(pawn, "Mining"));  // the miner's yield, as for a pick
    }
    const int count = std::max(1, static_cast<int>(std::floor(amount + 0.5)));
    dropItems(def, count, job.cell);
    Event event;
    event.type = EventType::DeepPortionDrilled;
    event.thing = job.site;
    event.def = def;
    event.amount = static_cast<float>(count);
    event.cell = portion.from;
    events_.push_back(std::move(event));
    return StepResult::Next;
}

}  // namespace colony
