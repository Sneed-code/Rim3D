// Hunting and butchering (task N2, docs/design/hunting.md): the Hunting work type's scanner and the Hunt job, the Hauling work
// type's corpse scanner and the HaulCorpse job, and the Butcher job a butcher table's bill makes.
#include "ColonySim/Animals.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// How close a pawn must be to lift a corpse (the same as for lifting a downed colonist).
constexpr float kHuntLiftReach = 1.8f;

// The range of the gun in the pawn's hands, in metres; 0 for a pawn with no gun (a melee weapon is no hunting weapon).
float huntGunRange(JobSystem& jobs, Entity pawn) {
    CombatSystem* combat = jobs.combat();
    const entt::registry& reg = jobs.things().registry();
    const Gear* gear = reg.try_get<Gear>(pawn);
    if (combat == nullptr || gear == nullptr || gear->weapon == kNoEntity || !reg.valid(gear->weapon)) {
        return 0.0f;
    }
    GunStats stats;
    return combat->gunStats(reg.get<Thing>(gear->weapon).def, stats) ? stats.range : 0.0f;
}

bool huntDead(const entt::registry& reg, Entity e) {
    const Health* h = reg.try_get<Health>(e);
    return h != nullptr && h->dead;
}

float huntDistSq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Hunting (RimWorld's WorkGiver_HunterHunt): a colonist with a gun in its hands goes after a wild animal marked for the hunt.
class HuntGiver final : public WorkGiver {
public:
    const char* task() const override { return "HuntDesignated"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        if (jobs.husbandry() == nullptr || !jobs.canUseHands(pawn) || huntGunRange(jobs, pawn) <= 0.0f) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        const double avoidChance = jobs.husbandry()->rules().huntAvoidManhunterChance;
        reg.view<const Animal, const AnimalCare, const Position>().each(
            [&](Entity animal, const Animal& a, const AnimalCare& care, const Position& pos) {
                if (!care.markHunt || !a.wild || pos.level != at.level || huntDead(reg, animal)) {
                    return;
                }
                // A bear or a thrumbo that turns on its hunter is no animal to send a colonist after.
                if (static_cast<double>(jobs.husbandry()->animals().manhunterChanceOf(animal)) >= avoidChance) {
                    return;
                }
                out.push_back(WorkCandidate{JobTarget{animal, CellPos{}}, huntDistSq(at, pos), 0, 0, false, CellPos{}});
            });
    }

    bool makeJob(JobSystem& jobs, Entity, const WorkCandidate& candidate, Job& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Entity animal = candidate.target.thing;
        if (!reg.valid(animal) || huntDead(reg, animal)) {
            return false;
        }
        out = Job{};
        out.type = JobType::Hunt;
        out.site = animal;
        out.cell = cellOf(reg.get<Position>(animal));
        return true;
    }
};

// Carrying a hunted or slaughtered animal's corpse to a stockpile (the corpses of RimWorld's WorkGiver_HaulGeneral).
class HaulCorpsesGiver final : public WorkGiver {
public:
    const char* task() const override { return "HaulCorpses"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const Husbandry* hu = jobs.husbandry();
        const entt::registry& reg = jobs.things().registry();
        if (hu == nullptr || jobs.zones().stockpiles().empty() || reg.all_of<Animal>(pawn) || !jobs.canUseHands(pawn)) {
            return;
        }
        const Position& at = reg.get<Position>(pawn);
        reg.view<const Animal, const Position>().each([&](Entity animal, const Animal&, const Position& pos) {
            if (pos.level != at.level || !hu->wantsHauling(animal) || jobs.zones().stockpileAt(cellOf(pos)) != nullptr) {
                return;
            }
            out.push_back(WorkCandidate{JobTarget{animal, CellPos{}}, huntDistSq(at, pos), 0, 0, false, CellPos{}});
        });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Entity corpse = candidate.target.thing;
        if (!jobs.husbandry()->wantsHauling(corpse)) {
            return false;
        }
        const Position& there = reg.get<Position>(corpse);
        const CellPos from = cellOf(there);
        const DefId def = reg.get<Thing>(corpse).def;
        // Cells where another corpse lies already.
        std::vector<CellPos> taken;
        reg.view<const Animal, const Position>().each([&](Entity other, const Animal&, const Position& pos) {
            if (other != corpse && huntDead(reg, other)) {
                taken.push_back(cellOf(pos));
            }
        });
        bool found = false;
        float bestDist = 0.0f;
        CellPos best;
        for (const StockpileZone& zone : jobs.zones().stockpiles()) {
            if (!zone.allows(def)) {
                continue;
            }
            for (const CellPos& cell : zone.cells) {
                const float dx = static_cast<float>(cell.x - from.x);
                const float dy = static_cast<float>(cell.y - from.y);
                const float dist = dx * dx + dy * dy;
                if ((found && dist >= bestDist) || cell.level != there.level || !jobs.map().walkable(cell) ||
                    !jobs.things().itemsAt(cell).empty() || std::find(taken.begin(), taken.end(), cell) != taken.end() ||
                    jobs.reservedByOther(JobTarget{kNoEntity, cell}, pawn) || !jobs.canReach(cellOf(reg.get<Position>(pawn)), from) ||
                    !jobs.canReach(from, cell)) {
                    continue;
                }
                found = true;
                bestDist = dist;
                best = cell;
            }
        }
        if (!found) {
            return false;
        }
        out = Job{};
        out.type = JobType::HaulCorpse;
        out.thing = corpse;
        out.cell = best;
        return true;
    }
};

}  // namespace

void addHuntWorkGivers(std::vector<std::unique_ptr<WorkGiver>>& givers) {
    givers.push_back(std::make_unique<HuntGiver>());
    givers.push_back(std::make_unique<HaulCorpsesGiver>());
}

// A butcher table's bill: the corpse nearest to the table that the bill's radius allows, carried to the cell beside the table.
bool makeButcherJob(JobSystem& jobs, Entity pawn, Entity building, const Bill& bill, CellPos stand, Job& out) {
    const Husbandry* hu = jobs.husbandry();
    if (hu == nullptr) {
        return false;
    }
    const entt::registry& reg = jobs.things().registry();
    const Position& at = reg.get<Position>(pawn);
    const Position& table = reg.get<Position>(building);
    const float radiusSq = bill.radius * bill.radius;
    const CellPos from = cellOf(at);
    Entity best = kNoEntity;
    float bestDist = 0.0f;
    reg.view<const Health, const Position>().each([&](Entity corpse, const Health&, const Position& pos) {
        if (pos.level != at.level || !hu->isCorpse(corpse)) {
            return;
        }
        const float dist = huntDistSq(table, pos);
        if ((bill.radius > 0.0f && dist > radiusSq) || (best != kNoEntity && (dist > bestDist || (dist == bestDist && corpse > best)))) {
            return;
        }
        const JobTarget target{corpse, CellPos{}};
        if (jobs.reservedByOther(target, pawn) || jobs.avoiding(pawn, target) || !jobs.canReach(from, cellOf(pos))) {
            return;
        }
        best = corpse;
        bestDist = dist;
    });
    if (best == kNoEntity) {
        return false;
    }
    out = Job{};
    out.type = JobType::Butcher;
    out.site = building;
    out.cell = stand;
    out.bill = bill.id;
    out.thing = best;
    return true;
}

// ---- Hunt -----------------------------------------------------------------------------------

bool JobSystem::failCheckHunt(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    const Entity animal = job.site;
    if (husbandry_ == nullptr || combat_ == nullptr) {
        why = "there is no hunting here";
        return false;
    }
    if (!reg.valid(animal) || !reg.all_of<Animal, Position, Health>(animal)) {
        why = "the animal is gone";
        return false;
    }
    if (reg.get<Health>(animal).dead) {
        return true;  // the hunt is over: the step sees to it
    }
    const AnimalCare* care = husbandry_->care(animal);
    if (care == nullptr || !care->markHunt || !reg.get<Animal>(animal).wild) {
        why = "the animal is no longer marked for the hunt";
        return false;
    }
    if (huntGunRange(*this, pawn) <= 0.0f) {
        why = "it has no gun in its hands";
        return false;
    }
    if (now() - job.startedAt > static_cast<int64_t>(husbandry_->rules().huntMaxSeconds)) {
        why = "it could not bring the animal down";
        return false;
    }
    return true;
}

// Walks at the animal until it is within shooting range and in sight, then shoots it until it is dead. The step does not end
// before the animal is dead, so a shot that missed or a runner is followed.
JobSystem::StepResult JobSystem::huntWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const HusbandryRules& rules = husbandry_->rules();
    const Entity animal = job.site;
    Mover& mover = reg.get<Mover>(pawn);
    if (reg.get<Health>(animal).dead) {
        if (job.walk == 1) {
            pathfinder_.cancel(job.path);
        }
        mover.path.clear();
        mover.next = 0;
        job.walk = 0;
        husbandry_->finishHunt(pawn, animal);
        return StepResult::Next;
    }
    const auto stand = [&] {
        if (job.walk == 1) {
            pathfinder_.cancel(job.path);
        }
        job.walk = 0;
        mover.path.clear();
        mover.next = 0;
    };
    const Shooter* shooter = reg.try_get<Shooter>(pawn);
    if (shooter != nullptr && (shooter->phase == ShotPhase::Aiming || shooter->phase == ShotPhase::Firing)) {
        stand();
        return StepResult::Running;  // holds still until the burst is out
    }
    const Position& pp = reg.get<Position>(pawn);
    const Position& ap = reg.get<Position>(animal);
    const double range = static_cast<double>(huntGunRange(*this, pawn));
    const double shootRange = std::min(range, std::max(rules.huntMinRange, range * rules.huntRangeShare));
    const double dx = static_cast<double>(pp.x - ap.x);
    const double dy = static_cast<double>(pp.y - ap.y);
    if (pp.level == ap.level && dx * dx + dy * dy <= shootRange * shootRange &&
        map_.cover().lineOfSight(Viewpoint{pp.x, pp.y, pp.level, Stance::Standing}, Viewpoint{ap.x, ap.y, ap.level, Stance::Standing})) {
        stand();
        combat_->shoot(pawn, animal, 0.0f, 0.0f, 0.0f, false);  // refused while the gun cools down or reloads: tried again next second
        return StepResult::Running;
    }
    // Close in. The animal moves: after a few cells or seconds the walk is planned again towards where it is now.
    const CellPos target = cellOf(ap);
    if (job.walk != 0 && (std::abs(target.x - job.cell.x) > 2 || std::abs(target.y - job.cell.y) > 2 ||
                          (job.walk == 2 && now() >= job.until))) {
        if (job.walk == 1) {
            pathfinder_.cancel(job.path);
        }
        job.walk = 0;
    }
    if (job.walk == 0) {
        job.cell = target;
        job.until = now() + static_cast<int64_t>(std::max(1.0, rules.huntRepathSeconds));
    }
    const StepResult result = walkTo(pawn, job, job.cell, why);
    if (result == StepResult::Next) {
        job.walk = 0;  // arrived, but not within sight of a shot: look again
        return StepResult::Running;
    }
    return result;
}

// ---- HaulCorpse -----------------------------------------------------------------------------

bool JobSystem::failCheckHaulCorpse(Entity, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (husbandry_ == nullptr || !husbandry_->isCorpse(job.thing)) {
        why = "the corpse is gone";
        return false;
    }
    if (zones_.stockpileAt(job.cell) == nullptr) {
        why = "the stockpile is gone";
        return false;
    }
    (void)reg;
    return true;
}

JobSystem::StepResult JobSystem::dropCorpse(Entity pawn, Job& job, std::string&) {
    entt::registry& reg = things_.registry();
    Position& there = reg.get<Position>(job.thing);
    there.x = static_cast<float>(job.cell.x) + 0.5f;
    there.y = static_cast<float>(job.cell.y) + 0.5f;
    there.level = job.cell.level;
    pawnDid(pawn, PawnAction::DropCorpse, job.thing);
    return StepResult::Next;
}

// ---- Butcher --------------------------------------------------------------------------------

bool JobSystem::failCheckButcher(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (husbandry_ == nullptr || !canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (!things_.spawned(job.site) || !reg.all_of<BillList>(job.site)) {
        why = "the butcher table is gone";
        return false;
    }
    if (!buildingRuns(job.site)) {
        why = "the butcher table has no power";
        return false;
    }
    const Bill* bill = bills_->find(job.site, job.bill);
    if (bill == nullptr || bill->suspended) {
        why = "the bill is gone or suspended";
        return false;
    }
    if (!husbandry_->isCorpse(job.thing)) {
        why = "the corpse is gone";
        return false;
    }
    return true;
}

double JobSystem::butcherEfficiency(Entity pawn) const {
    const HusbandryRules& rules = husbandry_->rules();
    const double skill = rules.butcherEfficiencyBase + rules.butcherEfficiencyPerLevel * skillLevel(pawn, "Cooking");
    const double manipulation = health_.capacityValue(pawn, "Manipulation");
    const double hands = std::max(0.0, 1.0 + rules.butcherManipulationImportance * (manipulation - 1.0));
    return std::clamp(skill * hands, 0.0, rules.butcherMaxEfficiency);
}

JobSystem::StepResult JobSystem::butcherWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the butcher table";
        return StepResult::Failed;
    }
    const Bill* bill = bills_->find(job.site, job.bill);
    const Recipe* recipe = bill != nullptr ? bills_->recipe(bill->recipe) : nullptr;
    if (recipe == nullptr) {
        why = "the bill is gone";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Butcher, job.thing);
    }
    // The corpse lies on the table: it stays where the pawn stands.
    Position& there = reg.get<Position>(job.thing);
    const Position& at = reg.get<Position>(pawn);
    there.x = at.x;
    there.y = at.y;
    there.level = at.level;
    job.progress += static_cast<float>(health_.capacityValue(pawn, "Manipulation")) * workFactor(pawn);
    return job.progress < recipe->work ? StepResult::Running : StepResult::Next;
}

JobSystem::StepResult JobSystem::butcherFinish(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Bill* bill = bills_->find(job.site, job.bill);
    const Recipe* recipe = bill != nullptr ? bills_->recipe(bill->recipe) : nullptr;
    if (recipe == nullptr) {
        why = "the bill is gone";
        return StepResult::Failed;
    }
    const Entity corpse = job.thing;
    if (recipe->cremate) {
        // Ash: no meat, no leather, no butcher's memory.
        things_.destroy(corpse);
        const bool burntLast = bills_->iterationDone(job.site, job.bill);
        Event burnt;
        burnt.type = EventType::RecipeDone;
        burnt.thing = pawn;
        burnt.def = recipe->def;
        burnt.target = job.site;
        burnt.cell = job.cell;
        burnt.amount = 1.0f;
        burnt.message = burntLast ? "bill finished" : std::string();
        events_.push_back(std::move(burnt));
        return StepResult::Next;
    }
    const DefId animalDef = reg.get<Thing>(corpse).def;
    const Husbandry::ButcherYield yield = husbandry_->butcherYield(corpse, butcherEfficiency(pawn));
    // The corpse is gone for good; the meat and leather lie by the pawn, where haulers find them.
    things_.destroy(corpse);
    if (yield.meatCount > 0) {
        dropNewItems(pawn, yield.meat, yield.meatCount, nullptr, nullptr, bill->storeZone);
    }
    if (yield.leatherCount > 0) {
        dropNewItems(pawn, yield.leather, yield.leatherCount, nullptr, nullptr, bill->storeZone);
    }
    learnSkill(pawn, "Cooking", husbandry_->rules().butcherXp);
    const DefId recipeDef = recipe->def;
    const bool ended = bills_->iterationDone(job.site, job.bill);
    Event butchered;
    butchered.type = EventType::AnimalButchered;
    butchered.thing = pawn;
    butchered.def = animalDef;
    butchered.target = job.site;
    butchered.cell = job.cell;
    butchered.amount = static_cast<float>(yield.meatCount);
    butchered.part = yield.leatherCount;
    events_.push_back(std::move(butchered));
    Event done;
    done.type = EventType::RecipeDone;
    done.thing = pawn;
    done.def = recipeDef;
    done.target = job.site;
    done.cell = job.cell;
    done.amount = static_cast<float>(yield.meatCount + yield.leatherCount);
    done.message = ended ? "bill finished" : std::string();
    events_.push_back(std::move(done));
    return StepResult::Next;
}

}  // namespace colony
