// Manning artillery (docs/design/artillery.md): the ManArtillery job walks a colonist beside a mortar or rocket
// launcher and keeps it there, at its post, until the player orders it off. The artillery system fires while it stands
// there. Part of the job system.
#include "ColonySim/Construction.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"

namespace colony {

namespace {

bool artilleryJobsIsGun(const DefDatabase& defs, const entt::registry& reg, Entity building) {
    return reg.valid(building) && reg.all_of<Thing, Footprint, Position>(building) && !reg.all_of<ConstructionSite>(building) &&
           defs.get(reg.get<Thing>(building).def).has("artillery");
}

}  // namespace

bool JobSystem::orderMan(Entity pawn, Entity building, std::string& error) {
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
    if (!artilleryJobsIsGun(defs_, reg, building)) {
        error = "that isn't a mortar or rocket launcher";
        return false;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    CellPos stand;
    if (!construction_.workCell(reg.get<Footprint>(building).rect(), at, stand) || !canReach(at, stand)) {
        error = "the pawn can't get to it";
        return false;
    }
    // Another colonist mans it: the newcomer takes over.
    if (const auto it = reservations_.find(keyOf(JobTarget{building, CellPos{}}, 2)); it != reservations_.end() && it->second != pawn) {
        interrupt(it->second, "another colonist took the gun");
    }
    if (reg.get<Worker>(pawn).job.type != JobType::None) {
        endJob(pawn, JobEnd::Interrupted, "ordered to man a gun");
    }
    Job job;
    job.type = JobType::ManArtillery;
    job.site = building;
    job.cell = stand;
    job.forced = true;
    startJob(pawn, std::move(job));
    return true;
}

bool JobSystem::failCheckMan(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!artilleryJobsIsGun(defs_, reg, job.site)) {
        why = "the gun is gone";
        return false;
    }
    if (reg.all_of<MarkedForDeconstruct>(job.site)) {
        why = "the gun is being taken down";
        return false;
    }
    if (health_.capacityValue(pawn, "Consciousness") <= 0.0f) {
        why = "it can't think";
        return false;
    }
    return true;
}

// At its post: the job goes on (Running) for as long as it stands there; the artillery system does the shooting.
JobSystem::StepResult JobSystem::manWork(Entity pawn, Job& job, std::string& why) {
    if (cellOf(things_.registry().get<Position>(pawn)) != job.cell) {
        why = "the pawn left its post beside the gun";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Man, job.site);
    }
    return StepResult::Running;
}

}  // namespace colony
