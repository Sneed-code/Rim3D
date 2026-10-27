// Medics and firefighters on duty while the alarm is on (task F10, docs/design/field-care.md): what they do instead of
// the think list. Part of the job system; FieldCare says who is on duty and which targets are safe.
#include "ColonySim/Jobs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/FieldCare.h"
#include "ColonySim/Things.h"

#include <cstdint>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// How often (game seconds) a pawn standing by looks for duty work again, and how long one StandBy lasts, the walk
// included, before its place behind the line is worked out again.
constexpr int64_t kDutyLookEvery = 5;
constexpr int64_t kDutyStandSeconds = 60;
// TendPatient's work step (MedicineJobs.cpp).
constexpr uint8_t kDutyTendWorkStep = 3;

}  // namespace

void JobSystem::dutyStep(Entity pawn, int dutyWork) {
    entt::registry& reg = things_.registry();
    const Job& current = reg.get<Worker>(pawn).job;
    // What it was doing when the alarm sounded ends, unless it is already its duty (a doctor tending, say).
    if (current.type != JobType::None && current.type != JobType::StandBy &&
        (current.workType != workTypes_[static_cast<size_t>(dutyWork)].def || current.forced)) {
        endJob(pawn, JobEnd::Interrupted, "alarm");
    }
    // Standing by, it looks for work every few seconds and takes it at once.
    if (reg.get<Worker>(pawn).job.type == JobType::StandBy &&
        (now() + static_cast<int64_t>(entt::to_integral(pawn))) % kDutyLookEvery == 0) {
        Job job;
        if (thinkWorkPass(pawn, job, false, dutyWork)) {
            endJob(pawn, JobEnd::Interrupted, "duty");
            startJob(pawn, std::move(job));
        }
    }
    if (reg.get<Worker>(pawn).job.type == JobType::None) {
        Job job;
        if (!thinkWorkPass(pawn, job, false, dutyWork)) {
            job = Job{};
            job.type = JobType::StandBy;
            job.cell = fieldCare_->rallyPoint(pawn);
            job.until = now() + kDutyStandSeconds;
        }
        startJob(pawn, std::move(job));
    }
    tickJob(pawn);
    // Tending, it stands still by its patient (jobHoldsStill in JobSystem.cpp); nobody pushes it off.
    const Job& job = reg.get<Worker>(pawn).job;
    reg.get<Mover>(pawn).hold = job.type == JobType::TendPatient && job.step == kDutyTendWorkStep;
}

bool JobSystem::failCheckStandBy(Entity pawn, Job&, std::string& why) {
    if (fieldCare_ == nullptr || fieldCare_->dutyWorkType(pawn) < 0) {
        why = "off duty";
        return false;
    }
    return true;
}

}  // namespace colony
