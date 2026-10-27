// Parties on the job system (docs/design/social.md): a wedding calls the colonists to a spot where they stand
// together, chat (the social system talks to anyone in earshot) and enjoy themselves.

#include "ColonySim/Jobs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

bool JobSystem::partyCellOk(Entity pawn, CellPos cell) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<Position>(pawn) && map_.walkable(cell) && canReach(cellOf(reg.get<Position>(pawn)), cell);
}

bool JobSystem::startParty(Entity pawn, CellPos cell, int64_t until, float joyPerHour, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Worker>(pawn) || !things_.spawned(pawn)) {
        error = "that pawn can't go to a party";
        return false;
    }
    if (reg.all_of<PlayerControl>(pawn)) {
        error = "the player is controlling that pawn";
        return false;
    }
    const Worker& worker = reg.get<Worker>(pawn);
    if (jobIsLyingDown(worker.job)) {
        error = "that pawn is asleep";
        return false;
    }
    if (!partyCellOk(pawn, cell)) {
        error = "the pawn can't reach the party";
        return false;
    }
    if (worker.job.type != JobType::None) {
        endJob(pawn, JobEnd::Interrupted, "off to a party");
    }
    Job job;
    job.type = JobType::Party;
    job.cell = cell;
    job.until = until;
    job.progress = std::max(0.0f, joyPerHour);
    job.forced = true;  // hunger and tiredness wait until the party is over
    startJob(pawn, std::move(job));
    return true;
}

bool JobSystem::failCheckParty(Entity pawn, Job&, std::string& why) {
    if (!things_.registry().all_of<PlayerControl>(pawn)) {
        return true;
    }
    why = "taken over by the player";
    return false;
}

// The pawn stays put; its recreation need rises at the party's rate. The job ends when the party does.
JobSystem::StepResult JobSystem::partyWork(Entity pawn, Job& job, std::string&) {
    if (now() >= job.until) {
        return StepResult::Next;
    }
    if (Joy* joy = things_.registry().try_get<Joy>(pawn)) {
        joy->level = std::min(1.0f, joy->level + job.progress / static_cast<float>(kSecondsPerHour));
    }
    return StepResult::Running;
}

}  // namespace colony
