// Operations (task H3, docs/design/surgery.md): the Operate job, which fetches the part to fit and a medicine, walks to a
// patient lying in bed and does a planned operation. Part of the job system.
#include "ColonySim/Jobs.h"

#include "ColonySim/Bills.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Random.h"
#include "ColonySim/RoomStats.h"
#include "ColonySim/Surgery.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float surgJobDistSq(const Position& a, const Position& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

}  // namespace

bool JobSystem::needsBedRest(Entity pawn) const {
    return health_.needsRest(pawn) || (surgery_ != nullptr && surgery_->pending(pawn));
}

bool JobSystem::lyingInBed(Entity patient) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(patient) || !reg.all_of<Health, Worker>(patient) || bedUnder(patient) == kNoEntity) {
        return false;
    }
    const Job& job = reg.get<Worker>(patient).job;
    return reg.get<Health>(patient).downed || jobIsLyingDown(job) || (job.type == JobType::BedRest && job.step >= 1);
}

Entity JobSystem::findPartItem(Entity doctor, DefId def) const {
    const entt::registry& reg = things_.registry();
    const Position& at = reg.get<Position>(doctor);
    const CellPos from = cellOf(at);
    Entity best = kNoEntity;
    float bestDist = 0.0f;
    for (const auto [item, thing, pos, stack] : reg.view<const Thing, const Position, const StackCount>().each()) {
        (void)stack;
        if (thing.def != def || pos.level != at.level || !things_.spawned(item)) {
            continue;
        }
        const float dist = surgJobDistSq(at, pos);
        if (best != kNoEntity && (dist > bestDist || (dist == bestDist && item > best))) {
            continue;
        }
        const JobTarget target{item, CellPos{}};
        if (reservedByOther(target, doctor) || avoiding(doctor, target) || !canReach(from, cellOf(pos))) {
            continue;
        }
        best = item;
        bestDist = dist;
    }
    return best;
}

bool JobSystem::planOperation(Entity doctor, Entity patient, Job& out) {
    entt::registry& reg = things_.registry();
    if (surgery_ == nullptr || medicine_ == nullptr || doctor == patient || !lyingInBed(patient) || !canUseHands(doctor)) {
        return false;
    }
    const Operations* ops = surgery_->of(patient);
    if (ops == nullptr) {
        return false;
    }
    const Entity bed = bedUnder(patient);
    const CellRect rect = reg.get<Footprint>(bed).rect();
    CellPos stand;
    if (!construction_.workCell(rect, cellOf(reg.get<Position>(doctor)), stand)) {
        return false;
    }
    const int skill = skillLevel(doctor, "Medicine");
    for (const Operation& op : ops->list) {
        const SurgeryRecipe* recipe = surgery_->recipe(op.recipe);
        std::string why;
        if (recipe == nullptr || skill < recipe->minSkill || !surgery_->eligible(patient, *recipe, op.part, why)) {
            continue;
        }
        std::vector<BillPick> picks;
        if (recipe->install) {
            const Entity part = findPartItem(doctor, recipe->ingredient);
            if (part == kNoEntity) {
                continue;
            }
            picks.push_back(BillPick{part, 1});
        }
        if (const Entity medicine = findMedicine(doctor, patient); medicine != kNoEntity) {
            picks.push_back(BillPick{medicine, 1});
        }
        out = Job{};
        out.type = JobType::Operate;
        out.site = patient;
        out.bill = op.id;
        out.cell = stand;
        if (!picks.empty()) {
            out.thing = picks.front().item;
            out.count = picks.front().count;
            out.picks.assign(picks.begin() + 1, picks.end());
        }
        return true;
    }
    return false;
}

bool JobSystem::failCheckOperate(Entity pawn, Job& job, std::string& why) {
    const entt::registry& reg = things_.registry();
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    if (surgery_ == nullptr || !things_.spawned(job.site) || !reg.all_of<Health>(job.site) || reg.get<Health>(job.site).dead) {
        why = "the patient is gone";
        return false;
    }
    const Operation* op = surgery_->find(job.site, job.bill);
    const SurgeryRecipe* recipe = op != nullptr ? surgery_->recipe(op->recipe) : nullptr;
    std::string reason;
    if (recipe == nullptr || !surgery_->eligible(job.site, *recipe, op->part, reason)) {
        why = "the operation was called off";
        return false;
    }
    if (!lyingInBed(job.site)) {
        why = "the patient is not in bed";
        return false;
    }
    if (job.thing != kNoEntity) {
        if (job.step <= kOperatePickUpStep) {
            if (!things_.spawned(job.thing)) {
                why = "what it was to fetch is gone";
                return false;
            }
        } else if (job.step <= kOperateDepositStep) {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
                why = "it lost what it carried";
                return false;
            }
        }
    }
    for (const BillPick& pick : job.picks) {
        if (!things_.spawned(pick.item)) {
            why = "something it was to fetch is gone";
            return false;
        }
    }
    return true;
}

JobSystem::StepResult JobSystem::operateGoto(Entity pawn, Job& job, std::string& why) {
    if (job.thing == kNoEntity) {
        return StepResult::Next;
    }
    return walkTo(pawn, job, cellOf(things_.registry().get<Position>(job.thing)), why);
}

JobSystem::StepResult JobSystem::operatePickUp(Entity pawn, Job& job, std::string& why) {
    return job.thing == kNoEntity ? StepResult::Next : pickUp(pawn, job, why);
}

JobSystem::StepResult JobSystem::operateDeposit(Entity pawn, Job& job, std::string& why) {
    return job.thing == kNoEntity ? StepResult::Next : depositIngredient(pawn, job, why);
}

JobSystem::StepResult JobSystem::operateWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the patient";
        return StepResult::Failed;
    }
    const Operation* op = surgery_->find(job.site, job.bill);
    const SurgeryRecipe* recipe = op != nullptr ? surgery_->recipe(op->recipe) : nullptr;
    if (recipe == nullptr) {
        why = "the operation was called off";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Operate, job.site);
    }
    const Recipe* billRecipe = bills_->recipe(recipe->def);
    const double speed = (billRecipe != nullptr ? bills_->workSpeed(pawn, *billRecipe) : 1.0) *
                         health_.capacityValue(pawn, "Manipulation") * lightWorkFactor(pawn);
    if (speed <= 0.0) {
        why = "it can't see to work";
        return StepResult::Failed;
    }
    job.progress += static_cast<float>(speed);
    return job.progress < recipe->work ? StepResult::Running : StepResult::Next;
}

JobSystem::StepResult JobSystem::operateFinish(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    const Entity patient = job.site;
    const Operation* found = surgery_->find(patient, job.bill);
    const SurgeryRecipe* recipe = found != nullptr ? surgery_->recipe(found->recipe) : nullptr;
    if (recipe == nullptr) {
        why = "the operation was called off";
        return StepResult::Failed;
    }
    const Operation op = *found;
    // What was brought is used up: the part to fit goes in, the medicine is spent whatever happens.
    const MedicineKind* medicine = nullptr;
    for (const BillUsed& used : job.used) {
        if (const MedicineKind* kind = medicine_->kindOf(used.def)) {
            medicine = kind;
        }
    }
    job.used.clear();

    SurgeryConditions conditions;
    conditions.skill = skillLevel(pawn, "Medicine");
    conditions.manipulation = health_.capacityValue(pawn, "Manipulation");
    conditions.sight = health_.capacityValue(pawn, "Sight");
    conditions.medicine = medicine;
    const Entity bed = bedUnder(patient);
    float bedOffset = 0.0f;
    if (bed != kNoEntity) {
        const Def& bedDef = defs_.get(reg.get<Thing>(bed).def);
        conditions.bedFactor = static_cast<float>(bedDef.number("stats.surgerySuccessChanceFactor", 1.0));
        bedOffset = static_cast<float>(bedDef.number("stats.medicalTendQualityOffset", 0.0));
    }
    if (roomStats_ != nullptr) {
        const CellPos cell = cellOf(reg.get<Position>(patient));
        const RoomStatValues* room = roomStats_->statsAt(cell);
        conditions.cleanliness = room != nullptr ? room->cleanliness : roomStats_->cellCleanliness(cell);
    }
    const float chance = surgery_->successChance(*recipe, conditions);
    // The rolls come from the second, the doctor, the patient and the operation, so who is asked first can't change them.
    Rng rng(splitMix64(static_cast<uint64_t>(now()) ^ (static_cast<uint64_t>(entt::to_integral(pawn)) << 32) ^
                       static_cast<uint64_t>(entt::to_integral(patient)) ^ (static_cast<uint64_t>(job.bill) << 48) ^ 0x4F50455241ULL));
    const bool success = rng.unit() < chance;
    const float deathRoll = rng.unit();
    const float quality = medicine_->tendQuality(conditions.skill, conditions.manipulation, medicine, bedOffset, false, rng.unit());
    const SurgeryResult result = surgery_->perform(patient, job.bill, chance, success, quality, deathRoll);
    if (result.returned.valid()) {
        dropNewItems(pawn, result.returned, 1);
    }
    if (result.done) {
        Event event;
        event.type = EventType::OperationDone;
        event.thing = pawn;
        event.target = patient;
        event.def = recipe->def;
        event.part = op.part;
        event.cell = cellOf(reg.get<Position>(patient));
        event.amount = chance;
        event.message = result.died ? "the patient died" : (result.success ? "success" : "failed");
        events_.push_back(std::move(event));
        learnSkill(pawn, "Medicine", recipe->xp > 0.0f ? recipe->xp : surgery_->numbers().xp);
    }
    return StepResult::Next;
}

}  // namespace colony
