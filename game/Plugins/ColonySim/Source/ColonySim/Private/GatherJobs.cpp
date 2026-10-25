// Mining, felling and picking (task C3, docs/design/mining.md): the jobs on designated rock and
// plants, and the skill maths behind them.
#include "ColonySim/Jobs.h"

#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Regions.h"
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
        gather_.constructionXpPerSecond = rules->number("construction.xpPerSecond", gather_.constructionXpPerSecond);
        gather_.growthYieldFloor = rules->number("growthYieldFloor", gather_.growthYieldFloor);
        gather_.unskilledLevel = static_cast<int>(rules->integer("unskilledSkillLevel", gather_.unskilledLevel));
        if (rules->count("yieldByLevel") > 0) {
            gather_.yieldByLevel.clear();
            for (size_t i = 0; i < rules->count("yieldByLevel"); ++i) {
                gather_.yieldByLevel.push_back(rules->numberAt("yieldByLevel", i, 1.0));
            }
        }
    }
    skillRules_ = SkillRules::read(defs_);
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
    return record != nullptr ? record->level : gather_.unskilledLevel;
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

double JobSystem::miningSpeed(Entity pawn) const {
    const double speed = gather_.miningSpeedBase + gather_.miningSpeedPerLevel * skillLevel(pawn, "Mining");
    return std::max(0.0, speed) * gatherManipulation(things_.registry(), pawn, manipulationIndex_);
}

double JobSystem::plantWorkSpeed(Entity pawn) const {
    const double speed = gather_.plantSpeedBase + gather_.plantSpeedPerLevel * skillLevel(pawn, "Plants");
    const double manip = gatherManipulation(things_.registry(), pawn, manipulationIndex_);
    return manip > 0.0 ? std::max(kGatherMinPlantSpeed, speed * manip) : 0.0;
}

// Experience, as RimWorld's Learn (Skills.cpp has the maths: passion, the daily cap, levels).
void JobSystem::learnSkill(Entity pawn, const char* skill, double xp) {
    Colonist* colonist = things_.registry().try_get<Colonist>(pawn);
    if (colonist == nullptr) {
        return;
    }
    for (SkillRecord& record : colonist->skills) {
        if (record.def == skill) {
            learnSkillXp(skillRules_, record, xp);
            return;
        }
    }
}

// Midnight: every colonist's skills start a new day (the cap resets, high skills decay).
void JobSystem::skillsNewDay() {
    for (const Entity pawn : things_.registry().view<Colonist>()) {
        for (SkillRecord& record : things_.registry().get<Colonist>(pawn).skills) {
            skillDayPassed(skillRules_, record);
        }
    }
}

std::string JobSystem::activeSkill(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Worker* worker = reg.valid(pawn) ? reg.try_get<Worker>(pawn) : nullptr;
    if (worker == nullptr) {
        return std::string();
    }
    const Job& job = worker->job;
    switch (job.type) {
        case JobType::Mine:
            return "Mining";
        case JobType::CutPlant:
        case JobType::HarvestPlant:
            return "Plants";
        case JobType::BuildFrame:
        case JobType::Deconstruct:
            return "Construction";
        case JobType::DoBill: {
            const Bill* bill = bills_->find(job.site, job.bill);
            const Recipe* recipe = bill != nullptr ? bills_->recipe(bill->recipe) : nullptr;
            return recipe != nullptr ? recipe->skill : std::string();
        }
        default:
            return std::string();
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
        default:
            if (designations_.at(job.cell) != DesignationKind::HarvestPlant) {
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
        event.message = tree ? "felled" : "cut";
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

}  // namespace colony
