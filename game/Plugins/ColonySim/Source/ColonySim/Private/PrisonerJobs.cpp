// Wardens and prisoners on the job system (task C22, docs/design/prisoners.md): the Warden work type's scanners, the capture,
// feeding and talking jobs, and a prisoner's own quiet day. Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// How close (metres) a warden must be to a prisoner to hand it food or talk to it.
constexpr float kPrisonReach = 1.8f;
constexpr uint8_t kPrisonApproachStep = 2;

float prisonDistSq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// A prisoner a warden could reach: it is on the map (not carried) and on the warden's level.
bool prisonReachable(const entt::registry& reg, Entity prisoner, const Position& at) {
    const Position* pos = reg.try_get<Position>(prisoner);
    return pos != nullptr && pos->level == at.level;
}

// CapturePrisoners (RimWorld's Warden capture): a downed enemy goes to a free prison bed, when no able enemy is close.
class CapturePrisonersGiver final : public WorkGiver {
public:
    const char* task() const override { return "CapturePrisoners"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        PrisonerSystem* prisoners = jobs.prisoners();
        if (prisoners == nullptr || !jobs.canUseHands(pawn)) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        const float safe = prisoners->rules().captureSafeRadius;
        // Enemies still on their feet, to keep clear of.
        std::vector<const Position*> armed;
        for (const auto [other, faction, pos] : reg.view<const Faction, const Position>().each()) {
            const Health* h = reg.try_get<Health>(other);
            const Animal* wild = reg.try_get<Animal>(other);
            const Fighter* f = reg.try_get<Fighter>(other);
            if (faction.id == kPlayerFaction || reg.all_of<Prisoner>(other) || (h != nullptr && (h->downed || h->dead)) ||
                (wild != nullptr && wild->temper == AnimalTemper::Calm) || (f != nullptr && f->mode == FightMode::Flee)) {
                continue;
            }
            armed.push_back(&pos);
        }
        for (const auto [patient, health, pos] : reg.view<const Health, const Position>().each()) {
            if (pos.level != at.level || !prisoners->capturable(patient)) {
                continue;
            }
            const bool danger = std::any_of(armed.begin(), armed.end(), [&](const Position* p) {
                return p->level == pos.level && prisonDistSq(*p, pos) < safe * safe;
            });
            if (!danger) {
                out.push_back(WorkCandidate{JobTarget{patient, CellPos{}}, prisonDistSq(at, pos), 0, 0, false, CellPos{}});
            }
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        PrisonerSystem* prisoners = jobs.prisoners();
        const Entity patient = candidate.target.thing;
        Entity bed = kNoEntity;
        CellPos cell;
        if (prisoners == nullptr || !prisoners->capturable(patient) || !prisoners->findPrisonBed(patient, pawn, bed, cell)) {
            return false;
        }
        out = Job{};
        out.type = JobType::CapturePrisoner;
        out.thing = patient;
        out.chair = bed;
        out.cell = cell;
        return true;
    }
};

// FeedPrisoners (WorkGiver_Warden_Feed): a hungry prisoner gets food from a warden.
class FeedPrisonersGiver final : public WorkGiver {
public:
    const char* task() const override { return "FeedPrisoners"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        PrisonerSystem* prisoners = jobs.prisoners();
        if (prisoners == nullptr) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        for (const auto [prisoner, p, needs, pos] : reg.view<const Prisoner, const Needs, const Position>().each()) {
            (void)p;
            if (pos.level == at.level && needs.food < prisoners->rules().feedBelow) {
                out.push_back(WorkCandidate{JobTarget{prisoner, CellPos{}}, prisonDistSq(at, pos), 0, 0, false, CellPos{}});
            }
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        return jobs.findFoodForPrisoner(pawn, candidate.target.thing, out);
    }
};

// ChatWithPrisoners (WorkGiver_Warden_Chat, also the release): a prisoner whose time for a talk has come.
class ChatWithPrisonersGiver final : public WorkGiver {
public:
    const char* task() const override { return "ChatWithPrisoners"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        PrisonerSystem* prisoners = jobs.prisoners();
        if (prisoners == nullptr) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        for (const auto [prisoner, p, pos] : reg.view<const Prisoner, const Position>().each()) {
            (void)p;
            if (pos.level == at.level && prisoners->chatDue(prisoner)) {
                out.push_back(WorkCandidate{JobTarget{prisoner, CellPos{}}, prisonDistSq(at, pos), 0, 0, false, CellPos{}});
            }
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const entt::registry& reg = jobs.things().registry();
        const Entity prisoner = candidate.target.thing;
        if (jobs.prisoners() == nullptr || !jobs.prisoners()->chatDue(prisoner) || prisoner == pawn) {
            return false;
        }
        out = Job{};
        out.type = JobType::WardenInteract;
        out.site = prisoner;
        out.cell = cellOf(reg.get<Position>(prisoner));
        return true;
    }
};

}  // namespace

void addPrisonerWorkGivers(std::vector<std::unique_ptr<WorkGiver>>& givers) {
    givers.push_back(std::make_unique<CapturePrisonersGiver>());
    givers.push_back(std::make_unique<FeedPrisonersGiver>());
    givers.push_back(std::make_unique<ChatWithPrisonersGiver>());
}

// ---- Food for a prisoner --------------------------------------------------------------------------

// The same choice a hungry pawn makes for itself (findFood): the tastiest kind it may eat, the nearest of that kind (here the
// nearest to the warden), as many as it takes to fill the stomach. Food only good for desperate times only when ravenous.
bool JobSystem::findFoodForPrisoner(Entity warden, Entity prisoner, Job& out) {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(prisoner) || !reg.all_of<Needs, Position>(prisoner)) {
        return false;
    }
    const Needs& needs = reg.get<Needs>(prisoner);
    const Position& at = reg.get<Position>(warden);
    const CellPos wardenCell = cellOf(at);
    Entity best = kNoEntity;
    int bestTier = -1;
    float bestDist = 0.0f;
    for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        (void)stack;
        const int tier = thing.def.value < foodTier_.size() ? foodTier_[thing.def.value] : -1;
        if (tier < 0 || pos.level != at.level || (tier == 0 && needs.food >= rules_.foodRavenousBelow)) {
            continue;
        }
        const float dist = prisonDistSq(at, pos);
        const bool better = best == kNoEntity || tier > bestTier || (tier == bestTier && (dist < bestDist || (dist == bestDist && item < best)));
        if (!better) {
            continue;
        }
        const JobTarget target{item, CellPos{}};
        if (reservedByOther(target, warden) || avoiding(warden, target) || !canReach(wardenCell, cellOf(pos))) {
            continue;
        }
        best = item;
        bestTier = tier;
        bestDist = dist;
    }
    if (best == kNoEntity) {
        return false;
    }
    const DefId def = reg.get<Thing>(best).def;
    const float nutrition = nutrition_[def.value];
    const int wanted = std::max(1, static_cast<int>(std::ceil((1.0f - needs.food) / nutrition - 0.001f)));
    out = Job{};
    out.type = JobType::FeedPrisoner;
    out.thing = best;
    out.count = std::min({reg.get<StackCount>(best).count, static_cast<int>(maxIngest_[def.value]), carryLimit(warden, def), wanted});
    out.site = prisoner;
    out.cell = cellOf(reg.get<Position>(prisoner));
    return out.count > 0;
}

// ---- The jobs' steps ------------------------------------------------------------------------------

bool JobSystem::failCheckPrisoner(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (prisoners_ == nullptr) {
        why = "there are no prisoners here";
        return false;
    }
    if (job.type == JobType::CapturePrisoner) {
        if (!prisoners_->capturable(job.thing)) {
            why = "it can't be captured now";
            return false;
        }
        if (!things_.spawned(job.chair) || !prisoners_->isPrisonBed(job.chair)) {
            why = "the prison bed is gone";
            return false;
        }
        if (bedOccupied(job.cell, job.thing)) {
            why = "someone lies in the bed";
            return false;
        }
        return true;
    }
    if (!prisoners_->isPrisoner(job.site) || !things_.spawned(job.site)) {
        why = "the prisoner is gone";
        return false;
    }
    if (job.type == JobType::WardenInteract) {
        if (!prisoners_->chatDue(job.site)) {
            why = "the prisoner doesn't want to talk now";
            return false;
        }
        return true;
    }
    // FeedPrisoner
    if (!reg.all_of<Needs>(job.site)) {
        why = "the prisoner needs no food";
        return false;
    }
    if (job.step <= kHandlePickUpStep) {
        if (!things_.spawned(job.thing)) {
            why = "the food is gone";
            return false;
        }
    } else {
        const Carrying* carrying = reg.try_get<Carrying>(pawn);
        if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
            why = "it lost the food";
            return false;
        }
    }
    return true;
}

// Walks up to the prisoner, wherever it has got to.
JobSystem::StepResult JobSystem::approachPrisoner(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    if (!prisonReachable(reg, job.site, at)) {
        why = "the prisoner isn't on this level";
        return StepResult::Failed;
    }
    const Position& there = reg.get<Position>(job.site);
    if (prisonDistSq(at, there) <= kPrisonReach * kPrisonReach) {
        if (job.walk == 1) {
            pathfinder_.cancel(job.path);
        }
        Mover& mover = reg.get<Mover>(pawn);
        mover.path.clear();
        mover.next = 0;
        job.walk = 0;
        return StepResult::Next;
    }
    const CellPos target = cellOf(there);
    if (job.walk != 0 && (std::abs(target.x - job.cell.x) > 2 || std::abs(target.y - job.cell.y) > 2)) {
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
        job.walk = 0;  // arrived, but the prisoner isn't within reach: look again
        return StepResult::Running;
    }
    return result;
}

JobSystem::StepResult JobSystem::feedPrisonerWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Entity prisoner = job.site;
    const Position& at = reg.get<Position>(pawn);
    if (!prisonReachable(reg, prisoner, at) || prisonDistSq(at, reg.get<Position>(prisoner)) > 4.0f * kPrisonReach * kPrisonReach) {
        if (job.repaths < maxRepaths_) {
            ++job.repaths;
            ++stats_.repaths;
            job.step = kPrisonApproachStep;  // it moved: walk up to it again
            job.walk = 0;
            return StepResult::Running;
        }
        why = "the prisoner moved away";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::FeedPawn, prisoner);
    }
    job.progress += 1.0f;
    if (job.progress < static_cast<float>(prisoners_->rules().feedWorkSeconds)) {
        return StepResult::Running;
    }
    const Entity food = job.thing;
    const DefId def = reg.get<Thing>(food).def;
    const int count = reg.get<StackCount>(food).count;
    Needs& needs = reg.get<Needs>(prisoner);
    const float before = needs.food;
    needs.food = std::min(1.0f, needs.food + nutrition_[def.value] * static_cast<float>(count));
    reg.remove<Carrying>(pawn);
    const bool humanIngredient = spoilage_ != nullptr && spoilage_->humanlikeIngredient(food);
    if (spoilage_ != nullptr) {
        spoilage_->eaten(prisoner, food);
    }
    things_.destroy(food);
    Event event;
    event.type = EventType::PawnAte;
    event.thing = prisoner;
    event.target = pawn;
    event.def = def;
    event.cell = cellOf(reg.get<Position>(prisoner));
    event.amount = needs.food - before;
    event.message = "fed";
    event.part = humanIngredient ? 1 : -1;
    events_.push_back(std::move(event));
    return StepResult::Next;
}

JobSystem::StepResult JobSystem::chatWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Entity prisoner = job.site;
    const Position& at = reg.get<Position>(pawn);
    if (!prisonReachable(reg, prisoner, at) || prisonDistSq(at, reg.get<Position>(prisoner)) > 4.0f * kPrisonReach * kPrisonReach) {
        if (job.repaths < maxRepaths_) {
            ++job.repaths;
            ++stats_.repaths;
            job.step = 0;  // it moved: walk up to it again
            job.walk = 0;
            return StepResult::Running;
        }
        why = "the prisoner moved away";
        return StepResult::Failed;
    }
    if (reg.get<Prisoner>(prisoner).mode == PrisonerMode::Execute) {
        // An execution: the same walk up, a short job, and no Social experience.
        if (job.walk == 0) {
            job.walk = 2;
            pawnDid(pawn, PawnAction::Slaughter, prisoner);
        }
        job.progress += 1.0f;
        if (job.progress < static_cast<float>(prisoners_->rules().executeWorkSeconds)) {
            return StepResult::Running;
        }
        prisoners_->execute(prisoner, pawn);
        return StepResult::Next;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Chat, prisoner);
    }
    job.progress += static_cast<float>(std::max(0.1f, health_.capacityValue(pawn, "Talking")));
    if (job.progress < static_cast<float>(prisoners_->rules().chatWorkSeconds)) {
        return StepResult::Running;
    }
    prisoners_->interact(pawn, prisoner);
    learnSkill(pawn, "Social", static_cast<double>(prisoners_->rules().chatXp));
    return StepResult::Next;
}

// ---- A prisoner's day -------------------------------------------------------------------------------

void JobSystem::stepPrisoner(Entity pawn) {
    entt::registry& reg = things_.registry();
    Worker& worker = reg.get<Worker>(pawn);
    const bool idle = worker.job.type == JobType::None || worker.job.type == JobType::Wait;
    if (idle) {
        Job job;
        bool have = false;
        const Needs* needs = reg.try_get<Needs>(pawn);
        if (needs != nullptr && needs->rest < sleepBelow(scheduleNow(pawn))) {
            have = makeSleepJob(pawn, job);
        } else if (needsBedRest(pawn)) {
            const Entity bed = bedOf(pawn);
            CellPos cell;
            if (bed != kNoEntity && bedSlotCell(bed, pawn, cell)) {
                job = Job{};
                job.type = JobType::BedRest;
                job.thing = bed;
                job.cell = cell;
                have = true;
            }
        }
        if (!have && worker.job.type == JobType::None) {
            have = thinkIdle(pawn, job);
        }
        if (have) {
            if (worker.job.type != JobType::None) {
                endJob(pawn, JobEnd::Interrupted, "needs");
            }
            startJob(pawn, std::move(job));
        }
    }
}

}  // namespace colony
