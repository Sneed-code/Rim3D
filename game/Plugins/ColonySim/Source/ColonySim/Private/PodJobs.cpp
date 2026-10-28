// The LoadPod job and its scanner (docs/design/transport-pods.md): a hauler carries a stack the player
// named into a transport pod being loaded, as RimWorld's WorkGiver_LoadTransporters. Part of the job system.
#include "ColonySim/Construction.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Things.h"
#include "ColonySim/TransportPods.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float podJobsDistSq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// Stacks named for pods that are loading, nearest first; the pod is the group's nearest to the stack.
class LoadPodsGiver final : public WorkGiver {
public:
    const char* task() const override { return "LoadPods"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const TransportPodSystem* pods = jobs.pods();
        if (pods == nullptr || pods->groups().empty() || !jobs.canUseHands(pawn)) {
            return;
        }
        const entt::registry& reg = jobs.things().registry();
        const Position& at = reg.get<Position>(pawn);
        for (const PodGroup& g : pods->groups()) {
            if (g.state != PodGroupState::Loading) {
                continue;
            }
            for (const Entity stack : g.wanted) {
                if (!jobs.things().spawned(stack)) {
                    continue;  // being carried
                }
                const Position& pos = reg.get<Position>(stack);
                if (pos.level != at.level) {
                    continue;
                }
                out.push_back(WorkCandidate{JobTarget{stack, CellPos{}}, podJobsDistSq(at, pos), 0, 0, false, CellPos{}});
            }
        }
    }

    bool makeJob(JobSystem& jobs, Entity pawn, const WorkCandidate& candidate, Job& out) override {
        const TransportPodSystem* pods = jobs.pods();
        const entt::registry& reg = jobs.things().registry();
        const Entity stack = candidate.target.thing;
        if (pods == nullptr || !jobs.things().spawned(stack)) {
            return false;
        }
        const CellPos from = cellOf(reg.get<Position>(stack));
        const Entity pod = pods->podWanting(stack, from);
        if (pod == kNoEntity) {
            return false;
        }
        CellPos stand;
        if (!jobs.construction().workCell(reg.get<Footprint>(pod).rect(), from, stand)) {
            return false;
        }
        out = Job{};
        out.type = JobType::LoadPod;
        out.thing = stack;
        out.site = pod;
        out.cell = stand;
        out.count = std::min(jobs.carryLimit(pawn, reg.get<Thing>(stack).def), reg.get<StackCount>(stack).count);
        return out.count > 0;
    }
};

}  // namespace

void addPodWorkGivers(std::vector<std::unique_ptr<WorkGiver>>& givers) {
    givers.push_back(std::make_unique<LoadPodsGiver>());
}

bool JobSystem::failCheckLoadPod(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (pods_ == nullptr || !things_.spawned(job.site) || !pods_->podInGroup(job.site)) {
        why = "the pod isn't loading any more";
        return false;
    }
    if (job.step <= kLoadPodPickUpStep) {
        if (!things_.spawned(job.thing) || pods_->podWanting(job.thing, cellOf(reg.get<Position>(job.thing))) == kNoEntity) {
            why = "the stack isn't wanted in a pod any more";
            return false;
        }
    } else {
        const Carrying* carrying = reg.try_get<Carrying>(pawn);
        if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
            why = "it lost what it carried";
            return false;
        }
    }
    return true;
}

// Puts the carried stack into the pod: it leaves the pawn's hands and is off the map, in the pod's group.
JobSystem::StepResult JobSystem::loadPodWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the pod";
        return StepResult::Failed;
    }
    const Entity carried = job.thing;
    if (!pods_->putIn(job.site, carried)) {
        why = "the pod isn't loading any more";
        return StepResult::Failed;
    }
    reg.remove<Carrying>(pawn);
    pawnDid(pawn, PawnAction::PutDown, carried);
    return StepResult::Next;
}

}  // namespace colony
