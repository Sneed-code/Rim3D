#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Medicine.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define MED_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/medicine.md are what is tested.
std::shared_ptr<const DefDatabase> medDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim medSim() { return Sim(SimSettings{7, 64, 0, 0, true}, medDefs()); }

// A colonist that works all day.
Entity medColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    return pawn;
}

// A colonist who is nobody's doctor, so the patient in a test is not tended by itself.
Entity medPatient(Sim& sim, CellPos cell) {
    const Entity pawn = medColonist(sim, cell);
    sim.submit(SetWorkPriorityCommand{pawn, "Doctor", 0});
    return pawn;
}

int medPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == label) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool medHit(Sim& sim, Entity pawn, const char* type, float amount, const std::string& part) {
    Rng rng(99);
    return sim.health().applyDamage(pawn, sim.health().damageTypeId(type), amount, medPart(sim, pawn, part), rng);
}

// Four bad bruises put a pawn down with pain, and a cut on the arm makes it bleed until someone tends it. All of it
// heals, in the end (a lost leg would not).
void medDownBleeding(Sim& sim, Entity pawn) {
    for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
        medHit(sim, pawn, "Blunt", 20.0f, part);
    }
    medHit(sim, pawn, "Cut", 6.0f, "right arm");
}

// Everything that happened, step by step.
struct MedLog {
    std::vector<Event> events;
    void step(Sim& sim) {
        sim.step();
        events.insert(events.end(), sim.events().begin(), sim.events().end());
    }
    const Event* find(const std::function<bool(const Event&)>& test) const {
        const auto it = std::find_if(events.begin(), events.end(), test);
        return it == events.end() ? nullptr : &*it;
    }
    const Event* job(JobType type, Entity pawn, bool started = true) const {
        return find([&](const Event& e) {
            return e.type == (started ? EventType::JobStarted : EventType::JobEnded) && e.job == type && e.thing == pawn;
        });
    }
    const Event* type(EventType type) const {
        return find([&](const Event& e) { return e.type == type; });
    }
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
};

bool medRunUntil(Sim& sim, MedLog& log, int64_t limit, const std::function<bool()>& done) {
    for (int64_t i = 0; i < limit; ++i) {
        log.step(sim);
        if (done()) {
            return true;
        }
    }
    return false;
}

int medItems(const Sim& sim, const char* def) {
    int total = 0;
    const DefId id = sim.defs().idOf("Things", def);
    for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
        (void)item;
        total += thing.def == id ? stack.count : 0;
    }
    return total;
}

CellPos medCell(const Sim& sim, Entity pawn) { return cellOf(sim.things().registry().get<Position>(pawn)); }

HealthReport medReport(const Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    return report;
}

int medHealable(const HealthReport& report) {
    return static_cast<int>(std::count_if(report.injuries.begin(), report.injuries.end(),
                                          [](const HealthReport::InjuryLine& l) { return !l.permanent; }));
}

}  // namespace

TEST_CASE("Medicine: tend quality follows the doctor's skill, the medicine and the bed") {
    Sim sim = medSim();
    const MedicineSystem& medicine = sim.medicine();
    MED_REQUIRE(medicine.rules().present);
    CHECK(medicine.kinds().size() == 3);
    const MedicineKind* herbal = medicine.kindOf(sim.defs().idOf("Things", "MedicineHerbal"));
    const MedicineKind* normal = medicine.kindOf(sim.defs().idOf("Things", "MedicineIndustrial"));
    const MedicineKind* lumen = medicine.kindOf(sim.defs().idOf("Things", "MedicineLumen"));
    MED_REQUIRE(herbal != nullptr && normal != nullptr && lumen != nullptr);
    CHECK(medicine.kinds().front().def == lumen->def);  // strongest first
    CHECK(medicine.kindOf(sim.defs().idOf("Things", "Steel")) == nullptr);

    // The wiki's skill table: 0.2 + 0.1 a level up to 1.0 at 8, then 0.05 a level to 1.5 at 18, 1.55 at 20.
    CHECK(medicine.skillQuality(0) == doctest::Approx(0.2f));
    CHECK(medicine.skillQuality(5) == doctest::Approx(0.7f));
    CHECK(medicine.skillQuality(8) == doctest::Approx(1.0f));
    CHECK(medicine.skillQuality(10) == doctest::Approx(1.1f));
    CHECK(medicine.skillQuality(18) == doctest::Approx(1.5f));
    CHECK(medicine.skillQuality(20) == doctest::Approx(1.55f));
    CHECK(medicine.skillQuality(30) == doctest::Approx(1.55f));

    // The roll 0.5 is the middle of 0.75 to 1.25, so it leaves the value alone.
    CHECK(medicine.tendQuality(5, 1.0f, normal, 0.0f, false, 0.5f) == doctest::Approx(0.7f));
    // Hurt hands tend worse; better medicine, better quality, up to what the medicine allows.
    CHECK(medicine.tendQuality(5, 0.5f, normal, 0.0f, false, 0.5f) == doctest::Approx(0.35f));
    CHECK(medicine.tendQuality(5, 1.0f, herbal, 0.0f, false, 0.5f) == doctest::Approx(0.42f));
    CHECK(medicine.tendQuality(15, 1.0f, lumen, 0.0f, false, 0.5f) == doctest::Approx(1.3f));  // 2.16 capped at 1.3
    CHECK(medicine.tendQuality(10, 1.0f, normal, 0.0f, false, 0.5f) == doctest::Approx(1.0f));  // 1.1 capped at 1.0
    CHECK(medicine.tendQuality(10, 1.0f, herbal, 0.5f, false, 1.0f) == doctest::Approx(0.7f));   // capped at 0.7
    // Without medicine: 0.7 of the skill, and never above 0.7. A hospital bed adds its offset.
    CHECK(medicine.tendQuality(5, 1.0f, nullptr, 0.0f, false, 0.5f) == doctest::Approx(0.49f));
    CHECK(medicine.tendQuality(5, 1.0f, nullptr, 0.15f, false, 0.5f) == doctest::Approx(0.64f));
    CHECK(medicine.tendQuality(20, 1.0f, nullptr, 0.0f, false, 1.0f) == doctest::Approx(0.7f));
    // The roll runs from 0.75 to 1.25 of the value, and tending yourself costs 30%.
    CHECK(medicine.tendQuality(5, 1.0f, normal, 0.0f, false, 0.0f) == doctest::Approx(0.525f));
    CHECK(medicine.tendQuality(5, 1.0f, normal, 0.0f, false, 1.0f) == doctest::Approx(0.875f));
    CHECK(medicine.tendQuality(5, 1.0f, normal, 0.0f, true, 0.5f) == doctest::Approx(0.49f));
}

TEST_CASE("Medicine: the care setting says which medicine a patient may get, and a command sets it") {
    Sim sim = medSim();
    const MedicineSystem& medicine = sim.medicine();
    const MedicineKind* herbal = medicine.kindOf(sim.defs().idOf("Things", "MedicineHerbal"));
    const MedicineKind* normal = medicine.kindOf(sim.defs().idOf("Things", "MedicineIndustrial"));
    const MedicineKind* lumen = medicine.kindOf(sim.defs().idOf("Things", "MedicineLumen"));
    MED_REQUIRE(herbal != nullptr && normal != nullptr && lumen != nullptr);
    CHECK(medicine.allows(MedicalCare::Best, *lumen));
    CHECK(medicine.allows(MedicalCare::Normal, *normal));
    CHECK_FALSE(medicine.allows(MedicalCare::Normal, *lumen));
    CHECK(medicine.allows(MedicalCare::Herbal, *herbal));
    CHECK_FALSE(medicine.allows(MedicalCare::Herbal, *normal));
    CHECK_FALSE(medicine.allows(MedicalCare::NoMedicine, *herbal));
    CHECK_FALSE(medicine.allows(MedicalCare::NoCare, *herbal));

    const Entity pawn = medColonist(sim, CellPos{10, 10, 0});
    CHECK(medicine.careOf(pawn) == MedicalCare::Best);  // the default
    sim.submit(SetMedicalCareCommand{pawn, "Herbal"});
    sim.step();
    CHECK(medicine.careOf(pawn) == MedicalCare::Herbal);
    sim.submit(SetMedicalCareCommand{pawn, "Lots"});
    sim.step();
    CHECK(std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) {
        return e.type == EventType::CommandFailed && e.message.find("no medical care 'Lots'") == 0;
    }));
    CHECK(medicine.careOf(pawn) == MedicalCare::Herbal);
    const Entity steel = sim.things().spawn("Steel", CellPos{12, 12, 0}, 5);
    sim.submit(SetMedicalCareCommand{steel, "Best"});
    sim.step();
    CHECK(std::any_of(sim.events().begin(), sim.events().end(), [](const Event& e) {
        return e.type == EventType::CommandFailed && e.message == "that thing has no body to care for";
    }));
    MedicalCare parsed = MedicalCare::NoCare;
    CHECK(medicalCareFromName(medicalCareName(MedicalCare::Normal), parsed));
    CHECK(parsed == MedicalCare::Normal);
    CHECK_FALSE(medicalCareFromName("normal", parsed));
}

TEST_CASE("Medicine: needsTending and needsRest follow the injuries") {
    Sim sim = medSim();
    const Entity pawn = medColonist(sim, CellPos{10, 10, 0});
    sim.step();
    CHECK_FALSE(sim.health().needsTending(pawn));
    CHECK_FALSE(sim.health().needsRest(pawn));
    // A bruise bleeds nothing and can't be tended.
    medHit(sim, pawn, "Blunt", 3.0f, "torso");
    sim.step();
    CHECK_FALSE(sim.health().needsTending(pawn));
    CHECK_FALSE(sim.health().needsRest(pawn));
    medHit(sim, pawn, "Cut", 5.0f, "left arm");
    sim.step();
    CHECK(sim.health().needsTending(pawn));
    CHECK(sim.health().urgentlyNeedsTending(pawn));  // it bleeds
    CHECK(sim.health().needsRest(pawn));
    CHECK(sim.health().tendAll(pawn, 0.8f) == 2);
    sim.step();
    CHECK_FALSE(sim.health().needsTending(pawn));
    CHECK_FALSE(sim.health().urgentlyNeedsTending(pawn));
    CHECK(sim.health().needsRest(pawn));  // a tended wound is still healing
    // The report carries the quality.
    const HealthReport report = medReport(sim, pawn);
    MED_REQUIRE(report.injuries.size() == 2);
    CHECK(report.injuries[1].tendQuality == doctest::Approx(0.8f));
    CHECK_FALSE(report.injuries[1].permanent);
}

TEST_CASE("Medicine: a healed injury may leave a scar, a big one an old wound, and a bruise never") {
    Sim sim = medSim();
    const DefId cut = sim.defs().idOf("Hediffs", "Cut");
    const DefId bruise = sim.defs().idOf("Hediffs", "Bruise");
    MED_REQUIRE(cut.valid() && bruise.valid());
    // Five hundred pawns each with a nearly healed injury that was once large (14 hit points), 500 with a small one
    // (3), and 500 with a bruise. The scar chance is 2% for the large ones, 0 for the others.
    struct Group {
        DefId hediff;
        float peak;
        std::vector<Entity> pawns;
    };
    std::vector<Group> groups = {{cut, 14.0f, {}}, {cut, 3.0f, {}}, {bruise, 14.0f, {}}, {cut, 8.0f, {}}};
    int n = 0;
    for (Group& group : groups) {
        for (int i = 0; i < 500; ++i, ++n) {
            const Entity pawn = sim.things().spawn("Human", CellPos{2 + n % 55, 2 + n / 55, 0});
            MED_REQUIRE(pawn != kNoEntity);
            Health& health = sim.things().registry().get<Health>(pawn);
            health.injuries.push_back(Injury{0, group.hediff, 0.05f, -1.0f, group.peak});
            health.dirty = true;
            group.pawns.push_back(pawn);
        }
    }
    MedLog log;
    for (int i = 0; i < 1000; ++i) {
        log.step(sim);
    }
    const auto scars = [&](const Group& group, const char* hediff) {
        int found = 0;
        for (const Entity pawn : group.pawns) {
            const Health& health = sim.things().registry().get<Health>(pawn);
            for (const Injury& injury : health.injuries) {
                if (sim.defs().get(injury.hediff).defName() == hediff) {
                    ++found;
                    CHECK(injury.severity == doctest::Approx(1.0f));  // HealthRules.scarSeverity
                }
            }
            CHECK(health.injuries.size() <= 1);
        }
        return found;
    };
    const int big = scars(groups[0], "OldWound");
    CHECK(scars(groups[0], "Scar") == 0);
    CHECK(big >= 1);
    CHECK(big <= 30);  // 2% of 500 is 10
    CHECK(scars(groups[1], "OldWound") + scars(groups[1], "Scar") == 0);  // too small: under 4 hit points lost
    CHECK(scars(groups[2], "OldWound") + scars(groups[2], "Scar") == 0);  // bruises never scar
    const int middle = scars(groups[3], "Scar");  // 8 of 14: about 0.9% of 500 are scars, none old wounds
    CHECK(scars(groups[3], "OldWound") == 0);
    CHECK(middle <= 20);
    CHECK(log.count(EventType::PawnScarred) == big + middle);
    const Event* scarEvent = log.type(EventType::PawnScarred);
    MED_REQUIRE(scarEvent != nullptr);
    CHECK(scarEvent->amount == doctest::Approx(1.0f));

    // A scar never heals and doesn't count as needing anything.
    for (const Entity pawn : groups[0].pawns) {
        if (sim.health().needsTending(pawn) || sim.health().needsRest(pawn)) {
            CHECK(false);
        }
    }
    const auto pawnWithScar = std::find_if(groups[0].pawns.begin(), groups[0].pawns.end(), [&](Entity pawn) {
        return !sim.things().registry().get<Health>(pawn).injuries.empty();
    });
    MED_REQUIRE(pawnWithScar != groups[0].pawns.end());
    const Entity scarred = *pawnWithScar;
    for (const Group& group : groups) {
        for (const Entity pawn : group.pawns) {
            if (pawn != scarred) {
                sim.things().destroy(pawn);  // the days that follow are cheaper with one pawn
            }
        }
    }
    sim.stepMany(3 * 86400);
    const HealthReport report = medReport(sim, scarred);
    MED_REQUIRE(report.injuries.size() == 1);
    CHECK(report.injuries[0].permanent);
    CHECK(report.injuries[0].severity == doctest::Approx(1.0f));
}

TEST_CASE("Medicine: a doctor tends a bleeding, downed colonist where it lies, then carries it to the hospital bed") {
    Sim sim = medSim();
    sim.mentalBreaks().setNatural(false);  // it runs for days
    const Entity bed = sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 5);
    sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 20);
    MED_REQUIRE(bed != kNoEntity);
    const Entity doctor = medColonist(sim, CellPos{10, 10, 0});
    const Entity patient = medPatient(sim, CellPos{30, 30, 0});
    sim.step();
    medDownBleeding(sim, patient);
    sim.step();
    MED_REQUIRE(sim.things().registry().get<Health>(patient).downed);
    MED_REQUIRE(sim.health().urgentlyNeedsTending(patient));

    MedLog log;
    MED_REQUIRE(medRunUntil(sim, log, 3000, [&] { return log.type(EventType::PawnTended) != nullptr; }));
    const Event* tended = log.type(EventType::PawnTended);
    CHECK(tended->thing == doctor);
    CHECK(tended->target == patient);
    CHECK(tended->message == "medicine");
    CHECK(tended->def == sim.defs().idOf("Things", "MedicineIndustrial"));
    CHECK(tended->amount >= 0.4f);  // skill 8 (0.68) x medicine 1.0 x a roll of 0.7 to 1.3
    CHECK(tended->amount <= 1.0f);
    CHECK(medItems(sim, "MedicineIndustrial") == 4);
    CHECK(log.job(JobType::TendPatient, doctor) != nullptr);
    CHECK(log.type(EventType::PawnDied) == nullptr);
    CHECK_FALSE(sim.health().needsTending(patient));
    CHECK(sim.things().registry().get<Health>(patient).bleedPerDay == 0.0f);
    CHECK(medCell(sim, patient) == CellPos{30, 30, 0});  // tended where it lay

    // Then the rescue: the bed's first cell, the hospital bed's rate of healing.
    MED_REQUIRE(medRunUntil(sim, log, 3000, [&] {
        return log.find([&](const Event& e) { return e.type == EventType::PawnAction && e.action == PawnAction::LayPawn; }) != nullptr;
    }));
    CHECK(log.job(JobType::Rescue, doctor) != nullptr);
    CHECK(log.find([&](const Event& e) { return e.type == EventType::PawnAction && e.action == PawnAction::LiftPawn && e.target == patient; }) != nullptr);
    CHECK(medCell(sim, patient) == CellPos{20, 20, 0});
    CHECK(sim.things().registry().get<Health>(patient).rest == RestKind::HospitalBed);
    CHECK(sim.things().registry().get<Health>(patient).downed);
    CHECK(sim.jobs().bedUnder(patient) == bed);
    MED_REQUIRE(medRunUntil(sim, log, 200, [&] { return log.job(JobType::Rescue, doctor, false) != nullptr; }));
    CHECK(log.job(JobType::Rescue, doctor, false)->message == "done");

    // It heals in the bed, gets up, rests until it is well, and the doctor is free again.
    MED_REQUIRE(medRunUntil(sim, log, 4 * 86400, [&] { return log.type(EventType::PawnGotUp) != nullptr; }));
    CHECK(log.type(EventType::PawnDied) == nullptr);
    // (It may eat first: it has been down a day.)
    MED_REQUIRE(medRunUntil(sim, log, 3000, [&] { return log.job(JobType::BedRest, patient) != nullptr; }));
    // (It gets up to eat and goes back: the last time it gets up, it is well.)
    MED_REQUIRE(medRunUntil(sim, log, 8 * 86400, [&] { return medHealable(medReport(sim, patient)) == 0; }));
    MED_REQUIRE(medRunUntil(sim, log, 200, [&] {
        return sim.things().registry().get<Worker>(patient).job.type != JobType::BedRest;
    }));
    CHECK(log.find([&](const Event& e) {
        return e.type == EventType::JobEnded && e.thing == patient && e.job == JobType::BedRest && e.message == "done";
    }) != nullptr);
    CHECK(medHealable(medReport(sim, patient)) == 0);
    CHECK(sim.things().registry().get<Health>(patient).rest == RestKind::Ground);
    CHECK_FALSE(std::any_of(log.events.begin(), log.events.end(), [](const Event& e) { return e.type == EventType::CommandFailed; }));
}

TEST_CASE("Medicine: without medicine, or when the patient wants none, the doctor tends by hand") {
    for (const char* care : {"Best", "NoMedicine"}) {
        Sim sim = medSim();
        sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 3);
        const Entity doctor = medColonist(sim, CellPos{10, 10, 0});
        const Entity patient = medPatient(sim, CellPos{14, 10, 0});
        if (std::string(care) == "Best") {
            // No medicine in reach of this one: take it away.
            for (const auto [item, thing] : sim.things().registry().view<const Thing>().each()) {
                if (thing.def == sim.defs().idOf("Things", "MedicineIndustrial")) {
                    sim.things().destroy(item);
                    break;
                }
            }
            CHECK(medItems(sim, "MedicineIndustrial") == 0);
        } else {
            sim.submit(SetMedicalCareCommand{patient, care});
        }
        sim.step();
        medHit(sim, patient, "Cut", 6.0f, "left arm");
        MedLog log;
        MED_REQUIRE(medRunUntil(sim, log, 3000, [&] { return log.type(EventType::PawnTended) != nullptr; }));
        const Event* tended = log.type(EventType::PawnTended);
        CHECK(tended->thing == doctor);
        CHECK(tended->message == "no medicine");
        CHECK_FALSE(tended->def.valid());
        CHECK(tended->amount > 0.0f);
        CHECK(tended->amount <= 0.7f);  // the most tending by hand can do
        CHECK(medItems(sim, "MedicineIndustrial") == (std::string(care) == "Best" ? 0 : 3));
    }
}

TEST_CASE("Medicine: a patient who wants no care is left alone, and the herbal setting leaves strong medicine be") {
    {
        Sim sim = medSim();
        medColonist(sim, CellPos{10, 10, 0});
        const Entity patient = medPatient(sim, CellPos{14, 10, 0});
        sim.submit(SetMedicalCareCommand{patient, "NoCare"});
        sim.step();
        medHit(sim, patient, "Cut", 6.0f, "left arm");
        MedLog log;
        for (int i = 0; i < 3000; ++i) {
            log.step(sim);
        }
        CHECK(log.type(EventType::PawnTended) == nullptr);
        CHECK(sim.health().needsTending(patient));
    }
    {
        Sim sim = medSim();
        sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 3);
        sim.things().spawn("MedicineHerbal", CellPos{12, 14, 0}, 3);
        const Entity doctor = medColonist(sim, CellPos{10, 10, 0});
        const Entity patient = medPatient(sim, CellPos{14, 10, 0});
        sim.submit(SetMedicalCareCommand{patient, "Herbal"});
        sim.step();
        const Entity picked = sim.jobs().findMedicine(doctor, patient);
        MED_REQUIRE(picked != kNoEntity);
        CHECK(sim.things().registry().get<Thing>(picked).def == sim.defs().idOf("Things", "MedicineHerbal"));
        sim.submit(SetMedicalCareCommand{patient, "Best"});
        sim.step();
        const Entity best = sim.jobs().findMedicine(doctor, patient);
        MED_REQUIRE(best != kNoEntity);
        CHECK(sim.things().registry().get<Thing>(best).def == sim.defs().idOf("Things", "MedicineIndustrial"));
    }
}

TEST_CASE("Medicine: a patient walking about in a mental break is not chased, and is tended once it is over") {
    Sim sim = medSim();
    medColonist(sim, CellPos{10, 10, 0});
    const Entity patient = medPatient(sim, CellPos{14, 10, 0});
    sim.step();
    medHit(sim, patient, "Cut", 6.0f, "left arm");
    sim.submit(StartMentalBreakCommand{patient, "WanderSad"});
    sim.step();
    MED_REQUIRE(sim.mentalBreaks().active(patient));
    MedLog log;
    for (int i = 0; i < 1500; ++i) {
        log.step(sim);
        if (!sim.mentalBreaks().active(patient)) {
            break;
        }
    }
    CHECK(sim.stats().repaths == 0);
    CHECK(log.type(EventType::PawnTended) == nullptr);
    sim.submit(EndMentalBreakCommand{patient});
    MED_REQUIRE(medRunUntil(sim, log, 4000, [&] { return log.type(EventType::PawnTended) != nullptr; }));
}

TEST_CASE("Medicine: with no hospital bed the downed go to an ordinary free bed, and a hospital bed is not slept in") {
    Sim sim = medSim();
    const Entity ordinary = sim.things().spawn("Bed", CellPos{20, 20, 0});
    MED_REQUIRE(ordinary != kNoEntity);
    sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 20);
    medColonist(sim, CellPos{10, 10, 0});
    const Entity patient = medPatient(sim, CellPos{30, 30, 0});
    sim.step();
    // The patient tends itself first? No: it is down. The doctor has no medicine and tends by hand, then carries.
    medDownBleeding(sim, patient);
    MedLog log;
    MED_REQUIRE(medRunUntil(sim, log, 6000, [&] {
        return log.find([&](const Event& e) { return e.type == EventType::PawnAction && e.action == PawnAction::LayPawn; }) != nullptr;
    }));
    CHECK(sim.jobs().bedUnder(patient) == ordinary);
    CHECK(sim.things().registry().get<Health>(patient).rest == RestKind::Bed);
    CHECK(sim.things().registry().get<Bed>(ordinary).owners.empty());  // it was only borrowed

    // A tired colonist with only a hospital bed sleeps on the ground.
    Sim second = medSim();
    const Entity hospital = second.things().spawn("HospitalBed", CellPos{20, 20, 0});
    const Entity sleeper = medColonist(second, CellPos{22, 20, 0});
    second.step();
    second.things().registry().get<Needs>(sleeper).rest = 0.02f;
    for (int h = 0; h < 24; ++h) {
        second.submit(SetScheduleCommand{sleeper, h, ScheduleKind::Sleep});
    }
    MedLog sleepLog;
    MED_REQUIRE(medRunUntil(second, sleepLog, 600, [&] {
        return jobIsLyingDown(second.things().registry().get<Worker>(sleeper).job);
    }));
    CHECK(second.things().registry().get<Worker>(sleeper).job.thing == kNoEntity);  // the ground
    CHECK(second.things().registry().get<Bed>(hospital).owners.empty());
}

TEST_CASE("Medicine: an idle doctor drops what it is doing for a patient within seconds") {
    Sim sim = medSim();
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 3);
    const Entity doctor = medColonist(sim, CellPos{10, 10, 0});
    const Entity patient = medPatient(sim, CellPos{14, 10, 0});
    MedLog log;
    for (int i = 0; i < 200; ++i) {
        log.step(sim);  // nothing to do: the doctor waits
    }
    CHECK(sim.things().registry().get<Worker>(doctor).job.type == JobType::Wait);
    medHit(sim, patient, "Cut", 6.0f, "left arm");
    MED_REQUIRE(medRunUntil(sim, log, 30, [&] { return log.job(JobType::TendPatient, doctor) != nullptr; }));
    CHECK(log.find([&](const Event& e) {
        return e.type == EventType::JobEnded && e.thing == doctor && e.job == JobType::Wait && e.message == "interrupted: emergency";
    }) != nullptr);
}

TEST_CASE("Medicine: a rescue in the middle, medical care and scars save and load, and a loaded game goes on the same") {
    Sim sim = medSim();
    sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 5);
    sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 20);
    const Entity doctor = medColonist(sim, CellPos{10, 10, 0});
    const Entity patient = medPatient(sim, CellPos{30, 30, 0});
    sim.submit(SetMedicalCareCommand{patient, "Normal"});
    sim.step();
    medDownBleeding(sim, patient);
    // Inject a scar and a peak so the new fields are in the file.
    Health& health = sim.things().registry().get<Health>(patient);
    health.injuries.push_back(Injury{0, sim.defs().idOf("Hediffs", "Scar"), 1.0f, -1.0f, 1.0f});
    health.dirty = true;
    MedLog log;
    // Stop while the doctor carries the patient.
    MED_REQUIRE(medRunUntil(sim, log, 8000, [&] {
        const Job& job = sim.things().registry().get<Worker>(doctor).job;
        return job.type == JobType::Rescue && job.step >= 2;
    }));
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, medDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    MED_REQUIRE(loaded.error.empty());
    MED_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.medicine().careOf(patient) == MedicalCare::Normal);
    CHECK(copy.things().registry().get<Worker>(doctor).job.type == JobType::Rescue);
    CHECK(saveSim(copy) == bytes);
    const Health& loadedHealth = copy.things().registry().get<Health>(patient);
    MED_REQUIRE(loadedHealth.injuries.size() == health.injuries.size());
    for (size_t i = 0; i < health.injuries.size(); ++i) {
        CHECK(loadedHealth.injuries[i].peak == doctest::Approx(health.injuries[i].peak));
    }

    sim.stepMany(5000);
    copy.stepMany(5000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(sim.jobs().bedUnder(patient) != kNoEntity);
    CHECK(copy.jobs().bedUnder(patient) == sim.jobs().bedUnder(patient));
    CHECK(saveSim(copy) == saveSim(sim));
}

TEST_CASE("Medicine: a recorded session with medical care and tending replays to the same state") {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, medDefs());
    std::vector<Entity> colonists;
    for (const auto [pawn, colonist] : recorded.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        colonists.push_back(pawn);
    }
    std::sort(colonists.begin(), colonists.end());
    MED_REQUIRE(colonists.size() >= 2);
    recorded.stepMany(5);
    recorded.submit(SetMedicalCareCommand{colonists[0], "Normal"});
    recorded.submit(DamagePawnCommand{colonists[0], "Cut", 8.0f, -1});
    recorded.stepMany(4000);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("SetMedicalCare") != std::string::npos);
    Replay parsed;
    std::string error;
    MED_REQUIRE(readReplay(text, parsed, error));
    CHECK(error.empty());
    Sim again(parsed.settings, medDefs());
    playReplay(parsed, again, replay.endStep);
    CHECK(again.medicine().careOf(colonists[0]) == MedicalCare::Normal);
    const ReplayCheck check = checkReplay(parsed, again);
    CHECK(check.matched());
    CHECK(check.compared > 5);
    CHECK(again.stateHash() == recorded.stateHash());
}
