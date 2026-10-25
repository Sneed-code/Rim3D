#include "ColonySim/Jobs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// The steps of each job type (RimWorld's toils), run in order. A step that ends at once lets the
// next one start in the same game second.
enum class JobStepKind : uint8_t { Wait, GotoThing, PickUp, CarryToCell, Place };

constexpr JobStepKind kJobsWaitSteps[] = {JobStepKind::Wait};
constexpr JobStepKind kJobsHaulSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::CarryToCell,
                                          JobStepKind::Place};
constexpr uint8_t kJobsHaulPickUpStep = 1;
constexpr uint8_t kJobsHaulCarryStep = 2;

// The most steps one pawn's job may run in one game second (guards against a loop of steps that
// all end at once).
constexpr int kJobsStepsPerTick = 8;
// How far from a pawn it looks for a free cell to put down what it carries, in cells.
constexpr int kJobsDropRadius = 6;

// Fallbacks when data/defs has no JobRules def (the test defs). The same values as JobRules.
constexpr double kJobsIdleSeconds = 60.0;
constexpr double kJobsAvoidSeconds = 600.0;
constexpr int kJobsMaxRepaths = 5;
// RimWorld's base carrying capacity, for pawn defs without `stats.carryingCapacity`.
constexpr double kJobsCarryingCapacity = 75.0;

struct JobsSteps {
    const JobStepKind* steps;
    int count;
};

JobsSteps jobsStepsOf(JobType type) {
    switch (type) {
        case JobType::None: return {nullptr, 0};
        case JobType::Wait: return {kJobsWaitSteps, static_cast<int>(std::size(kJobsWaitSteps))};
        case JobType::HaulToCell: return {kJobsHaulSteps, static_cast<int>(std::size(kJobsHaulSteps))};
    }
    return {nullptr, 0};
}

void jobsStartWalk(const Map& map, const Position& pos, Mover& mover) {
    std::vector<CellPos> route = std::move(mover.path);
    smoothPath(map, pos.x, pos.y, route);
    mover.walkTo(pos.x, pos.y, std::move(route));
}

float jobsDistSq(CellPos a, CellPos b) {
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    return dx * dx + dy * dy;
}

// Candidates nearest first; ties by Work tab column, scanner, then target.
bool jobsCandidateAfter(const WorkCandidate& a, const WorkCandidate& b) {
    if (a.distSq != b.distSq) return a.distSq > b.distSq;
    if (a.workType != b.workType) return a.workType > b.workType;
    if (a.giver != b.giver) return a.giver > b.giver;
    const uint32_t ta = entt::to_integral(a.target.thing);
    const uint32_t tb = entt::to_integral(b.target.thing);
    if (ta != tb) return ta > tb;
    if (a.target.cell.y != b.target.cell.y) return a.target.cell.y > b.target.cell.y;
    return a.target.cell.x > b.target.cell.x;
}

}  // namespace

const char* jobTypeName(JobType type) {
    switch (type) {
        case JobType::None: return "None";
        case JobType::Wait: return "Wait";
        case JobType::HaulToCell: return "HaulToCell";
    }
    return "?";
}

int jobStepCount(JobType type) {
    return jobsStepsOf(type).count;
}

JobSystem::JobSystem(Map& map, Things& things, Pathfinder& pathfinder, Zones& zones, const DefDatabase& defs,
                     const HealthSystem& health, const GameTime& time, SimStats& stats, std::vector<Event>& events)
    : map_(map),
      things_(things),
      pathfinder_(pathfinder),
      zones_(zones),
      defs_(defs),
      health_(health),
      time_(time),
      stats_(stats),
      events_(events),
      givers_(makeWorkGivers()) {
    for (const DefId id : defs_.idsOfKind("WorkTypes")) {
        const Def& def = defs_.get(id);
        WorkType type;
        type.def = id;
        type.defName = def.defName();
        type.naturalPriority = static_cast<int>(def.integer("naturalPriority"));
        for (size_t i = 0; i < def.count("workTags"); ++i) {
            type.workTags.push_back(def.textAt("workTags", i));
        }
        for (size_t i = 0; i < def.count("tasks"); ++i) {
            const std::string task = def.textAt("tasks", i);
            for (const std::unique_ptr<WorkGiver>& giver : givers_) {
                if (task == giver->task()) {
                    type.givers.push_back(giver.get());
                }
            }
        }
        workTypes_.push_back(std::move(type));
    }
    std::stable_sort(workTypes_.begin(), workTypes_.end(), [](const WorkType& a, const WorkType& b) {
        return a.naturalPriority != b.naturalPriority ? a.naturalPriority > b.naturalPriority : a.defName < b.defName;
    });

    standable_.assign(defs_.size(), 0);
    stackLimits_.assign(defs_.size(), 1);
    for (uint32_t i = 0; i < defs_.size(); ++i) {
        const Def& def = defs_.get(DefId{i});
        if (def.kind() != "Things") {
            continue;
        }
        standable_[i] = def.text("category") == "Building" && def.text("passability") == "Standable" ? 1 : 0;
        stackLimits_[i] = std::max(1, static_cast<int>(def.integer("stackLimit", 1)));
    }

    const Def* rules = defs_.find("Rules", "JobRules");
    idleSeconds_ = static_cast<int64_t>(rules ? rules->number("idleSeconds", kJobsIdleSeconds) : kJobsIdleSeconds);
    avoidSeconds_ = static_cast<int64_t>(rules ? rules->number("avoidFailedTargetSeconds", kJobsAvoidSeconds) : kJobsAvoidSeconds);
    maxRepaths_ = static_cast<int>(rules ? rules->integer("maxRepaths", kJobsMaxRepaths) : kJobsMaxRepaths);
    defaultPriority_ = static_cast<uint8_t>(std::clamp(
        static_cast<int>(rules ? rules->integer("defaultWorkPriority", kDefaultPriority) : kDefaultPriority), 1, kMaxPriority));
}

JobSystem::~JobSystem() = default;

int64_t JobSystem::now() const {
    return time_.seconds;
}

int JobSystem::workTypeIndex(DefId def) const {
    for (size_t i = 0; i < workTypes_.size(); ++i) {
        if (workTypes_[i].def == def) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

WorkSettings JobSystem::defaultWorkSettings(const std::vector<std::string>& disabledWorkTags) const {
    WorkSettings settings;
    for (const WorkType& type : workTypes_) {
        const bool disabled = std::any_of(type.workTags.begin(), type.workTags.end(), [&](const std::string& tag) {
            return std::find(disabledWorkTags.begin(), disabledWorkTags.end(), tag) != disabledWorkTags.end();
        });
        settings.entries.push_back(WorkPriority{type.def, disabled ? uint8_t{0} : defaultPriority_, disabled});
    }
    return settings;
}

void JobSystem::addWorker(Entity pawn, const std::vector<std::string>& disabledWorkTags) {
    entt::registry& reg = things_.registry();
    reg.emplace_or_replace<Worker>(pawn);
    reg.emplace_or_replace<WorkSettings>(pawn, defaultWorkSettings(disabledWorkTags));
}

// ---- Reservations ---------------------------------------------------------------------------

JobSystem::TargetKey JobSystem::keyOf(const JobTarget& target) {
    TargetKey key;
    if (target.thing != kNoEntity) {
        key.thing = entt::to_integral(target.thing);
    } else {
        key.x = target.cell.x;
        key.y = target.cell.y;
        key.level = target.cell.level;
    }
    return key;
}

Entity JobSystem::reservedBy(const JobTarget& target) const {
    const auto it = reservations_.find(keyOf(target));
    return it == reservations_.end() ? kNoEntity : it->second;
}

bool JobSystem::reservedByOther(const JobTarget& target, Entity pawn) const {
    const Entity holder = reservedBy(target);
    return holder != kNoEntity && holder != pawn;
}

void JobSystem::reserve(Entity pawn, const Job& job) {
    if (job.type == JobType::HaulToCell) {
        reservations_[keyOf(JobTarget{job.thing, CellPos{}})] = pawn;
        reservations_[keyOf(JobTarget{kNoEntity, job.cell})] = pawn;
    }
}

void JobSystem::release(Entity pawn, const Job& job) {
    if (job.type != JobType::HaulToCell) {
        return;
    }
    for (const JobTarget& target : {JobTarget{job.thing, CellPos{}}, JobTarget{kNoEntity, job.cell}}) {
        const auto it = reservations_.find(keyOf(target));
        if (it != reservations_.end() && it->second == pawn) {
            reservations_.erase(it);
        }
    }
}

bool JobSystem::avoiding(Entity pawn, const JobTarget& target) const {
    const Worker* worker = things_.registry().try_get<Worker>(pawn);
    if (worker == nullptr) {
        return false;
    }
    const TargetKey key = keyOf(target);
    for (const AvoidTarget& avoid : worker->avoid) {
        const TargetKey other = keyOf(avoid.target);
        if (!(key < other) && !(other < key) && avoid.until > now()) {
            return true;
        }
    }
    return false;
}

bool JobSystem::canReach(CellPos from, CellPos to) const {
    return map_.regions().canReach(from, to);
}

// ---- Storage --------------------------------------------------------------------------------

int JobSystem::stackLimit(DefId def) const {
    return def.valid() && def.value < stackLimits_.size() ? stackLimits_[def.value] : 1;
}

int JobSystem::carryLimit(Entity pawn, DefId def) const {
    const Thing* thing = things_.registry().try_get<Thing>(pawn);
    const double capacity =
        thing ? defs_.get(thing->def).number("stats.carryingCapacity", kJobsCarryingCapacity) : kJobsCarryingCapacity;
    return std::clamp(static_cast<int>(std::lround(capacity)), 1, stackLimit(def));
}

StoragePriority JobSystem::storedPriority(Entity item) const {
    const entt::registry& reg = things_.registry();
    const Position* pos = reg.try_get<Position>(item);
    if (pos == nullptr) {
        return StoragePriority::Unstored;
    }
    const StockpileZone* zone = zones_.stockpileAt(cellOf(*pos));
    return zone != nullptr && zone->allows(reg.get<Thing>(item).def) ? zone->priority : StoragePriority::Unstored;
}

int JobSystem::roomFor(DefId def, CellPos cell) const {
    if (!map_.walkable(cell)) {
        return 0;
    }
    const entt::registry& reg = things_.registry();
    const Entity building = map_.buildingAt(cell);
    if (building != kNoEntity && standable_[reg.get<Thing>(building).def.value] == 0) {
        return 0;
    }
    const std::vector<Entity>& items = things_.itemsAt(cell);
    if (items.empty()) {
        return stackLimit(def);
    }
    if (items.size() == 1 && reg.get<Thing>(items[0]).def == def) {
        return std::max(0, stackLimit(def) - reg.get<StackCount>(items[0]).count);
    }
    return 0;
}

bool JobSystem::findStorageCell(DefId def, CellPos from, StoragePriority better, Entity pawn, CellPos& out) const {
    for (int p = static_cast<int>(StoragePriority::Critical); p > static_cast<int>(better); --p) {
        bool found = false;
        float bestDist = 0.0f;
        for (const StockpileZone& zone : zones_.stockpiles()) {
            if (static_cast<int>(zone.priority) != p || !zone.allows(def)) {
                continue;
            }
            for (const CellPos& c : zone.cells) {
                const float dist = jobsDistSq(from, c);
                if (found && dist >= bestDist) {
                    continue;
                }
                if (roomFor(def, c) <= 0 || reservedByOther(JobTarget{kNoEntity, c}, pawn) || !canReach(from, c)) {
                    continue;
                }
                found = true;
                bestDist = dist;
                out = c;
            }
        }
        if (found) {
            return true;
        }
    }
    return false;
}

// ---- Thinking -------------------------------------------------------------------------------

void JobSystem::step() {
    entt::registry& reg = things_.registry();
    order_.clear();
    for (const Entity pawn : reg.view<Worker, Position, Mover>()) {
        order_.push_back(pawn);
    }
    std::sort(order_.begin(), order_.end());

    // The think list, in order. Later tasks put theirs in place: emergencies first (downed, on
    // fire: F1 and later), then the defense plan (F6), urgent needs and the schedule (C6), work
    // and finally idling. Player orders don't wait for a think: the command starts them.
    static constexpr ThinkNode kThinkList[] = {&JobSystem::thinkWork, &JobSystem::thinkIdle};

    for (const Entity pawn : order_) {
        // The player walks this one (U9): no think, no job, until control is let go.
        if (reg.all_of<PlayerControl>(pawn)) {
            continue;
        }
        // Downed or dead (F1): the job ends, what it carried is put down, and it doesn't think.
        if (health_.incapacitated(pawn)) {
            if (reg.get<Worker>(pawn).job.type != JobType::None) {
                endJob(pawn, JobEnd::Interrupted, reg.get<Health>(pawn).dead ? "died" : "downed");
            }
            continue;
        }
        Worker& worker = reg.get<Worker>(pawn);
        const int64_t t = now();
        worker.avoid.erase(std::remove_if(worker.avoid.begin(), worker.avoid.end(),
                                          [t](const AvoidTarget& a) { return a.until <= t; }),
                           worker.avoid.end());
        if (worker.job.type == JobType::None) {
            for (const ThinkNode node : kThinkList) {
                Job job;
                if ((this->*node)(pawn, job)) {
                    startJob(pawn, std::move(job));
                    break;
                }
            }
        }
        if (reg.get<Worker>(pawn).job.type != JobType::None) {
            tickJob(pawn);
        }
    }
}

bool JobSystem::thinkWork(Entity pawn, Job& out) {
    entt::registry& reg = things_.registry();
    const WorkSettings* settings = reg.try_get<WorkSettings>(pawn);
    if (settings == nullptr) {
        return false;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    for (int priority = 1; priority <= kMaxPriority; ++priority) {
        // Every scanner of every work type at this priority offers its targets; the nearest wins,
        // whichever work type it is (game design doc: within a priority, closer jobs first).
        candidates_.clear();
        for (size_t w = 0; w < settings->entries.size() && w < workTypes_.size(); ++w) {
            const WorkPriority& entry = settings->entries[w];
            if (entry.disabled || entry.priority != priority) {
                continue;
            }
            for (size_t g = 0; g < workTypes_[w].givers.size(); ++g) {
                const size_t first = candidates_.size();
                workTypes_[w].givers[g]->scan(*this, pawn, candidates_);
                for (size_t i = first; i < candidates_.size(); ++i) {
                    candidates_[i].workType = static_cast<uint16_t>(w);
                    candidates_[i].giver = static_cast<uint16_t>(g);
                }
            }
        }
        std::make_heap(candidates_.begin(), candidates_.end(), jobsCandidateAfter);
        while (!candidates_.empty()) {
            std::pop_heap(candidates_.begin(), candidates_.end(), jobsCandidateAfter);
            const WorkCandidate c = candidates_.back();
            candidates_.pop_back();
            if (reservedByOther(c.target, pawn) || avoiding(pawn, c.target)) {
                continue;
            }
            const CellPos targetCell = c.target.thing != kNoEntity ? cellOf(reg.get<Position>(c.target.thing)) : c.target.cell;
            if (!canReach(at, targetCell)) {
                continue;
            }
            Job job;
            if (workTypes_[c.workType].givers[c.giver]->makeJob(*this, pawn, c, job)) {
                job.workType = workTypes_[c.workType].def;
                out = std::move(job);
                candidates_.clear();
                return true;
            }
        }
    }
    return false;
}

bool JobSystem::thinkIdle(Entity, Job& out) {
    out = Job{};
    out.type = JobType::Wait;
    out.until = now() + idleSeconds_;
    return true;
}

// ---- Running jobs ---------------------------------------------------------------------------

void JobSystem::startJob(Entity pawn, Job job) {
    job.startedAt = now();
    job.step = 0;
    job.walk = 0;
    job.repaths = 0;
    job.path = PathTicket{};
    reserve(pawn, job);
    Worker& worker = things_.registry().get<Worker>(pawn);
    worker.job = std::move(job);
    jobEvent(pawn, worker.job, true, std::string());
}

void JobSystem::tickJob(Entity pawn) {
    entt::registry& reg = things_.registry();
    for (int guard = 0; guard < kJobsStepsPerTick; ++guard) {
        Job& job = reg.get<Worker>(pawn).job;
        std::string why;
        if (!failCheck(pawn, job, why)) {
            endJob(pawn, JobEnd::Failed, why);
            return;
        }
        const StepResult result = runStep(pawn, job, why);
        if (result == StepResult::Running) {
            return;
        }
        if (result == StepResult::Failed) {
            endJob(pawn, JobEnd::Failed, why);
            return;
        }
        Job& still = reg.get<Worker>(pawn).job;
        ++still.step;
        still.walk = 0;
        if (still.step >= jobStepCount(still.type)) {
            endJob(pawn, JobEnd::Done, std::string());
            return;
        }
    }
}

bool JobSystem::failCheck(Entity pawn, Job& job, std::string& why) {
    if (job.type != JobType::HaulToCell) {
        return true;
    }
    entt::registry& reg = things_.registry();
    if (job.step <= kJobsHaulPickUpStep) {
        if (!things_.spawned(job.thing)) {
            why = "the item is gone";
            return false;
        }
        const StockpileZone* zone = zones_.stockpileAt(job.cell);
        const DefId def = reg.get<Thing>(job.thing).def;
        if (zone == nullptr || !zone->allows(def) || roomFor(def, job.cell) <= 0) {
            why = "the stockpile can't take it any more";
            return false;
        }
        return true;
    }
    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
        why = "it lost what it carried";
        return false;
    }
    const StockpileZone* zone = zones_.stockpileAt(job.cell);
    const DefId def = reg.get<Thing>(job.thing).def;
    if ((zone == nullptr || !zone->allows(def) || roomFor(def, job.cell) <= 0) && !retargetStorage(pawn, job)) {
        why = "no stockpile can take it any more";
        return false;
    }
    return true;
}

JobSystem::StepResult JobSystem::runStep(Entity pawn, Job& job, std::string& why) {
    const JobsSteps steps = jobsStepsOf(job.type);
    if (job.step >= steps.count) {
        return StepResult::Next;
    }
    switch (steps.steps[job.step]) {
        case JobStepKind::Wait:
            return now() >= job.until ? StepResult::Next : StepResult::Running;
        case JobStepKind::GotoThing:
            return walkTo(pawn, job, cellOf(things_.registry().get<Position>(job.thing)), why);
        case JobStepKind::PickUp:
            return pickUp(pawn, job, why);
        case JobStepKind::CarryToCell:
            return walkTo(pawn, job, job.cell, why);
        case JobStepKind::Place:
            return place(pawn, job, why);
    }
    return StepResult::Failed;
}

JobSystem::StepResult JobSystem::walkTo(Entity pawn, Job& job, CellPos goal, std::string& why) {
    entt::registry& reg = things_.registry();
    Mover& mover = reg.get<Mover>(pawn);
    switch (job.walk) {
        case 0:
            job.path = pathfinder_.request(cellOf(reg.get<Position>(pawn)), goal);
            job.walk = 1;
            return StepResult::Running;
        case 1: {
            if (pathfinder_.status(job.path) == PathStatus::Queued) {
                return StepResult::Running;
            }
            mover.next = 0;
            const bool found = pathfinder_.takePath(job.path, mover.path);
            job.path = PathTicket{};
            if (!found) {
                why = "it found no way there";
                return StepResult::Failed;
            }
            jobsStartWalk(map_, reg.get<Position>(pawn), mover);
            job.walk = 2;
            return StepResult::Running;
        }
        default:
            if (mover.gaveUp) {
                // Held up for good (a pawn or a wall in the way of its line): a new path from here.
                mover.gaveUp = false;
                if (job.repaths >= maxRepaths_) {
                    why = "it was held up for good";
                    return StepResult::Failed;
                }
                ++job.repaths;
                ++stats_.repaths;
                job.path = pathfinder_.request(cellOf(reg.get<Position>(pawn)), goal);
                job.walk = 1;
                return StepResult::Running;
            }
            return mover.arrived() ? StepResult::Next : StepResult::Running;
    }
}

JobSystem::StepResult JobSystem::pickUp(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(job.thing)) != cellOf(reg.get<Position>(pawn))) {
        why = "the item isn't where the pawn stands";
        return StepResult::Failed;
    }
    const int stack = reg.get<StackCount>(job.thing).count;
    const int count = std::min(job.count, stack);
    Entity carried = job.thing;
    if (count < stack) {
        carried = things_.splitOff(job.thing, count);
        if (carried == kNoEntity) {
            why = "it couldn't take part of the stack";
            return StepResult::Failed;
        }
    } else {
        things_.despawn(job.thing);
    }
    release(pawn, job);
    job.thing = carried;
    job.count = count;
    reserve(pawn, job);
    reg.emplace_or_replace<Carrying>(pawn, Carrying{carried});
    pawnDid(pawn, PawnAction::PickUp, carried);
    return StepResult::Next;
}

JobSystem::StepResult JobSystem::place(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn isn't at the stockpile cell";
        return StepResult::Failed;
    }
    const Entity carried = job.thing;
    const DefId def = reg.get<Thing>(carried).def;
    const int count = reg.get<StackCount>(carried).count;
    const int room = roomFor(def, job.cell);
    const std::vector<Entity>& there = things_.itemsAt(job.cell);
    if (there.empty() && room >= count) {
        reg.remove<Carrying>(pawn);
        things_.spawnAt(carried, job.cell);
        pawnDid(pawn, PawnAction::PutDown, carried);
    } else if (!there.empty() && room > 0) {
        // Onto the stack already there; what doesn't fit stays in the pawn's hands.
        const int put = std::min(room, count);
        const Entity stack = there.front();
        reg.get<StackCount>(stack).count += put;
        pawnDid(pawn, PawnAction::PutDown, stack);
        if (put == count) {
            reg.remove<Carrying>(pawn);
            things_.destroy(carried);
        } else {
            reg.get<StackCount>(carried).count -= put;
        }
    }
    if (reg.all_of<Carrying>(pawn)) {
        // Some or all of it didn't fit after all: on to another cell, or put it down here.
        if (retargetStorage(pawn, job)) {
            return StepResult::Running;
        }
        dropCarried(pawn);
    }
    ++stats_.hauls;
    return StepResult::Next;
}

bool JobSystem::retargetStorage(Entity pawn, Job& job) {
    entt::registry& reg = things_.registry();
    const DefId def = reg.get<Thing>(job.thing).def;
    CellPos cell;
    if (!findStorageCell(def, cellOf(reg.get<Position>(pawn)), StoragePriority::Unstored, pawn, cell)) {
        return false;
    }
    release(pawn, job);
    if (job.walk == 1) {
        pathfinder_.cancel(job.path);
        job.path = PathTicket{};
    }
    job.cell = cell;
    job.step = kJobsHaulCarryStep;
    job.walk = 0;
    reserve(pawn, job);
    return true;
}

void JobSystem::dropCarried(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    if (carrying == nullptr) {
        return;
    }
    const Entity thing = carrying->thing;
    reg.remove<Carrying>(pawn);
    if (!reg.valid(thing)) {
        return;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    const DefId def = reg.get<Thing>(thing).def;
    const int count = reg.get<StackCount>(thing).count;
    // The nearest cell, ring by ring, with room for all of it and no other pawn's claim.
    for (int r = 0; r <= kJobsDropRadius; ++r) {
        for (int y = at.y - r; y <= at.y + r; ++y) {
            for (int x = at.x - r; x <= at.x + r; ++x) {
                if (std::abs(x - at.x) != r && std::abs(y - at.y) != r) {
                    continue;
                }
                const CellPos c{x, y, at.level};
                if (!map_.inBounds(c) || roomFor(def, c) < count || reservedByOther(JobTarget{kNoEntity, c}, pawn)) {
                    continue;
                }
                const std::vector<Entity>& there = things_.itemsAt(c);
                if (there.empty()) {
                    things_.spawnAt(thing, c);
                    pawnDid(pawn, PawnAction::PutDown, thing);
                } else {
                    const Entity stack = there.front();
                    reg.get<StackCount>(stack).count += count;
                    pawnDid(pawn, PawnAction::PutDown, stack);
                    things_.destroy(thing);
                }
                return;
            }
        }
    }
    things_.spawnAt(thing, at);  // nowhere better: on top of whatever is here
    pawnDid(pawn, PawnAction::PutDown, thing);
}

void JobSystem::endJob(Entity pawn, JobEnd end, const std::string& why) {
    entt::registry& reg = things_.registry();
    const Job job = reg.get<Worker>(pawn).job;
    release(pawn, job);
    if (job.walk == 1) {
        pathfinder_.cancel(job.path);
    }
    if (end != JobEnd::Done) {
        Mover& mover = reg.get<Mover>(pawn);
        mover.path.clear();
        mover.next = 0;
    }
    dropCarried(pawn);
    Worker& worker = reg.get<Worker>(pawn);
    if (end == JobEnd::Failed) {
        ++stats_.failedJobs;
        if (job.thing != kNoEntity) {
            worker.avoid.push_back(AvoidTarget{JobTarget{job.thing, CellPos{}}, now() + avoidSeconds_});
            if (worker.avoid.size() > kMaxAvoid) {
                worker.avoid.erase(worker.avoid.begin());
            }
        }
    }
    worker.job = Job{};
    jobEvent(pawn, job, false,
             end == JobEnd::Done ? std::string("done")
                                 : (end == JobEnd::Failed ? "failed: " : "interrupted: ") + why);
}

void JobSystem::jobEvent(Entity pawn, const Job& job, bool started, const std::string& message) {
    Event event;
    event.type = started ? EventType::JobStarted : EventType::JobEnded;
    event.thing = pawn;
    event.def = things_.registry().get<Thing>(pawn).def;
    event.job = job.type;
    event.target = job.thing;
    event.cell = job.cell;
    event.message = message;
    events_.push_back(std::move(event));
}

void JobSystem::pawnDid(Entity pawn, PawnAction action, Entity target) {
    Event event;
    event.type = EventType::PawnAction;
    event.thing = pawn;
    event.def = things_.registry().get<Thing>(pawn).def;
    event.action = action;
    event.target = target;
    events_.push_back(std::move(event));
}

// ---- Commands and outside changes -------------------------------------------------------------

bool JobSystem::setPriority(Entity pawn, DefId workType, int priority, std::string& error) {
    entt::registry& reg = things_.registry();
    WorkSettings* settings = reg.valid(pawn) ? reg.try_get<WorkSettings>(pawn) : nullptr;
    if (settings == nullptr) {
        error = "that pawn doesn't work";
        return false;
    }
    const int index = workTypeIndex(workType);
    if (index < 0 || static_cast<size_t>(index) >= settings->entries.size()) {
        error = "no such work type";
        return false;
    }
    if (priority < 0 || priority > kMaxPriority) {
        error = "a work priority is 0 (off) or 1 to 4";
        return false;
    }
    WorkPriority& entry = settings->entries[static_cast<size_t>(index)];
    if (entry.disabled && priority > 0) {
        error = "that pawn can't do " + workTypes_[static_cast<size_t>(index)].defName + " work";
        return false;
    }
    entry.priority = static_cast<uint8_t>(priority);
    return true;
}

bool JobSystem::orderHaul(Entity pawn, Entity thing, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Worker, Position, Mover>(pawn)) {
        error = "that isn't a pawn who works";
        return false;
    }
    if (health_.incapacitated(pawn)) {
        error = "the pawn can't work now (downed or dead)";
        return false;
    }
    if (reg.all_of<PlayerControl>(pawn)) {
        error = "the player is controlling that pawn";
        return false;
    }
    if (!things_.spawned(thing) || !reg.all_of<StackCount>(thing)) {
        error = "that isn't an item on the map";
        return false;
    }
    if (reservedByOther(JobTarget{thing, CellPos{}}, pawn)) {
        error = "another pawn is already using it";
        return false;
    }
    if (!canReach(cellOf(reg.get<Position>(pawn)), cellOf(reg.get<Position>(thing)))) {
        error = "the pawn can't reach it";
        return false;
    }
    if (reg.get<Worker>(pawn).job.type != JobType::None) {
        endJob(pawn, JobEnd::Interrupted, "ordered to haul");
    }
    Job job;
    if (!makeHaulJob(*this, pawn, thing, job)) {
        error = "no stockpile has room for it";
        return false;
    }
    job.forced = true;
    startJob(pawn, std::move(job));
    return true;
}

void JobSystem::interrupt(Entity pawn, const std::string& why) {
    const entt::registry& reg = things_.registry();
    if (reg.valid(pawn) && reg.all_of<Worker>(pawn) && reg.get<Worker>(pawn).job.type != JobType::None) {
        endJob(pawn, JobEnd::Interrupted, why);
    }
}

void JobSystem::forget(Entity thing) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(thing)) {
        return;
    }
    // The pawn carrying it ends its job; what it carried is gone, so there is nothing to drop.
    std::vector<Entity> carriers;
    for (const auto [pawn, carrying] : reg.view<const Carrying>().each()) {
        if (carrying.thing == thing) {
            carriers.push_back(pawn);
        }
    }
    std::sort(carriers.begin(), carriers.end());
    for (const Entity pawn : carriers) {
        reg.remove<Carrying>(pawn);
        if (reg.all_of<Worker>(pawn) && reg.get<Worker>(pawn).job.type != JobType::None) {
            endJob(pawn, JobEnd::Interrupted, "what it carried is gone");
        }
    }
    // A pawn that goes ends its job and puts down what it carries.
    if (reg.all_of<Worker, Mover>(thing) && reg.get<Worker>(thing).job.type != JobType::None) {
        if (reg.all_of<Position>(thing)) {
            endJob(thing, JobEnd::Interrupted, "the pawn is gone");
        } else {
            release(thing, reg.get<Worker>(thing).job);
        }
    }
}

bool JobSystem::afterLoad(std::string& error) {
    entt::registry& reg = things_.registry();
    reservations_.clear();
    std::vector<Entity> workers;
    for (const Entity pawn : reg.view<Worker>()) {
        workers.push_back(pawn);
    }
    std::sort(workers.begin(), workers.end());
    for (const Entity pawn : workers) {
        const Job& job = reg.get<Worker>(pawn).job;
        if (job.type == JobType::HaulToCell) {
            for (const JobTarget& target : {JobTarget{job.thing, CellPos{}}, JobTarget{kNoEntity, job.cell}}) {
                if (!reservations_.emplace(keyOf(target), pawn).second) {
                    error = "the save has two jobs holding the same thing or cell";
                    return false;
                }
            }
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            const bool carries = carrying != nullptr && carrying->thing == job.thing;
            if (job.thing == kNoEntity || (job.step > kJobsHaulPickUpStep) != carries) {
                error = "the save has a hauling job that doesn't match what the pawn carries";
                return false;
            }
        }
        if (job.type != JobType::None && !reg.all_of<Mover>(pawn)) {
            error = "the save has a job on something that can't walk";
            return false;
        }
        if (job.walk == 1 && pathfinder_.status(job.path) == PathStatus::Unknown) {
            error = "the save has a pawn waiting for a path nobody is looking for";
            return false;
        }
    }
    return true;
}

}  // namespace colony
