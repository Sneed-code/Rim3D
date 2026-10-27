// Taking drugs on the job system (Stage 2, docs/design/drugs.md): choosing a dose as the pawn's drug policy
// says, and the player's order. The dose itself is JobSystem::eat, which hands a drug to the DrugSystem.
#include "ColonySim/Jobs.h"

#include "ColonySim/Drugs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float drugJobsDistSq(const Position& a, CellPos b) {
    const float dx = a.x - (static_cast<float>(b.x) + 0.5f);
    const float dy = a.y - (static_cast<float>(b.y) + 0.5f);
    return dx * dx + dy * dy;
}

}  // namespace

// The best drug the pawn wants (an addiction before a schedule before recreation), the nearest item of the first
// kind that has one it can reach. Nothing found: the pawn is left alone for a while.
bool JobSystem::thinkDrugs(Entity pawn, Job& out) {
    if (drugs_ == nullptr) {
        return false;
    }
    const std::vector<DrugWant> wants = drugs_->wants(pawn, now(), scheduleNow(pawn));
    if (wants.empty()) {
        return false;
    }
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    const CellPos pawnCell = cellOf(at);
    for (const DrugWant& want : wants) {
        Entity best = kNoEntity;
        float bestDist = 0.0f;
        for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
            (void)stack;
            if (thing.def != want.drug) {
                continue;
            }
            const float dist = drugJobsDistSq(at, cellOf(pos));
            if (best != kNoEntity && (dist > bestDist || (dist == bestDist && item >= best))) {
                continue;
            }
            const JobTarget target{item, CellPos{}};
            if (reservedByOther(target, pawn) || avoiding(pawn, target) || !canReach(pawnCell, cellOf(pos))) {
                continue;
            }
            best = item;
            bestDist = dist;
        }
        if (best == kNoEntity) {
            continue;
        }
        out = Job{};
        out.type = JobType::Ingest;
        out.thing = best;
        out.count = 1;
        out.cell = cellOf(reg.get<Position>(best));  // taken where it lies
        return true;
    }
    drugs_->searchFailed(pawn, now());
    return false;
}

bool JobSystem::orderTakeDrug(Entity pawn, Entity thing, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Worker, Position, Mover>(pawn)) {
        error = "that isn't a pawn who works";
        return false;
    }
    if (drugs_ == nullptr || !drugs_->eligible(pawn)) {
        error = "that pawn can't take drugs";
        return false;
    }
    if (health_.incapacitated(pawn)) {
        error = "the pawn can't act now (downed or dead)";
        return false;
    }
    if (reg.all_of<PlayerControl>(pawn)) {
        error = "the pawn is under your control";
        return false;
    }
    if (!things_.spawned(thing) || !reg.all_of<Thing, StackCount>(thing) || !drugs_->isDrug(reg.get<Thing>(thing).def)) {
        error = "that isn't a drug on the map";
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
        endJob(pawn, JobEnd::Interrupted, "ordered to take a drug");
    }
    Job job;
    job.type = JobType::Ingest;
    job.thing = thing;
    job.count = 1;
    job.cell = cellOf(reg.get<Position>(thing));
    job.forced = true;
    startJob(pawn, std::move(job));
    return true;
}

}  // namespace colony
