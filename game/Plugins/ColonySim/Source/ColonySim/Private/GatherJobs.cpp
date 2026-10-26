// Mining, felling and picking (task C3, docs/design/mining.md): the jobs on designated rock and
// plants, and the skill maths behind them.
#include "ColonySim/Jobs.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// RimWorld's Mining yield curve by skill level (60% at 0, 100% at 8, 113% at 20), used when the
// WorkRules def is missing (the test defs).
constexpr double kGatherYield[] = {0.6,  0.7,  0.8,  0.85, 0.9,  0.925, 0.95, 0.975, 1.0,  1.01, 1.02,
                                   1.03, 1.04, 1.05, 1.06, 1.07, 1.08,  1.09, 1.1,   1.12, 1.13};
constexpr double kGatherMinPlantSpeed = 0.1;  // RimWorld's floor for plant work speed

float gatherDistSq(CellPos a, CellPos b) {
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    return dx * dx + dy * dy;
}

// The Manipulation capacity (1 for a pawn with no body or no such capacity).
double gatherManipulation(const entt::registry& reg, Entity pawn, int index) {
    const Health* health = reg.try_get<Health>(pawn);
    if (health == nullptr || index < 0 || static_cast<size_t>(index) >= health->capacities.size()) {
        return 1.0;
    }
    return std::max(0.0, static_cast<double>(health->capacities[static_cast<size_t>(index)]));
}

}  // namespace

void JobSystem::loadGatherRules() {
    for (const double v : kGatherYield) {
        gather_.yieldByLevel.push_back(v);
    }
    if (const Def* rules = defs_.find("Rules", "WorkRules")) {
        gather_.miningSpeedBase = rules->number("mining.speedBase", gather_.miningSpeedBase);
        gather_.miningSpeedPerLevel = rules->number("mining.speedPerLevel", gather_.miningSpeedPerLevel);
        gather_.hitDamage = rules->number("mining.hitDamage", gather_.hitDamage);
        gather_.hitSeconds = rules->number("mining.hitSeconds", gather_.hitSeconds);
        gather_.miningXpPerSecond = rules->number("mining.xpPerSecond", gather_.miningXpPerSecond);
        gather_.plantSpeedBase = rules->number("plants.speedBase", gather_.plantSpeedBase);
        gather_.plantSpeedPerLevel = rules->number("plants.speedPerLevel", gather_.plantSpeedPerLevel);
        gather_.plantXpPerSecond = rules->number("plants.xpPerSecond", gather_.plantXpPerSecond);
        gather_.growthYieldFloor = rules->number("growthYieldFloor", gather_.growthYieldFloor);
        gather_.unskilledLevel = static_cast<int>(rules->integer("unskilledSkillLevel", gather_.unskilledLevel));
        if (rules->count("yieldByLevel") > 0) {
            gather_.yieldByLevel.clear();
            for (size_t i = 0; i < rules->count("yieldByLevel"); ++i) {
                gather_.yieldByLevel.push_back(rules->numberAt("yieldByLevel", i, 1.0));
            }
        }
    }
    if (const Def* skills = defs_.find("Rules", "SkillRules")) {
        for (size_t i = 0; i < skills->count("xpToNextLevel"); ++i) {
            gather_.xpToNextLevel.push_back(skills->numberAt("xpToNextLevel", i, 0.0));
        }
        gather_.passionLearn[0] = skills->number("passionLearnFactor.None", gather_.passionLearn[0]);
        gather_.passionLearn[1] = skills->number("passionLearnFactor.Minor", gather_.passionLearn[1]);
        gather_.passionLearn[2] = skills->number("passionLearnFactor.Major", gather_.passionLearn[2]);
    }
    const std::vector<DefId> capacities = defs_.idsOfKind("Capacities");
    for (size_t i = 0; i < capacities.size(); ++i) {
        if (defs_.get(capacities[i]).defName() == "Manipulation") {
            manipulationIndex_ = static_cast<int>(i);
        }
    }
}

// ---- Skill maths ------------------------------------------------------------------------------

int JobSystem::skillLevel(Entity pawn, const char* skill) const {
    const entt::registry& reg = things_.registry();
    const Colonist* colonist = reg.valid(pawn) ? reg.try_get<Colonist>(pawn) : nullptr;
    const SkillRecord* record = colonist != nullptr ? colonist->skill(skill) : nullptr;
    const int level = record != nullptr ? record->level : gather_.unskilledLevel;
    // Genes add skill levels (aptitudes); a pawn's own level and its gene bonus stay apart.
    const PawnGenes* genes = reg.valid(pawn) ? reg.try_get<PawnGenes>(pawn) : nullptr;
    return genes != nullptr ? std::max(0, level + geneAptitude(*genes, skill)) : level;
}

double JobSystem::yieldFactor(int level) const {
    if (gather_.yieldByLevel.empty()) {
        return 1.0;
    }
    const size_t i = static_cast<size_t>(std::clamp(level, 0, static_cast<int>(gather_.yieldByLevel.size()) - 1));
    return gather_.yieldByLevel[i];
}

bool JobSystem::canUseHands(Entity pawn) const {
    return gatherManipulation(things_.registry(), pawn, manipulationIndex_) > 0.0;
}

float JobSystem::lightWorkFactor(Entity pawn) const {
    const Position* at = lights_ != nullptr ? things_.registry().try_get<Position>(pawn) : nullptr;
    return at != nullptr ? lights_->workSpeedAt(cellOf(*at)) : 1.0f;
}

double JobSystem::miningSpeed(Entity pawn) const {
    const double speed = gather_.miningSpeedBase + gather_.miningSpeedPerLevel * skillLevel(pawn, "Mining");
    const PawnGenes* genes = things_.registry().valid(pawn) ? things_.registry().try_get<PawnGenes>(pawn) : nullptr;
    return std::max(0.0, speed) * gatherManipulation(things_.registry(), pawn, manipulationIndex_) * lightWorkFactor(pawn) *
           (genes != nullptr ? genes->effects.miningSpeed : 1.0);
}

double JobSystem::plantWorkSpeed(Entity pawn) const {
    const double speed = gather_.plantSpeedBase + gather_.plantSpeedPerLevel * skillLevel(pawn, "Plants");
    const double manip = gatherManipulation(things_.registry(), pawn, manipulationIndex_);
    const PawnGenes* genes = things_.registry().valid(pawn) ? things_.registry().try_get<PawnGenes>(pawn) : nullptr;
    return manip > 0.0 ? std::max(kGatherMinPlantSpeed, speed * manip) * lightWorkFactor(pawn) *
                             (genes != nullptr ? genes->effects.plantWorkSpeed : 1.0)
                       : 0.0;
}

// Experience, as RimWorld's Learn: scaled by the pawn's passion for the skill; levels rise when the
// experience passes what the level needs. (RimWorld's daily cap and the decay of high skills are not
// in yet.)
void JobSystem::learnSkill(Entity pawn, const char* skill, double xp) {
    entt::registry& reg = things_.registry();
    Colonist* colonist = reg.try_get<Colonist>(pawn);
    if (colonist == nullptr) {
        return;
    }
    for (SkillRecord& record : colonist->skills) {
        if (record.def != skill) {
            continue;
        }
        const int maxLevel = static_cast<int>(gather_.xpToNextLevel.size()) - 1;
        if (record.disabled || maxLevel < 1 || record.level >= maxLevel) {
            return;
        }
        record.xp += static_cast<float>(xp * gather_.passionLearn[std::min(2, static_cast<int>(record.passion))]);
        while (record.level < maxLevel && record.xp >= static_cast<float>(gather_.xpToNextLevel[static_cast<size_t>(record.level)])) {
            record.xp -= static_cast<float>(gather_.xpToNextLevel[static_cast<size_t>(record.level)]);
            ++record.level;
        }
        return;
    }
}

// ---- Plants and reach -------------------------------------------------------------------------

bool JobSystem::harvestableNow(CellPos cell) const {
    if (!plants_.has(cell)) {
        return false;
    }
    const Def& def = defs_.get(plants_.defAt(cell));
    return def.has("plant.harvestedThingDef") && plants_.growth(cell) >= static_cast<float>(def.number("plant.harvestMinGrowth", 0.65));
}

// What a pawn gets from the plant in the cell now: the def's `harvestYield`, scaled by how grown it is
// (a plant at its harvest threshold gives half, a fully grown one all) and by the pawn's Plants level
// (60% at 0, 100% at 8), rounded to the nearest whole item. Below the threshold it gives nothing: a
// sapling felled is just gone.
int JobSystem::plantYield(Entity pawn, CellPos cell) const {
    if (!harvestableNow(cell)) {
        return 0;
    }
    const Def& def = defs_.get(plants_.defAt(cell));
    const double minGrowth = def.number("plant.harvestMinGrowth", 0.65);
    const double along = minGrowth >= 1.0 ? 1.0 : (static_cast<double>(plants_.growth(cell)) - minGrowth) / (1.0 - minGrowth);
    const double byGrowth = gather_.growthYieldFloor + (1.0 - gather_.growthYieldFloor) * std::clamp(along, 0.0, 1.0);
    const double amount = def.number("plant.harvestYield", 0.0) * byGrowth * yieldFactor(skillLevel(pawn, "Plants"));
    return std::max(0, static_cast<int>(std::floor(amount + 0.5)));
}

bool JobSystem::touchCell(CellPos from, CellPos target, CellPos& out) const {
    const CellPos around[4] = {{target.x, target.y - 1, target.level},
                               {target.x + 1, target.y, target.level},
                               {target.x, target.y + 1, target.level},
                               {target.x - 1, target.y, target.level}};
    bool found = false;
    float best = 0.0f;
    for (const CellPos& n : around) {
        if (!map_.walkable(n) || !canReach(from, n)) {
            continue;
        }
        const float d = gatherDistSq(from, n);
        if (!found || d < best) {
            found = true;
            best = d;
            out = n;
        }
    }
    if (found) {
        return true;
    }
    if (map_.walkable(target) && canReach(from, target)) {
        out = target;
        return true;
    }
    return false;
}

void JobSystem::dropItems(DefId def, int count, CellPos cell) {
    const int limit = stackLimit(def);
    while (count > 0) {
        const int n = std::min(count, limit);
        if (things_.spawn(def, cell, n) == kNoEntity) {
            return;
        }
        count -= n;
    }
}

void JobSystem::gatherEvent(Entity pawn, PawnAction action, CellPos cell) {
    Event event;
    event.type = EventType::PawnAction;
    event.thing = pawn;
    event.def = things_.registry().get<Thing>(pawn).def;
    event.action = action;
    event.cell = cell;
    events_.push_back(std::move(event));
}

void JobSystem::cancelGatherAt(CellPos cell) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> pawns;
    for (const auto [pawn, worker] : reg.view<const Worker>().each()) {
        if (jobIsGather(worker.job.type) && worker.job.cell == cell) {
            pawns.push_back(pawn);
        }
    }
    std::sort(pawns.begin(), pawns.end());
    for (const Entity pawn : pawns) {
        endJob(pawn, JobEnd::Interrupted, "the order was cancelled");
    }
}

// ---- Fail checks and steps --------------------------------------------------------------------

bool JobSystem::failCheckGather(Entity pawn, Job& job, std::string& why) {
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    switch (job.type) {
        case JobType::Mine:
            if (designations_.at(job.cell) != DesignationKind::Mine) {
                why = "the order was cancelled";
                return false;
            }
            if (!mineables_.has(job.cell)) {
                why = "the rock is gone";
                return false;
            }
            return true;
        case JobType::CutPlant:
            if (designations_.at(job.cell) != DesignationKind::CutPlant) {
                why = "the order was cancelled";
                return false;
            }
            break;
        case JobType::SowCrop: {
            const std::string blocker = farming_ != nullptr ? farming_->sowBlocker(job.cell) : std::string("no farming");
            if (!blocker.empty()) {
                why = blocker;
                return false;
            }
            return true;
        }
        default:
            // A mark, or a crop in a growing zone that is fully grown.
            if (designations_.at(job.cell) != DesignationKind::HarvestPlant && !(farming_ != nullptr && farming_->readyToHarvest(job.cell))) {
                why = "the order was cancelled";
                return false;
            }
            break;
    }
    if (!plants_.has(job.cell)) {
        why = "the plant is gone";
        return false;
    }
    if (job.type == JobType::HarvestPlant && !harvestableNow(job.cell)) {
        why = "the plant isn't ready";
        return false;
    }
    return true;
}

// Walks to the cell the pawn works from. Already standing next to the target at the start: no walk.
JobSystem::StepResult JobSystem::goToTouch(Entity pawn, Job& job, std::string& why) {
    if (job.handsOn != 0) {
        return StepResult::Next;  // the player is already within reach (checked every step)
    }
    const CellPos from = cellOf(things_.registry().get<Position>(pawn));
    CellPos stand;
    if (!touchCell(from, job.cell, stand)) {
        why = "it found no way there";
        return StepResult::Failed;
    }
    if (job.walk == 0 && from == stand) {
        return StepResult::Next;
    }
    return walkTo(pawn, job, stand, why);
}

// RimWorld's mining toil: the pick falls every 100 ticks at 100% speed (144 game seconds), 80 damage
// a hit, until the rock's hit points are gone. The damage stays on the rock if the pawn leaves.
JobSystem::StepResult JobSystem::mineRock(Entity pawn, Job& job, std::string& why) {
    const double speed = miningSpeed(pawn);
    if (!(speed > 0.0)) {
        why = "it can't use its hands";
        return StepResult::Failed;
    }
    job.progress += static_cast<float>(speed);
    learnSkill(pawn, "Mining", gather_.miningXpPerSecond);
    if (static_cast<double>(job.progress) < gather_.hitSeconds) {
        return StepResult::Running;
    }
    job.progress -= static_cast<float>(gather_.hitSeconds);
    gatherEvent(pawn, PawnAction::Mine, job.cell);

    const DefId rock = mineables_.defAt(job.cell);
    const int damage = static_cast<int>(gather_.hitDamage);
    const MineDrop drop = mineables_.hitPoints(job.cell) <= damage ? mineables_.dropOf(job.cell) : MineDrop{};
    if (!mineables_.hit(job.cell, damage)) {
        return StepResult::Running;
    }

    designations_.set(job.cell, DesignationKind::None);
    Event mined;
    mined.type = EventType::CellMined;
    mined.cell = job.cell;
    mined.def = rock;
    mined.amount = static_cast<float>(mineables_.maxHitPoints(rock));
    events_.push_back(std::move(mined));
    Event changed;
    changed.type = EventType::DesignationsChanged;
    events_.push_back(std::move(changed));
    if (drop.def.valid()) {
        // The miner's yield rounds to the nearest whole item, and a hit always leaves something.
        const double amount = static_cast<double>(drop.count) * yieldFactor(skillLevel(pawn, "Mining"));
        dropItems(drop.def, std::max(1, static_cast<int>(std::floor(amount + 0.5))), job.cell);
    }
    return StepResult::Next;
}

// RimWorld's plant work toil: work piles up at the pawn's plant work speed until it reaches the
// plant's `harvestWork`. Cutting a tree gives its wood; cutting anything else gives nothing. Picking
// from a plant that regrows (a berry bush) sets it back to `harvestAfterGrowth`; picking from one
// that doesn't (a tree) takes it down.
JobSystem::StepResult JobSystem::plantWork(Entity pawn, Job& job, std::string& why) {
    const double speed = plantWorkSpeed(pawn);
    if (!(speed > 0.0)) {
        why = "it can't use its hands";
        return StepResult::Failed;
    }
    if (job.type == JobType::SowCrop) {
        return sowWork(pawn, job, why);
    }
    const bool harvest = job.type == JobType::HarvestPlant;
    if (job.walk == 0) {
        job.walk = 2;  // work steps don't walk: this marks that the animation event went out
        gatherEvent(pawn, harvest ? PawnAction::Harvest : PawnAction::Cut, job.cell);
    }
    job.progress += static_cast<float>(speed);
    learnSkill(pawn, "Plants", gather_.plantXpPerSecond);
    const DefId plant = plants_.defAt(job.cell);
    const Def& def = defs_.get(plant);
    if (static_cast<double>(job.progress) < def.number("plant.harvestWork", 1.0)) {
        return StepResult::Running;
    }

    const bool tree = def.flag("plant.isTree");
    const int yield = harvest || tree ? plantYield(pawn, job.cell) : 0;
    const DefId item = yield > 0 ? defs_.idOf("Things", def.text("plant.harvestedThingDef")) : DefId{};
    const bool regrows = harvest && def.has("plant.harvestAfterGrowth");
    Event event;
    event.cell = job.cell;
    event.def = plant;
    event.amount = static_cast<float>(item.valid() ? yield : 0);
    if (regrows) {
        plants_.setGrowth(job.cell, static_cast<float>(def.number("plant.harvestAfterGrowth", 0.0)));
        event.type = EventType::PlantHarvested;
        event.message = "picked";
    } else {
        plants_.remove(job.cell);
        event.type = EventType::PlantRemoved;
        event.message = tree ? "felled" : (harvest ? "harvested" : "cut");
        if (harvest && farming_ != nullptr && farming_->cropAt(job.cell).valid()) {
            farming_->noteHarvested();
        }
    }
    events_.push_back(std::move(event));
    designations_.set(job.cell, DesignationKind::None);
    Event changed;
    changed.type = EventType::DesignationsChanged;
    events_.push_back(std::move(changed));
    if (item.valid()) {
        dropItems(item, yield, job.cell);
    }
    return StepResult::Next;
}

// RimWorld's sowing toil: work piles up at the pawn's plant work speed until it reaches the crop's
// `sowWork`; the seedling goes in at growth 0, replacing a wild plant that was in the way. Needs the
// crop's `sowMinSkill` (checked by the scanner).
JobSystem::StepResult JobSystem::sowWork(Entity pawn, Job& job, std::string& why) {
    const double speed = plantWorkSpeed(pawn);
    if (!(speed > 0.0)) {
        why = "it can't use its hands";
        return StepResult::Failed;
    }
    const DefId crop = farming_->cropAt(job.cell);
    const Def& def = defs_.get(crop);
    if (job.walk == 0) {
        job.walk = 2;
        gatherEvent(pawn, PawnAction::Harvest, job.cell);
    }
    job.progress += static_cast<float>(speed);
    learnSkill(pawn, "Plants", gather_.plantXpPerSecond);
    if (static_cast<double>(job.progress) < def.number("plant.sowWork", 1.0)) {
        return StepResult::Running;
    }
    if (plants_.has(job.cell)) {
        Event removed;
        removed.type = EventType::PlantRemoved;
        removed.cell = job.cell;
        removed.def = plants_.defAt(job.cell);
        removed.message = "cut";
        plants_.remove(job.cell);
        events_.push_back(std::move(removed));
    }
    if (!plants_.place(job.cell, crop, 0.0f)) {
        why = "it can't be sown there";
        return StepResult::Failed;
    }
    farming_->noteSown();
    Event sown;
    sown.type = EventType::PlantSown;
    sown.cell = job.cell;
    sown.def = crop;
    events_.push_back(std::move(sown));
    return StepResult::Next;
}

}  // namespace colony
