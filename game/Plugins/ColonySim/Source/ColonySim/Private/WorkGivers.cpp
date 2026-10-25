// The work scanners, one per task name in the WorkTypes defs (docs/design/jobs.md).
#include "ColonySim/Jobs.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Map.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <tuple>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// Hauling (RimWorld's WorkGiver_HaulGeneral): items that lie outside storage, or in storage worse
// than a stockpile that would take them, go to the best stockpile cell with room.
class HaulToStockpileGiver final : public WorkGiver {
public:
    const char* task() const override { return "HaulToStockpile"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        noRoom_.clear();
        StoragePriority best = StoragePriority::Unstored;
        for (const StockpileZone& zone : jobs.zones().stockpiles()) {
            best = std::max(best, zone.priority);
        }
        if (best == StoragePriority::Unstored) {
            return;  // no stockpiles
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        reg.view<const Position, const StackCount>().each([&](Entity item, const Position& pos, const StackCount&) {
            if (pos.level != at.level || jobs.storedPriority(item) >= best) {
                return;
            }
            const float dx = pos.x - at.x;
            const float dy = pos.y - at.y;
            out.push_back(WorkCandidate{JobTarget{item, CellPos{}}, dx * dx + dy * dy, 0, 0, false, CellPos{}});
        });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        // Once one item of a def finds no room from one place, the rest of this think skips the
        // same def from the same reachability group and storage level: the answer would be the same.
        const entt::registry& reg = jobs.things().registry();
        const Entity item = candidate.target.thing;
        const DefId def = reg.get<Thing>(item).def;
        const uint32_t group = jobs.map().regions().groupAt(cellOf(reg.get<Position>(item)));
        const auto key = std::make_tuple(def.value, group, static_cast<int>(jobs.storedPriority(item)));
        if (std::find(noRoom_.begin(), noRoom_.end(), key) != noRoom_.end()) {
            return false;
        }
        if (makeHaulJob(jobs, pawn, item, out)) {
            return true;
        }
        noRoom_.push_back(key);
        return false;
    }

private:
    std::vector<std::tuple<uint32_t, uint32_t, int>> noRoom_;  // def, group, stored priority
};

float workGiverDistSq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Construction (docs/design/construction.md). A pawn builds from a cell beside the footprint, so these
// scanners hand the job system that cell to check reachability with (`WorkCandidate::reach`).

// Materials to blueprints and frames (RimWorld's WorkGiver_ConstructDeliverResources): the nearest
// stack of a material a site still needs, carried to a cell beside the site.
class ConstructDeliverGiver final : public WorkGiver {
public:
    const char* task() const override { return "ConstructDeliverResources"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        noSource_.clear();
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        reg.view<const ConstructionSite, const Position>().each([&](Entity site, const ConstructionSite& s, const Position& pos) {
            if (pos.level != at.level || s.hasAllMaterials() || jobs.deliveryReservedByOther(site, pawn)) {
                return;
            }
            CellPos stand;
            if (!jobs.construction().workCell(s.rect(), cellOf(at), stand)) {
                return;
            }
            out.push_back(WorkCandidate{JobTarget{site, CellPos{}}, workGiverDistSq(at, pos), 0, 0, true, stand});
        });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Entity site = candidate.target.thing;
        const ConstructionSite* s = reg.try_get<ConstructionSite>(site);
        if (s == nullptr || jobs.deliveryReservedByOther(site, pawn)) {
            return false;
        }
        const Position& at = reg.get<Position>(pawn);
        const CellPos from = cellOf(at);
        for (const MaterialCount& m : s->materials) {
            if (m.have >= m.needed || std::find(noSource_.begin(), noSource_.end(), m.item.value) != noSource_.end()) {
                continue;
            }
            Entity best = kNoEntity;
            float bestDist = 0.0f;
            reg.view<const Thing, const Position, const StackCount>().each(
                [&](Entity item, const Thing& thing, const Position& pos, const StackCount&) {
                    if (thing.def != m.item || pos.level != at.level) {
                        return;
                    }
                    const float dist = workGiverDistSq(at, pos);
                    if (best != kNoEntity && (dist > bestDist || (dist == bestDist && item > best))) {
                        return;
                    }
                    const JobTarget target{item, CellPos{}};
                    if (jobs.reservedByOther(target, pawn) || jobs.avoiding(pawn, target) || !jobs.canReach(from, cellOf(pos))) {
                        return;
                    }
                    best = item;
                    bestDist = dist;
                });
            if (best == kNoEntity) {
                noSource_.push_back(m.item.value);
                continue;
            }
            out = Job{};
            out.type = JobType::DeliverResources;
            out.thing = best;
            out.site = site;
            out.cell = candidate.reach;
            out.count = std::min({m.needed - m.have, jobs.carryLimit(pawn, m.item), reg.get<StackCount>(best).count});
            return out.count > 0;
        }
        return false;
    }

private:
    std::vector<uint32_t> noSource_;  // item defs with no stack to fetch, this think
};

// Frames with all their materials (RimWorld's WorkGiver_ConstructFinishFrames).
class ConstructFinishGiver final : public WorkGiver {
public:
    const char* task() const override { return "ConstructFinishFrames"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        reg.view<const ConstructionSite, const Position>().each([&](Entity site, const ConstructionSite& s, const Position& pos) {
            if (pos.level != at.level || s.stage != ConstructionStage::Frame || !s.hasAllMaterials()) {
                return;
            }
            CellPos stand;
            if (!jobs.construction().workCell(s.rect(), cellOf(at), stand)) {
                return;
            }
            out.push_back(WorkCandidate{JobTarget{site, CellPos{}}, workGiverDistSq(at, pos), 0, 0, true, stand});
        });
    }

    bool makeJob(JobSystem&, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::BuildFrame;
        out.thing = candidate.target.thing;
        out.cell = candidate.reach;
        return true;
    }
};

// Buildings marked to be taken down (RimWorld's WorkGiver_Deconstruct).
class DeconstructGiver final : public WorkGiver {
public:
    const char* task() const override { return "Deconstruct"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        reg.view<const MarkedForDeconstruct, const Footprint, const Position>().each(
            [&](Entity building, const Footprint& footprint, const Position& pos) {
                if (pos.level != at.level) {
                    return;
                }
                CellPos stand;
                if (!jobs.construction().workCell(footprint.rect(), cellOf(at), stand)) {
                    return;
                }
                out.push_back(WorkCandidate{JobTarget{building, CellPos{}}, workGiverDistSq(at, pos), 0, 0, true, stand});
            });
    }

    bool makeJob(JobSystem&, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::Deconstruct;
        out.thing = candidate.target.thing;
        out.cell = candidate.reach;
        return true;
    }
};

}  // namespace

bool makeHaulJob(JobSystem& jobs, Entity pawn, Entity item, Job& out) {
    const entt::registry& reg = jobs.things().registry();
    const DefId def = reg.get<Thing>(item).def;
    const CellPos from = cellOf(reg.get<Position>(item));
    CellPos cell;
    if (!jobs.findStorageCell(def, from, jobs.storedPriority(item), pawn, cell)) {
        return false;
    }
    out = Job{};
    out.type = JobType::HaulToCell;
    out.thing = item;
    out.cell = cell;
    out.count = std::min({reg.get<StackCount>(item).count, jobs.carryLimit(pawn, def), jobs.roomFor(def, cell)});
    return out.count > 0;
}

std::vector<std::unique_ptr<WorkGiver>> makeWorkGivers() {
    std::vector<std::unique_ptr<WorkGiver>> givers;
    givers.push_back(std::make_unique<HaulToStockpileGiver>());
    givers.push_back(std::make_unique<ConstructDeliverGiver>());
    givers.push_back(std::make_unique<ConstructFinishGiver>());
    givers.push_back(std::make_unique<DeconstructGiver>());
    return givers;
}

}  // namespace colony
