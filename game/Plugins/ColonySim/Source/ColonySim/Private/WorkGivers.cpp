// The work scanners, one per task name in the WorkTypes defs (docs/design/jobs.md).
#include "ColonySim/Jobs.h"

#include "ColonySim/Bills.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Research.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
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

// Mining (RimWorld's WorkGiver_Miner): designated rock and ore. The target is the rock cell; the pawn
// works from a cell next to it, which the job system checks it can reach.
class MineGiver final : public WorkGiver {
public:
    const char* task() const override { return "Mine"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        if (!jobs.canUseHands(pawn)) {
            return;
        }
        const Position& at = jobs.things().registry().get<Position>(pawn);
        for (const auto& [index, kind] : jobs.designations().all()) {
            if (kind != DesignationKind::Mine) {
                continue;
            }
            const CellPos cell = jobs.map().cellAt(index);
            if (cell.level != at.level || !jobs.mineables().has(cell)) {
                continue;
            }
            const float dx = static_cast<float>(cell.x) + 0.5f - at.x;
            const float dy = static_cast<float>(cell.y) + 0.5f - at.y;
            out.push_back(WorkCandidate{JobTarget{kNoEntity, cell}, dx * dx + dy * dy, 0, 0, false, CellPos{}});
        }
    }

    bool makeJob(JobSystem&, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::Mine;
        out.cell = candidate.target.cell;
        return true;
    }
};

// Cutting and picking designated plants (RimWorld's WorkGiver_PlantsCut and Harvest, in the
// Plant cut work type). A harvest mark waits until the plant has grown enough.
class CutOrHarvestPlantsGiver final : public WorkGiver {
public:
    const char* task() const override { return "CutOrHarvestDesignatedPlants"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        if (!jobs.canUseHands(pawn)) {
            return;
        }
        const Position& at = jobs.things().registry().get<Position>(pawn);
        for (const auto& [index, kind] : jobs.designations().all()) {
            if (kind != DesignationKind::CutPlant && kind != DesignationKind::HarvestPlant) {
                continue;
            }
            const CellPos cell = jobs.map().cellAt(index);
            if (cell.level != at.level || !jobs.plants().has(cell) ||
                (kind == DesignationKind::HarvestPlant && !jobs.harvestableNow(cell))) {
                continue;
            }
            const float dx = static_cast<float>(cell.x) + 0.5f - at.x;
            const float dy = static_cast<float>(cell.y) + 0.5f - at.y;
            out.push_back(WorkCandidate{JobTarget{kNoEntity, cell}, dx * dx + dy * dy, 0, 0, false, CellPos{}});
        }
    }

    bool makeJob(JobSystem& jobs, Entity, const WorkCandidate& candidate, Job& out) override {
        const DesignationKind kind = jobs.designations().at(candidate.target.cell);
        if (kind != DesignationKind::CutPlant && kind != DesignationKind::HarvestPlant) {
            return false;
        }
        out = Job{};
        out.type = kind == DesignationKind::CutPlant ? JobType::CutPlant : JobType::HarvestPlant;
        out.cell = candidate.target.cell;
        return true;
    }
};

// Bills on a stove (RimWorld's WorkGiver_DoBill): the first bill in the building's list that should
// be done now, that the pawn is skilled enough for, and whose ingredients it can fetch. The
// ingredients are the nearest free stacks to the building; the pawn carries them one stack at a
// time (docs/design/bills.md).
class DoBillsCookStoveGiver final : public WorkGiver {
public:
    const char* task() const override { return "DoBillsCookStove"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        if (!jobs.canUseHands(pawn)) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        reg.view<const BillList, const Footprint, const Position>().each(
            [&](Entity building, const BillList& list, const Footprint& footprint, const Position& pos) {
                if (pos.level != at.level || reg.all_of<MarkedForDeconstruct>(building) || !jobs.buildingRuns(building)) {
                    return;
                }
                // Cheap check only: some bill that isn't suspended (the stock of an "until you
                // have" bill is counted when the job is made).
                const bool any = std::any_of(list.bills.begin(), list.bills.end(), [](const Bill& bill) {
                    return !bill.suspended && (bill.mode != BillMode::Count || bill.count > 0);
                });
                CellPos stand;
                if (!any || !jobs.construction().workCell(footprint.rect(), cellOf(at), stand)) {
                    return;
                }
                out.push_back(WorkCandidate{JobTarget{building, CellPos{}}, workGiverDistSq(at, pos), 0, 0, true, stand});
            });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Entity building = candidate.target.thing;
        const BillList* list = reg.try_get<BillList>(building);
        if (list == nullptr) {
            return false;
        }
        const BillSystem& bills = jobs.bills();
        for (const Bill& bill : list->bills) {
            const Recipe* recipe = bills.recipe(bill.recipe);
            if (recipe == nullptr || recipe->work <= 0.0f || !bills.shouldDo(bill) ||
                bills.skillLevel(pawn, recipe->skill) < recipe->minSkill) {
                continue;
            }
            std::vector<BillPick> picks;
            if (!planIngredients(jobs, pawn, building, *recipe, picks)) {
                continue;
            }
            out = Job{};
            out.type = JobType::DoBill;
            out.site = building;
            out.cell = candidate.reach;
            out.bill = bill.id;
            out.thing = picks.front().item;
            out.count = picks.front().count;
            out.picks.assign(picks.begin() + 1, picks.end());
            return true;
        }
        return false;
    }

private:
    struct Stack {
        float distSq = 0.0f;
        Entity item = kNoEntity;
    };

    // Picks stacks that cover every ingredient of the recipe, nearest to the building first, never
    // one that is held, avoided or unreachable. False if the map doesn't have enough.
    static bool planIngredients(JobSystem& jobs, Entity pawn, Entity building, const Recipe& recipe,
                                std::vector<BillPick>& picks) {
        const entt::registry& reg = jobs.things().registry();
        const BillSystem& bills = jobs.bills();
        const Position& at = reg.get<Position>(pawn);
        const Position& stove = reg.get<Position>(building);
        const CellPos from = cellOf(at);
        std::vector<Stack> stacks;
        for (const RecipeIngredient& ingredient : recipe.ingredients) {
            stacks.clear();
            reg.view<const Thing, const Position, const StackCount>().each(
                [&](Entity item, const Thing& thing, const Position& pos, const StackCount&) {
                    if (pos.level != at.level ||
                        std::find(ingredient.items.begin(), ingredient.items.end(), thing.def) == ingredient.items.end()) {
                        return;
                    }
                    const JobTarget target{item, CellPos{}};
                    if (jobs.reservedByOther(target, pawn) || jobs.avoiding(pawn, target) ||
                        std::any_of(picks.begin(), picks.end(), [item](const BillPick& p) { return p.item == item; }) ||
                        !jobs.canReach(from, cellOf(pos))) {
                        return;
                    }
                    stacks.push_back(Stack{workGiverDistSq(stove, pos), item});
                });
            std::sort(stacks.begin(), stacks.end(), [](const Stack& a, const Stack& b) {
                return a.distSq != b.distSq ? a.distSq < b.distSq : a.item < b.item;
            });
            double left = ingredient.amount;  // nutrition or count still to cover
            for (const Stack& stack : stacks) {
                if (left <= 1.0e-6) {
                    break;
                }
                const DefId def = reg.get<Thing>(stack.item).def;
                const double unit = ingredient.byNutrition ? bills.nutritionOf(def) : 1.0;
                const int wanted = ingredient.byNutrition ? static_cast<int>(std::ceil(left / unit - 1.0e-6))
                                                          : static_cast<int>(std::lround(left));
                const int take = std::min({wanted, reg.get<StackCount>(stack.item).count, jobs.carryLimit(pawn, def)});
                if (take <= 0) {
                    continue;
                }
                if (static_cast<int>(picks.size()) >= bills.numbers().maxIngredientStacks) {
                    return false;
                }
                picks.push_back(BillPick{stack.item, take});
                left -= take * unit;
            }
            if (left > 1.0e-6) {
                return false;
            }
        }
        return !picks.empty();
    }
};

// Research (RimWorld's WorkGiver_Researcher): a research bench that can work on the colony's current
// project, with nobody else at it (docs/design/research.md). The pawn sits at a cell beside the bench.
class ResearchGiver final : public WorkGiver {
public:
    const char* task() const override { return "Research"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const ResearchSystem* research = jobs.research();
        if (research == nullptr || research->current().empty()) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        reg.view<const Thing, const Footprint, const Position>().each(
            [&](Entity bench, const Thing& thing, const Footprint& footprint, const Position& pos) {
                const ResearchBenchInfo* info = research->benchInfo(thing.def);
                if (info == nullptr || pos.level != at.level || reg.all_of<ConstructionSite>(bench) ||
                    reg.all_of<MarkedForDeconstruct>(bench) || !research->benchCanWork(*info) ||
                    !jobs.buildingRuns(bench)) {
                    return;
                }
                CellPos stand;
                if (!jobs.construction().workCell(footprint.rect(), cellOf(at), stand)) {
                    return;
                }
                out.push_back(WorkCandidate{JobTarget{bench, CellPos{}}, workGiverDistSq(at, pos), 0, 0, true, stand});
            });
    }

    bool makeJob(JobSystem&, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::Research;
        out.site = candidate.target.thing;
        out.cell = candidate.reach;
        return true;
    }
};

// Medicine (task H1, docs/design/medicine.md). RimWorld's doctor tasks: tend whoever needs it (the bleeding first),
// carry the downed to a bed; and the patient's own: go to a medical bed, rest in any bed.

// A pawn of the player's side with a body that thinks (a colonist): who doctors work on.
bool medicalColonist(const entt::registry& reg, Entity pawn) {
    if (!reg.all_of<Health, Position, Worker>(pawn) || reg.get<Health>(pawn).dead) {
        return false;
    }
    const Faction* faction = reg.try_get<Faction>(pawn);
    return faction == nullptr || faction->id == kPlayerFaction;
}

// TendPatients (WorkGiver_Tend): a patient with an injury or lost part nobody has tended, the pawn itself too, as
// long as it isn't down. If anyone is bleeding, only those are offered.
class TendPatientsGiver final : public WorkGiver {
public:
    const char* task() const override { return "TendPatients"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        if (jobs.medicine() == nullptr || !jobs.canUseHands(pawn)) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        const size_t first = out.size();
        bool urgent = false;
        for (const auto [patient, health, pos] : reg.view<const Health, const Position>().each()) {
            if (pos.level != at.level || !medicalColonist(reg, patient) || !jobs.health().needsTending(patient) ||
                jobs.medicine()->careOf(patient) == MedicalCare::NoCare) {
                continue;
            }
            const bool bleeds = jobs.health().urgentlyNeedsTending(patient);
            if (urgent && !bleeds) {
                continue;
            }
            if (bleeds && !urgent) {
                urgent = true;
                out.resize(first);  // the ones found so far are not urgent
            }
            out.push_back(WorkCandidate{JobTarget{patient, CellPos{}}, workGiverDistSq(at, pos), 0, 0, false, CellPos{}});
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const Entity patient = candidate.target.thing;
        if (!jobs.health().needsTending(patient)) {
            return false;
        }
        out = Job{};
        out.type = JobType::TendPatient;
        out.thing = patient;
        out.site = jobs.findMedicine(pawn, patient);
        return true;
    }
};

// Rescue (WorkGiver_RescueDowned): a downed colonist that no bed holds goes to the best free bed.
class RescueGiver final : public WorkGiver {
public:
    const char* task() const override { return "Rescue"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        if (jobs.medicine() == nullptr) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        for (const auto [patient, health, pos] : reg.view<const Health, const Position>().each()) {
            if (patient == pawn || pos.level != at.level || !jobs.needsRescue(patient)) {
                continue;
            }
            out.push_back(WorkCandidate{JobTarget{patient, CellPos{}}, workGiverDistSq(at, pos), 0, 0, false, CellPos{}});
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const Entity patient = candidate.target.thing;
        Entity bed = kNoEntity;
        CellPos cell;
        if (!jobs.needsRescue(patient) || !jobs.findPatientBed(patient, pawn, false, bed, cell)) {
            return false;
        }
        out = Job{};
        out.type = JobType::Rescue;
        out.thing = patient;
        out.chair = bed;
        out.cell = cell;
        return true;
    }
};

// A patient that isn't down goes to bed to heal: GoToMedicalBed only to a medical bed, RestToHeal to any bed.
class BedRestGiver : public WorkGiver {
public:
    explicit BedRestGiver(bool medicalOnly) : medicalOnly_(medicalOnly) {}

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const entt::registry& reg = jobs.things().registry();
        if (!reg.all_of<Health, Worker>(pawn) || reg.get<Health>(pawn).downed || !jobs.health().needsRest(pawn)) {
            return;
        }
        const Entity under = jobs.bedUnder(pawn);
        if (under != kNoEntity && (!medicalOnly_ || jobs.isMedicalBed(under))) {
            // Already on a bed that will do: rest there.
            out.push_back(WorkCandidate{JobTarget{under, CellPos{}}, 0.0f, 0, 0, true, cellOf(reg.get<Position>(pawn))});
            return;
        }
        Entity bed = kNoEntity;
        CellPos cell;
        if (jobs.findPatientBed(pawn, pawn, medicalOnly_, bed, cell)) {
            out.push_back(WorkCandidate{JobTarget{bed, CellPos{}}, workGiverDistSq(reg.get<Position>(pawn), reg.get<Position>(bed)),
                                        0, 0, true, cell});
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        if (!jobs.health().needsRest(pawn)) {
            return false;
        }
        out = Job{};
        out.type = JobType::BedRest;
        out.thing = candidate.target.thing;
        out.cell = candidate.reach;
        return true;
    }

private:
    bool medicalOnly_;
};

class GoToMedicalBedGiver final : public BedRestGiver {
public:
    GoToMedicalBedGiver() : BedRestGiver(true) {}
    const char* task() const override { return "GoToMedicalBed"; }
};

class RestToHealGiver final : public BedRestGiver {
public:
    RestToHealGiver() : BedRestGiver(false) {}
    const char* task() const override { return "RestToHeal"; }
};

// Cleaning filth (RimWorld's WorkGiver_CleanFilth, in the Cleaning work type): any cell indoors with
// filth in it. Outdoor filth waits for the home area, which RimWorld's cleaners are limited to.
class CleanFilthGiver final : public WorkGiver {
public:
    const char* task() const override { return "CleanFilth"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        Filth* filth = jobs.filth();
        Rooms* rooms = jobs.rooms();
        if (filth == nullptr || rooms == nullptr || filth->cellCount() == 0 || !jobs.canUseHands(pawn)) {
            return;
        }
        const Position& at = jobs.things().registry().get<Position>(pawn);
        for (const auto& [index, piles] : filth->all()) {
            (void)piles;
            const CellPos cell = jobs.map().cellAt(index);
            if (cell.level != at.level || !rooms->indoors(cell)) {
                continue;
            }
            const float dx = static_cast<float>(cell.x) + 0.5f - at.x;
            const float dy = static_cast<float>(cell.y) + 0.5f - at.y;
            out.push_back(WorkCandidate{JobTarget{kNoEntity, cell}, dx * dx + dy * dy, 0, 0, false, CellPos{}});
        }
    }

    bool makeJob(JobSystem& jobs, Entity, const WorkCandidate& candidate, Job& out) override {
        if (jobs.filth() == nullptr || !jobs.filth()->has(candidate.target.cell)) {
            return false;
        }
        out = Job{};
        out.type = JobType::CleanFilth;
        out.cell = candidate.target.cell;
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
    givers.push_back(std::make_unique<MineGiver>());
    givers.push_back(std::make_unique<CutOrHarvestPlantsGiver>());
    givers.push_back(std::make_unique<DoBillsCookStoveGiver>());
    givers.push_back(std::make_unique<ResearchGiver>());
    givers.push_back(std::make_unique<CleanFilthGiver>());
    givers.push_back(std::make_unique<TendPatientsGiver>());
    givers.push_back(std::make_unique<RescueGiver>());
    givers.push_back(std::make_unique<GoToMedicalBedGiver>());
    givers.push_back(std::make_unique<RestToHealGiver>());
    return givers;
}

}  // namespace colony
