// Hands-on work (task U10, docs/design/handson.md): the player's colonist does the sim's own jobs
// (mine, chop, pick, build, deliver, pick up) in place, while the player holds the interact key.
#include "ColonySim/Jobs.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

DesignationKind handsOnMark(HandsOnKind kind) {
    switch (kind) {
        case HandsOnKind::Mine: return DesignationKind::Mine;
        case HandsOnKind::Chop:
        case HandsOnKind::Cut: return DesignationKind::CutPlant;
        case HandsOnKind::Harvest: return DesignationKind::HarvestPlant;
        default: return DesignationKind::None;
    }
}

}  // namespace

bool JobSystem::handsReach(float x, float y, int level, const CellRect& rect) {
    if (rect.level != level) {
        return false;
    }
    const float dx = std::max({static_cast<float>(rect.minX) - x, 0.0f, x - static_cast<float>(rect.minX + rect.sizeX)});
    const float dy = std::max({static_cast<float>(rect.minY) - y, 0.0f, y - static_cast<float>(rect.minY + rect.sizeY)});
    return dx * dx + dy * dy <= kHandsOnReach * kHandsOnReach;
}

bool JobSystem::handsOnInReach(Entity pawn, const Job& job) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    CellRect rect{job.cell.x, job.cell.y, 1, 1, job.cell.level};
    if (job.type == JobType::BuildFrame || job.type == JobType::DeliverResources) {
        const Entity site = job.type == JobType::BuildFrame ? job.thing : job.site;
        const ConstructionSite* s = reg.valid(site) ? reg.try_get<ConstructionSite>(site) : nullptr;
        if (s == nullptr) {
            return true;  // gone: the fail check says so
        }
        rect = s->rect();
    }
    return handsReach(at.x, at.y, at.level, rect);
}

HandsOnPlan JobSystem::planHandsOn(Entity pawn, Entity thing, CellPos cell) const {
    HandsOnPlan plan;
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<PlayerControl, Worker, Position>(pawn)) {
        plan.error = "you aren't controlling a colonist";
        return plan;
    }
    if (health_.incapacitated(pawn)) {
        plan.error = "you are down";
        return plan;
    }
    if (!canUseHands(pawn)) {
        plan.error = "your hands can't do that";
        return plan;
    }
    const Position& at = reg.get<Position>(pawn);
    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    const bool carries = carrying != nullptr && reg.valid(carrying->thing);
    const auto labelOf = [this](DefId def) { return defs_.get(def).label(); };

    if (thing != kNoEntity) {
        if (!things_.spawned(thing)) {
            plan.error = "that is gone";
            return plan;
        }
        if (const ConstructionSite* site = reg.try_get<ConstructionSite>(thing)) {
            const std::string name = labelOf(reg.get<Thing>(thing).def);
            plan.label = name;
            plan.inReach = handsReach(at.x, at.y, at.level, site->rect());
            if (carries) {
                const DefId held = reg.get<Thing>(carrying->thing).def;
                const bool needed = std::any_of(site->materials.begin(), site->materials.end(),
                                                [held](const MaterialCount& m) { return m.item == held && m.have < m.needed; });
                if (!needed) {
                    plan.error = "the " + name + " doesn't need that";
                    return plan;
                }
                plan.kind = HandsOnKind::Deliver;
                plan.label = "put " + labelOf(held) + " into the " + name;
                if (deliveryReservedByOther(thing, pawn)) {
                    plan.error = "another colonist is bringing materials there";
                }
                return plan;
            }
            if (site->stage == ConstructionStage::Frame && site->hasAllMaterials()) {
                plan.kind = HandsOnKind::Build;
                plan.label = "build the " + name;
                if (reservedByOther(JobTarget{thing, CellPos{}}, pawn)) {
                    plan.error = "another colonist is building it";
                }
                return plan;
            }
            plan.error = "the " + name + " needs materials: pick some up and bring them";
            return plan;
        }
        if (reg.all_of<StackCount>(thing)) {
            const CellPos where = cellOf(reg.get<Position>(thing));
            plan.kind = HandsOnKind::PickUp;
            plan.label = "pick up " + labelOf(reg.get<Thing>(thing).def);
            plan.inReach = handsReach(at.x, at.y, at.level, CellRect{where.x, where.y, 1, 1, where.level});
            if (carries) {
                plan.error = "your hands are full";
            } else if (reservedByOther(JobTarget{thing, CellPos{}}, pawn)) {
                plan.error = "another colonist is about to take it";
            }
            return plan;
        }
        plan.error = "nothing to do with that";
        return plan;
    }

    if (!map_.inBounds(cell)) {
        plan.error = "nothing there";
        return plan;
    }
    plan.inReach = handsReach(at.x, at.y, at.level, CellRect{cell.x, cell.y, 1, 1, cell.level});
    if (mineables_.has(cell)) {
        plan.kind = HandsOnKind::Mine;
        plan.label = "mine " + labelOf(mineables_.defAt(cell));
    } else if (plants_.has(cell)) {
        const DefId plant = plants_.defAt(cell);
        const bool tree = defs_.get(plant).flag("plant.isTree");
        plan.kind = !tree && harvestableNow(cell) ? HandsOnKind::Harvest : (tree ? HandsOnKind::Chop : HandsOnKind::Cut);
        plan.label = std::string(plan.kind == HandsOnKind::Harvest ? "pick " : (tree ? "chop " : "cut ")) + labelOf(plant);
    } else {
        plan.error = "nothing to work on there";
        return plan;
    }
    if (carries) {
        plan.error = "your hands are full";
    } else if (reservedByOther(JobTarget{kNoEntity, cell}, pawn)) {
        plan.error = "another colonist is working on it";
    }
    return plan;
}

bool JobSystem::handsOnPickUp(Entity pawn, Entity item, std::string& error) {
    entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(item).def;
    const int stack = reg.get<StackCount>(item).count;
    const int count = std::min(stack, carryLimit(pawn, def));
    if (count <= 0) {
        error = "you can't carry that";
        return false;
    }
    Entity carried = item;
    if (count < stack) {
        carried = things_.splitOff(item, count);
        if (carried == kNoEntity) {
            error = "you couldn't take part of the stack";
            return false;
        }
    } else {
        things_.despawn(item);
    }
    reg.emplace_or_replace<Carrying>(pawn, Carrying{carried});
    pawnDid(pawn, PawnAction::PickUp, carried);
    return true;
}

bool JobSystem::handsOn(Entity pawn, Entity thing, CellPos cell, std::string& error) {
    const HandsOnPlan plan = planHandsOn(pawn, thing, cell);
    if (plan.kind == HandsOnKind::None || !plan.error.empty()) {
        error = plan.error.empty() ? "nothing to do there" : plan.error;
        return false;
    }
    if (!plan.inReach) {
        error = "too far to " + plan.label;
        return false;
    }
    entt::registry& reg = things_.registry();
    const Job& running = reg.get<Worker>(pawn).job;
    if (plan.kind == HandsOnKind::PickUp) {
        interrupt(pawn, "the player picked something up");
        return handsOnPickUp(pawn, thing, error);
    }

    Job job;
    job.handsOn = 1;
    const CellPos here = cellOf(reg.get<Position>(pawn));
    switch (plan.kind) {
        case HandsOnKind::Mine: job.type = JobType::Mine; break;
        case HandsOnKind::Chop:
        case HandsOnKind::Cut: job.type = JobType::CutPlant; break;
        case HandsOnKind::Harvest: job.type = JobType::HarvestPlant; break;
        case HandsOnKind::Build:
            job.type = JobType::BuildFrame;
            job.thing = thing;
            job.cell = here;
            break;
        case HandsOnKind::Deliver:
            job.type = JobType::DeliverResources;
            job.thing = reg.get<Carrying>(pawn).thing;
            job.count = reg.get<StackCount>(job.thing).count;
            job.site = thing;
            job.cell = here;
            job.step = static_cast<uint8_t>(jobStepCount(JobType::DeliverResources) - 1);  // the material is in hand
            break;
        default: break;
    }
    if (jobIsGather(job.type)) {
        job.cell = cell;
    }
    // Already doing this: nothing to restart (the player pressed again, or the command repeated).
    if (running.type == job.type && running.handsOn != 0 &&
        (jobIsGather(job.type) ? running.cell == job.cell : (job.type == JobType::BuildFrame ? running.thing == job.thing : running.site == job.site))) {
        return true;
    }
    if (running.type != JobType::None) {
        endJob(pawn, JobEnd::Interrupted, "the player changed what it is doing");
    }
    if (jobIsGather(job.type)) {
        const DesignationKind mark = handsOnMark(plan.kind);
        if (designations_.at(cell) != mark) {
            // No mark yet (or another kind): the player marks it just for this, and it goes again if the work is dropped.
            designations_.set(cell, mark);
            job.handsOn = 2;
            Event changed;
            changed.type = EventType::DesignationsChanged;
            events_.push_back(std::move(changed));
        }
    }
    startJob(pawn, std::move(job));
    return true;
}

bool JobSystem::handsOnPutDown(Entity pawn, CellPos cell, std::string& error) {
    entt::registry& reg = things_.registry();
    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    if (carrying == nullptr || !reg.valid(carrying->thing)) {
        error = "you carry nothing";
        return false;
    }
    const Position& at = reg.get<Position>(pawn);
    if (map_.inBounds(cell) && handsReach(at.x, at.y, at.level, CellRect{cell.x, cell.y, 1, 1, cell.level}) && placeCarriedAt(pawn, cell)) {
        return true;
    }
    dropCarried(pawn);
    return true;
}

void JobSystem::handsOnStop(Entity pawn) {
    const entt::registry& reg = things_.registry();
    if (reg.valid(pawn) && reg.all_of<Worker>(pawn) && reg.get<Worker>(pawn).job.handsOn != 0) {
        endJob(pawn, JobEnd::Interrupted, "the player let go");
    }
}

float JobSystem::jobProgress(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Worker* worker = reg.valid(pawn) ? reg.try_get<Worker>(pawn) : nullptr;
    if (worker == nullptr) {
        return 0.0f;
    }
    const Job& job = worker->job;
    double fraction = 0.0;
    switch (job.type) {
        case JobType::Mine:
            fraction = static_cast<double>(job.progress) / gather_.hitSeconds;
            break;
        case JobType::CutPlant:
        case JobType::HarvestPlant:
            if (plants_.has(job.cell)) {
                fraction = static_cast<double>(job.progress) / std::max(1.0, defs_.get(plants_.defAt(job.cell)).number("plant.harvestWork", 1.0));
            }
            break;
        case JobType::BuildFrame: {
            const ConstructionSite* site = reg.valid(job.thing) ? reg.try_get<ConstructionSite>(job.thing) : nullptr;
            if (site != nullptr && site->workTotal > 0.0f) {
                fraction = static_cast<double>(site->workDone) / static_cast<double>(site->workTotal);
            }
            break;
        }
        default: break;
    }
    return static_cast<float>(std::clamp(fraction, 0.0, 1.0));
}

}  // namespace colony
