#include "ColonySim/Jobs.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Social.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Talents.h"
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
enum class JobStepKind : uint8_t {
    Wait,
    GotoThing,
    PickUp,
    CarryToCell,  // walk to the job's cell, carrying
    Place,
    WalkToSpot,
    Eat,
    Lie,
    GotoCell,     // walk to the job's cell
    Deliver,
    BuildWork,
    DeconstructWork,
    GotoTouch,   // walk to a cell beside the job's rock
    MineWork,
    PlantWork,
    DepositIngredient,
    BillWork,
    BillFinish,
    Play,  // recreation: stay for the activity's time (RecreationJobs.cpp)
    ResearchWork,  // research: add points to the current project (ResearchJobs.cpp)
    CleanWork,
    TendMedicine,      // medicine jobs (MedicineJobs.cpp)
    TendPickUp,
    TendGotoPatient,
    TendWork,
    LiftPawn,
    CarryPawn,
    LayPawn,
    BedRestLie,
    ApparelGoto,  // walk to the clothes (none to take off a worn piece)
    ApparelWork,  // put on or take off
    StripWork,    // take the clothes and weapon off a downed or dead pawn
    ApproachAnimal,  // walk up to the job's animal (HusbandryJobs.cpp)
    HandleWork,      // tame, train, kill or feed it, standing beside it
    ExtinguishWork,  // beat the fire in the job's cell out (FireJobs.cpp)
    ApproachPrisoner,  // walk up to the job's prisoner (`site`) (PrisonerJobs.cpp)
    FeedWork,          // hand the carried food over
    ChatWork           // talk to it
};

constexpr JobStepKind kJobsWaitSteps[] = {JobStepKind::Wait};
constexpr JobStepKind kJobsHaulSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::CarryToCell,
                                          JobStepKind::Place};
constexpr JobStepKind kJobsIngestSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::WalkToSpot,
                                            JobStepKind::Eat};
constexpr JobStepKind kJobsLayDownSteps[] = {JobStepKind::WalkToSpot, JobStepKind::Lie};
constexpr JobStepKind kJobsMineSteps[] = {JobStepKind::GotoTouch, JobStepKind::MineWork};
constexpr JobStepKind kJobsPlantSteps[] = {JobStepKind::GotoTouch, JobStepKind::PlantWork};
constexpr JobStepKind kJobsCleanSteps[] = {JobStepKind::GotoCell, JobStepKind::CleanWork};
constexpr uint8_t kJobsHaulPickUpStep = 1;
constexpr uint8_t kJobsHaulCarryStep = 2;
constexpr JobStepKind kJobsDeliverSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::CarryToCell,
                                             JobStepKind::Deliver};
constexpr uint8_t kJobsDeliverPickUpStep = 1;
constexpr JobStepKind kJobsBuildSteps[] = {JobStepKind::GotoCell, JobStepKind::BuildWork};
constexpr JobStepKind kJobsDeconstructSteps[] = {JobStepKind::GotoCell, JobStepKind::DeconstructWork};
constexpr JobStepKind kJobsDoBillSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::CarryToCell,
                                            JobStepKind::DepositIngredient, JobStepKind::BillWork,
                                            JobStepKind::BillFinish};
static_assert(kDoBillPickUpStep == 1 && kDoBillDepositStep == 3, "the DoBill steps moved");
constexpr JobStepKind kJobsRecreateSteps[] = {JobStepKind::WalkToSpot, JobStepKind::Play};
constexpr JobStepKind kJobsResearchSteps[] = {JobStepKind::GotoCell, JobStepKind::ResearchWork};
constexpr JobStepKind kJobsBedRestSteps[] = {JobStepKind::WalkToSpot, JobStepKind::BedRestLie};
constexpr JobStepKind kJobsTendSteps[] = {JobStepKind::TendMedicine, JobStepKind::TendPickUp, JobStepKind::TendGotoPatient,
                                          JobStepKind::TendWork};
constexpr JobStepKind kJobsRescueSteps[] = {JobStepKind::TendGotoPatient, JobStepKind::LiftPawn, JobStepKind::CarryPawn,
                                            JobStepKind::LayPawn};

// Work types with a natural priority from here up are the emergency ones (firefight, patient, doctor, bed rest): at one
// priority level their scanners are asked before the others, whatever the distance (RimWorld asks work types in
// order; the rest of ours go nearest first). Ours.
constexpr int kJobsUrgentWorkFrom = 1100;
constexpr JobStepKind kJobsWearSteps[] = {JobStepKind::ApparelGoto, JobStepKind::ApparelWork};
constexpr JobStepKind kJobsStripSteps[] = {JobStepKind::GotoThing, JobStepKind::StripWork};
constexpr JobStepKind kJobsHandleFoodSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::ApproachAnimal,
                                                JobStepKind::HandleWork};
constexpr JobStepKind kJobsHandleSteps[] = {JobStepKind::ApproachAnimal, JobStepKind::HandleWork};
constexpr JobStepKind kJobsExtinguishSteps[] = {JobStepKind::GotoTouch, JobStepKind::ExtinguishWork};
constexpr JobStepKind kJobsFeedPrisonerSteps[] = {JobStepKind::GotoThing, JobStepKind::PickUp, JobStepKind::ApproachPrisoner,
                                                  JobStepKind::FeedWork};
constexpr JobStepKind kJobsChatSteps[] = {JobStepKind::ApproachPrisoner, JobStepKind::ChatWork};

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
// Walking speed lost at a full load (ours; RimWorld has none for carried items).
constexpr double kJobsCarrySlowdown = 0.25;

struct JobsSteps {
    const JobStepKind* steps;
    int count;
};

JobsSteps jobsStepsOf(JobType type) {
    switch (type) {
        case JobType::None: return {nullptr, 0};
        case JobType::Wait: return {kJobsWaitSteps, static_cast<int>(std::size(kJobsWaitSteps))};
        case JobType::HaulToCell: return {kJobsHaulSteps, static_cast<int>(std::size(kJobsHaulSteps))};
        case JobType::Ingest: return {kJobsIngestSteps, static_cast<int>(std::size(kJobsIngestSteps))};
        case JobType::LayDown: return {kJobsLayDownSteps, static_cast<int>(std::size(kJobsLayDownSteps))};
        case JobType::DeliverResources: return {kJobsDeliverSteps, static_cast<int>(std::size(kJobsDeliverSteps))};
        case JobType::BuildFrame: return {kJobsBuildSteps, static_cast<int>(std::size(kJobsBuildSteps))};
        case JobType::Deconstruct: return {kJobsDeconstructSteps, static_cast<int>(std::size(kJobsDeconstructSteps))};
        case JobType::Mine: return {kJobsMineSteps, static_cast<int>(std::size(kJobsMineSteps))};
        case JobType::CutPlant:
        case JobType::SowCrop:
        case JobType::HarvestPlant: return {kJobsPlantSteps, static_cast<int>(std::size(kJobsPlantSteps))};
        case JobType::DoBill: return {kJobsDoBillSteps, static_cast<int>(std::size(kJobsDoBillSteps))};
        case JobType::Recreate: return {kJobsRecreateSteps, static_cast<int>(std::size(kJobsRecreateSteps))};
        case JobType::Research: return {kJobsResearchSteps, static_cast<int>(std::size(kJobsResearchSteps))};
        case JobType::CleanFilth: return {kJobsCleanSteps, static_cast<int>(std::size(kJobsCleanSteps))};
        case JobType::BedRest: return {kJobsBedRestSteps, static_cast<int>(std::size(kJobsBedRestSteps))};
        case JobType::TendPatient: return {kJobsTendSteps, static_cast<int>(std::size(kJobsTendSteps))};
        case JobType::Rescue: return {kJobsRescueSteps, static_cast<int>(std::size(kJobsRescueSteps))};
        case JobType::WearApparel: return {kJobsWearSteps, static_cast<int>(std::size(kJobsWearSteps))};
        case JobType::StripPawn: return {kJobsStripSteps, static_cast<int>(std::size(kJobsStripSteps))};
        case JobType::TameAnimal:
        case JobType::TrainAnimal:
        case JobType::FeedAnimal: return {kJobsHandleFoodSteps, static_cast<int>(std::size(kJobsHandleFoodSteps))};
        case JobType::SlaughterAnimal:
        case JobType::GatherAnimal: return {kJobsHandleSteps, static_cast<int>(std::size(kJobsHandleSteps))};
        case JobType::ExtinguishFire: return {kJobsExtinguishSteps, static_cast<int>(std::size(kJobsExtinguishSteps))};
        case JobType::CapturePrisoner: return {kJobsRescueSteps, static_cast<int>(std::size(kJobsRescueSteps))};
        case JobType::FeedPrisoner: return {kJobsFeedPrisonerSteps, static_cast<int>(std::size(kJobsFeedPrisonerSteps))};
        case JobType::WardenInteract: return {kJobsChatSteps, static_cast<int>(std::size(kJobsChatSteps))};
    }
    return {nullptr, 0};
}

// Job steps the pawn does standing still, in its working cell: nobody may push it out of it.
bool jobHoldsStill(const Job& job) {
    const JobsSteps steps = jobsStepsOf(job.type);
    if (job.type == JobType::None || job.step >= steps.count) {
        return false;
    }
    switch (steps.steps[job.step]) {
        case JobStepKind::BuildWork:
        case JobStepKind::DeconstructWork:
        case JobStepKind::MineWork:
        case JobStepKind::PlantWork:
        case JobStepKind::BillWork:
        case JobStepKind::ResearchWork:
        case JobStepKind::CleanWork:
        case JobStepKind::ApparelWork:
        case JobStepKind::StripWork:
        case JobStepKind::HandleWork:
        case JobStepKind::FeedWork:
        case JobStepKind::ChatWork:
        case JobStepKind::Eat:
        case JobStepKind::Lie:
        case JobStepKind::BedRestLie:
        case JobStepKind::TendWork:
            return true;
        case JobStepKind::Play:
            return job.count == 0;  // a stroll walks about
        default:
            return false;
    }
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
        case JobType::Ingest: return "Ingest";
        case JobType::LayDown: return "LayDown";
        case JobType::DeliverResources: return "DeliverResources";
        case JobType::BuildFrame: return "BuildFrame";
        case JobType::Deconstruct: return "Deconstruct";
        case JobType::Mine: return "Mine";
        case JobType::CutPlant: return "CutPlant";
        case JobType::HarvestPlant: return "HarvestPlant";
        case JobType::DoBill: return "DoBill";
        case JobType::Recreate: return "Recreate";
        case JobType::Research: return "Research";
        case JobType::CleanFilth: return "CleanFilth";
        case JobType::BedRest: return "BedRest";
        case JobType::TendPatient: return "TendPatient";
        case JobType::Rescue: return "Rescue";
        case JobType::WearApparel: return "WearApparel";
        case JobType::StripPawn: return "StripPawn";
        case JobType::TameAnimal: return "TameAnimal";
        case JobType::TrainAnimal: return "TrainAnimal";
        case JobType::SlaughterAnimal: return "SlaughterAnimal";
        case JobType::FeedAnimal: return "FeedAnimal";
        case JobType::GatherAnimal: return "GatherAnimal";
        case JobType::SowCrop: return "SowCrop";
        case JobType::ExtinguishFire: return "ExtinguishFire";
        case JobType::CapturePrisoner: return "CapturePrisoner";
        case JobType::FeedPrisoner: return "FeedPrisoner";
        case JobType::WardenInteract: return "WardenInteract";
    }
    return "?";
}

int jobStepCount(JobType type) {
    return jobsStepsOf(type).count;
}

JobSystem::JobSystem(Map& map, Things& things, Pathfinder& pathfinder, Zones& zones, Construction& construction,
                     Plants& plants, Mineables& mineables, Designations& designations, const DefDatabase& defs,
                     HealthSystem& health, const NeedsSystem& needs, const GameTime& time, SimStats& stats,
                     std::vector<Event>& events)
    : map_(map),
      things_(things),
      pathfinder_(pathfinder),
      zones_(zones),
      construction_(construction),
      plants_(plants),
      mineables_(mineables),
      designations_(designations),
      defs_(defs),
      health_(health),
      needs_(needs),
      rules_(needs.rules()),
      time_(time),
      stats_(stats),
      events_(events),
      bills_(std::make_unique<BillSystem>(things, defs)),
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
    masses_.assign(defs_.size(), 1.0);
    foodTier_.assign(defs_.size(), -1);
    nutrition_.assign(defs_.size(), 0.0f);
    maxIngest_.assign(defs_.size(), 1);
    ingestSeconds_.assign(defs_.size(), 0.0f);
    isBed_.assign(defs_.size(), 0);
    isMedical_.assign(defs_.size(), 0);
    isTable_.assign(defs_.size(), 0);
    isChair_.assign(defs_.size(), 0);
    for (uint32_t i = 0; i < defs_.size(); ++i) {
        const Def& def = defs_.get(DefId{i});
        if (def.kind() != "Things") {
            continue;
        }
        standable_[i] = def.text("category") == "Building" && def.text("passability") == "Standable" ? 1 : 0;
        stackLimits_[i] = std::max(1, static_cast<int>(def.integer("stackLimit", 1)));
        masses_[i] = def.number("stats.mass", 1.0);
        isBed_[i] = def.flag("building.bed") ? 1 : 0;
        isMedical_[i] = def.flag("building.bed") && def.flag("building.medical") ? 1 : 0;
        isTable_[i] = def.flag("building.isTable") ? 1 : 0;
        isChair_[i] = def.flag("building.isSittable") ? 1 : 0;
        if (def.has("ingestible") && def.number("stats.nutrition") > 0.0) {
            // Food tiers, tastiest last (docs/design/needs.md). NeverForNutrition is not food.
            const std::string taste = def.text("ingestible.preferability");
            int tier = 1;
            if (taste == "NeverForNutrition") tier = -1;
            else if (taste == "DesperateOnly" || taste == "DesperateOnlyForHumanlikes") tier = 0;
            else if (taste == "RawBad") tier = 1;
            else if (taste == "RawTasty") tier = 2;
            else if (taste == "MealAwful") tier = 3;
            else if (taste == "MealSimple") tier = 4;
            else if (taste == "MealFine") tier = 5;
            else if (taste == "MealLavish") tier = 6;
            foodTier_[i] = static_cast<int8_t>(tier);
            nutrition_[i] = static_cast<float>(def.number("stats.nutrition"));
            maxIngest_[i] = static_cast<uint16_t>(std::clamp<int64_t>(
                def.integer("ingestible.maxNumToIngestAtOnce", rules_.maxNumToIngestAtOnce), 1, 1000));
            ingestSeconds_[i] = static_cast<float>(def.number("ingestible.baseIngestSeconds", rules_.ingestSeconds));
        }
    }

    recreation_ = RecreationRules::read(defs_);

    const Def* rules = defs_.find("Rules", "JobRules");
    idleSeconds_ = static_cast<int64_t>(rules ? rules->number("idleSeconds", kJobsIdleSeconds) : kJobsIdleSeconds);
    avoidSeconds_ = static_cast<int64_t>(rules ? rules->number("avoidFailedTargetSeconds", kJobsAvoidSeconds) : kJobsAvoidSeconds);
    carrySlowdown_ = std::clamp(rules ? rules->number("carrySlowdown", kJobsCarrySlowdown) : kJobsCarrySlowdown, 0.0, 0.9);
    maxRepaths_ = static_cast<int>(rules ? rules->integer("maxRepaths", kJobsMaxRepaths) : kJobsMaxRepaths);
    defaultPriority_ = static_cast<uint8_t>(std::clamp(
        static_cast<int>(rules ? rules->integer("defaultWorkPriority", kDefaultPriority) : kDefaultPriority), 1, kMaxPriority));
    loadGatherRules();
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

void JobSystem::addWorker(Entity pawn, const std::vector<std::string>& disabledWorkTags, bool withNeeds) {
    entt::registry& reg = things_.registry();
    reg.emplace_or_replace<Worker>(pawn);
    reg.emplace_or_replace<WorkSettings>(pawn, defaultWorkSettings(disabledWorkTags));
    if (withNeeds && rules_.present) {
        reg.emplace_or_replace<Needs>(pawn);
        reg.emplace_or_replace<Schedule>(pawn, rules_.standardSchedule());
    }
}

// ---- Reservations ---------------------------------------------------------------------------

JobSystem::TargetKey JobSystem::keyOf(const JobTarget& target, uint8_t purpose) {
    TargetKey key;
    if (target.thing != kNoEntity) {
        key.thing = entt::to_integral(target.thing);
    } else {
        key.x = target.cell.x;
        key.y = target.cell.y;
        key.level = target.cell.level;
    }
    key.purpose = purpose;
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

bool JobSystem::deliveryReservedByOther(Entity site, Entity pawn) const {
    const auto it = reservations_.find(keyOf(JobTarget{site, CellPos{}}, 1));
    return it != reservations_.end() && it->second != pawn;
}

// What a job holds. Hauling: the item and the storage cell. Eating: the food and, if the pawn eats
// in a chair, the chair's cell. Sleeping: the bed's cell (none for the ground). Delivering: the
// item and the site (purpose 1, so the builder of the frame can hold it too). Building and taking
// down: the frame or the building.
int JobSystem::reservationsOf(const Job& job, TargetKey out[kMaxReservations]) {
    switch (job.type) {
        case JobType::HaulToCell:
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            out[1] = keyOf(JobTarget{kNoEntity, job.cell});
            return 2;
        case JobType::Ingest:
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            if (job.chair != kNoEntity) {
                out[1] = keyOf(JobTarget{kNoEntity, job.cell});
                return 2;
            }
            return 1;
        case JobType::LayDown:
            if (job.thing == kNoEntity) {
                return 0;
            }
            out[0] = keyOf(JobTarget{kNoEntity, job.cell});
            return 1;
        case JobType::DeliverResources:
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            out[1] = keyOf(JobTarget{job.site, CellPos{}}, 1);
            return 2;
        case JobType::BuildFrame:
        case JobType::Deconstruct:
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            return 1;
        case JobType::Mine:
        case JobType::CutPlant:
        case JobType::HarvestPlant:
        case JobType::SowCrop:
        case JobType::CleanFilth:
            out[0] = keyOf(JobTarget{kNoEntity, job.cell});
            return 1;
        case JobType::DoBill: {
            // The building, the stack being fetched and the stacks still to fetch (at most
            // kMaxReservations - 1 together; the scanner never plans more).
            int n = 0;
            out[n++] = keyOf(JobTarget{job.site, CellPos{}});
            if (job.thing != kNoEntity) {
                out[n++] = keyOf(JobTarget{job.thing, CellPos{}});
            }
            for (const BillPick& pick : job.picks) {
                if (n < kMaxReservations) {
                    out[n++] = keyOf(JobTarget{pick.item, CellPos{}});
                }
            }
            return n;
        }
        case JobType::Recreate:
            // The place it plays at: a cell beside the building, the chair it sits in, or none (a stroll).
            if (job.thing == kNoEntity && job.chair == kNoEntity) {
                return 0;
            }
            out[0] = keyOf(JobTarget{kNoEntity, job.cell});
            return 1;
        case JobType::Research:
            out[0] = keyOf(JobTarget{job.site, CellPos{}});
            return 1;
        case JobType::BedRest:
            if (job.thing == kNoEntity) {
                return 0;
            }
            out[0] = keyOf(JobTarget{kNoEntity, job.cell});
            return 1;
        case JobType::TendPatient: {
            int n = 0;
            out[n++] = keyOf(JobTarget{job.thing, CellPos{}});
            if (job.site != kNoEntity) {
                out[n++] = keyOf(JobTarget{job.site, CellPos{}});
            }
            return n;
        }
        case JobType::Rescue:
        case JobType::CapturePrisoner:
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            out[1] = keyOf(JobTarget{kNoEntity, job.cell});
            return 2;
        case JobType::FeedPrisoner: {
            // The prisoner, and the food being fetched or carried to it.
            int n = 0;
            out[n++] = keyOf(JobTarget{job.site, CellPos{}});
            if (job.thing != kNoEntity) {
                out[n++] = keyOf(JobTarget{job.thing, CellPos{}});
            }
            return n;
        }
        case JobType::WardenInteract:
            out[0] = keyOf(JobTarget{job.site, CellPos{}});
            return 1;
        case JobType::WearApparel:
            if (job.count != 0) {
                return 0;  // taking a worn piece off holds nothing
            }
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            return 1;
        case JobType::StripPawn:
            out[0] = keyOf(JobTarget{job.thing, CellPos{}});
            return 1;
        case JobType::TameAnimal:
        case JobType::TrainAnimal:
        case JobType::SlaughterAnimal:
        case JobType::GatherAnimal:
        case JobType::FeedAnimal: {
            // The animal, and the food being fetched or carried to it.
            int n = 0;
            out[n++] = keyOf(JobTarget{job.site, CellPos{}});
            if (job.thing != kNoEntity) {
                out[n++] = keyOf(JobTarget{job.thing, CellPos{}});
            }
            return n;
        }
        case JobType::None:
        case JobType::Wait:
        case JobType::ExtinguishFire:  // colonists fight a fire together: nothing is reserved
            return 0;
    }
    return 0;
}

void JobSystem::reserve(Entity pawn, const Job& job) {
    TargetKey keys[kMaxReservations];
    const int n = reservationsOf(job, keys);
    for (int i = 0; i < n; ++i) {
        reservations_[keys[i]] = pawn;
    }
}

void JobSystem::release(Entity pawn, const Job& job) {
    TargetKey keys[kMaxReservations];
    const int n = reservationsOf(job, keys);
    for (int i = 0; i < n; ++i) {
        const auto it = reservations_.find(keys[i]);
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

double JobSystem::carryCapacity(Entity pawn) const {
    const Thing* thing = things_.registry().try_get<Thing>(pawn);
    const double capacity =
        thing ? defs_.get(thing->def).number("stats.carryingCapacity", kJobsCarryingCapacity) : kJobsCarryingCapacity;
    return talents_ != nullptr ? capacity * talents_->statFactor(pawn, "carryCapacity") : capacity;
}

double JobSystem::massOf(DefId def) const {
    return def.valid() && def.value < masses_.size() ? masses_[def.value] : 1.0;
}

// The mass of the weapons a pawn carries on its body besides the one in its hands (P1). They take
// room in the load, so a pawn with a rifle and a sidearm on it hauls a little less and walks a little slower.
double JobSystem::bodyWeaponMass(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Gear* gear = reg.try_get<Gear>(pawn);
    double mass = 0.0;
    if (gear != nullptr) {
        for (const Entity item : gear->stowed) {
            mass += reg.valid(item) && reg.all_of<Thing>(item) ? massOf(reg.get<Thing>(item).def) : 0.0;
        }
    }
    return mass;
}

// The mass of the pawn's hands: its capacity less the weapons on its body decides how many fit, whole stacks or not.
int JobSystem::carryLimit(Entity pawn, DefId def) const {
    const double mass = std::max(massOf(def), 1e-6);
    const double room = std::max(carryCapacity(pawn) - bodyWeaponMass(pawn), 0.0);
    return std::max(1, static_cast<int>(std::floor(room / mass + 1e-9)));
}

float JobSystem::loadSpeedFactor(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    double mass = bodyWeaponMass(pawn);
    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    if (carrying != nullptr && reg.valid(carrying->thing) && reg.all_of<StackCount, Thing>(carrying->thing)) {
        mass += reg.get<StackCount>(carrying->thing).count * massOf(reg.get<Thing>(carrying->thing).def);
    }
    if (mass <= 0.0) {
        return 1.0f;
    }
    const double fraction = std::clamp(mass / std::max(carryCapacity(pawn), 1e-6), 0.0, 1.0);
    return static_cast<float>(1.0 - carrySlowdown_ * fraction);
}

// Stacks of the same def lying right beside the pawn that would be hauled to the same zone anyway
// join the load, up to what its hands can carry (several stacks in one trip).
void JobSystem::gatherBeside(Entity pawn, Job& job, Entity carried) {
    entt::registry& reg = things_.registry();
    const StockpileZone* zone = zones_.stockpileAt(job.cell);
    if (zone == nullptr) {
        return;
    }
    const DefId def = reg.get<Thing>(carried).def;
    const int limit = carryLimit(pawn, def);
    const CellPos here = cellOf(reg.get<Position>(pawn));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const CellPos cell{here.x + dx, here.y + dy, here.level};
            if (!map_.inBounds(cell)) {
                continue;
            }
            const std::vector<Entity> items = things_.itemsAt(cell);
            for (const Entity other : items) {
                const int have = reg.get<StackCount>(carried).count;
                if (have >= limit) {
                    return;
                }
                if (other == carried || reg.get<Thing>(other).def != def || reservedByOther(JobTarget{other, {}}, pawn) ||
                    static_cast<int>(storedPriority(other)) >= static_cast<int>(zone->priority)) {
                    continue;
                }
                const int take = std::min(limit - have, reg.get<StackCount>(other).count);
                if (take >= reg.get<StackCount>(other).count) {
                    things_.destroy(other);
                } else {
                    reg.get<StackCount>(other).count -= take;
                }
                reg.get<StackCount>(carried).count += take;
                job.count += take;
            }
        }
    }
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
    static constexpr ThinkNode kThinkList[] = {&JobSystem::thinkNeeds, &JobSystem::thinkSchedule, &JobSystem::thinkApparel,
                                               &JobSystem::thinkWork, &JobSystem::thinkJoy, &JobSystem::thinkIdle};

    for (const Entity pawn : order_) {
        reg.get<Mover>(pawn).hold = false;
        // The player walks this one (U9): no think, until control is let go. Its only job is a
        // hands-on one the player started (U10), done in place.
        if (reg.all_of<PlayerControl>(pawn)) {
            if (reg.get<Worker>(pawn).job.type != JobType::None) {
                tickJob(pawn);
            }
            continue;
        }
        // It fights (drafted, or another faction's: F4): no think, no job, until it is undrafted.
        if (reg.all_of<Fighter>(pawn)) {
            continue;
        }
        // It is in a fight with fists with a pawn it quarrelled with (C20): the social system acts for it.
        if (reg.all_of<SocialFight>(pawn)) {
            continue;
        }
        // Downed or dead (F1): the job ends, what it carried is put down, and it doesn't think.
        if (health_.incapacitated(pawn)) {
            if (reg.get<Worker>(pawn).job.type != JobType::None) {
                endJob(pawn, JobEnd::Interrupted, reg.get<Health>(pawn).dead ? "died" : "downed");
            }
            dropCarried(pawn);  // the player's colonist may carry something with no job (U10)
            restUnderPawn(pawn);
            continue;
        }
        if (Health* health = reg.try_get<Health>(pawn)) {
            // Only lying in a bed heals faster: a pawn that got up, or was downed in one, goes back to the ground.
            const Job& lying = reg.get<Worker>(pawn).job;
            if (health->rest != RestKind::Ground && !jobIsLyingDown(lying) &&
                !(lying.type == JobType::BedRest && lying.step >= 1)) {
                health->rest = RestKind::Ground;
            }
        }
        // A prisoner sleeps and rests in its cell and does nothing else (PrisonerJobs.cpp).
        if (reg.all_of<Prisoner>(pawn)) {
            stepPrisoner(pawn);
            if (reg.get<Worker>(pawn).job.type != JobType::None) {
                tickJob(pawn);
                reg.get<Mover>(pawn).hold = jobHoldsStill(reg.get<Worker>(pawn).job);
            }
            continue;
        }
        // An animal that hauls works only while it is awake, fed and unhurt.
        if (animalGate_ && reg.all_of<Animal>(pawn) && !animalGate_(pawn)) {
            if (reg.get<Worker>(pawn).job.type != JobType::None) {
                endJob(pawn, JobEnd::Interrupted, "it can't work now");
            }
            continue;
        }
        Worker& worker = reg.get<Worker>(pawn);
        const int64_t t = now();
        worker.avoid.erase(std::remove_if(worker.avoid.begin(), worker.avoid.end(),
                                          [t](const AvoidTarget& a) { return a.until <= t; }),
                           worker.avoid.end());
        if (worker.job.type != JobType::None) {
            interruptForNeeds(pawn);
            interruptForEmergency(pawn);
        }
        if (reg.get<Worker>(pawn).job.type == JobType::None) {
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
            reg.get<Mover>(pawn).hold = jobHoldsStill(reg.get<Worker>(pawn).job);
        }
    }
}

bool JobSystem::thinkWork(Entity pawn, Job& out) {
    return thinkWorkPass(pawn, out, false);
}

bool JobSystem::thinkWorkPass(Entity pawn, Job& out, bool urgentOnly) {
    entt::registry& reg = things_.registry();
    const WorkSettings* settings = reg.try_get<WorkSettings>(pawn);
    if (settings == nullptr) {
        return false;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    for (int priority = 1; priority <= kMaxPriority; ++priority) {
        // Every scanner of every work type at this priority offers its targets; the nearest wins,
        // whichever work type it is (game design doc: within a priority, closer jobs first), except that
        // the emergency work types (doctor, patient...) are asked first.
        for (int pass = 0; pass < (urgentOnly ? 1 : 2); ++pass) {
            candidates_.clear();
            for (size_t w = 0; w < settings->entries.size() && w < workTypes_.size(); ++w) {
                const WorkPriority& entry = settings->entries[w];
                if (entry.disabled || entry.priority != priority ||
                    (workTypes_[w].naturalPriority >= kJobsUrgentWorkFrom) != (pass == 0)) {
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
                const CellPos targetCell = c.hasReach ? c.reach
                                           : c.target.thing != kNoEntity ? cellOf(reg.get<Position>(c.target.thing))
                                                                         : c.target.cell;
                // Rock can't be walked into: the pawn works from a cell next to it.
                CellPos stand;
                if (map_.walkable(targetCell) ? !canReach(at, targetCell) : !touchCell(at, targetCell, stand)) {
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
    if (job.handsOn == 0) {
        job.step = 0;  // a hands-on job may start part-way (a delivery starts with the material in hand)
    }
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
        if (job.handsOn != 0 && !handsOnInReach(pawn, job)) {
            endJob(pawn, JobEnd::Interrupted, "out of reach");
            return;
        }
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
        if (result == StepResult::Repeat) {
            still.step = 0;
            still.walk = 0;
            continue;
        }
        ++still.step;
        still.walk = 0;
        if (still.step >= jobStepCount(still.type)) {
            endJob(pawn, JobEnd::Done, std::string());
            return;
        }
    }
}

bool JobSystem::failCheck(Entity pawn, Job& job, std::string& why) {
    if (jobIsGather(job.type) || job.type == JobType::SowCrop) {
        return failCheckGather(pawn, job, why);
    }
    if (job.type == JobType::Ingest) {
        return failCheckIngest(pawn, job, why);
    }
    if (job.type == JobType::LayDown) {
        return failCheckLayDown(pawn, job, why);
    }
    if (job.type == JobType::DoBill) {
        return failCheckDoBill(pawn, job, why);
    }
    if (job.type == JobType::Recreate) {
        return failCheckRecreate(pawn, job, why);
    }
    if (job.type == JobType::Research) {
        return failCheckResearch(pawn, job, why);
    }
    if (job.type == JobType::CleanFilth) {
        return failCheckClean(pawn, job, why);
    }
    if (job.type == JobType::TendPatient) {
        return failCheckTend(pawn, job, why);
    }
    if (job.type == JobType::Rescue) {
        return failCheckRescue(pawn, job, why);
    }
    if (job.type == JobType::CapturePrisoner || job.type == JobType::FeedPrisoner || job.type == JobType::WardenInteract) {
        return failCheckPrisoner(pawn, job, why);
    }
    if (job.type == JobType::BedRest) {
        return failCheckBedRest(pawn, job, why);
    }
    if (job.type == JobType::ExtinguishFire) {
        return failCheckExtinguish(pawn, job, why);
    }
    if (job.type == JobType::WearApparel || job.type == JobType::StripPawn) {
        return failCheckApparel(pawn, job, why);
    }
    if (jobIsHandling(job.type)) {
        return failCheckHandle(pawn, job, why);
    }
    entt::registry& reg = things_.registry();
    if (job.type == JobType::DeliverResources) {
        const ConstructionSite* site = reg.valid(job.site) ? reg.try_get<ConstructionSite>(job.site) : nullptr;
        if (site == nullptr) {
            why = "the building plan is gone";
            return false;
        }
        if (job.step <= kJobsDeliverPickUpStep) {
            if (!things_.spawned(job.thing)) {
                why = "the material is gone";
                return false;
            }
        } else {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            if (carrying == nullptr || carrying->thing != job.thing || !reg.valid(job.thing)) {
                why = "it lost what it carried";
                return false;
            }
        }
        const DefId def = reg.get<Thing>(job.thing).def;
        const bool needed = std::any_of(site->materials.begin(), site->materials.end(),
                                        [def](const MaterialCount& m) { return m.item == def && m.have < m.needed; });
        if (!needed) {
            why = "the site needs no more of that";
            return false;
        }
        return true;
    }
    if (job.type == JobType::BuildFrame) {
        const ConstructionSite* site = reg.valid(job.thing) ? reg.try_get<ConstructionSite>(job.thing) : nullptr;
        if (site == nullptr || site->stage != ConstructionStage::Frame || !site->hasAllMaterials()) {
            why = "the frame is gone or is missing materials";
            return false;
        }
        return true;
    }
    if (job.type == JobType::Deconstruct) {
        if (!things_.spawned(job.thing) || !reg.all_of<MarkedForDeconstruct>(job.thing)) {
            why = "the building is gone or no longer marked";
            return false;
        }
        return true;
    }
    if (job.type != JobType::HaulToCell) {
        return true;
    }
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
        case JobStepKind::WalkToSpot:
            // Already there (a pawn eating where the food lay, sleeping where it stands): no walk.
            return cellOf(things_.registry().get<Position>(pawn)) == job.cell ? StepResult::Next : walkTo(pawn, job, job.cell, why);
        case JobStepKind::Eat:
            return eat(pawn, job, why);
        case JobStepKind::Lie:
            return lie(pawn, job, why);
        case JobStepKind::GotoCell:
            return job.handsOn != 0 ? StepResult::Next : walkTo(pawn, job, job.cell, why);
        case JobStepKind::Deliver:
            return deliver(pawn, job, why);
        case JobStepKind::BuildWork:
            return buildWork(pawn, job, why);
        case JobStepKind::DeconstructWork:
            return deconstructWork(pawn, job, why);
        case JobStepKind::GotoTouch:
            return goToTouch(pawn, job, why);
        case JobStepKind::MineWork:
            return mineRock(pawn, job, why);
        case JobStepKind::PlantWork:
            return plantWork(pawn, job, why);
        case JobStepKind::DepositIngredient:
            return depositIngredient(pawn, job, why);
        case JobStepKind::BillWork:
            return billWork(pawn, job, why);
        case JobStepKind::BillFinish:
            return billFinish(pawn, job, why);
        case JobStepKind::Play:
            return play(pawn, job, why);
        case JobStepKind::ResearchWork:
            return researchWork(pawn, job, why);
        case JobStepKind::CleanWork:
            return cleanWork(pawn, job, why);
        case JobStepKind::TendMedicine:
            return walkToMedicine(pawn, job, why);
        case JobStepKind::TendPickUp:
            return pickUpMedicine(pawn, job, why);
        case JobStepKind::TendGotoPatient:
            return walkToPatient(pawn, job, why);
        case JobStepKind::TendWork:
            return tendWork(pawn, job, why);
        case JobStepKind::LiftPawn:
            return liftPawn(pawn, job, why);
        case JobStepKind::CarryPawn:
            return carryPawn(pawn, job, why);
        case JobStepKind::LayPawn:
            return layPawn(pawn, job, why);
        case JobStepKind::BedRestLie:
            return bedRest(pawn, job, why);
        case JobStepKind::ApparelGoto:
            return apparelGoto(pawn, job, why);
        case JobStepKind::ApparelWork:
            return apparelWork(pawn, job, why);
        case JobStepKind::StripWork:
            return stripWork(pawn, job, why);
        case JobStepKind::ApproachAnimal:
            return approachAnimal(pawn, job, why);
        case JobStepKind::HandleWork:
            return handleWork(pawn, job, why);
        case JobStepKind::ExtinguishWork:
            return extinguishWork(pawn, job, why);
        case JobStepKind::ApproachPrisoner:
            return approachPrisoner(pawn, job, why);
        case JobStepKind::FeedWork:
            return feedPrisonerWork(pawn, job, why);
        case JobStepKind::ChatWork:
            return chatWork(pawn, job, why);
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
        if (job.repaths < maxRepaths_) {
            // The stack was moved while the pawn walked (a wall went up under it): walk to where it is now.
            ++job.repaths;
            ++stats_.repaths;
            return StepResult::Repeat;
        }
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
    if (job.type == JobType::HaulToCell) {
        gatherBeside(pawn, job, carried);
    }
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
    } else if (there.empty() && room > 0) {
        // More than one cell takes (a load of several stacks): fill this cell, the rest goes on.
        const Entity part = things_.splitOff(carried, room);
        if (part != kNoEntity) {
            things_.spawnAt(part, job.cell);
            pawnDid(pawn, PawnAction::PutDown, part);
        }
    } else if (!there.empty() && room > 0) {
        // Onto the stack already there; what doesn't fit stays in the pawn's hands.
        const int put = std::min(room, count);
        const Entity stack = there.front();
        things_.addToStack(stack, carried, put);
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

// Construction (docs/design/construction.md). Work steps use `job.walk` as "started": 0 until the
// pawn's first second of work, which sends the PawnAction.

float JobSystem::constructionSpeed(Entity pawn) const {
    const float talent = talents_ != nullptr ? static_cast<float>(talents_->statFactor(pawn, "buildSpeed")) : 1.0f;
    return construction_.skillSpeed(pawn) * health_.capacityValue(pawn, "Manipulation") * lightWorkFactor(pawn) * talent;
}

JobSystem::StepResult JobSystem::deliver(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (job.handsOn == 0 && cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn isn't beside the building plan";
        return StepResult::Failed;
    }
    const Entity carried = job.thing;
    const DefId def = reg.get<Thing>(carried).def;
    const int count = reg.get<StackCount>(carried).count;
    const int taken = construction_.deliver(job.site, def, count);
    if (taken <= 0) {
        why = "the building plan needs no more of that";
        return StepResult::Failed;
    }
    pawnDid(pawn, PawnAction::PutDown, job.site);
    if (taken == count) {
        reg.remove<Carrying>(pawn);
        things_.destroy(carried);
    } else {
        reg.get<StackCount>(carried).count -= taken;  // the rest is put down when the job ends
    }
    return StepResult::Next;
}

JobSystem::StepResult JobSystem::buildWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (job.handsOn == 0 && cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the frame";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Build, job.thing);
    }
    ConstructionSite& site = reg.get<ConstructionSite>(job.thing);
    site.workDone += constructionSpeed(pawn) * static_cast<float>(construction_.numbers().buildWorkPerSecond);
    if (site.workDone < site.workTotal) {
        return StepResult::Running;
    }
    const Entity building = construction_.complete(job.thing);
    if (building == kNoEntity) {
        why = "the building can't stand there any more";
        return StepResult::Failed;
    }
    release(pawn, job);
    job.thing = building;
    reserve(pawn, job);
    return StepResult::Next;
}

JobSystem::StepResult JobSystem::deconstructWork(Entity pawn, Job& job, std::string& why) {
    entt::registry& reg = things_.registry();
    if (cellOf(reg.get<Position>(pawn)) != job.cell) {
        why = "the pawn left its place beside the building";
        return StepResult::Failed;
    }
    if (job.walk == 0) {
        job.walk = 2;
        pawnDid(pawn, PawnAction::Deconstruct, job.thing);
    }
    job.progress += constructionSpeed(pawn) * static_cast<float>(construction_.numbers().deconstructWorkPerSecond);
    if (job.progress < construction_.deconstructWork(job.thing)) {
        return StepResult::Running;
    }
    if (!construction_.deconstruct(job.thing)) {
        why = "it couldn't take the building down";
        return StepResult::Failed;
    }
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
    if (!reg.valid(thing)) {
        reg.remove<Carrying>(pawn);
        return;
    }
    const CellPos at = cellOf(reg.get<Position>(pawn));
    // The nearest cell, ring by ring, with room for all of it and no other pawn's claim.
    for (int r = 0; r <= kJobsDropRadius; ++r) {
        for (int y = at.y - r; y <= at.y + r; ++y) {
            for (int x = at.x - r; x <= at.x + r; ++x) {
                if ((std::abs(x - at.x) == r || std::abs(y - at.y) == r) && placeCarriedAt(pawn, CellPos{x, y, at.level})) {
                    return;
                }
            }
        }
    }
    reg.remove<Carrying>(pawn);
    things_.spawnAt(thing, at);  // nowhere better: on top of whatever is here
    pawnDid(pawn, PawnAction::PutDown, thing);
}

bool JobSystem::placeCarriedAt(Entity pawn, CellPos c) {
    entt::registry& reg = things_.registry();
    const Carrying* carrying = reg.try_get<Carrying>(pawn);
    if (carrying == nullptr || !reg.valid(carrying->thing)) {
        return false;
    }
    const Entity thing = carrying->thing;
    const DefId def = reg.get<Thing>(thing).def;
    const int count = reg.get<StackCount>(thing).count;
    if (!map_.inBounds(c) || roomFor(def, c) < count || reservedByOther(JobTarget{kNoEntity, c}, pawn)) {
        return false;
    }
    reg.remove<Carrying>(pawn);
    const std::vector<Entity>& there = things_.itemsAt(c);
    if (there.empty()) {
        things_.spawnAt(thing, c);
        pawnDid(pawn, PawnAction::PutDown, thing);
    } else {
        const Entity stack = there.front();
        things_.addToStack(stack, thing, count);
        pawnDid(pawn, PawnAction::PutDown, stack);
        things_.destroy(thing);
    }
    return true;
}

void JobSystem::endJob(Entity pawn, JobEnd end, const std::string& why) {
    entt::registry& reg = things_.registry();
    const Job job = reg.get<Worker>(pawn).job;
    release(pawn, job);
    if (job.handsOn == 2 && jobIsGather(job.type) && designations_.at(job.cell) != DesignationKind::None) {
        // The player made this mark just to work by hand; if the work didn't finish, it goes again.
        designations_.set(job.cell, DesignationKind::None);
        Event changed;
        changed.type = EventType::DesignationsChanged;
        events_.push_back(std::move(changed));
    }
    if (job.walk == 1) {
        pathfinder_.cancel(job.path);
    }
    if (end != JobEnd::Done) {
        Mover& mover = reg.get<Mover>(pawn);
        mover.path.clear();
        mover.next = 0;
    }
    dropCarried(pawn);
    if (job.type == JobType::DoBill && end != JobEnd::Done) {
        returnIngredients(pawn, job);
    }
    if (jobIsLyingDown(job) || job.type == JobType::BedRest) {
        wakeUp(pawn, job);
    }
    Worker& worker = reg.get<Worker>(pawn);
    if (end == JobEnd::Failed) {
        ++stats_.failedJobs;
        for (const Entity target : {job.thing, job.site}) {
            if (target != kNoEntity) {
                worker.avoid.push_back(AvoidTarget{JobTarget{target, CellPos{}}, now() + avoidSeconds_});
                if (worker.avoid.size() > kMaxAvoid) {
                    worker.avoid.erase(worker.avoid.begin());
                }
            }
        }
        if (jobIsGather(job.type)) {
            worker.avoid.push_back(AvoidTarget{JobTarget{kNoEntity, job.cell}, now() + avoidSeconds_});
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
    event.target = jobIsHandling(job.type) ? job.site : job.thing;
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
        TargetKey held[kMaxReservations];
        const int heldCount = reservationsOf(job, held);
        for (int i = 0; i < heldCount; ++i) {
            if (!reservations_.emplace(held[i], pawn).second) {
                error = "the save has two jobs holding the same thing or cell";
                return false;
            }
        }
        if (job.type == JobType::Ingest) {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            const bool carries = carrying != nullptr && carrying->thing == job.thing;
            if (job.thing == kNoEntity || (job.step > kIngestPickUpStep) != carries) {
                error = "the save has an eating job that doesn't match what the pawn carries";
                return false;
            }
        }
        if (job.type == JobType::DoBill) {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            const bool carries = carrying != nullptr && carrying->thing == job.thing && job.thing != kNoEntity;
            const bool shouldCarry = job.step > kDoBillPickUpStep && job.step <= kDoBillDepositStep;
            if (job.site == kNoEntity || job.bill == 0 || shouldCarry != carries ||
                (job.step <= kDoBillDepositStep && job.thing == kNoEntity) ||
                job.picks.size() + 1 > static_cast<size_t>(kMaxReservations)) {
                error = "the save has a bill job that doesn't match what the pawn carries";
                return false;
            }
        }
        if (job.type == JobType::HaulToCell || job.type == JobType::DeliverResources) {
            const Carrying* carrying = reg.try_get<Carrying>(pawn);
            const bool carries = carrying != nullptr && carrying->thing == job.thing;
            const uint8_t pickUpStep = job.type == JobType::HaulToCell ? kJobsHaulPickUpStep : kJobsDeliverPickUpStep;
            if (job.thing == kNoEntity || (job.step > pickUpStep) != carries) {
                error = "the save has a carrying job that doesn't match what the pawn carries";
                return false;
            }
        }
        if ((job.type == JobType::BuildFrame || job.type == JobType::Deconstruct) && job.thing == kNoEntity) {
            error = "the save has a building job with nothing to work on";
            return false;
        }
        if (job.type == JobType::DeliverResources && job.site == kNoEntity) {
            error = "the save has a delivery with nowhere to deliver";
            return false;
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
