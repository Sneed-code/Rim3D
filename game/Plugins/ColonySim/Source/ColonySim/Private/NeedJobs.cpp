// Eating, sleeping and the schedule on the job system (task C6, docs/design/needs.md).
#include "ColonySim/Jobs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float needJobsDistSq(CellPos a, CellPos b) {
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    return dx * dx + dy * dy;
}

float needJobsDistSq(const Position& a, CellPos b) {
    const float dx = a.x - (static_cast<float>(b.x) + 0.5f);
    const float dy = a.y - (static_cast<float>(b.y) + 0.5f);
    return dx * dx + dy * dy;
}

}  // namespace

// ---- The think list ---------------------------------------------------------------------------

ScheduleKind JobSystem::scheduleNow(Entity pawn) const {
    const Schedule* schedule = things_.registry().try_get<Schedule>(pawn);
    return schedule != nullptr ? schedule->hours[static_cast<size_t>(time_.hourOfDay())] : ScheduleKind::Anything;
}

// The rest level below which a pawn goes to bed in this hour's schedule.
float JobSystem::sleepBelow(ScheduleKind slot) const {
    switch (slot) {
        case ScheduleKind::Sleep: return rules_.fallAsleepBelow;
        case ScheduleKind::Work: return rules_.restTiredBelow;
        case ScheduleKind::Anything:
        case ScheduleKind::Joy: break;
    }
    return rules_.restDrowsyBelow;
}

// Urgent needs: eat when the stomach is under the seek level, sleep when worn out. Whatever the
// schedule says, a pawn that is hungry or exhausted looks after it first.
bool JobSystem::thinkNeeds(Entity pawn, Job& out) {
    entt::registry& reg = things_.registry();
    Needs* needs = reg.try_get<Needs>(pawn);
    if (needs == nullptr) {
        return false;
    }
    if (needs->food < rules_.foodSeekBelow && now() >= needs->foodSearchAfter) {
        if (findFood(pawn, out)) {
            return true;
        }
        needs->foodSearchAfter = now() + idleSeconds_;  // nothing to eat now: look again in a minute
    }
    const ScheduleKind slot = scheduleNow(pawn);
    if (needs->rest < (slot == ScheduleKind::Work ? rules_.restTiredBelow : rules_.restDrowsyBelow)) {
        return makeSleepJob(pawn, out);
    }
    return false;
}

// The schedule: a Sleep hour sends a pawn that isn't well rested to bed; a Joy hour keeps it from
// working (there is no recreation yet, so it waits about).
bool JobSystem::thinkSchedule(Entity pawn, Job& out) {
    const Needs* needs = things_.registry().try_get<Needs>(pawn);
    if (needs == nullptr) {
        return false;
    }
    switch (scheduleNow(pawn)) {
        case ScheduleKind::Sleep:
            return needs->rest < rules_.fallAsleepBelow && makeSleepJob(pawn, out);
        case ScheduleKind::Joy:
            out = Job{};
            out.type = JobType::Wait;
            out.until = now() + idleSeconds_;
            return true;
        case ScheduleKind::Anything:
        case ScheduleKind::Work: break;
    }
    return false;
}

// A pawn on a job it may drop (hauling, waiting) who has become hungry enough to eat or tired
// enough to sleep puts that job down. Player orders and the need jobs themselves run to the end.
void JobSystem::interruptForNeeds(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Job& current = reg.get<Worker>(pawn).job;
    if (current.forced || (current.type != JobType::Wait && current.type != JobType::HaulToCell && !jobIsGather(current.type))) {
        return;
    }
    const Needs* needs = reg.try_get<Needs>(pawn);
    if (needs == nullptr) {
        return;
    }
    const bool hungry = needs->food < rules_.foodSeekBelow && now() >= needs->foodSearchAfter;
    const bool sleepy = needs->rest < sleepBelow(scheduleNow(pawn));
    if (!hungry && !sleepy) {
        return;
    }
    Job job;
    if (!(thinkNeeds(pawn, job) || thinkSchedule(pawn, job)) || (job.type != JobType::Ingest && job.type != JobType::LayDown)) {
        return;
    }
    endJob(pawn, JobEnd::Interrupted, job.type == JobType::Ingest ? "hungry" : "tired");
    startJob(pawn, std::move(job));
}

// ---- Eating -----------------------------------------------------------------------------------

// RimWorld's "best food": the tastiest kind the pawn can reach, the nearest of that kind. Food only
// good for desperate times is eaten only when the pawn is ravenous.
bool JobSystem::findFood(Entity pawn, Job& out) {
    const entt::registry& reg = things_.registry();
    const Needs& needs = reg.get<Needs>(pawn);
    const Position& at = reg.get<Position>(pawn);
    const CellPos pawnCell = cellOf(at);
    Entity best = kNoEntity;
    int bestTier = -1;
    float bestDist = 0.0f;
    for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        (void)stack;
        const int tier = thing.def.value < foodTier_.size() ? foodTier_[thing.def.value] : -1;
        if (tier < 0 || pos.level != at.level || (tier == 0 && needs.food >= rules_.foodRavenousBelow)) {
            continue;
        }
        const float dist = needJobsDistSq(at, cellOf(pos));
        const bool better = best == kNoEntity || tier > bestTier || (tier == bestTier && (dist < bestDist || (dist == bestDist && item < best)));
        if (!better) {
            continue;
        }
        const JobTarget target{item, CellPos{}};
        if (reservedByOther(target, pawn) || avoiding(pawn, target) || !canReach(pawnCell, cellOf(pos))) {
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
    // As many as it takes to fill the stomach, up to what a pawn eats at once.
    const int wanted = std::max(1, static_cast<int>(std::ceil((1.0f - needs.food) / nutrition - 0.001f)));
    const CellPos foodCell = cellOf(reg.get<Position>(best));
    out = Job{};
    out.type = JobType::Ingest;
    out.thing = best;
    out.count = std::min({reg.get<StackCount>(best).count, static_cast<int>(maxIngest_[def.value]), wanted});
    out.chair = findChair(pawn, foodCell, out.cell);
    if (out.chair == kNoEntity) {
        out.cell = foodCell;  // no table to sit at: eat where the food lies
    }
    return true;
}

Entity JobSystem::findChair(Entity pawn, CellPos from, CellPos& cell) const {
    const entt::registry& reg = things_.registry();
    Entity best = kNoEntity;
    float bestDist = 0.0f;
    const float radiusSq = rules_.eatSpotRadius * rules_.eatSpotRadius;
    for (const auto [chair, thing, footprint] : reg.view<const Thing, const Footprint>().each()) {
        if (isChair_[thing.def.value] == 0 || !reg.all_of<Position>(chair)) {
            continue;
        }
        const CellRect rect = footprint.rect();
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                const CellPos c{x, y, rect.level};
                const float dist = needJobsDistSq(from, c);
                if (c.level != from.level || dist > radiusSq || !map_.walkable(c)) {
                    continue;
                }
                if (best != kNoEntity && (dist > bestDist || (dist == bestDist && chair >= best))) {
                    continue;
                }
                bool besideTable = false;
                const CellPos around[4] = {{c.x + 1, c.y, c.level}, {c.x - 1, c.y, c.level}, {c.x, c.y + 1, c.level}, {c.x, c.y - 1, c.level}};
                for (const CellPos& n : around) {
                    const Entity building = map_.inBounds(n) ? map_.buildingAt(n) : kNoEntity;
                    besideTable = besideTable || (building != kNoEntity && isTable_[reg.get<Thing>(building).def.value] != 0);
                }
                if (!besideTable || reservedByOther(JobTarget{kNoEntity, c}, pawn) ||
                    avoiding(pawn, JobTarget{kNoEntity, c}) || !canReach(from, c)) {
                    continue;
                }
                best = chair;
                bestDist = dist;
                cell = c;
            }
        }
    }
    return best;
}

bool JobSystem::failCheckIngest(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (job.step <= kIngestPickUpStep) {
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
    if (job.chair != kNoEntity && !things_.spawned(job.chair)) {
        // The chair was taken away: eat wherever the pawn is, once it has the food.
        release(pawn, job);
        job.chair = kNoEntity;
        job.cell = job.step <= kIngestPickUpStep ? cellOf(reg.get<Position>(job.thing)) : cellOf(reg.get<Position>(pawn));
        reserve(pawn, job);
    }
    return true;
}

JobSystem::StepResult JobSystem::eat(Entity pawn, Job& job, std::string&) {
    entt::registry& reg = things_.registry();
    const Entity food = job.thing;
    const DefId def = reg.get<Thing>(food).def;
    if (job.until == 0) {
        // Sitting down to it. The pawn eats all it carries in one sitting, as RimWorld's.
        job.until = now() + std::max<int64_t>(1, static_cast<int64_t>(std::lround(ingestSeconds_[def.value])));
        pawnDid(pawn, PawnAction::Eat, food);
        return StepResult::Running;
    }
    if (now() < job.until) {
        return StepResult::Running;
    }
    const int count = reg.get<StackCount>(food).count;
    Needs& needs = reg.get<Needs>(pawn);
    const float before = needs.food;
    needs.food = std::min(1.0f, needs.food + nutrition_[def.value] * static_cast<float>(count));
    reg.remove<Carrying>(pawn);
    things_.destroy(food);
    Event event;
    event.type = EventType::PawnAte;
    event.thing = pawn;
    event.def = def;
    event.cell = cellOf(reg.get<Position>(pawn));
    event.amount = needs.food - before;
    event.message = job.chair != kNoEntity ? "table" : "ground";
    events_.push_back(std::move(event));
    return StepResult::Next;
}

// ---- Sleeping ---------------------------------------------------------------------------------

Entity JobSystem::bedOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    for (const auto [bed, owners] : reg.view<const Bed>().each()) {
        if (std::find(owners.owners.begin(), owners.owners.end(), pawn) != owners.owners.end()) {
            return bed;
        }
    }
    return kNoEntity;
}

// The cell of the bed that is `pawn`'s: its slot's cell, the i-th cell of the bed's footprint.
bool JobSystem::bedSlotCell(Entity bed, Entity pawn, CellPos& out) const {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(bed) || !reg.all_of<Bed, Footprint>(bed)) {
        return false;
    }
    const std::vector<Entity>& owners = reg.get<Bed>(bed).owners;
    const auto it = std::find(owners.begin(), owners.end(), pawn);
    if (it == owners.end()) {
        return false;
    }
    const CellRect rect = reg.get<Footprint>(bed).rect();
    const int index = static_cast<int>(it - owners.begin());
    if (index >= rect.sizeX * rect.sizeY) {
        return false;
    }
    out = CellPos{rect.minX + index % rect.sizeX, rect.minY + index / rect.sizeX, rect.level};
    return true;
}

// The pawn's own bed, or else the nearest free one, which then becomes its own (RimWorld's
// colonists claim a bed by sleeping in it). False: sleep on the ground.
bool JobSystem::findBed(Entity pawn, Entity& bed, CellPos& cell) {
    entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos pawnCell = cellOf(at);
    const auto usable = [&](Entity candidate, CellPos c) {
        return map_.walkable(c) && !reservedByOther(JobTarget{kNoEntity, c}, pawn) &&
               !avoiding(pawn, JobTarget{candidate, CellPos{}}) && canReach(pawnCell, c);
    };

    const Entity owned = bedOf(pawn);
    if (owned != kNoEntity) {
        CellPos c;
        if (bedSlotCell(owned, pawn, c) && usable(owned, c)) {
            bed = owned;
            cell = c;
            return true;
        }
        return false;
    }

    Entity best = kNoEntity;
    float bestDist = 0.0f;
    for (const auto [candidate, thing, owners, footprint] : reg.view<const Thing, const Bed, const Footprint>().each()) {
        const int slots = std::max(1, static_cast<int>(defs_.get(thing.def).integer("building.sleepingSlots", 1)));
        if (static_cast<int>(owners.owners.size()) >= slots || !things_.spawned(candidate)) {
            continue;
        }
        const CellRect rect = footprint.rect();
        const int index = static_cast<int>(owners.owners.size());
        if (index >= rect.sizeX * rect.sizeY) {
            continue;
        }
        const CellPos c{rect.minX + index % rect.sizeX, rect.minY + index / rect.sizeX, rect.level};
        const float dist = needJobsDistSq(at, c);
        if (c.level != at.level || (best != kNoEntity && (dist > bestDist || (dist == bestDist && candidate >= best))) ||
            !usable(candidate, c)) {
            continue;
        }
        best = candidate;
        bestDist = dist;
        cell = c;
    }
    if (best == kNoEntity) {
        return false;
    }
    reg.get<Bed>(best).owners.push_back(pawn);
    bed = best;
    return true;
}

bool JobSystem::makeSleepJob(Entity pawn, Job& out) {
    Entity bed = kNoEntity;
    CellPos cell;
    out = Job{};
    out.type = JobType::LayDown;
    if (findBed(pawn, bed, cell)) {
        out.thing = bed;
        out.cell = cell;
    } else {
        out.cell = cellOf(things_.registry().get<Position>(pawn));  // the ground, right here
    }
    return true;
}

bool JobSystem::failCheckLayDown(Entity pawn, Job& job, std::string& why) {
    if (job.thing == kNoEntity) {
        return true;
    }
    CellPos cell;
    if (!bedSlotCell(job.thing, pawn, cell) || cell != job.cell) {
        why = "the bed is gone or no longer its own";
        return false;
    }
    return true;
}

JobSystem::StepResult JobSystem::lie(Entity pawn, Job& job, std::string&) {
    entt::registry& reg = things_.registry();
    if (Health* health = reg.try_get<Health>(pawn)) {
        health->rest = job.thing != kNoEntity ? RestKind::Bed : RestKind::Ground;
    }
    if (job.until == 0) {
        job.until = now() + 1;  // marks that the pawn has lain down
        pawnDid(pawn, PawnAction::LieDown, job.thing);
        return StepResult::Running;
    }
    Needs& needs = reg.get<Needs>(pawn);
    if (needs.rest >= rules_.wakeAt) {
        return StepResult::Next;
    }
    if (scheduleNow(pawn) != ScheduleKind::Sleep && needs.rest >= rules_.wakeOutsideSleepAt) {
        return StepResult::Next;
    }
    // A pawn that is starving wakes to eat, if there is something to eat.
    if (needs.food < rules_.foodRavenousBelow && now() >= needs.foodSearchAfter) {
        Job food;
        if (findFood(pawn, food)) {
            return StepResult::Next;
        }
        needs.foodSearchAfter = now() + idleSeconds_;
    }
    return StepResult::Running;
}

void JobSystem::wakeUp(Entity pawn, const Job& job) {
    entt::registry& reg = things_.registry();
    if (Health* health = reg.try_get<Health>(pawn)) {
        health->rest = RestKind::Ground;
    }
    if (job.until == 0) {
        return;  // never lay down
    }
    pawnDid(pawn, PawnAction::GetUp, job.thing);
    Event event;
    event.type = EventType::PawnWokeUp;
    event.thing = pawn;
    event.def = reg.get<Thing>(pawn).def;
    event.cell = job.cell;
    event.target = job.thing;
    event.message = job.thing != kNoEntity ? "bed" : "ground";
    if (const Needs* needs = reg.try_get<Needs>(pawn)) {
        event.amount = needs->rest;
    }
    events_.push_back(std::move(event));
}

// ---- Commands ---------------------------------------------------------------------------------

bool JobSystem::setSchedule(Entity pawn, int hour, ScheduleKind kind, std::string& error) {
    entt::registry& reg = things_.registry();
    Schedule* schedule = reg.valid(pawn) ? reg.try_get<Schedule>(pawn) : nullptr;
    if (schedule == nullptr) {
        error = "that pawn has no schedule";
        return false;
    }
    if (hour < 0 || hour > 23) {
        error = "an hour is 0 to 23";
        return false;
    }
    schedule->hours[static_cast<size_t>(hour)] = kind;
    return true;
}

bool JobSystem::assignBed(Entity bed, Entity pawn, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(bed) || !reg.all_of<Bed, Thing>(bed)) {
        error = "that isn't a bed";
        return false;
    }
    Bed& owned = reg.get<Bed>(bed);
    if (pawn == kNoEntity) {
        owned.owners.clear();
        return true;
    }
    if (!reg.valid(pawn) || !reg.all_of<Needs>(pawn)) {
        error = "that pawn doesn't sleep";
        return false;
    }
    if (std::find(owned.owners.begin(), owned.owners.end(), pawn) != owned.owners.end()) {
        return true;
    }
    const int slots = std::max(1, static_cast<int>(defs_.get(reg.get<Thing>(bed).def).integer("building.sleepingSlots", 1)));
    if (static_cast<int>(owned.owners.size()) >= slots) {
        error = "that bed is full";
        return false;
    }
    // A pawn has one bed: it gives up the one it had.
    for (const auto [other, beds] : reg.view<Bed>().each()) {
        (void)other;
        beds.owners.erase(std::remove(beds.owners.begin(), beds.owners.end(), pawn), beds.owners.end());
    }
    owned.owners.push_back(pawn);
    return true;
}

}  // namespace colony
