#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Surgery.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define SURG_REQUIRE(...)      \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/surgery.md are what is tested.
std::shared_ptr<const DefDatabase> surgDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim surgSim(uint64_t seed = 7) { return Sim(SimSettings{seed, 64, 0, 0, true}, surgDefs()); }

Entity surgColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    return pawn;
}

// A patient is nobody's doctor.
Entity surgPatient(Sim& sim, CellPos cell) {
    const Entity pawn = surgColonist(sim, cell);
    sim.submit(SetWorkPriorityCommand{pawn, "Doctor", 0});
    return pawn;
}

int surgPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == label) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Destroys a part with a blow far bigger than it.
void surgDestroy(Sim& sim, Entity pawn, const std::string& label) {
    Rng rng(5);
    sim.health().applyDamage(pawn, sim.health().damageTypeId("Cut"), 500.0f, surgPart(sim, pawn, label), rng);
}

HealthReport surgReport(const Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    return report;
}

float surgCapacity(const HealthReport& report, const std::string& name) {
    for (const HealthReport::CapacityLine& line : report.capacities) {
        if (line.name == name) {
            return line.value;
        }
    }
    return -1.0f;
}

struct SurgLog {
    std::vector<Event> events;
    void step(Sim& sim) {
        sim.step();
        events.insert(events.end(), sim.events().begin(), sim.events().end());
    }
    const Event* type(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
    const Event* job(JobType type, Entity pawn, bool started = true) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) {
            return e.type == (started ? EventType::JobStarted : EventType::JobEnded) && e.job == type && e.thing == pawn;
        });
        return it == events.end() ? nullptr : &*it;
    }
};

bool surgRunUntil(Sim& sim, SurgLog& log, int64_t limit, const std::function<bool()>& done) {
    for (int64_t i = 0; i < limit; ++i) {
        log.step(sim);
        if (done()) {
            return true;
        }
    }
    return false;
}

int surgItems(const Sim& sim, const char* def) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)item;
        total += thing.def == id ? stack.count : 0;
    }
    return total;
}

bool surgFailed(const SurgLog& log) { return log.type(EventType::CommandFailed) != nullptr; }

}  // namespace

TEST_CASE("Surgery: the recipes are read, and the success chance follows RimWorld's rule") {
    Sim sim = surgSim();
    const SurgerySystem& surgery = sim.surgery();
    const SurgeryRecipe* peg = surgery.recipe("InstallPegLeg");
    const SurgeryRecipe* amputate = surgery.recipe("AmputateLimb");
    const SurgeryRecipe* bionic = surgery.recipe("InstallBionicArm");
    SURG_REQUIRE(peg != nullptr);
    SURG_REQUIRE(amputate != nullptr);
    SURG_REQUIRE(bionic != nullptr);
    CHECK(surgery.recipe("CookMealSimple") == nullptr);
    CHECK(peg->install);
    CHECK(peg->requiresMissing);
    CHECK(peg->partDef == "Leg");
    CHECK(peg->ingredient == sim.defs().idOf("Things", "PegLeg"));
    CHECK(amputate->removes);
    CHECK(amputate->limbsOnly);
    CHECK(bionic->minSkill == 8);
    CHECK_FALSE(bionic->requiresMissing);

    // Skill: 0.5 at level 0, 1.0 at 10, 1.4 at 20, read in between.
    CHECK(surgery.skillFactor(0) == doctest::Approx(0.5f));
    CHECK(surgery.skillFactor(10) == doctest::Approx(1.0f));
    CHECK(surgery.skillFactor(20) == doctest::Approx(1.4f));
    CHECK(surgery.skillFactor(7) == doctest::Approx(0.85f));
    SurgeryConditions c;
    c.skill = 10;
    // No medicine costs 30%; the cap is 98%.
    CHECK(surgery.successChance(*amputate, c) == doctest::Approx(0.7f));
    c.bedFactor = 1.07f;
    CHECK(surgery.successChance(*amputate, c) == doctest::Approx(0.7f * 1.07f));
    c.cleanliness = -5.0f;
    CHECK(surgery.successChance(*amputate, c) == doctest::Approx(0.7f * 1.07f * 0.85f));
    c.cleanliness = 0.0f;
    const MedicineKind* medicine = sim.medicine().kindOf(sim.defs().idOf("Things", "MedicineIndustrial"));
    SURG_REQUIRE(medicine != nullptr);
    c.medicine = medicine;
    c.skill = 20;
    CHECK(surgery.successChance(*amputate, c) == doctest::Approx(0.98f));
    // A clumsy doctor does worse.
    c.skill = 10;
    c.manipulation = 0.5f;
    CHECK(surgery.successChance(*amputate, c) == doctest::Approx(0.5f * 1.07f * 1.0f));
}

TEST_CASE("Surgery: a lost leg, fitted with a peg leg, a prosthetic or a bionic, moves at the part's efficiency") {
    Sim sim = surgSim();
    const Entity pawn = surgPatient(sim, CellPos{10, 10, 0});
    sim.step();
    const float whole = surgCapacity(surgReport(sim, pawn), "Moving");
    CHECK(whole == doctest::Approx(1.0f));
    surgDestroy(sim, pawn, "left leg");
    sim.step();
    HealthReport report = surgReport(sim, pawn);
    CHECK(surgCapacity(report, "Moving") < 0.5f);  // it hurts and bleeds
    sim.health().tendAll(pawn, 0.5f);
    sim.step();
    report = surgReport(sim, pawn);
    CHECK(surgCapacity(report, "Moving") == doctest::Approx(0.5f));
    CHECK(std::find(report.missingParts.begin(), report.missingParts.end(), "left leg") != report.missingParts.end());

    const int leg = surgPart(sim, pawn, "left leg");
    const int foot = surgPart(sim, pawn, "left foot");
    CHECK(sim.health().partCondition(pawn, leg) == PartCondition::Missing);
    CHECK(sim.health().partCondition(pawn, foot) == PartCondition::Under);
    const DefId pegLeg = sim.defs().idOf("Hediffs", "PegLeg");
    // A foot can't be fitted under a lost leg, and a leg takes a leg.
    CHECK_FALSE(sim.health().installPart(pawn, foot, sim.defs().idOf("Hediffs", "WoodenFoot"), 0.5f));
    CHECK_FALSE(sim.health().installPart(pawn, surgPart(sim, pawn, "left arm"), pegLeg, 0.5f));
    SURG_REQUIRE(sim.health().installPart(pawn, leg, pegLeg, 0.5f));
    sim.step();
    report = surgReport(sim, pawn);
    CHECK(sim.health().partCondition(pawn, leg) == PartCondition::Fitted);
    CHECK(sim.health().partCondition(pawn, foot) == PartCondition::Covered);
    CHECK(sim.health().addedAt(pawn, leg) == pegLeg);
    CHECK(surgCapacity(report, "Moving") == doctest::Approx(0.8f));  // (1.0 + 0.6) / 2
    CHECK(report.missingParts.empty());
    SURG_REQUIRE(report.addedParts.size() == 1);
    CHECK(report.addedParts[0].part == "left leg");
    CHECK(report.addedParts[0].label == "peg leg");
    CHECK(report.addedParts[0].efficiency == doctest::Approx(0.6f));
    CHECK_FALSE(sim.health().needsTending(pawn));  // the wound was closed by the operation
    CHECK(surgReport(sim, pawn).bleedPerDay == 0.0f);
    // The covered foot can't be hit.
    Rng rng(3);
    CHECK_FALSE(sim.health().applyDamage(pawn, sim.health().damageTypeId("Cut"), 3.0f, foot, rng));

    // Off again: the leg is lost, and the peg leg's item comes back.
    const DefId taken = sim.health().uninstallPart(pawn, leg, 0.5f);
    CHECK(taken == pegLeg);
    sim.step();
    CHECK(sim.health().partCondition(pawn, leg) == PartCondition::Missing);
    CHECK(surgCapacity(surgReport(sim, pawn), "Moving") == doctest::Approx(0.5f));
    CHECK_FALSE(sim.health().needsTending(pawn));

    // A prosthetic leg is better, a bionic one on the other, healthy leg is better than a leg.
    SURG_REQUIRE(sim.health().installPart(pawn, leg, sim.defs().idOf("Hediffs", "ProstheticLeg"), 0.5f));
    SURG_REQUIRE(sim.health().installPart(pawn, surgPart(sim, pawn, "right leg"), sim.defs().idOf("Hediffs", "BionicLeg"), 0.5f));
    sim.step();
    CHECK(surgCapacity(surgReport(sim, pawn), "Moving") == doctest::Approx((0.8f + 1.25f) / 2.0f));
    CHECK(sim.health().partCondition(pawn, surgPart(sim, pawn, "right foot")) == PartCondition::Covered);
}

TEST_CASE("Surgery: a bionic part replaces a wounded one, and breaks like any part") {
    Sim sim = surgSim();
    const Entity pawn = surgPatient(sim, CellPos{10, 10, 0});
    sim.step();
    const int arm = surgPart(sim, pawn, "left arm");
    Rng rng(4);
    CHECK(sim.health().applyDamage(pawn, sim.health().damageTypeId("Cut"), 12.0f, arm, rng));
    sim.step();
    const float hurt = surgCapacity(surgReport(sim, pawn), "Manipulation");
    CHECK(hurt < 1.0f);
    const DefId bionicArm = sim.defs().idOf("Hediffs", "BionicArm");
    SURG_REQUIRE(sim.health().installPart(pawn, arm, bionicArm, 0.5f));
    sim.step();
    HealthReport report = surgReport(sim, pawn);
    CHECK(report.injuries.empty());  // the wounded arm was replaced
    CHECK(surgCapacity(report, "Manipulation") == doctest::Approx((1.25f + 1.0f) / 2.0f));
    CHECK(sim.health().partCondition(pawn, arm) == PartCondition::Fitted);

    // Shot to pieces, it is a lost arm and the bionic part is gone.
    surgDestroy(sim, pawn, "left arm");
    sim.step();
    CHECK(sim.health().partCondition(pawn, arm) == PartCondition::Missing);
    CHECK_FALSE(sim.health().addedAt(pawn, arm).valid());
    CHECK(surgReport(sim, pawn).addedParts.empty());
}

TEST_CASE("Surgery: which parts an operation can take off") {
    Sim sim = surgSim();
    const Entity pawn = surgPatient(sim, CellPos{10, 10, 0});
    sim.step();
    CHECK(sim.health().canRemovePart(pawn, surgPart(sim, pawn, "left arm")));
    CHECK(sim.health().canRemovePart(pawn, surgPart(sim, pawn, "left eye")));
    CHECK(sim.health().partIsLimb(pawn, surgPart(sim, pawn, "left arm")));
    CHECK(sim.health().partIsLimb(pawn, surgPart(sim, pawn, "left leg")));
    CHECK_FALSE(sim.health().partIsLimb(pawn, surgPart(sim, pawn, "left eye")));
    CHECK_FALSE(sim.health().canRemovePart(pawn, 0));  // the torso
    CHECK_FALSE(sim.health().canRemovePart(pawn, surgPart(sim, pawn, "head")));  // the brain is in it
    const SurgerySystem& surgery = sim.surgery();
    const SurgeryRecipe* amputate = surgery.recipe("AmputateLimb");
    const SurgeryRecipe* remove = surgery.recipe("RemoveBodyPart");
    SURG_REQUIRE(amputate != nullptr);
    SURG_REQUIRE(remove != nullptr);
    std::string why;
    CHECK(surgery.eligible(pawn, *amputate, surgPart(sim, pawn, "left arm"), why));
    CHECK_FALSE(surgery.eligible(pawn, *amputate, surgPart(sim, pawn, "left eye"), why));
    CHECK(why == "only limbs can be amputated");
    CHECK(surgery.eligible(pawn, *remove, surgPart(sim, pawn, "left eye"), why));
    CHECK_FALSE(surgery.eligible(pawn, *remove, 0, why));
    CHECK_FALSE(surgery.eligible(pawn, *remove, 9999, why));
    CHECK(why == "the body has no such part");
    // The install recipes need the right part, researched.
    const SurgeryRecipe* peg = surgery.recipe("InstallPegLeg");
    CHECK_FALSE(surgery.eligible(pawn, *peg, surgPart(sim, pawn, "left leg"), why));
    CHECK(why == "the part must be gone first");
    surgDestroy(sim, pawn, "left leg");
    sim.step();
    CHECK(surgery.eligible(pawn, *peg, surgPart(sim, pawn, "left leg"), why));
    CHECK_FALSE(surgery.eligible(pawn, *peg, surgPart(sim, pawn, "left arm"), why));
    CHECK_FALSE(surgery.eligible(pawn, *peg, surgPart(sim, pawn, "left foot"), why));
    CHECK(surgery.targets(pawn, *peg) == std::vector<int>{surgPart(sim, pawn, "left leg")});
    const SurgeryRecipe* bionic = surgery.recipe("InstallBionicLeg");
    CHECK_FALSE(surgery.eligible(pawn, *bionic, surgPart(sim, pawn, "right leg"), why));  // BionicReplacements isn't done
    CHECK_FALSE(why.empty());
    sim.submit(UnlockResearchCommand{"BionicReplacements"});
    sim.step();
    CHECK(surgery.eligible(pawn, *bionic, surgPart(sim, pawn, "right leg"), why));
    CHECK(surgery.eligible(pawn, *bionic, surgPart(sim, pawn, "left leg"), why));  // or on the lost one
}

TEST_CASE("Surgery: the commands plan and cancel operations, and refuse what can't be done") {
    Sim sim = surgSim();
    const Entity pawn = surgPatient(sim, CellPos{10, 10, 0});
    sim.step();
    const int arm = surgPart(sim, pawn, "left arm");
    SurgLog log;
    sim.submit(AddOperationCommand{pawn, "AmputateLimb", arm});
    log.step(sim);
    CHECK_FALSE(surgFailed(log));
    SURG_REQUIRE(sim.surgery().of(pawn) != nullptr);
    CHECK(sim.surgery().of(pawn)->list.size() == 1);
    const std::vector<OperationLine> lines = sim.surgery().lines(pawn);
    SURG_REQUIRE(lines.size() == 1);
    CHECK(lines[0].recipe == "AmputateLimb");
    CHECK(lines[0].part == "left arm");
    CHECK(lines[0].id == 1);

    int failures = 0;
    const auto refused = [&](Command command) {
        SurgLog attempt;
        sim.submit(std::move(command));
        attempt.step(sim);
        failures += surgFailed(attempt) ? 1 : 0;
    };
    refused(AddOperationCommand{pawn, "AmputateLimb", arm});               // already planned
    refused(AddOperationCommand{pawn, "NoSuchOperation", arm});            // unknown
    refused(AddOperationCommand{pawn, "InstallPegLeg", arm});              // wrong part
    refused(AddOperationCommand{pawn, "RemoveBodyPart", 0});               // the torso
    refused(AddOperationCommand{pawn, "AmputateLimb", 9999});              // no such part
    refused(AddOperationCommand{kNoEntity, "AmputateLimb", arm});          // nobody
    refused(AddOperationCommand{sim.things().spawn("Steel", CellPos{12, 12, 0}), "AmputateLimb", arm});  // no body
    refused(RemoveOperationCommand{pawn, 99});                             // no such line
    CHECK(failures == 8);
    CHECK(sim.surgery().of(pawn)->list.size() == 1);
    sim.submit(RemoveOperationCommand{pawn, 1});
    sim.step();
    CHECK(sim.surgery().of(pawn)->list.empty());
    CHECK_FALSE(sim.surgery().pending(pawn));
    // Ids are not reused.
    sim.submit(AddOperationCommand{pawn, "AmputateLimb", arm});
    sim.step();
    CHECK(sim.surgery().of(pawn)->list.back().id == 2);
}

TEST_CASE("Surgery: perform does what the roll says, and a failure hurts and may kill") {
    Sim sim = surgSim();
    const Entity pawn = surgPatient(sim, CellPos{10, 10, 0});
    sim.step();
    const int arm = surgPart(sim, pawn, "left arm");
    std::string error;
    // A failed amputation: the arm stays, cut about a third of its hit points, and (with a high death roll) the patient lives.
    uint32_t id = sim.surgery().add(pawn, "AmputateLimb", arm, error);
    SURG_REQUIRE(id != 0);
    SurgeryResult result = sim.surgery().perform(pawn, id, 0.5f, false, 0.5f, 0.99f);
    CHECK(result.done);
    CHECK_FALSE(result.success);
    CHECK_FALSE(result.died);
    CHECK(sim.health().partCondition(pawn, arm) == PartCondition::Whole);
    HealthReport report = surgReport(sim, pawn);
    SURG_REQUIRE(report.injuries.size() == 1);
    CHECK(report.injuries[0].part == "left arm");
    CHECK(report.injuries[0].severity == doctest::Approx(0.3f * sim.health().partMaxHitPoints(pawn, arm)));
    CHECK(sim.surgery().of(pawn)->list.empty());

    // With a roll under (1 - chance) x 0.1 the patient dies of it.
    id = sim.surgery().add(pawn, "AmputateLimb", arm, error);
    SURG_REQUIRE(id != 0);
    result = sim.surgery().perform(pawn, id, 0.5f, false, 0.5f, 0.01f);
    CHECK(result.died);
    sim.step();
    CHECK(sim.things().registry().get<Health>(pawn).dead);
    CHECK(sim.things().registry().get<Health>(pawn).cause == DeathCause::Surgery);

    // A success takes the arm off, tended: no bleeding, no pain from the lost arm.
    const Entity other = surgPatient(sim, CellPos{14, 10, 0});
    sim.step();
    id = sim.surgery().add(other, "AmputateLimb", arm, error);
    SURG_REQUIRE(id != 0);
    result = sim.surgery().perform(other, id, 0.9f, true, 0.6f, 0.5f);
    CHECK(result.success);
    CHECK(sim.health().partCondition(other, arm) == PartCondition::Missing);
    sim.step();
    CHECK(surgReport(sim, other).bleedPerDay == 0.0f);
    CHECK(surgReport(sim, other).pain == 0.0f);
    CHECK_FALSE(sim.health().needsTending(other));
    CHECK(surgCapacity(surgReport(sim, other), "Manipulation") == doctest::Approx(0.5f));

    // An operation whose part has gone meanwhile is dropped, not done.
    const Entity third = surgPatient(sim, CellPos{18, 10, 0});
    sim.step();
    const uint32_t first = sim.surgery().add(third, "RemoveBodyPart", arm, error);
    const uint32_t second = sim.surgery().add(third, "AmputateLimb", arm, error);
    SURG_REQUIRE(first != 0);
    SURG_REQUIRE(second != 0);
    result = sim.surgery().perform(third, first, 0.9f, true, 0.6f, 0.5f);
    CHECK(result.success);
    CHECK(sim.surgery().of(third)->list.empty());  // the amputation had nothing left to take
    CHECK_FALSE(sim.surgery().perform(third, second, 0.9f, true, 0.6f, 0.5f).done);

    // Taking a peg leg off hands the item back.
    const Entity fourth = surgPatient(sim, CellPos{22, 10, 0});
    sim.step();
    const int leg = surgPart(sim, fourth, "left leg");
    surgDestroy(sim, fourth, "left leg");
    sim.step();
    SURG_REQUIRE(sim.health().installPart(fourth, leg, sim.defs().idOf("Hediffs", "PegLeg"), 0.5f));
    id = sim.surgery().add(fourth, "RemoveBodyPart", leg, error);
    SURG_REQUIRE(id != 0);
    result = sim.surgery().perform(fourth, id, 0.9f, true, 0.6f, 0.5f);
    CHECK(result.success);
    CHECK(result.returned == sim.defs().idOf("Things", "PegLeg"));
    CHECK(sim.health().partCondition(fourth, leg) == PartCondition::Missing);
}

TEST_CASE("Surgery: a patient goes to bed, a doctor amputates, and the medicine is used up") {
    Sim sim = surgSim();
    sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 3);
    const Entity doctor = surgColonist(sim, CellPos{10, 10, 0});
    const Entity patient = surgPatient(sim, CellPos{30, 30, 0});
    sim.step();
    const int arm = surgPart(sim, patient, "left arm");
    sim.submit(AddOperationCommand{patient, "AmputateLimb", arm});
    SurgLog log;
    SURG_REQUIRE(surgRunUntil(sim, log, 4000, [&] { return log.type(EventType::OperationDone) != nullptr; }));
    const Event* done = log.type(EventType::OperationDone);
    CHECK(done->thing == doctor);
    CHECK(done->target == patient);
    CHECK(done->def == sim.defs().idOf("Recipes", "AmputateLimb"));
    CHECK(done->part == arm);
    // Skill 8 (0.9), the hospital bed (1.07), medicine (1.0), a room that is no worse than neutral: near the cap.
    CHECK(done->amount >= 0.7f);
    CHECK(done->amount <= 0.98f);
    CHECK(log.job(JobType::BedRest, patient) != nullptr);  // it lay down first
    CHECK(log.job(JobType::Operate, doctor) != nullptr);
    CHECK(surgItems(sim, "MedicineIndustrial") == 2);
    CHECK(sim.surgery().of(patient)->list.empty());
    // The roll is the game's own (seed 7): this one works.
    CHECK(done->message == "success");
    CHECK(sim.health().partCondition(patient, arm) == PartCondition::Missing);
    // One arm is half the Manipulation (a chill on a bare colonist takes a little more).
    const HealthReport after = surgReport(sim, patient);
    CHECK(surgCapacity(after, "Manipulation") <= 0.5f);
    CHECK(surgCapacity(after, "Manipulation") > 0.3f);
    CHECK(after.pain == 0.0f);
    CHECK(after.bleedPerDay == 0.0f);
    CHECK_FALSE(surgFailed(log));
}

TEST_CASE("Surgery: fitting a peg leg takes the item to the patient and the patient walks again") {
    Sim sim = surgSim();
    sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("PegLeg", CellPos{12, 12, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{13, 12, 0}, 2);
    const Entity doctor = surgColonist(sim, CellPos{10, 10, 0});
    const Entity patient = surgPatient(sim, CellPos{30, 30, 0});
    sim.step();
    surgDestroy(sim, patient, "left leg");
    const int leg = surgPart(sim, patient, "left leg");
    sim.submit(AddOperationCommand{patient, "InstallPegLeg", leg});
    SurgLog log;
    SURG_REQUIRE(surgRunUntil(sim, log, 6000, [&] { return log.type(EventType::OperationDone) != nullptr; }));
    const Event* done = log.type(EventType::OperationDone);
    CHECK(done->thing == doctor);
    CHECK(surgItems(sim, "PegLeg") == 0);  // used up whether or not it worked
    CHECK(surgItems(sim, "MedicineIndustrial") <= 2);
    CHECK(done->message == "success");
    CHECK(sim.health().partCondition(patient, leg) == PartCondition::Fitted);
    sim.stepMany(10);
    CHECK(surgCapacity(surgReport(sim, patient), "Moving") > 0.7f);
    // The wound of the lost leg was tended before, by the same doctor, or closed by the operation.
    CHECK_FALSE(sim.health().needsTending(patient));
}

TEST_CASE("Surgery: with no part to fit, or too little skill, nobody operates and the patient waits in bed") {
    Sim sim = surgSim();
    sim.things().spawn("Bed", CellPos{20, 20, 0});
    const Entity doctor = surgColonist(sim, CellPos{10, 10, 0});
    const Entity patient = surgPatient(sim, CellPos{14, 10, 0});
    sim.step();
    surgDestroy(sim, patient, "left leg");
    const int leg = surgPart(sim, patient, "left leg");
    sim.submit(AddOperationCommand{patient, "InstallPegLeg", leg});
    SurgLog log;
    sim.stepMany(3000);
    CHECK(sim.things().registry().get<Worker>(doctor).job.type != JobType::Operate);
    CHECK(sim.surgery().pending(patient));
    // Cancelling it lets the patient get up.
    surgRunUntil(sim, log, 10, [&] { return false; });
    sim.submit(RemoveOperationCommand{patient, 1});
    sim.stepMany(200);
    CHECK_FALSE(sim.surgery().pending(patient));
    CHECK(sim.things().registry().get<Worker>(patient).job.type != JobType::BedRest);
}

TEST_CASE("Surgery: operations and artificial parts save and load, and a loaded game goes on the same") {
    Sim sim = surgSim();
    sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 3);
    sim.things().spawn("PegLeg", CellPos{13, 12, 0});
    const Entity doctor = surgColonist(sim, CellPos{10, 10, 0});
    const Entity patient = surgPatient(sim, CellPos{14, 10, 0});
    const Entity fitted = surgPatient(sim, CellPos{16, 10, 0});
    sim.step();
    surgDestroy(sim, fitted, "right leg");
    sim.step();
    SURG_REQUIRE(sim.health().installPart(fitted, surgPart(sim, fitted, "right leg"), sim.defs().idOf("Hediffs", "PegLeg"), 0.4f));
    surgDestroy(sim, patient, "left leg");
    sim.submit(AddOperationCommand{patient, "InstallPegLeg", surgPart(sim, patient, "left leg")});
    sim.submit(AddOperationCommand{patient, "RemoveBodyPart", surgPart(sim, patient, "left eye")});
    SurgLog log;
    // Stop while the doctor carries something to the patient.
    SURG_REQUIRE(surgRunUntil(sim, log, 3000, [&] {
        const Job& job = sim.things().registry().get<Worker>(doctor).job;
        return job.type == JobType::Operate && job.step >= 2;
    }));
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, surgDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    SURG_REQUIRE(loaded.error.empty());
    SURG_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(saveSim(copy) == bytes);
    SURG_REQUIRE(copy.surgery().of(patient) != nullptr);
    CHECK(copy.surgery().of(patient)->list.size() == sim.surgery().of(patient)->list.size());
    CHECK(copy.surgery().of(patient)->nextId == sim.surgery().of(patient)->nextId);
    CHECK(copy.health().addedAt(fitted, surgPart(copy, fitted, "right leg")) == sim.defs().idOf("Hediffs", "PegLeg"));
    CHECK(copy.things().registry().get<Worker>(doctor).job.type == JobType::Operate);
    sim.stepMany(4000);
    copy.stepMany(4000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(saveSim(copy) == saveSim(sim));
}

TEST_CASE("Surgery: a recorded session with operations replays to the same state") {
    SimSettings settings;
    settings.seed = 11;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, surgDefs());
    std::vector<Entity> colonists;
    for (const auto [pawn, colonist] : recorded.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        colonists.push_back(pawn);
    }
    std::sort(colonists.begin(), colonists.end());
    SURG_REQUIRE(colonists.size() >= 2);
    recorded.stepMany(5);
    recorded.submit(AddOperationCommand{colonists[0], "RemoveBodyPart", surgPart(recorded, colonists[0], "left eye")});
    recorded.submit(AddOperationCommand{colonists[1], "AmputateLimb", surgPart(recorded, colonists[1], "right arm")});
    recorded.submit(RemoveOperationCommand{colonists[1], 1});
    recorded.stepMany(3000);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("AddOperation") != std::string::npos);
    CHECK(text.find("RemoveOperation") != std::string::npos);
    Replay parsed;
    std::string error;
    SURG_REQUIRE(readReplay(text, parsed, error));
    Sim again(parsed.settings, surgDefs());
    playReplay(parsed, again, replay.endStep);
    const ReplayCheck check = checkReplay(parsed, again);
    CHECK(check.matched());
    CHECK(again.stateHash() == recorded.stateHash());
}
