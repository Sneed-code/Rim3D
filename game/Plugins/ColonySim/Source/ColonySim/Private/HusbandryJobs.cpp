// Taming, training, slaughtering and feeding animals (task N3, docs/design/husbandry.md): the Handling work type's
// scanners and the steps their jobs run.
#include "ColonySim/Animals.h"
#include "ColonySim/Breeding.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

bool husbandryJobHasFood(JobType type) {
    return type == JobType::TameAnimal || type == JobType::TrainAnimal || type == JobType::FeedAnimal;
}

// The nutrition a trip should bring: an attempt's feed, or for hand-feeding enough to fill the animal to 90%.
double husbandryWanted(const Husbandry& hu, JobType type, Entity animal) {
    const double attempt = hu.feedNutrition(animal);
    if (type != JobType::FeedAnimal) {
        return attempt;
    }
    const AnimalSystem& animals = hu.animals();
    return std::max(attempt, static_cast<double>(animals.foodCapacity(animal) * 0.9f - animals.foodLevel(animal)));
}

// The nearest item the animal eats that the pawn can fetch: lying on the map, in nobody's hands, reachable, and (for an
// attempt) in one stack big enough for the whole feed; hand-feeding takes what the stack has down to one attempt's worth.
// `count` is how many units to carry.
bool husbandryFindFood(JobSystem& jobs, Entity pawn, Entity animal, JobType type, Entity& item, int& count) {
    const Husbandry& hu = *jobs.husbandry();
    const entt::registry& reg = jobs.things().registry();
    const Position& at = reg.get<Position>(pawn);
    const double wanted = husbandryWanted(hu, type, animal);
    const double minimum = hu.feedNutrition(animal);
    const bool partial = type == JobType::FeedAnimal;
    bool found = false;
    float bestDist = 0.0f;
    reg.view<const Position, const StackCount, const Thing>().each([&](Entity e, const Position& pos, const StackCount& stack, const Thing& thing) {
        if (pos.level != at.level || !hu.animalEats(animal, thing.def)) {
            return;
        }
        const double perUnit = static_cast<double>(hu.nutritionOf(thing.def));
        if (perUnit <= 0.0) {
            return;
        }
        const int needUnits = std::max(1, static_cast<int>(std::ceil((partial ? minimum : wanted) / perUnit)));
        if (stack.count < needUnits || jobs.reservedByOther(JobTarget{e, CellPos{}}, pawn) ||
            jobs.avoiding(pawn, JobTarget{e, CellPos{}})) {
            return;
        }
        const float dx = pos.x - at.x;
        const float dy = pos.y - at.y;
        const float d = dx * dx + dy * dy;
        if (found && (d > bestDist || (d == bestDist && e > item))) {
            return;
        }
        if (!jobs.canReach(cellOf(at), cellOf(pos))) {
            return;
        }
        found = true;
        bestDist = d;
        item = e;
        const int units = static_cast<int>(std::ceil(wanted / perUnit));
        count = std::clamp(units, needUnits, std::max(needUnits, std::min(stack.count, jobs.carryLimit(pawn, thing.def))));
        count = std::min(count, stack.count);
    });
    return found;
}

bool husbandryAlive(const entt::registry& reg, Entity animal) {
    const Health* h = reg.try_get<Health>(animal);
    return h == nullptr || !h->dead;
}

// Shared by the four scanners: the animals that need the work, nearest first.
template <typename Wants>
void husbandryScan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out, Wants wants) {
    if (jobs.husbandry() == nullptr) {
        return;
    }
    const entt::registry& reg = jobs.things().registry();
    const Position& at = reg.get<Position>(pawn);
    reg.view<const Animal, const Position>().each([&](Entity animal, const Animal&, const Position& pos) {
        if (pos.level != at.level || !husbandryAlive(reg, animal) || !wants(animal)) {
            return;
        }
        const float dx = pos.x - at.x;
        const float dy = pos.y - at.y;
        out.push_back(WorkCandidate{JobTarget{animal, CellPos{}}, dx * dx + dy * dy, 0, 0, false, CellPos{}});
    });
}

bool husbandryMakeFoodJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, JobType type, uint32_t trainable, Job& out) {
    const entt::registry& reg = jobs.things().registry();
    const Entity animal = candidate.target.thing;
    Entity item = kNoEntity;
    int count = 0;
    if (!reg.valid(animal) || !husbandryFindFood(jobs, pawn, animal, type, item, count)) {
        return false;
    }
    out = Job{};
    out.type = type;
    out.thing = item;
    out.count = count;
    out.site = animal;
    out.bill = trainable;
    out.cell = cellOf(reg.get<Position>(animal));
    return true;
}

class TameGiver final : public WorkGiver {
public:
    const char* task() const override { return "Tame"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const entt::registry& reg = jobs.things().registry();
        husbandryScan(jobs, pawn, out, [&](Entity animal) {
            const AnimalCare* care = reg.try_get<AnimalCare>(animal);
            return care != nullptr && care->markTame && reg.get<Animal>(animal).wild;
        });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        return husbandryMakeFoodJob(jobs, pawn, candidate, JobType::TameAnimal, 0, out);
    }
};

class TrainGiver final : public WorkGiver {
public:
    const char* task() const override { return "Train"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const Husbandry* hu = jobs.husbandry();
        husbandryScan(jobs, pawn, out, [&](Entity animal) {
            Trainable what = Trainable::Obedience;
            return hu->nextToTrain(animal, what) && hu->trainingDue(animal);
        });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        Trainable what = Trainable::Obedience;
        if (!jobs.husbandry()->nextToTrain(candidate.target.thing, what)) {
            return false;
        }
        return husbandryMakeFoodJob(jobs, pawn, candidate, JobType::TrainAnimal, static_cast<uint32_t>(what), out);
    }
};

class SlaughterGiver final : public WorkGiver {
public:
    const char* task() const override { return "Slaughter"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Husbandry* hu = jobs.husbandry();
        husbandryScan(jobs, pawn, out, [&](Entity animal) {
            const AnimalCare* care = reg.try_get<AnimalCare>(animal);
            return care != nullptr && care->markSlaughter && hu->isTame(animal);
        });
    }

    bool makeJob(JobSystem& jobs, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::SlaughterAnimal;
        out.site = candidate.target.thing;
        out.cell = cellOf(jobs.things().registry().get<Position>(candidate.target.thing));
        return true;
    }
};

// A tame animal that is hungry, and has not found food for itself, is fed by hand.
class FeedAnimalsGiver final : public WorkGiver {
public:
    const char* task() const override { return "FeedAnimals"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const Husbandry* hu = jobs.husbandry();
        const entt::registry& reg = jobs.things().registry();
        husbandryScan(jobs, pawn, out, [&](Entity animal) {
            const Animal& a = reg.get<Animal>(animal);
            return !a.wild && a.started && hu->animals().foodShare(animal) < static_cast<float>(hu->rules().handFeedBelow);
        });
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        return husbandryMakeFoodJob(jobs, pawn, candidate, JobType::FeedAnimal, 0, out);
    }
};

// Milking and shearing: a grown tame animal whose milk or wool is full (docs/design/breeding.md).
class GatherGiver final : public WorkGiver {
public:
    GatherGiver(AnimalProduct what, const char* task) : what_(what), task_(task) {}
    const char* task() const override { return task_; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const Breeding* breeding = jobs.breeding();
        if (breeding == nullptr) {
            return;
        }
        husbandryScan(jobs, pawn, out, [&](Entity animal) { return breeding->readyToGather(animal, what_); });
    }

    bool makeJob(JobSystem& jobs, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::GatherAnimal;
        out.site = candidate.target.thing;
        out.bill = static_cast<uint32_t>(what_);
        out.cell = cellOf(jobs.things().registry().get<Position>(candidate.target.thing));
        return true;
    }

private:
    AnimalProduct what_;
    const char* task_;
};

}  // namespace

void addHusbandryWorkGivers(std::vector<std::unique_ptr<WorkGiver>>& givers) {
    givers.push_back(std::make_unique<TameGiver>());
    givers.push_back(std::make_unique<TrainGiver>());
    givers.push_back(std::make_unique<SlaughterGiver>());
    givers.push_back(std::make_unique<FeedAnimalsGiver>());
    givers.push_back(std::make_unique<GatherGiver>(AnimalProduct::Milk, "Milk"));
    givers.push_back(std::make_unique<GatherGiver>(AnimalProduct::Wool, "Shear"));
}

// ---- The job's steps ------------------------------------------------------------------------

bool JobSystem::failCheckHandle(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (husbandry_ == nullptr) {
        why = "there is no animal care";
        return false;
    }
    const Entity animal = job.site;
    if (!reg.valid(animal) || !reg.all_of<Animal, Position>(animal)) {
        why = "the animal is gone";
        return false;
    }
    if (!husbandryAlive(reg, animal)) {
        why = "the animal is dead";
        return false;
    }
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    const AnimalCare* care = husbandry_->care(animal);
    const bool tame = husbandry_->isTame(animal);
    switch (job.type) {
        case JobType::TameAnimal:
            if (tame || care == nullptr || !care->markTame) {
                why = "the animal is no longer marked for taming";
                return false;
            }
            break;
        case JobType::TrainAnimal: {
            Trainable what = Trainable::Obedience;
            if (!husbandry_->nextToTrain(animal, what) || static_cast<uint32_t>(what) != job.bill) {
                why = "the training was called off";
                return false;
            }
            break;
        }
        case JobType::SlaughterAnimal:
            if (!tame || care == nullptr || !care->markSlaughter) {
                why = "the animal is no longer marked for slaughter";
                return false;
            }
            break;
        case JobType::GatherAnimal:
            if (breeding_ == nullptr || !breeding_->readyToGather(animal, static_cast<AnimalProduct>(job.bill))) {
                why = "the animal has nothing to give now";
                return false;
            }
            break;
        default:  // FeedAnimal
            if (!tame || husbandry_->animals().foodShare(animal) >= 2.0f * static_cast<float>(husbandry_->rules().handFeedBelow)) {
                why = "the animal doesn't need feeding";
                return false;
            }
            break;
    }
    if (husbandryJobHasFood(job.type)) {
        if (job.step <= kHandlePickUpStep) {
            if (!things_.spawned(job.thing)) {
                why = "the food is gone";
                return false;
            }
        } else {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
                why = "it lost the food it carried";
                return false;
            }
        }
    }
    return true;
}

// Walks up to the animal, wherever it has got to. Within the hold radius the animal is kept still, so the walk ends.
JobSystem::StepResult JobSystem::approachAnimal(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const HusbandryRules& rules = husbandry_->rules();
    const Entity animal = job.site;
    const Position& pp = reg.get<Position>(pawn);
    const Position& ap = reg.get<Position>(animal);
    const double dx = static_cast<double>(pp.x - ap.x);
    const double dy = static_cast<double>(pp.y - ap.y);
    const double d2 = dx * dx + dy * dy;
    const bool sameLevel = pp.level == ap.level;
    if (sameLevel && d2 <= rules.holdRadius * rules.holdRadius) {
        husbandry_->hold(animal, now() + 3);
    }
    if (sameLevel && d2 <= rules.handReach * rules.handReach) {
        if (job.walk == 1) {
            pathfinder_.cancel(job.path);
        }
        Mover& mover = reg.get<Mover>(pawn);
        mover.path.clear();
        mover.next = 0;
        job.walk = 0;
        return StepResult::Next;
    }
    const CellPos target = cellOf(ap);
    if (job.walk != 0 && (std::abs(target.x - job.cell.x) > 2 || std::abs(target.y - job.cell.y) > 2)) {
        // It moved while the pawn walked: a new walk to where it is now.
        if (job.walk == 1) {
            pathfinder_.cancel(job.path);
        }
        job.walk = 0;
    }
    if (job.walk == 0) {
        job.cell = target;
    }
    const StepResult result = walkTo(pawn, job, job.cell, why);
    if (result == StepResult::Next) {
        job.walk = 0;  // arrived, but the animal isn't within reach: look again
        return StepResult::Running;
    }
    return result;
}

JobSystem::StepResult JobSystem::handleWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const HusbandryRules& rules = husbandry_->rules();
    const Entity animal = job.site;
    if (!reg.all_of<Position>(animal)) {
        why = "the animal is gone";
        return StepResult::Failed;
    }
    const Position& pp = reg.get<Position>(pawn);
    const Position& ap = reg.get<Position>(animal);
    const double dx = static_cast<double>(pp.x - ap.x);
    const double dy = static_cast<double>(pp.y - ap.y);
    if (pp.level != ap.level || dx * dx + dy * dy > 4.0 * rules.handReach * rules.handReach) {
        // It got away from the pawn: walk up to it again, keeping the work done.
        job.step = static_cast<uint8_t>(jobStepCount(job.type) - 2);
        job.walk = 0;
        return StepResult::Running;
    }
    husbandry_->hold(animal, now() + 3);

    double total = rules.interactSeconds;
    double xp = rules.tameXp;
    if (job.type == JobType::GatherAnimal) {
        total = breeding_->gatherSeconds(static_cast<AnimalProduct>(job.bill));
        xp = breeding_->rules().gatherXp;
    } else if (job.type == JobType::SlaughterAnimal) {
        total = rules.slaughterSeconds;
        xp = rules.slaughterXp;
    } else if (job.type == JobType::FeedAnimal) {
        total = rules.feedSeconds;
        xp = rules.feedXp;
    } else if (job.type == JobType::TrainAnimal) {
        xp = rules.trainXp;
    }
    if (job.progress == 0.0f) {
        pawnDid(pawn, job.type == JobType::SlaughterAnimal ? PawnAction::Slaughter : PawnAction::Handle, animal);
    }
    job.progress += 1.0f;
    learnSkill(pawn, "Animals", xp / total);
    if (static_cast<double>(job.progress) < total) {
        return StepResult::Running;
    }

    const int skill = skillLevel(pawn, "Animals");
    if (husbandryJobHasFood(job.type)) {
        // The animal eats what the pawn brought.
        const Entity food = job.thing;
        const float nutrition = husbandry_->nutritionOf(reg.get<Thing>(food).def) * static_cast<float>(reg.get<StackCount>(food).count);
        reg.remove<Carrying>(pawn);
        things_.destroy(food);
        husbandry_->feed(animal, nutrition);
    }
    switch (job.type) {
        case JobType::TameAnimal: husbandry_->finishTame(pawn, animal, skill); break;
        case JobType::TrainAnimal: husbandry_->finishTrain(pawn, animal, static_cast<Trainable>(job.bill), skill); break;
        case JobType::SlaughterAnimal: husbandry_->finishSlaughter(pawn, animal); break;
        case JobType::GatherAnimal: breeding_->finishGather(pawn, animal, static_cast<AnimalProduct>(job.bill)); break;
        default: break;
    }
    return StepResult::Next;
}

// ---- Animals as workers ---------------------------------------------------------------------

void JobSystem::setAnimalHauler(Entity animal, bool on) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(animal) || !reg.all_of<Animal, Mover>(animal)) {
        return;
    }
    if (on) {
        // A hauler (Haul) takes the Hauling work type, a rescuer (Rescue) the Doctor one, which for an animal is only
        // carrying the downed to a bed.
        if (!reg.all_of<Worker>(animal)) {
            addWorker(animal, {}, false);
        }
        const bool haul = husbandry_ != nullptr && husbandry_->learned(animal, Trainable::Haul);
        const bool rescue = husbandry_ != nullptr && husbandry_->learned(animal, Trainable::Rescue);
        WorkSettings& settings = reg.get<WorkSettings>(animal);
        for (size_t w = 0; w < settings.entries.size() && w < workTypes_.size(); ++w) {
            const std::string& work = workTypes_[w].defName;
            const bool takes = (haul && work == "Hauling") || (rescue && work == "Doctor");
            settings.entries[w].priority = takes ? static_cast<uint8_t>(kDefaultPriority) : uint8_t{0};
        }
    } else if (reg.all_of<Worker>(animal)) {
        if (reg.get<Worker>(animal).job.type != JobType::None && reg.all_of<Position>(animal)) {
            endJob(animal, JobEnd::Interrupted, "it is no longer a hauler");
        } else {
            dropCarried(animal);
        }
        reg.remove<Worker>(animal);
        reg.remove<WorkSettings>(animal);
    }
}

}  // namespace colony
