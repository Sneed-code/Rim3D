// Doctors, patients and rescue (task H1, docs/design/medicine.md): the TendPatient, Rescue and BedRest jobs and the
// bed and medicine searches their scanners use. Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Inspirations.h"
#include "ColonySim/Map.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr uint8_t kMedTendPickUpStep = 1;
constexpr uint8_t kMedTendGotoPatientStep = 2;
constexpr uint8_t kMedRescueGotoStep = 0;
// How close (metres) a doctor must be to its patient to tend it, or a rescuer to lift it.
constexpr float kMedReach = 1.8f;
// How often (game seconds) a pawn on a droppable job looks for emergency work.
constexpr int64_t kMedEmergencyEvery = 10;

float medDistSq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

float medDistSq(const Position& a, CellPos c) {
    const float dx = a.x - (static_cast<float>(c.x) + 0.5f);
    const float dy = a.y - (static_cast<float>(c.y) + 0.5f);
    return dx * dx + dy * dy;
}

}  // namespace

// ---- Searches -----------------------------------------------------------------------------------

bool JobSystem::isMedicalBed(Entity bed) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(bed) && reg.all_of<Thing>(bed) && isMedical_[reg.get<Thing>(bed).def.value] != 0;
}

Entity JobSystem::bedUnder(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Position* pos = reg.valid(pawn) ? reg.try_get<Position>(pawn) : nullptr;
    if (pos == nullptr) {
        return kNoEntity;
    }
    const CellPos cell = cellOf(*pos);
    if (!map_.inBounds(cell)) {
        return kNoEntity;
    }
    const Entity building = map_.buildingAt(cell);
    return building != kNoEntity && reg.valid(building) && reg.all_of<Bed>(building) ? building : kNoEntity;
}

bool JobSystem::bedOccupied(CellPos cell, Entity except) const {
    const entt::registry& reg = things_.registry();
    for (const auto [pawn, health, pos] : reg.view<const Health, const Position>().each()) {
        if (pawn != except && !health.dead && health.downed && cellOf(pos) == cell) {
            return true;
        }
    }
    for (const auto [pawn, worker] : reg.view<const Worker>().each()) {
        const Job& job = worker.job;
        if (pawn != except && job.cell == cell &&
            (jobIsLyingDown(job) || (job.type == JobType::BedRest && job.step >= 1))) {
            return true;
        }
    }
    return false;
}

bool JobSystem::needsRescue(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Health, Position, Worker>(pawn)) {
        return false;
    }
    const Health& health = reg.get<Health>(pawn);
    const Faction* faction = reg.try_get<Faction>(pawn);
    if (reg.all_of<Kidnapped>(pawn)) {
        return false;  // a raider is carrying it off
    }
    return health.downed && !health.dead && (faction == nullptr || faction->id == kPlayerFaction || reg.all_of<Prisoner>(pawn)) &&
           bedUnder(pawn) == kNoEntity;
}

bool JobSystem::findPatientBed(Entity patient, Entity helper, bool medicalOnly, Entity& bed, CellPos& cell) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(patient) || !reg.all_of<Position>(patient) || !reg.valid(helper) || !reg.all_of<Position>(helper)) {
        return false;
    }
    const Position& at = reg.get<Position>(patient);
    const CellPos from = cellOf(reg.get<Position>(helper));
    int bestRank = -1;
    float bestDist = 0.0f;
    Entity bestBed = kNoEntity;
    CellPos bestCell;
    for (const auto [candidate, thing, owners, footprint] : reg.view<const Thing, const Bed, const Footprint>().each()) {
        if (!things_.spawned(candidate)) {
            continue;
        }
        const bool medical = isMedical_[thing.def.value] != 0;
        if (medicalOnly && !medical) {
            continue;
        }
        // Prison beds are for prisoners and nobody else.
        if (reg.all_of<PrisonBed>(candidate) != reg.all_of<Prisoner>(patient)) {
            continue;
        }
        const int slots = std::max(1, static_cast<int>(defs_.get(thing.def).integer("building.sleepingSlots", 1)));
        const CellRect rect = footprint.rect();
        const int cells = rect.sizeX * rect.sizeY;
        for (int i = 0; i < slots && i < cells; ++i) {
            const bool owned = static_cast<size_t>(i) < owners.owners.size();
            if (owned && owners.owners[static_cast<size_t>(i)] != patient) {
                continue;
            }
            const CellPos c{rect.minX + i % rect.sizeX, rect.minY + i / rect.sizeX, rect.level};
            if (c.level != at.level || !map_.walkable(c) || bedOccupied(c, patient) ||
                reservedByOther(JobTarget{kNoEntity, c}, helper) || avoiding(helper, JobTarget{candidate, CellPos{}}) ||
                !canReach(from, c)) {
                continue;
            }
            const int rank = medical ? 2 : (owned ? 1 : 0);
            const float dist = medDistSq(at, c);
            const bool better = rank > bestRank || (rank == bestRank && (dist < bestDist || (dist == bestDist && candidate < bestBed)));
            if (bestRank < 0 || better) {
                bestRank = rank;
                bestDist = dist;
                bestBed = candidate;
                bestCell = c;
            }
        }
    }
    bed = bestBed;
    cell = bestCell;
    return bestRank >= 0;
}

Entity JobSystem::findMedicine(Entity doctor, Entity patient) const {
    if (medicine_ == nullptr) {
        return kNoEntity;
    }
    const entt::registry& reg = things_.registry();
    const MedicalCare care = medicine_->careOf(patient);
    const Position& at = reg.get<Position>(doctor);
    const CellPos from = cellOf(at);
    Entity best = kNoEntity;
    float bestPotency = 0.0f;
    float bestDist = 0.0f;
    for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        (void)stack;
        const MedicineKind* kind = medicine_->kindOf(thing.def);
        if (kind == nullptr || pos.level != at.level || !medicine_->allows(care, *kind)) {
            continue;
        }
        const float dist = medDistSq(at, pos);
        const bool better = best == kNoEntity || kind->potency > bestPotency ||
                            (kind->potency == bestPotency && (dist < bestDist || (dist == bestDist && item < best)));
        if (!better) {
            continue;
        }
        const JobTarget target{item, CellPos{}};
        if (reservedByOther(target, doctor) || avoiding(doctor, target) || !canReach(from, cellOf(pos))) {
            continue;
        }
        best = item;
        bestPotency = kind->potency;
        bestDist = dist;
    }
    return best;
}

// A downed pawn lies on the bed under it (or on the ground): that decides how fast it heals.
void JobSystem::restUnderPawn(Entity pawn) {
    entt::registry& reg = things_.registry();
    Health* health = reg.try_get<Health>(pawn);
    if (health == nullptr || health->dead) {
        return;
    }
    const Entity bed = bedUnder(pawn);
    health->rest = bed == kNoEntity ? RestKind::Ground : (isMedicalBed(bed) ? RestKind::HospitalBed : RestKind::Bed);
}

void JobSystem::interruptForEmergency(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Job& current = reg.get<Worker>(pawn).job;
    if (medicine_ == nullptr || current.forced || current.handsOn != 0 ||
        (current.type != JobType::Wait && current.type != JobType::HaulToCell && current.type != JobType::Recreate &&
         current.type != JobType::Research && current.type != JobType::DeepWork && current.type != JobType::CleanFilth && !jobIsGather(current.type))) {
        return;
    }
    if ((now() + static_cast<int64_t>(entt::to_integral(pawn))) % kMedEmergencyEvery != 0) {
        return;
    }
    Job job;
    if (!thinkWorkPass(pawn, job, true)) {
        return;
    }
    endJob(pawn, JobEnd::Interrupted, "emergency");
    startJob(pawn, std::move(job));
}

// ---- Tending ------------------------------------------------------------------------------------

bool JobSystem::failCheckTend(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (medicine_ == nullptr || !things_.spawned(job.thing) || !reg.all_of<Health>(job.thing) ||
        reg.get<Health>(job.thing).dead) {
        why = "the patient is gone";
        return false;
    }
    if (medicine_->careOf(job.thing) == MedicalCare::NoCare) {
        why = "the patient wants no care";
        return false;
    }
    if (health_.capacityValue(pawn, "Manipulation") <= 0.0f) {
        why = "it can't use its hands";
        return false;
    }
    if (job.site != kNoEntity) {
        if (job.step <= kMedTendPickUpStep) {
            if (!things_.spawned(job.site)) {
                why = "the medicine is gone";
                return false;
            }
        } else {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            if (carrying == nullptr || carrying->thing != job.site || !reg.valid(job.site)) {
                why = "it lost the medicine";
                return false;
            }
        }
    }
    return true;
}

JobSystem::StepResult JobSystem::walkToMedicine(Entity pawn, Job& job, std::string& why) {
    if (job.site == kNoEntity) {
        return StepResult::Next;
    }
    return walkTo(pawn, job, cellOf(things_.registry().get<Position>(job.site)), why);
}

JobSystem::StepResult JobSystem::pickUpMedicine(Entity pawn, Job& job, std::string& why) {
    if (job.site == kNoEntity) {
        return StepResult::Next;
    }
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(job.site)) != cellOf(reg.get<Position>(pawn))) {
        if (job.repaths < maxRepaths_) {
            ++job.repaths;
            ++stats_.repaths;
            job.step = 0;  // the stack was moved while the pawn walked: go to where it is now
            job.walk = 0;
            return StepResult::Running;
        }
        why = "the medicine isn't where the pawn stands";
        return StepResult::Failed;
    }
    Entity carried = job.site;
    if (reg.get<StackCount>(job.site).count > 1) {
        carried = things_.splitOff(job.site, 1);
        if (carried == kNoEntity) {
            why = "it couldn't take one medicine from the stack";
            return StepResult::Failed;
        }
    } else {
        things_.despawn(job.site);
    }
    release(pawn, job);
    job.site = carried;
    reserve(pawn, job);
    reg.emplace_or_replace<Carrying>(pawn, Carrying{carried});
    pawnDid(pawn, PawnAction::PickUp, carried);
    return StepResult::Next;
}

// Walks to the patient (or, for a rescue, to the downed pawn): done when the pawn is within reach of it.
JobSystem::StepResult JobSystem::walkToPatient(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (job.thing == pawn) {
        return StepResult::Next;  // tending itself
    }
    const Position& at = reg.get<Position>(pawn);
    const Position& there = reg.get<Position>(job.thing);
    if (job.walk == 0 && medDistSq(at, there) <= kMedReach * kMedReach) {
        return StepResult::Next;
    }
    return walkTo(pawn, job, cellOf(there), why);
}

JobSystem::StepResult JobSystem::tendWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Entity patient = job.thing;
    if (!health_.needsTending(patient)) {
        return StepResult::Next;  // someone else did, or it healed: nothing left to do
    }
    if (patient != pawn && medDistSq(reg.get<Position>(pawn), reg.get<Position>(patient)) > kMedReach * kMedReach) {
        if (job.repaths < maxRepaths_) {
            ++job.repaths;
            ++stats_.repaths;
            job.step = kMedTendGotoPatientStep;  // it walked off: follow it
            job.walk = 0;
            return StepResult::Running;
        }
        why = "the patient moved away";
        return StepResult::Failed;
    }
    const MedicineRules& rules = medicine_->rules();
    const double manipulation = health_.capacityValue(pawn, "Manipulation");
    const double speed = manipulation * lightWorkFactor(pawn);
    if (speed <= 0.0) {
        why = "it can't see to work";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Tend, patient);
    }
    job.progress += static_cast<float>(speed);
    if (job.progress < static_cast<float>(rules.tendWorkSeconds)) {
        return StepResult::Running;
    }

    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    const Entity medicineItem = job.site != kNoEntity && carrying != nullptr && carrying->thing == job.site ? job.site : kNoEntity;
    const MedicineKind* kind = medicineItem != kNoEntity ? medicine_->kindOf(reg.get<Thing>(medicineItem).def) : nullptr;
    const Entity bed = bedUnder(patient);
    const float bedOffset = bed != kNoEntity ? static_cast<float>(defs_.get(reg.get<Thing>(bed).def).number("stats.medicalTendQualityOffset", 0.0)) : 0.0f;
    // The roll comes from the second, the doctor and the patient, so it doesn't depend on who is asked first.
    Rng rng(splitMix64(static_cast<uint64_t>(now()) ^ (static_cast<uint64_t>(entt::to_integral(pawn)) << 32) ^
                       static_cast<uint64_t>(entt::to_integral(patient)) ^ 0x7E4D1C8AULL));
    const float quality = medicine_->tendQuality(skillLevel(pawn, "Medicine"), static_cast<float>(manipulation), kind, bedOffset,
                                                 patient == pawn, rng.unit());
    const int tended = health_.tendAll(patient, quality);
    DefId used;
    // An inspired doctor's tend uses no medicine (the inspiration is used up).
    const bool free = tended > 0 && medicineItem != kNoEntity && inspirations_ != nullptr && inspirations_->takeFreeMedicine(pawn);
    if (tended > 0 && medicineItem != kNoEntity) {
        used = reg.get<Thing>(medicineItem).def;
    }
    if (tended > 0 && medicineItem != kNoEntity) {
        release(pawn, job);
        job.site = kNoEntity;
        reserve(pawn, job);
        if (free) {
            dropCarried(pawn);  // kept: it lies where the doctor stands
        } else {
            reg.remove<Carrying>(pawn);
            things_.destroy(medicineItem);
        }
    }
    if (tended > 0) {
        Event event;
        event.type = EventType::PawnTended;
        event.thing = pawn;
        event.target = patient;
        event.def = used;
        event.cell = cellOf(reg.get<Position>(patient));
        event.amount = quality;
        event.message = used.valid() ? "medicine" : "no medicine";
        events_.push_back(std::move(event));
        learnSkill(pawn, "Medicine", rules.tendXp);
    }
    return StepResult::Next;
}

// ---- Rescue -------------------------------------------------------------------------------------

bool JobSystem::failCheckRescue(Entity, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(job.thing) || !reg.all_of<Health>(job.thing) || !reg.get<Health>(job.thing).downed ||
        reg.get<Health>(job.thing).dead) {
        why = "the patient is up or gone";
        return false;
    }
    if (reg.all_of<Kidnapped>(job.thing)) {
        why = "a raider carried the patient off";
        return false;
    }
    if (!things_.spawned(job.chair) || !reg.all_of<Bed>(job.chair)) {
        why = "the bed is gone";
        return false;
    }
    if (bedOccupied(job.cell, job.thing)) {
        why = "someone lies in the bed";
        return false;
    }
    return true;
}

JobSystem::StepResult JobSystem::liftPawn(Entity pawn, Job& job, std::string&) {
    entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    Position& there = reg.get<Position>(job.thing);
    if (medDistSq(at, there) > kMedReach * kMedReach && job.repaths < maxRepaths_) {
        ++job.repaths;
        ++stats_.repaths;
        job.step = kMedRescueGotoStep;  // it was moved: go to it again
        job.walk = 0;
        return StepResult::Running;
    }
    if (Mover* mover = reg.try_get<Mover>(job.thing)) {
        mover->path.clear();
        mover->next = 0;
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
    there.x = at.x;
    there.y = at.y;
    there.level = at.level;
    pawnDid(pawn, PawnAction::LiftPawn, job.thing);
    return StepResult::Next;
}

// The carried pawn goes where the rescuer goes: its position follows the rescuer's every second.
JobSystem::StepResult JobSystem::carryPawn(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(pawn);
    Position& there = reg.get<Position>(job.thing);
    there.x = at.x;
    there.y = at.y;
    there.level = at.level;
    return walkTo(pawn, job, job.cell, why);
}

JobSystem::StepResult JobSystem::layPawn(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    Position& there = reg.get<Position>(job.thing);
    there.x = static_cast<float>(job.cell.x) + 0.5f;
    there.y = static_cast<float>(job.cell.y) + 0.5f;
    there.level = job.cell.level;
    if (Health* health = reg.try_get<Health>(job.thing)) {
        health->rest = isMedicalBed(job.chair) ? RestKind::HospitalBed : RestKind::Bed;
    }
    pawnDid(pawn, PawnAction::LayPawn, job.thing);
    if (job.type == JobType::CapturePrisoner) {
        std::string error;
        if (prisoners_ == nullptr || !prisoners_->imprison(job.thing, job.chair, pawn, error)) {
            why = error.empty() ? "there are no prisoners here" : error;
            return StepResult::Failed;
        }
    }
    return StepResult::Next;
}

// ---- Bed rest -----------------------------------------------------------------------------------

bool JobSystem::failCheckBedRest(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(job.thing) || !reg.all_of<Bed, Footprint>(job.thing)) {
        why = "the bed is gone";
        return false;
    }
    const CellRect rect = reg.get<Footprint>(job.thing).rect();
    if (job.cell.x < rect.minX || job.cell.x >= rect.minX + rect.sizeX || job.cell.y < rect.minY ||
        job.cell.y >= rect.minY + rect.sizeY || job.cell.level != rect.level) {
        why = "the bed moved";
        return false;
    }
    (void)pawn;
    return true;
}

JobSystem::StepResult JobSystem::bedRest(Entity pawn, Job& job, std::string&) {
    entt::registry& reg = things_.registry();
    if (!needsBedRest(pawn)) {
        return StepResult::Next;
    }
    if (Health* health = reg.try_get<Health>(pawn)) {
        health->rest = isMedicalBed(job.thing) ? RestKind::HospitalBed : RestKind::Bed;
    }
    if (job.until == 0) {
        job.until = now() + 1;  // marks that the pawn has lain down
        pawnDid(pawn, PawnAction::LieDown, job.thing);
    }
    return StepResult::Running;
}

}  // namespace colony
