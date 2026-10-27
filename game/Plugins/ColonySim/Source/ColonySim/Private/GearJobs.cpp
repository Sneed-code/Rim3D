// The RepairGear job: fetch a marked piece of gear, carry it to a bench and mend it, or fit an upgrade part to a
// gun (task P4, docs/design/repair.md); and MendWorn, mending one's own clothes at a bench while wearing them
// (docs/design/apparel-durability.md). Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Crafting.h"
#include "ColonySim/Health.h"
#include "ColonySim/Repair.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

// A job's `bill` is 0 for a repair, else the part's def id + 1.
DefId gearPartOf(const Job& job) { return job.bill == 0 ? DefId{} : DefId{job.bill - 1}; }

}  // namespace

bool JobSystem::failCheckGear(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (repair_ == nullptr || !reg.valid(job.thing) || !reg.all_of<Thing>(job.thing)) {
        why = "the gear is gone";
        return false;
    }
    if (!things_.spawned(job.site) || reg.all_of<ConstructionSite>(job.site) || reg.all_of<MarkedForDeconstruct>(job.site)) {
        why = "the bench is gone";
        return false;
    }
    if (!buildingRuns(job.site)) {
        why = "the bench has no power";
        return false;
    }
    const DefId gear = reg.get<Thing>(job.thing).def;
    const DefId bench = reg.get<Thing>(job.site).def;
    const DefId part = gearPartOf(job);
    if (part.valid()) {
        const FitOrder* order = reg.try_get<FitOrder>(job.thing);
        if (order == nullptr || order->part != part || !repair_->benchFits(bench)) {
            why = "the fitting was called off";
            return false;
        }
    } else if (!reg.all_of<RepairMark>(job.thing) || !repair_->benchRepairs(bench, repair_->kindOf(gear))) {
        why = "the repair was called off";
        return false;
    }
    if (job.type == JobType::MendWorn) {
        if (apparel_ == nullptr || std::find(apparel_->worn(pawn).begin(), apparel_->worn(pawn).end(), job.thing) == apparel_->worn(pawn).end()) {
            why = "it no longer wears that";
            return false;
        }
        return true;
    }
    if (job.step <= kGearPickUpStep) {
        if (!things_.spawned(job.thing)) {
            why = "the gear is gone";
            return false;
        }
    } else {
        const Carrying* carrying = reg.try_get<Carrying>(pawn);
        if (carrying == nullptr || carrying->thing != job.thing) {
            why = "it lost what it carried";
            return false;
        }
    }
    return true;
}

// Stands beside the bench working for the repair's (or the fitting's) game seconds at the worker's speed; then
// takes the material (or the part) from the stacks near the bench and mends (or fits). Nothing is used up if the
// job is interrupted before the end.
JobSystem::StepResult JobSystem::gearWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the bench";
        return StepResult::Failed;
    }
    const Entity item = job.thing;
    const DefId part = gearPartOf(job);
    const Position& benchPos = reg.get<Position>(job.site);
    const CellPos benchCell = cellOf(benchPos);
    float needed = 0.0f;
    DefId material;
    int count = 0;
    if (part.valid()) {
        const UpgradeSpec* spec = repair_->upgrade(part);
        if (spec == nullptr) {
            why = "that is not an upgrade part";
            return StepResult::Failed;
        }
        const std::string blocker = repair_->fitBlocker(item, part);
        if (!blocker.empty()) {
            why = blocker;
            return StepResult::Failed;
        }
        needed = spec->work;
        material = part;
        count = 1;
    } else {
        const RepairCost cost = repair_->costOf(item);
        if (cost.count <= 0 || !cost.material.valid()) {
            why = "it isn't damaged";
            return StepResult::Failed;
        }
        needed = cost.work;
        material = cost.material;
        count = cost.count;
    }
    if (repair_->stockNear(material, benchCell, benchPos.level) < count) {
        why = part.valid() ? "the part isn't near the bench any more" : "there isn't enough material near the bench";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Repair, job.site);
    }
    const BillNumbers& numbers = bills_->numbers();
    const int skill = bills_->skillLevel(pawn, repair_->numbers().skill);
    const float benchFactor = crafting_ != nullptr ? crafting_->benchSpeed(reg.get<Thing>(job.site).def) : 1.0f;
    job.progress += static_cast<float>(numbers.skillSpeedBase + numbers.skillSpeedPerLevel * skill) *
                    health_.capacityValue(pawn, "Manipulation") * lightWorkFactor(pawn) * benchFactor;
    if (job.progress < needed) {
        return StepResult::Running;
    }
    if (!repair_->takeNear(material, count, benchCell, benchPos.level)) {
        why = "the material was taken away";
        return StepResult::Failed;
    }
    Event event;
    event.thing = pawn;
    event.target = item;
    event.cell = job.cell;
    if (part.valid()) {
        repair_->finishFit(item, part);
        event.type = EventType::WeaponUpgraded;
        event.def = part;
        event.amount = 1.0f;
    } else {
        event.def = reg.get<Thing>(item).def;
        event.amount = static_cast<float>(repair_->finishRepair(item));
        event.type = EventType::GearRepaired;
    }
    events_.push_back(std::move(event));
    return StepResult::Next;
}

bool JobSystem::thinkMendWorn(Entity pawn, Job& out) {
    if (repair_ == nullptr || apparel_ == nullptr || !canUseHands(pawn)) {
        return false;
    }
    const entt::registry& reg = things_.registry();
    const RepairNumbers& numbers = repair_->numbers();
    if (bills_->skillLevel(pawn, numbers.skill) < repair_->repairMinSkill()) {
        return false;
    }
    // The piece: one the player marked, else the most worn one at or under the line.
    Entity piece = kNoEntity;
    float worst = 2.0f;
    for (const Entity item : apparel_->worn(pawn)) {
        const float condition = ApparelStats::condition(reg, item);
        const bool marked = reg.all_of<RepairMark>(item);
        if (condition >= 1.0f || (!marked && condition > numbers.mendWornAtOrBelow) || reservedByOther(JobTarget{item, CellPos{}}, pawn)) {
            continue;
        }
        const float rank = marked ? condition - 1.0f : condition;  // marked pieces first
        if (rank < worst) {
            worst = rank;
            piece = item;
        }
    }
    if (piece == kNoEntity) {
        return false;
    }
    const RepairCost cost = repair_->costOf(piece);
    if (cost.count <= 0 || !cost.material.valid()) {
        return false;
    }
    // The nearest tailor bench that runs, is free, has a place to stand and the material within reach.
    const Position& at = reg.get<Position>(pawn);
    const CellPos from = cellOf(at);
    Entity bench = kNoEntity;
    CellPos stand;
    float bestDist = 0.0f;
    reg.view<const Thing, const Footprint, const Position>().each([&](Entity b, const Thing& thing, const Footprint& footprint, const Position& pos) {
        if (pos.level != at.level || !repair_->benchRepairs(thing.def, "Apparel") || reg.all_of<ConstructionSite>(b) ||
            reg.all_of<MarkedForDeconstruct>(b) || !buildingRuns(b) || deliveryReservedByOther(b, pawn)) {
            return;
        }
        const float dx = pos.x - at.x;
        const float dy = pos.y - at.y;
        const float dist = dx * dx + dy * dy;
        if (bench != kNoEntity && (dist > bestDist || (dist == bestDist && b > bench))) {
            return;
        }
        CellPos cell;
        if (!construction().workCell(footprint.rect(), from, cell) || !canReach(from, cell) ||
            repair_->stockNear(cost.material, cellOf(pos), pos.level) < cost.count) {
            return;
        }
        bench = b;
        stand = cell;
        bestDist = dist;
    });
    if (bench == kNoEntity) {
        return false;
    }
    things_.registry().emplace_or_replace<RepairMark>(piece);  // so the job checks it like any repair
    out = Job{};
    out.type = JobType::MendWorn;
    out.thing = piece;
    out.site = bench;
    out.cell = stand;
    out.count = 1;
    return true;
}

}  // namespace colony
