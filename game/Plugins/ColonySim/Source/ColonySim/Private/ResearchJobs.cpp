// The Research job: walk to a research bench, sit beside it and add points to the colony's current
// project until it is done (task C21, docs/design/research.md). Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Research.h"
#include "ColonySim/Things.h"

namespace colony {

bool JobSystem::failCheckResearch(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (research_ == nullptr || !things_.spawned(job.site) || reg.all_of<ConstructionSite>(job.site) ||
        research_->benchInfo(reg.get<Thing>(job.site).def) == nullptr) {
        why = "the research bench is gone";
        return false;
    }
    if (reg.all_of<MarkedForDeconstruct>(job.site)) {
        why = "the research bench is being taken down";
        return false;
    }
    if (!buildingRuns(job.site)) {
        why = "the research bench has no power";
        return false;
    }
    if (health_.capacityValue(pawn, "Consciousness") <= 0.0f) {
        why = "it can't think";
        return false;
    }
    return true;
}

// One game second of work. The job ends (done, not failed) when there is nothing for this bench to do any
// more: the project was finished, put down, or needs a better bench than this one.
JobSystem::StepResult JobSystem::researchWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the research bench";
        return StepResult::Failed;
    }
    const ResearchBenchInfo* bench = research_->benchInfo(reg.get<Thing>(job.site).def);
    if (bench == nullptr || !research_->benchCanWork(*bench)) {
        return StepResult::Next;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Research, job.site);
    }
    const double points = research_->speedAt(pawn, *bench) * health_.capacityValue(pawn, "Consciousness");
    return research_->addProgress(points, pawn, events_) ? StepResult::Next : StepResult::Running;
}

}  // namespace colony
