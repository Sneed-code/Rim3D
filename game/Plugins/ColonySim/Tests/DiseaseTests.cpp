#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Spoilage.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define DIS_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/diseases.md are what is tested.
std::shared_ptr<const DefDatabase> disDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim disSim() { return Sim(SimSettings{7, 64, 0, 0, true}, disDefs()); }

// A bare pawn: a body and nothing else, so nothing but the disease moves its health.
Entity disPawn(Sim& sim, CellPos cell = CellPos{10, 10, 0}) { return sim.things().spawn("Human", cell); }

DefId disId(const Sim& sim, const char* name) { return sim.health().diseaseId(name); }

Health& disHealth(Sim& sim, Entity pawn) { return sim.things().registry().get<Health>(pawn); }

HealthReport disReport(const Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    return report;
}

int disPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == label) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct DisLog {
    std::vector<Event> events;
    void step(Sim& sim) {
        sim.step();
        events.insert(events.end(), sim.events().begin(), sim.events().end());
    }
    int count(EventType type) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }));
    }
    const Event* type(EventType type) const {
        const auto it = std::find_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
        return it == events.end() ? nullptr : &*it;
    }
};

// Runs `seconds`, tending the pawn at `quality` whenever a doctor could (each time a tend has run out), if quality > 0.
void disRun(Sim& sim, DisLog& log, Entity pawn, int64_t seconds, float quality) {
    for (int64_t i = 0; i < seconds; ++i) {
        log.step(sim);
        if (quality > 0.0f && !disHealth(sim, pawn).dead && sim.health().needsTending(pawn)) {
            sim.health().tendAll(pawn, quality);
        }
    }
}

}  // namespace

TEST_CASE("Diseases: the data has them, and contracting one follows the rules") {
    Sim sim = disSim();
    const Entity pawn = disPawn(sim);
    sim.step();
    for (const char* name : {"Infection", "Flu", "Plague", "Malaria", "SleepingSickness", "GutWorms", "MuscleParasites", "FoodPoisoning"}) {
        CHECK(sim.health().diseaseId(name).valid());
    }
    CHECK_FALSE(sim.health().diseaseId("Cut").valid());   // an injury
    CHECK_FALSE(sim.health().diseaseId("Hypothermia").valid());
    CHECK_FALSE(sim.health().diseaseId("Nothing").valid());
    const DefId flu = disId(sim, "Flu");
    CHECK_FALSE(sim.health().sick(pawn));
    DisLog log;
    CHECK(sim.health().contract(pawn, flu));
    CHECK_FALSE(sim.health().contract(pawn, flu));  // has it already
    CHECK(sim.health().sick(pawn));
    CHECK(sim.health().diseaseSeverity(pawn, flu) == doctest::Approx(0.001f));
    CHECK(sim.health().immunity(pawn, flu) == 0.0f);
    // An infection needs the wound, and a missing hand has none.
    const DefId infection = disId(sim, "Infection");
    CHECK_FALSE(sim.health().contract(pawn, infection));
    CHECK_FALSE(sim.health().contract(pawn, infection, -1.0f, 9999));
    CHECK(sim.health().contract(pawn, infection, -1.0f, disPart(sim, pawn, "left arm")));
    CHECK(sim.health().contract(pawn, infection, -1.0f, disPart(sim, pawn, "right arm")));
    CHECK_FALSE(sim.health().contract(pawn, infection, -1.0f, disPart(sim, pawn, "right arm")));
    log.step(sim);
    CHECK(log.count(EventType::PawnSick) == 0);  // those came before the log started
    const HealthReport report = disReport(sim, pawn);
    CHECK(report.diseases.size() == 3);
    // A dead pawn, a thing with no body and a full immunity refuse.
    sim.health().setImmunity(pawn, disId(sim, "Plague"), 1.0f);
    CHECK_FALSE(sim.health().contract(pawn, disId(sim, "Plague")));
    CHECK_FALSE(sim.health().contract(kNoEntity, flu));
    const Entity rock = sim.things().spawn("MealSimple", CellPos{12, 12, 0});
    CHECK_FALSE(sim.health().contract(rock, flu));
    CHECK(sim.health().kill(pawn, DeathCause::BloodLoss));
    CHECK_FALSE(sim.health().contract(pawn, disId(sim, "Malaria")));
}

TEST_CASE("Diseases: the command catches one, or says why not") {
    Sim sim = disSim();
    const Entity pawn = disPawn(sim);
    sim.step();
    DisLog log;
    sim.submit(ContractDiseaseCommand{pawn, "Malaria", -1.0f, -1});
    log.step(sim);
    CHECK(sim.health().diseaseSeverity(pawn, disId(sim, "Malaria")) > 0.0f);
    CHECK(log.count(EventType::PawnSick) == 1);
    CHECK(log.type(EventType::PawnSick)->thing == pawn);
    sim.submit(ContractDiseaseCommand{pawn, "Malaria", -1.0f, -1});   // again
    sim.submit(ContractDiseaseCommand{pawn, "Cut", -1.0f, -1});       // not a disease
    sim.submit(ContractDiseaseCommand{kNoEntity, "Flu", -1.0f, -1});  // nobody
    log.step(sim);
    CHECK(log.count(EventType::CommandFailed) == 3);
}

TEST_CASE("Diseases: the race between immunity and severity (flu: 0.2388 immunity a day against 0.2488 severity)") {
    Sim sim = disSim();
    const Entity pawn = disPawn(sim);
    sim.step();
    const DefId flu = disId(sim, "Flu");
    sim.health().contract(pawn, flu);
    DisLog log;
    disRun(sim, log, pawn, 2 * 86400, 0.0f);
    CHECK(sim.health().immunity(pawn, flu) == doctest::Approx(0.2388f * 2).epsilon(0.01));
    CHECK(sim.health().diseaseSeverity(pawn, flu) == doctest::Approx(0.001f + 0.2488f * 2).epsilon(0.01));
    // Day 2 is the flu's "minor" stage; it shows in the report and the capacities.
    HealthReport report = disReport(sim, pawn);
    DIS_REQUIRE(report.diseases.size() == 1);
    CHECK(report.diseases[0].stage == "minor");
    CHECK_FALSE(report.diseases[0].tended);
    CHECK(report.diseases[0].immunity == doctest::Approx(0.4776f).epsilon(0.01));
    float breathing = 1.0f;
    for (const auto& c : report.capacities) {
        breathing = c.name == "Breathing" ? c.value : breathing;
    }
    CHECK(breathing == doctest::Approx(0.9f).epsilon(0.02));
    disRun(sim, log, pawn, 86400, 0.0f);  // day 3: severity 0.75, the major stage
    CHECK(disReport(sim, pawn).diseases[0].stage == "major");

    // Nobody tends it, on the bare ground: severity reaches 1 at day 4.02, before immunity does (day 4.19), and the pawn dies.
    disRun(sim, log, pawn, static_cast<int64_t>(1.1f * 86400), 0.0f);
    CHECK(disHealth(sim, pawn).dead);
    CHECK(disHealth(sim, pawn).cause == DeathCause::Disease);
    const Event* died = log.type(EventType::PawnDied);
    DIS_REQUIRE(died != nullptr);
    CHECK(died->message == "illness");
}

TEST_CASE("Diseases: a hospital bed or good tending lets a healthy pawn win the race, and immunity stays") {
    {   // A hospital bed: immunity gain x1.07 is enough, with a hair to spare.
        Sim sim = disSim();
        const Entity pawn = disPawn(sim);
        sim.step();
        const DefId flu = disId(sim, "Flu");
        disHealth(sim, pawn).rest = RestKind::HospitalBed;
        sim.health().contract(pawn, flu);
        DisLog log;
        disRun(sim, log, pawn, 6 * 86400, 0.0f);
        CHECK_FALSE(disHealth(sim, pawn).dead);
        CHECK(sim.health().diseaseSeverity(pawn, flu) == 0.0f);   // gone
        CHECK(log.count(EventType::PawnRecovered) == 1);
        CHECK(sim.health().immunity(pawn, flu) == doctest::Approx(1.0f));
        CHECK_FALSE(sim.health().sick(pawn));
        CHECK_FALSE(sim.health().contract(pawn, flu));  // immune now
    }
    {   // Tended every twelve hours at quality 1 on the ground: severity climbs 0.17 a day instead of 0.25.
        Sim sim = disSim();
        const Entity pawn = disPawn(sim);
        sim.step();
        const DefId flu = disId(sim, "Flu");
        sim.health().contract(pawn, flu);
        DisLog log;
        disRun(sim, log, pawn, 2 * 86400, 1.0f);
        CHECK(sim.health().diseaseSeverity(pawn, flu) == doctest::Approx(0.001f + (0.2488f - 0.0773f) * 2).epsilon(0.03));
        CHECK(disReport(sim, pawn).diseases[0].tended);
        disRun(sim, log, pawn, 8 * 86400, 1.0f);
        CHECK_FALSE(disHealth(sim, pawn).dead);
        CHECK(log.count(EventType::PawnRecovered) == 1);
        CHECK(sim.health().immunity(pawn, flu) == doctest::Approx(1.0f));
    }
}

TEST_CASE("Diseases: needsTending, needsRest and urgency follow the disease and its tends") {
    Sim sim = disSim();
    const Entity pawn = disPawn(sim);
    sim.step();
    CHECK_FALSE(sim.health().needsRest(pawn));
    sim.health().contract(pawn, disId(sim, "Flu"));
    CHECK(sim.health().needsTending(pawn));
    CHECK(sim.health().needsRest(pawn));
    CHECK_FALSE(sim.health().urgentlyNeedsTending(pawn));  // not bleeding, not dying
    CHECK(sim.health().tendAll(pawn, 0.9f) == 1);
    CHECK_FALSE(sim.health().needsTending(pawn));
    CHECK(sim.health().needsRest(pawn));
    CHECK(sim.health().tendAll(pawn, 0.9f) == 0);   // still working
    DisLog log;
    for (int i = 0; i < 12 * 3600 + 5; ++i) {
        log.step(sim);
    }
    CHECK(sim.health().needsTending(pawn));  // the tend has run out: half a day
    // A flu at its worst is urgent; food poisoning can't be tended at all.
    Sim other = disSim();
    const Entity second = disPawn(other);
    other.step();
    other.health().contract(second, disId(other, "Flu"), 0.9f);
    CHECK(other.health().urgentlyNeedsTending(second));
    other.health().contract(second, disId(other, "FoodPoisoning"));
    other.health().tendAll(second, 1.0f);
    HealthReport report = disReport(other, second);
    int tended = 0;
    for (const auto& line : report.diseases) {
        tended += line.tended ? 1 : 0;
    }
    CHECK(tended == 1);  // the flu
    CHECK(report.diseases.size() == 2);
}

TEST_CASE("Diseases: a wound may turn infected; tending in time makes that rarer; an untended infection kills") {
    // 300 pawns each take the same cut, half are tended at once with a good quality. The roll is the seed's, so the count is fixed.
    Sim sim = disSim();
    std::vector<Entity> untended;
    std::vector<Entity> tended;
    for (int i = 0; i < 300; ++i) {
        const Entity pawn = disPawn(sim, CellPos{2 + i % 50, 2 + i / 50, 0});
        (i % 2 == 0 ? untended : tended).push_back(pawn);
    }
    sim.step();
    for (const Entity pawn : untended) {
        CHECK(sim.health().applyHit(pawn, sim.health().damageTypeId("Cut"), 12.0f, disPart(sim, pawn, "left arm")));
    }
    for (const Entity pawn : tended) {
        CHECK(sim.health().applyHit(pawn, sim.health().damageTypeId("Cut"), 12.0f, disPart(sim, pawn, "left arm")));
    }
    int doomedUntended = 0;
    int doomedTended = 0;
    for (const Entity pawn : untended) {
        doomedUntended += disHealth(sim, pawn).injuries[0].infectAt >= 0 ? 1 : 0;
    }
    for (const Entity pawn : tended) {
        doomedTended += disHealth(sim, pawn).injuries[0].infectAt >= 0 ? 1 : 0;
        sim.health().tendAll(pawn, 1.0f);
    }
    // Cut: 15% (the room's cleanliness moves it a little).
    CHECK(doomedUntended >= 10);
    CHECK(doomedUntended <= 40);
    int stillDoomed = 0;
    for (const Entity pawn : tended) {
        stillDoomed += disHealth(sim, pawn).injuries[0].infectAt >= 0 ? 1 : 0;
    }
    CHECK(stillDoomed <= doomedTended / 2 + 2);  // quality 1 leaves 5% of them
    DisLog log;
    for (int i = 0; i < 20 * 3600; ++i) {  // past the longest delay (18 hours)
        log.step(sim);
    }
    int infectedUntended = 0;
    const DefId infection = disId(sim, "Infection");
    for (const Entity pawn : untended) {
        infectedUntended += sim.health().diseaseSeverity(pawn, infection) > 0.0f ? 1 : 0;
        for (const Disease& d : disHealth(sim, pawn).diseases) {
            CHECK(d.part == disPart(sim, pawn, "left arm"));
        }
    }
    CHECK(infectedUntended == doomedUntended);
    CHECK(log.count(EventType::PawnSick) == doomedUntended + stillDoomed);
    const Event* sick = log.type(EventType::PawnSick);
    DIS_REQUIRE(sick != nullptr);
    CHECK(sick->def == infection);
    CHECK(sick->part == disPart(sim, sick->thing, "left arm"));
    // The report names the wound.
    for (const Entity pawn : untended) {
        if (sim.health().diseaseSeverity(pawn, infection) > 0.0f) {
            const HealthReport report = disReport(sim, pawn);
            DIS_REQUIRE(report.diseases.size() == 1);
            CHECK(report.diseases[0].part == "left arm");
            CHECK(report.diseases[0].name == "infection");
            break;
        }
    }
}

TEST_CASE("Diseases: an infection untended is deadly in a day or so, and tended every twelve hours it is beaten") {
    Sim sim = disSim();
    const Entity lost = disPawn(sim, CellPos{10, 10, 0});
    const Entity saved = disPawn(sim, CellPos{12, 10, 0});
    sim.step();
    const DefId infection = disId(sim, "Infection");
    for (const Entity pawn : {lost, saved}) {
        sim.health().contract(pawn, infection, -1.0f, disPart(sim, pawn, "right leg"));
    }
    DisLog log;
    for (int64_t i = 0; i < 4 * 86400; ++i) {
        log.step(sim);
        if (!disHealth(sim, saved).dead && sim.health().needsTending(saved)) {
            sim.health().tendAll(saved, 1.0f);
        }
    }
    CHECK(disHealth(sim, lost).dead);
    CHECK(disHealth(sim, lost).cause == DeathCause::Disease);
    CHECK_FALSE(disHealth(sim, saved).dead);
    CHECK(sim.health().diseaseSeverity(saved, infection) == 0.0f);
    // An infection does not leave the pawn immune: it can happen again.
    CHECK(sim.health().immunity(saved, infection) == 0.0f);
    CHECK(sim.health().contract(saved, infection, -1.0f, disPart(sim, saved, "right leg")));
    CHECK(log.count(EventType::PawnDied) == 1);
}

TEST_CASE("Diseases: gut worms and muscle parasites don't progress and go after three tends' worth of quality") {
    Sim sim = disSim();
    const Entity pawn = disPawn(sim);
    sim.step();
    const DefId worms = disId(sim, "GutWorms");
    const DefId parasites = disId(sim, "MuscleParasites");
    CHECK(sim.health().foodRateFactor(pawn) == 1.0f);
    CHECK(sim.health().restFallFactor(pawn) == 1.0f);
    sim.health().contract(pawn, worms);
    sim.health().contract(pawn, parasites);
    CHECK(sim.health().foodRateFactor(pawn) == doctest::Approx(2.0f));
    CHECK(sim.health().restFallFactor(pawn) == doctest::Approx(2.0f));
    DisLog log;
    for (int i = 0; i < 3 * 86400; ++i) {
        log.step(sim);
    }
    CHECK(sim.health().diseaseSeverity(pawn, worms) == doctest::Approx(0.5f));  // no worse, no better
    CHECK_FALSE(disHealth(sim, pawn).dead);
    CHECK(disHealth(sim, pawn).capacities.size() > 0);
    CHECK(sim.health().capacityValue(pawn, "Moving") < 0.8f);   // -30%, and the ache
    CHECK(sim.health().capacityValue(pawn, "Moving") > 0.4f);
    // Tend 1.0, 1.0 (two days apart each time) and a third: gone with the third.
    for (int round = 0; round < 3; ++round) {
        CHECK(sim.health().needsTending(pawn));
        CHECK(sim.health().tendAll(pawn, 1.0f) == 2);
        CHECK_FALSE(sim.health().needsTending(pawn));
        if (round < 2) {
            CHECK(sim.health().sick(pawn));
            for (int i = 0; i < 2 * 86400 + 60; ++i) {
                log.step(sim);
            }
        }
    }
    CHECK_FALSE(sim.health().sick(pawn));
    CHECK(sim.health().foodRateFactor(pawn) == 1.0f);
    CHECK(sim.health().restFallFactor(pawn) == 1.0f);
    CHECK(sim.health().capacityValue(pawn, "Moving") == doctest::Approx(1.0f));
}

TEST_CASE("Diseases: gut worms make a colonist hungrier and muscle parasites tire it faster") {
    Sim sim = disSim();
    std::vector<Entity> pawns;
    for (int i = 0; i < 3; ++i) {
        const Entity pawn = sim.things().spawn("Human", CellPos{10 + 4 * i, 10, 0});
        sim.jobs().addWorker(pawn);
        sim.submit(SetWorkPriorityCommand{pawn, "Doctor", 0});
        for (int h = 0; h < 24; ++h) {
            sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Anything});
        }
        pawns.push_back(pawn);
    }
    sim.step();
    sim.health().contract(pawns[1], disId(sim, "GutWorms"));
    sim.health().contract(pawns[2], disId(sim, "MuscleParasites"));
    const auto needs = [&](size_t i) { return sim.things().registry().get<Needs>(pawns[i]); };
    const Needs before[3] = {needs(0), needs(1), needs(2)};
    sim.stepMany(900);
    const float food0 = before[0].food - needs(0).food;
    const float food1 = before[1].food - needs(1).food;
    CHECK(food0 > 0.0f);
    CHECK(food1 / food0 == doctest::Approx(2.0f).epsilon(0.05));
    const float rest0 = before[0].rest - needs(0).rest;
    const float rest2 = before[2].rest - needs(2).rest;
    CHECK(rest0 > 0.0f);
    CHECK(rest2 / rest0 == doctest::Approx(2.0f).epsilon(0.05));
}

TEST_CASE("Diseases: a sick colonist has the Sick thought, a poisoned meal gives food poisoning, and it passes by itself") {
    Sim sim = disSim();
    const Entity pawn = sim.things().spawn("Human", CellPos{10, 10, 0});
    sim.jobs().addWorker(pawn);
    sim.submit(SetWorkPriorityCommand{pawn, "Doctor", 0});
    sim.stepMany(5);
    const auto hasSick = [&] {
        const std::vector<ThoughtView> thoughts = sim.mood().thoughts(pawn, sim.time().seconds);
        return std::any_of(thoughts.begin(), thoughts.end(), [](const ThoughtView& t) { return t.def == "Sick"; });
    };
    CHECK_FALSE(hasSick());
    // Eating a poisoned meal: chance 35% each; repeat until it takes.
    const Entity bad = sim.things().spawn("MealSimple", CellPos{25, 25, 0});
    sim.things().registry().get<Spoilage>(bad).poisoned = true;
    int tries = 0;
    while (!sim.health().sick(pawn) && tries < 200) {
        sim.spoilage().eaten(pawn, bad);
        ++tries;
    }
    DIS_REQUIRE(sim.health().sick(pawn));
    const DefId poisoning = disId(sim, "FoodPoisoning");
    CHECK(sim.health().diseaseSeverity(pawn, poisoning) == doctest::Approx(1.0f));
    sim.step();
    CHECK(hasSick());
    CHECK_FALSE(sim.health().needsTending(pawn));   // nothing to tend
    DisLog log;
    for (int i = 0; i < 2 * 86400; ++i) {
        log.step(sim);
    }
    CHECK_FALSE(sim.health().sick(pawn));            // gone in a day and a quarter
    CHECK(log.count(EventType::PawnRecovered) == 1);
    CHECK_FALSE(disHealth(sim, pawn).dead);
}

TEST_CASE("Diseases: an outbreak picks victims among those who can catch it") {
    Sim sim = disSim();
    std::vector<Entity> pawns;
    for (int i = 0; i < 10; ++i) {
        pawns.push_back(disPawn(sim, CellPos{5 + 3 * i, 8, 0}));
    }
    sim.step();
    const DefId plague = disId(sim, "Plague");
    sim.health().setImmunity(pawns[0], plague, 1.0f);
    sim.health().contract(pawns[1], plague);
    sim.health().kill(pawns[2], DeathCause::BloodLoss);
    Rng rng(5);
    const std::vector<Entity> caught = sim.health().outbreak(plague, pawns, 4, rng);
    CHECK(caught.size() == 4);
    for (const Entity pawn : caught) {
        CHECK(pawn != pawns[0]);
        CHECK(pawn != pawns[1]);
        CHECK(pawn != pawns[2]);
        CHECK(sim.health().sick(pawn));
    }
    // Asking for more than there are takes everyone left (10 - the immune, the sick one and the dead one = 7, 4 already).
    Rng again(6);
    CHECK(sim.health().outbreak(plague, pawns, 20, again).size() == 3);
    Rng none(7);
    CHECK(sim.health().outbreak(plague, pawns, 20, none).empty());
    CHECK(sim.health().onsetMtbDays(disId(sim, "Flu")) == doctest::Approx(50.0f));
    CHECK(sim.health().onsetMtbDays(disId(sim, "Infection")) == 0.0f);
}

TEST_CASE("Diseases: a doctor tends a sick colonist, who rests in a bed until well") {
    Sim sim = disSim();
    sim.mentalBreaks().setNatural(false);  // it runs for days
    const Entity bed = sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
    sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 10);
    for (int i = 0; i < 12; ++i) {
        sim.things().spawn("MealSurvivalPack", CellPos{14 + i, 14, 0}, 10);   // a stack holds ten
    }
    DIS_REQUIRE(bed != kNoEntity);
    const Entity doctor = sim.things().spawn("Human", CellPos{10, 10, 0});
    sim.jobs().addWorker(doctor);
    const Entity patient = sim.things().spawn("Human", CellPos{30, 30, 0});
    sim.jobs().addWorker(patient);
    for (const Entity pawn : {doctor, patient}) {
        for (int h = 0; h < 24; ++h) {
            sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
        }
    }
    sim.submit(SetWorkPriorityCommand{patient, "Doctor", 0});
    sim.step();
    const DefId flu = disId(sim, "Flu");
    sim.health().contract(patient, flu);
    DisLog log;
    bool tended = false;
    bool rested = false;
    for (int i = 0; i < 12000 && !(tended && rested); ++i) {
        log.step(sim);
        tended = log.type(EventType::PawnTended) != nullptr;
        rested = rested || sim.things().registry().get<Worker>(patient).job.type == JobType::BedRest;
    }
    CHECK(tended);
    CHECK(rested);
    const Event* event = log.type(EventType::PawnTended);
    DIS_REQUIRE(event != nullptr);
    CHECK(event->thing == doctor);
    CHECK(event->target == patient);
    // The doctor tends again each time the tend runs out, and the patient comes through.
    for (int i = 0; i < 8 * 86400; ++i) {
        log.step(sim);
    }
    CHECK_FALSE(disHealth(sim, patient).dead);
    CHECK(log.count(EventType::PawnTended) >= 4);
    CHECK(log.count(EventType::PawnRecovered) == 1);
    CHECK(sim.health().immunity(patient, flu) == doctest::Approx(1.0f));
}

TEST_CASE("Diseases: sickness, immunity and an infected wound save and load, and the loaded game goes on the same") {
    Sim sim = disSim();
    const Entity sick = disPawn(sim, CellPos{10, 10, 0});
    const Entity doomed = disPawn(sim, CellPos{14, 10, 0});
    sim.step();
    sim.health().contract(sick, disId(sim, "Plague"), 0.3f);
    sim.health().contract(sick, disId(sim, "Infection"), -1.0f, disPart(sim, sick, "torso"));
    sim.health().setImmunity(sick, disId(sim, "Flu"), 0.6f);
    sim.health().tendAll(sick, 0.8f);
    sim.stepMany(3000);
    Health& h = disHealth(sim, doomed);
    Injury wound;
    wound.part = static_cast<uint16_t>(disPart(sim, doomed, "left leg"));
    wound.hediff = sim.defs().idOf("Hediffs", "Cut");
    wound.severity = 2.0f;
    wound.infectAt = sim.time().seconds + 5000;
    h.injuries.push_back(wound);
    h.dirty = true;
    sim.stepMany(10);
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, disDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    DIS_REQUIRE(loaded.error.empty());
    DIS_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(saveSim(copy) == bytes);
    const Health& a = disHealth(sim, sick);
    const Health& b = copy.things().registry().get<Health>(sick);
    DIS_REQUIRE(b.diseases.size() == a.diseases.size());
    DIS_REQUIRE(b.immunities.size() == a.immunities.size());
    for (size_t i = 0; i < a.diseases.size(); ++i) {
        CHECK(b.diseases[i].severity == a.diseases[i].severity);
        CHECK(b.diseases[i].tendedUntil == a.diseases[i].tendedUntil);
        CHECK(b.diseases[i].part == a.diseases[i].part);
    }
    CHECK(copy.health().immunity(sick, disId(copy, "Flu")) == doctest::Approx(0.6f));
    CHECK(copy.things().registry().get<Health>(doomed).injuries[0].infectAt == wound.infectAt);

    sim.stepMany(6000);   // the wound turns infected in here
    copy.stepMany(6000);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(sim.health().diseaseSeverity(doomed, disId(sim, "Infection")) > 0.0f);
    CHECK(copy.health().diseaseSeverity(doomed, disId(copy, "Infection")) == sim.health().diseaseSeverity(doomed, disId(sim, "Infection")));
    CHECK(saveSim(copy) == saveSim(sim));
}

TEST_CASE("Diseases: a recorded session with a disease replays to the same state") {
    SimSettings settings;
    settings.seed = 9;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, disDefs());
    std::vector<Entity> colonists;
    for (const auto [pawn, colonist] : recorded.things().registry().view<const Colonist>().each()) {
        (void)colonist;
        colonists.push_back(pawn);
    }
    std::sort(colonists.begin(), colonists.end());
    DIS_REQUIRE(colonists.size() >= 2);
    recorded.stepMany(5);
    recorded.submit(ContractDiseaseCommand{colonists[0], "Flu", -1.0f, -1});
    recorded.submit(DamagePawnCommand{colonists[1], "Cut", 8.0f, -1});
    recorded.stepMany(4000);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("ContractDisease") != std::string::npos);
    Replay parsed;
    std::string error;
    DIS_REQUIRE(readReplay(text, parsed, error));
    CHECK(error.empty());
    Sim again(parsed.settings, disDefs());
    playReplay(parsed, again, replay.endStep);
    CHECK(again.health().diseaseSeverity(colonists[0], disId(again, "Flu")) > 0.0f);
    const ReplayCheck check = checkReplay(parsed, again);
    CHECK(check.matched());
    CHECK(again.stateHash() == recorded.stateHash());
}

TEST_CASE("Diseases: an illness's cap on a capacity comes after every offset, as in RimWorld") {
    // An extreme infection caps consciousness at 0.1 and food poisoning takes 0.1 off it. Capped stage by stage, the
    // poisoning pushed consciousness to 0 and the pawn died of it (year run, seed 1); the cap comes last, so it stays at 0.1.
    Sim sim = disSim();
    const Entity pawn = disPawn(sim);
    sim.step();
    DIS_REQUIRE(sim.health().contract(pawn, disId(sim, "Infection"), 0.9f, disPart(sim, pawn, "left arm")));
    DIS_REQUIRE(sim.health().contract(pawn, disId(sim, "FoodPoisoning")));
    sim.step();
    const HealthReport report = disReport(sim, pawn);
    CHECK_FALSE(report.dead);
    float consciousness = -1.0f;
    for (const auto& line : report.capacities) {
        consciousness = line.name == "Consciousness" ? line.value : consciousness;
    }
    CHECK(consciousness == doctest::Approx(0.1f));
}
