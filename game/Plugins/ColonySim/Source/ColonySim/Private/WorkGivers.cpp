// The work scanners, one per task name in the WorkTypes defs (docs/design/jobs.md).
#include "ColonySim/Jobs.h"

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
            out.push_back(WorkCandidate{JobTarget{item, CellPos{}}, dx * dx + dy * dy, 0, 0});
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
    return givers;
}

}  // namespace colony
