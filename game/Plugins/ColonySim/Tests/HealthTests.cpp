#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define HEALTH_REQUIRE(...)    \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// These tests run on the game's real data (the Human body, capacities, injuries): they check the
// numbers in docs/design/health.md, so they are meant to change when those numbers do.
std::shared_ptr<const DefDatabase> healthTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> healthTestSim(int pawns = 0, int items = 0) {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.pawns = pawns;
    settings.items = items;
    return std::make_unique<Sim>(settings, healthTestDefs());
}

Entity healthTestPawn(Sim& sim, int x = 10, int y = 10) { return sim.things().spawn("Human", CellPos{x, y, 0}); }

int healthTestPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels =
        sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i] == label) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Damage straight into the health system (as combat will), by damage type name.
bool healthTestHit(Sim& sim, Entity pawn, const char* type, float amount, const std::string& partLabel = "") {
    Rng rng(99);
    const int part = partLabel.empty() ? -1 : healthTestPart(sim, pawn, partLabel);
    return sim.health().applyDamage(pawn, sim.health().damageTypeId(type), amount, part, rng);
}

float healthTestCapacity(const Sim& sim, Entity pawn, const char* name) {
    HealthReport report;
    sim.healthReport(pawn, report);
    for (const HealthReport::CapacityLine& c : report.capacities) {
        if (c.name == name) {
            return c.value;
        }
    }
    return -1.0f;
}

HealthReport healthTestReport(const Sim& sim, Entity pawn) {
    HealthReport report;
    sim.healthReport(pawn, report);
    return report;
}

// Steps and collects the health events of every step.
struct HealthTestEvents {
    std::vector<EventType> types;
    std::string diedOf;
    void collect(const Sim& sim) {
        for (const Event& e : sim.events()) {
            if (e.type == EventType::PawnDied) {
                diedOf = e.message;
            }
            if (e.type >= EventType::PawnInjured && e.type <= EventType::PawnDied) {
                types.push_back(e.type);
            }
        }
    }
    bool has(EventType type) const { return std::find(types.begin(), types.end(), type) != types.end(); }
    long index(EventType type) const {
        return static_cast<long>(std::find(types.begin(), types.end(), type) - types.begin());
    }
};

void healthTestRun(Sim& sim, int64_t steps, HealthTestEvents* events = nullptr) {
    for (int64_t i = 0; i < steps; ++i) {
        sim.step();
        if (events != nullptr) {
            events->collect(sim);
        }
    }
}

}  // namespace

TEST_CASE("Health: a pawn with a body starts whole, and a thing without one has no health") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    HEALTH_REQUIRE(pawn != kNoEntity);
    CHECK(sim->things().registry().all_of<Health>(pawn));
    CHECK(sim->health().tracks(pawn));
    CHECK(sim->health().partCount(sim->things().registry().get<Thing>(pawn).def) == 64);
    sim->step();

    const HealthReport report = healthTestReport(*sim, pawn);
    CHECK_FALSE(report.dead);
    CHECK_FALSE(report.downed);
    CHECK(report.pain == 0.0f);
    CHECK(report.bleedPerDay == 0.0f);
    CHECK(report.injuries.empty());
    CHECK(report.capacities.size() == 11);
    for (const HealthReport::CapacityLine& c : report.capacities) {
        CHECK_MESSAGE(c.value == doctest::Approx(1.0f).epsilon(0.001), c.name);
    }

    const Entity steel = sim->things().spawn("Steel", CellPos{12, 12, 0});
    CHECK_FALSE(sim->things().registry().all_of<Health>(steel));
    CHECK_FALSE(sim->health().tracks(steel));
    CHECK_FALSE(healthTestHit(*sim, steel, "Bullet", 5.0f));
}

TEST_CASE("Health: the protagonist has twice the hit points on every part") {
    auto sim = healthTestSim();
    const Entity colonist = healthTestPawn(*sim);
    const Entity hero = healthTestPawn(*sim, 12, 10);
    sim->things().registry().get<Health>(hero).protagonist = true;
    const int torso = healthTestPart(*sim, colonist, "torso");
    CHECK(torso == 0);
    CHECK(sim->health().partMaxHitPoints(colonist, torso) == 40.0f);
    CHECK(sim->health().partMaxHitPoints(hero, torso) == 80.0f);
    CHECK(sim->health().partMaxHitPoints(hero, healthTestPart(*sim, hero, "left hand")) == 40.0f);

    // 50 damage destroys a colonist's torso, which kills; the protagonist takes it as a wound.
    CHECK(healthTestHit(*sim, colonist, "Cut", 50.0f, "torso"));
    CHECK(healthTestHit(*sim, hero, "Cut", 50.0f, "torso"));
    CHECK(healthTestReport(*sim, colonist).dead);
    const HealthReport heroReport = healthTestReport(*sim, hero);
    CHECK_FALSE(heroReport.dead);
    HEALTH_REQUIRE(heroReport.injuries.size() >= 1);
    CHECK(heroReport.injuries[0].severity == 50.0f);
}

TEST_CASE("Health: an injury hurts, bleeds, and is reported") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    CHECK(healthTestHit(*sim, pawn, "Cut", 6.0f, "left arm"));
    const HealthReport report = healthTestReport(*sim, pawn);
    HEALTH_REQUIRE(report.injuries.size() == 1);
    CHECK(report.injuries[0].part == "left arm");
    CHECK(report.injuries[0].injury == "cut");
    CHECK(report.injuries[0].severity == 6.0f);
    CHECK_FALSE(report.injuries[0].tended);
    CHECK(report.pain == doctest::Approx(6.0f * 0.0125f));
    CHECK(report.bleedPerDay == doctest::Approx(6.0f * 0.06f));
    CHECK_FALSE(report.downed);

    bool injured = false;
    for (const Event& e : sim->events()) {
        if (e.type == EventType::PawnInjured) {
            injured = true;
            CHECK(e.thing == pawn);
            CHECK(e.part == healthTestPart(*sim, pawn, "left arm"));
            CHECK(e.amount == 6.0f);
            CHECK(sim->defs().get(e.def).defName() == "Cut");
        }
    }
    CHECK(injured);
}

TEST_CASE("Health: bleeding raises blood loss until the wound is tended, then it recovers") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    healthTestHit(*sim, pawn, "Cut", 6.0f, "left arm");  // bleeds 0.36 a day
    healthTestRun(*sim, 3600);
    const float afterHour = sim->things().registry().get<Health>(pawn).bloodLoss;
    CHECK(afterHour == doctest::Approx(0.36f / 24.0f).epsilon(0.01));

    CHECK(sim->health().tendAll(pawn, 1.0f) == 1);
    CHECK(healthTestReport(*sim, pawn).bleedPerDay == 0.0f);
    CHECK(healthTestReport(*sim, pawn).injuries[0].tended);
    healthTestRun(*sim, 3600);
    const float afterTending = sim->things().registry().get<Health>(pawn).bloodLoss;
    CHECK(afterTending == doctest::Approx(afterHour - 0.333f / 24.0f).epsilon(0.01));
    CHECK(sim->health().tendAll(pawn, 1.0f) == 0);  // nothing left to tend
}

TEST_CASE("Health: losing an arm halves manipulation, a leg halves moving, both legs down the pawn") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    HealthTestEvents events;

    CHECK(healthTestHit(*sim, pawn, "Cut", 40.0f, "left arm"));
    events.collect(*sim);  // hits report their events at once, and the next step clears them
    CHECK(events.has(EventType::PawnPartLost));
    CHECK_FALSE(events.has(EventType::PawnInjured));
    HealthReport report = healthTestReport(*sim, pawn);
    HEALTH_REQUIRE(report.missingParts.size() == 1);
    CHECK(report.missingParts[0] == "left arm");
    // A lost part bleeds like 2 x its hit points lost: 30 x 2 x 0.12, and hurts until tended.
    CHECK(report.bleedPerDay == doctest::Approx(30.0f * 2.0f * 0.12f));
    CHECK(report.pain == doctest::Approx(30.0f * 0.0125f));
    CHECK(sim->health().tendAll(pawn, 1.0f) == 1);
    CHECK(healthTestReport(*sim, pawn).pain == 0.0f);
    CHECK(healthTestCapacity(*sim, pawn, "Manipulation") == doctest::Approx(0.5f));
    CHECK(healthTestCapacity(*sim, pawn, "Moving") == doctest::Approx(1.0f).epsilon(0.001));

    CHECK(healthTestHit(*sim, pawn, "Cut", 40.0f, "left leg"));
    sim->health().tendAll(pawn, 1.0f);
    healthTestRun(*sim, 1, &events);
    CHECK(healthTestCapacity(*sim, pawn, "Moving") == doctest::Approx(0.5f));
    CHECK(sim->things().registry().get<Health>(pawn).movingFactor == doctest::Approx(0.5f));
    CHECK_FALSE(healthTestReport(*sim, pawn).downed);

    CHECK(healthTestHit(*sim, pawn, "Cut", 40.0f, "right leg"));
    events.collect(*sim);
    sim->health().tendAll(pawn, 1.0f);
    healthTestRun(*sim, 1, &events);
    CHECK(healthTestCapacity(*sim, pawn, "Moving") == doctest::Approx(0.0f));
    CHECK(healthTestReport(*sim, pawn).downed);
    CHECK_FALSE(healthTestReport(*sim, pawn).dead);
    CHECK(events.has(EventType::PawnDowned));
    CHECK(sim->things().registry().get<Health>(pawn).movingFactor == 0.0f);
    // The one who lost a leg and an arm and was tended is still alive a day on.
    healthTestRun(*sim, 86400);
    CHECK_FALSE(healthTestReport(*sim, pawn).dead);
}

TEST_CASE("Health: toes and fingers count for a part of a limb") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    // All five toes of the left foot go (each is 8 hit points): that leg works at 60%, so moving
    // is the mean of 60% and 100%.
    for (const char* toe : {"left little toe", "left fourth toe", "left middle toe", "left second toe", "left big toe"}) {
        CHECK(healthTestHit(*sim, pawn, "Cut", 20.0f, toe));
    }
    sim->health().tendAll(pawn, 1.0f);
    sim->step();
    CHECK(healthTestCapacity(*sim, pawn, "Moving") == doctest::Approx(0.8f).epsilon(0.001));
}

TEST_CASE("Health: destroying a vital part kills; the dead can't be hurt or tended") {
    for (const char* part : {"heart", "brain", "head", "neck", "torso", "liver"}) {
        CAPTURE(part);
        auto sim = healthTestSim();
        const Entity pawn = healthTestPawn(*sim);
        HealthTestEvents events;
        CHECK(healthTestHit(*sim, pawn, "Cut", 60.0f, part));
        events.collect(*sim);
        healthTestRun(*sim, 1, &events);
        const HealthReport report = healthTestReport(*sim, pawn);
        CHECK(report.dead);
        CHECK(report.cause == DeathCause::VitalPartLost);
        CHECK(events.has(EventType::PawnDied));
        CHECK(events.diedOf == "a vital part destroyed");
        CHECK_FALSE(healthTestHit(*sim, pawn, "Bullet", 5.0f));
        CHECK(sim->health().tendAll(pawn, 1.0f) == 0);
    }
}

TEST_CASE("Health: a lung or a kidney is not vital, losing both lungs is") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    CHECK(healthTestHit(*sim, pawn, "Cut", 40.0f, "left lung"));
    CHECK(healthTestHit(*sim, pawn, "Cut", 40.0f, "left kidney"));
    sim->health().tendAll(pawn, 1.0f);
    sim->step();
    CHECK(healthTestCapacity(*sim, pawn, "Breathing") == doctest::Approx(0.5f));
    CHECK(healthTestCapacity(*sim, pawn, "BloodFiltration") == doctest::Approx(0.5f));
    CHECK_FALSE(healthTestReport(*sim, pawn).dead);

    CHECK(healthTestHit(*sim, pawn, "Cut", 40.0f, "right lung"));
    sim->step();
    CHECK(healthTestReport(*sim, pawn).dead);
    CHECK(healthTestReport(*sim, pawn).cause == DeathCause::VitalCapacity);
}

TEST_CASE("Health: a bad head wound knocks a pawn out, and it gets up as the wound heals") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    HealthTestEvents events;
    // A bruise of 7.2 on the brain (10 hit points) leaves it at 28%: consciousness under 30%.
    CHECK(healthTestHit(*sim, pawn, "Blunt", 7.2f, "brain"));
    events.collect(*sim);
    healthTestRun(*sim, 1, &events);
    CHECK(events.has(EventType::PawnDowned));
    HealthReport report = healthTestReport(*sim, pawn);
    CHECK(report.downed);
    CHECK_FALSE(report.dead);
    CHECK(healthTestCapacity(*sim, pawn, "Consciousness") < 0.3f);
    CHECK(report.injuries.size() == 1);

    for (int day = 0; day < 8 && !events.has(EventType::PawnGotUp); ++day) {
        healthTestRun(*sim, 86400, &events);
    }
    CHECK(events.has(EventType::PawnGotUp));
    CHECK(events.index(EventType::PawnDowned) < events.index(EventType::PawnGotUp));
    CHECK_FALSE(healthTestReport(*sim, pawn).downed);
    CHECK_FALSE(healthTestReport(*sim, pawn).dead);
}

TEST_CASE("Health: pain alone can put a pawn down") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    // Bruises on the torso, legs and an arm: no part works much worse, but the pain adds up to 1.0.
    for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
        CHECK(healthTestHit(*sim, pawn, "Blunt", 20.0f, part));
    }
    sim->step();
    const HealthReport report = healthTestReport(*sim, pawn);
    CHECK(report.pain > 0.8f);
    CHECK(report.downed);
    CHECK_FALSE(report.dead);
    CHECK(healthTestCapacity(*sim, pawn, "Consciousness") > 0.3f);
}

TEST_CASE("Health: an untended wound bleeds a pawn out: down, then dead of blood loss") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    HealthTestEvents events;
    CHECK(healthTestHit(*sim, pawn, "Bullet", 30.0f, "torso"));  // bleeds 1.8 a day
    for (int hour = 0; hour < 24 && !events.has(EventType::PawnDied); ++hour) {
        healthTestRun(*sim, 3600, &events);
    }
    CHECK(events.has(EventType::PawnDowned));
    CHECK(events.has(EventType::PawnDied));
    CHECK(events.index(EventType::PawnDowned) < events.index(EventType::PawnDied));
    CHECK(events.diedOf == "blood loss");
    const HealthReport report = healthTestReport(*sim, pawn);
    CHECK(report.dead);
    CHECK(report.cause == DeathCause::BloodLoss);
    CHECK(report.bloodLoss >= 1.0f);
}

TEST_CASE("Health: tending stops the bleeding and the wound heals in about a day") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    CHECK(healthTestHit(*sim, pawn, "Bullet", 30.0f, "torso"));
    healthTestRun(*sim, 1000);
    CHECK(sim->health().tendAll(pawn, 1.0f) == 1);
    healthTestRun(*sim, 3 * 86400);
    const HealthReport report = healthTestReport(*sim, pawn);
    CHECK_FALSE(report.dead);
    CHECK_FALSE(report.downed);
    CHECK(report.injuries.empty());
    CHECK(report.bloodLoss == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(healthTestCapacity(*sim, pawn, "Moving") == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("Health: wounds heal slowly by themselves, faster in a bed") {
    auto ground = healthTestSim();
    auto bed = healthTestSim();
    const Entity a = healthTestPawn(*ground);
    const Entity b = healthTestPawn(*bed);
    bed->things().registry().get<Health>(b).rest = RestKind::Bed;
    healthTestHit(*ground, a, "Blunt", 20.0f, "left leg");
    healthTestHit(*bed, b, "Blunt", 20.0f, "left leg");
    healthTestRun(*ground, 86400);
    healthTestRun(*bed, 86400);
    // One bruise (it doesn't bleed) heals 100 times a day, each time by the day's rate / 100:
    // 8 + 4 = 12 a day on the ground, 8 + 8 = 16 in a bed.
    const HealthReport onGround = healthTestReport(*ground, a);
    const HealthReport inBed = healthTestReport(*bed, b);
    HEALTH_REQUIRE(onGround.injuries.size() == 1);
    HEALTH_REQUIRE(inBed.injuries.size() == 1);
    CHECK(onGround.injuries[0].severity == doctest::Approx(8.0f).epsilon(0.001));
    CHECK(inBed.injuries[0].severity == doctest::Approx(4.0f).epsilon(0.001));
}

TEST_CASE("Health: hits land by how much of the body each part covers, and never on lost parts") {
    auto sim = healthTestSim();
    const Entity probe = healthTestPawn(*sim);
    const std::vector<std::string> labels = sim->health().partLabels(sim->things().registry().get<Thing>(probe).def);
    const std::vector<std::string> inside = {"skull", "brain", "tongue", "spine", "ribcage", "sternum", "stomach", "heart",
                                             "left lung", "right lung", "left kidney", "right kidney", "liver",
                                             "left clavicle", "right clavicle", "left humerus", "right humerus",
                                             "left radius", "right radius", "pelvis", "left femur", "right femur",
                                             "left tibia", "right tibia"};
    const auto isInside = [&](const std::string& label) { return std::find(inside.begin(), inside.end(), label) != inside.end(); };

    for (const char* type : {"Blunt", "Bullet"}) {
        CAPTURE(type);
        std::map<std::string, int> hits;
        int insideHits = 0;
        for (int i = 0; i < 3000; ++i) {
            const Entity pawn = healthTestPawn(*sim, 20, 20);
            sim->step();  // clears the events
            Rng rng(1000 + static_cast<uint64_t>(i));
            CHECK(sim->health().applyDamage(pawn, sim->health().damageTypeId(type), 0.5f, -1, rng));
            for (const Event& e : sim->events()) {
                if (e.type == EventType::PawnInjured) {
                    ++hits[labels[static_cast<size_t>(e.part)]];
                    insideHits += isInside(labels[static_cast<size_t>(e.part)]) ? 1 : 0;
                }
            }
            sim->things().destroy(pawn);
        }
        CHECK(hits["torso"] > hits["head"]);
        CHECK(hits["left leg"] > hits["left hand"]);
        CHECK(hits["left leg"] > 100);
        CHECK(hits["head"] > 20);
        if (std::string(type) == "Blunt") {
            CHECK(insideHits == 0);  // blunt damage stays on the outside
        } else {
            CHECK(insideHits > 100);  // bullets get in to organs and bones
            CHECK(hits["heart"] > 0);
        }
    }

    // A pawn without a left arm is never hit there.
    const Entity pawn = healthTestPawn(*sim, 22, 20);
    healthTestHit(*sim, pawn, "Cut", 60.0f, "left arm");
    sim->health().tendAll(pawn, 1.0f);
    sim->step();  // clears the events of the loops above
    const std::vector<std::string> armParts = {"left arm", "left humerus", "left radius", "left hand", "left pinky", "left thumb"};
    for (int i = 0; i < 1500; ++i) {
        Rng rng(5000 + static_cast<uint64_t>(i));
        sim->health().applyDamage(pawn, sim->health().damageTypeId("Bullet"), 0.01f, -1, rng);
    }
    for (const Event& e : sim->events()) {
        if (e.type == EventType::PawnInjured) {
            const std::string& label = labels[static_cast<size_t>(e.part)];
            CHECK(std::find(armParts.begin(), armParts.end(), label) == armParts.end());
        }
    }
    CHECK_FALSE(healthTestHit(*sim, pawn, "Cut", 1.0f, "left thumb"));
}

TEST_CASE("Health: a part that can't be destroyed is left with one hit point") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    CHECK(healthTestHit(*sim, pawn, "Bullet", 100.0f, "ribcage"));
    CHECK(sim->health().partHitPoints(pawn, healthTestPart(*sim, pawn, "ribcage")) == doctest::Approx(1.0f));
    CHECK_FALSE(healthTestReport(*sim, pawn).dead);
    CHECK(healthTestReport(*sim, pawn).missingParts.empty());
}

TEST_CASE("Health: a downed or dead hauler drops its job, and the others carry on") {
    auto sim = healthTestSim(3, 30);
    HEALTH_REQUIRE(sim->haulers().size() == 3);
    healthTestRun(*sim, 400);
    const Entity victim = sim->haulers()[0];
    const uint64_t haulsBefore = sim->stats().hauls;
    CHECK(healthTestHit(*sim, victim, "Blunt", 9.0f, "brain"));  // consciousness 10%
    healthTestRun(*sim, 1);
    CHECK(sim->health().incapacitated(victim));
    const Position where = sim->things().registry().get<Position>(victim);
    healthTestRun(*sim, 600);
    const Position later = sim->things().registry().get<Position>(victim);
    CHECK(later.x == where.x);
    CHECK(later.y == where.y);
    // Nothing it was going for stays reserved: the others finish all the items.
    healthTestRun(*sim, 30000);
    CHECK(sim->stats().hauls > haulsBefore);
    CHECK(sim->itemsLeft() == 0);
}

TEST_CASE("Health: injuries slow a pawn down") {
    auto sim = healthTestSim();
    const Entity whole = healthTestPawn(*sim, 5, 10);
    const Entity hurt = healthTestPawn(*sim, 5, 20);
    healthTestHit(*sim, hurt, "Cut", 40.0f, "left leg");
    sim->health().tendAll(hurt, 1.0f);
    for (const Entity pawn : {whole, hurt}) {
        const Position pos = sim->things().registry().get<Position>(pawn);
        sim->things().registry().get<Mover>(pawn).walkTo(pos.x, pos.y, {CellPos{50, static_cast<int>(pos.y), 0}});
    }
    healthTestRun(*sim, 120);
    const float wholeWalked = sim->things().registry().get<Position>(whole).x - 5.5f;
    const float hurtWalked = sim->things().registry().get<Position>(hurt).x - 5.5f;
    CHECK(wholeWalked > 10.0f);
    CHECK(hurtWalked / wholeWalked == doctest::Approx(0.5f).epsilon(0.1));
}

TEST_CASE("Health: the snapshot says who is downed or dead") {
    auto sim = healthTestSim();
    const Entity down = healthTestPawn(*sim, 10, 10);
    const Entity dead = healthTestPawn(*sim, 12, 10);
    const Entity well = healthTestPawn(*sim, 14, 10);
    healthTestHit(*sim, down, "Blunt", 9.0f, "brain");
    healthTestHit(*sim, dead, "Cut", 60.0f, "heart");
    sim->step();
    Snapshot snap;
    sim->writeSnapshot(snap);
    const ThingView* a = snap.find(down);
    const ThingView* b = snap.find(dead);
    const ThingView* c = snap.find(well);
    const bool found = a != nullptr && b != nullptr && c != nullptr;
    HEALTH_REQUIRE(found);
    CHECK(a->downed);
    CHECK_FALSE(a->dead);
    CHECK_FALSE(b->downed);
    CHECK(b->dead);
    CHECK_FALSE(c->downed);
    CHECK_FALSE(c->dead);
}

TEST_CASE("Health: commands hurt and tend pawns, and say why when they can't") {
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    const Entity wall = sim->things().spawn("Wall", CellPos{30, 30, 0});
    const int armPart = healthTestPart(*sim, pawn, "right arm");

    sim->submit(DamagePawnCommand{pawn, "Cut", 5.0f, armPart});    // 1: fine
    sim->submit(DamagePawnCommand{wall, "Cut", 5.0f, -1});          // 2: no body
    sim->submit(DamagePawnCommand{pawn, "Nonsense", 5.0f, -1});     // 3: unknown type
    sim->submit(DamagePawnCommand{pawn, "Cut", 0.0f, -1});          // 4: no damage
    sim->submit(DamagePawnCommand{pawn, "Cut", 5.0f, 9999});        // 5: no such part
    sim->submit(TendPawnCommand{pawn, 1.0f});                       // 6: fine
    sim->submit(TendPawnCommand{pawn, 1.0f});                       // 7: nothing to tend
    sim->submit(TendPawnCommand{wall, 1.0f});                       // 8: no body
    sim->step();
    std::vector<uint64_t> failed;
    for (const Event& e : sim->events()) {
        if (e.type == EventType::CommandFailed) {
            failed.push_back(e.command);
            CHECK_FALSE(e.message.empty());
        }
    }
    CHECK(failed == std::vector<uint64_t>{2, 3, 4, 5, 7, 8});
    const HealthReport report = healthTestReport(*sim, pawn);
    HEALTH_REQUIRE(report.injuries.size() == 1);
    CHECK(report.injuries[0].tended);
    CHECK(std::string(commandName(sim->commandLog()[0].command)) == "DamagePawn");
    CHECK(std::string(commandName(sim->commandLog()[5].command)) == "TendPawn");
}

namespace {

// A session of wounds and tending, as the dev commands would make it.
void healthTestSession(Sim& sim, const std::vector<Entity>& pawns, int64_t from, int64_t to) {
    for (int64_t i = from; i < to; ++i) {
        if (i % 700 == 0) {
            sim.submit(DamagePawnCommand{pawns[static_cast<size_t>(i / 700) % pawns.size()], "Bullet", 4.0f, -1});
        }
        if (i % 1900 == 0) {
            sim.submit(DamagePawnCommand{pawns[0], "Cut", 3.0f, -1});
        }
        if (i % 3100 == 1500) {
            sim.submit(TendPawnCommand{pawns[1], 0.8f});
        }
        if (i == 5000) {
            sim.submit(DamagePawnCommand{pawns[2], "Cut", 45.0f, 29});  // a limb or an organ
        }
        sim.step();
    }
}

}  // namespace

TEST_CASE("Health: save and load, replay, and straight runs all give the same game") {
    const auto defs = healthTestDefs();
    const auto make = [&] {
        auto sim = healthTestSim();
        std::vector<Entity> pawns;
        for (int i = 0; i < 3; ++i) {
            pawns.push_back(healthTestPawn(*sim, 10 + 3 * i, 10));
        }
        sim->things().registry().get<Health>(pawns[0]).protagonist = true;
        sim->things().registry().get<Health>(pawns[1]).rest = RestKind::Bed;
        return std::make_pair(std::move(sim), pawns);
    };

    auto [straight, pawns] = make();
    healthTestSession(*straight, pawns, 0, 12000);

    auto [saved, pawns2] = make();
    healthTestSession(*saved, pawns2, 0, 6000);
    const std::vector<uint8_t> bytes = saveSim(*saved);
    SaveLoadResult loaded = loadSim(bytes, defs);
    HEALTH_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->stateHash() == saved->stateHash());
    healthTestSession(*loaded.sim, pawns2, 6000, 12000);
    healthTestSession(*saved, pawns2, 6000, 12000);

    CHECK(loaded.sim->stateHash() == straight->stateHash());
    CHECK(saved->stateHash() == straight->stateHash());
    CHECK(saveSim(*loaded.sim) == saveSim(*straight));

    // Something happened: wounds, and the save shows them by name.
    const std::string json = saveToJson(*loaded.sim);
    CHECK(json.find("\"health\"") != std::string::npos);
    CHECK(json.find("Gunshot") != std::string::npos);
    CHECK(healthTestReport(*loaded.sim, pawns2[0]).protagonist);

    // The derived values are worked out again on loading, so they match.
    for (const Entity pawn : pawns2) {
        const HealthReport a = healthTestReport(*loaded.sim, pawn);
        const HealthReport b = healthTestReport(*straight, pawn);
        CHECK(a.downed == b.downed);
        CHECK(a.pain == b.pain);
        CHECK(a.bleedPerDay == b.bleedPerDay);
        CHECK(a.injuries.size() == b.injuries.size());
    }

    // The replay of the log, written to text and read again, plays the same game.
    const Replay recorded = recordReplay(*straight);
    Replay reread;
    std::string error;
    HEALTH_REQUIRE(readReplay(writeReplay(recorded), reread, error));
    auto replayed = healthTestSim();
    for (int i = 0; i < 3; ++i) {
        healthTestPawn(*replayed, 10 + 3 * i, 10);
    }
    replayed->things().registry().get<Health>(pawns[0]).protagonist = true;
    replayed->things().registry().get<Health>(pawns[1]).rest = RestKind::Bed;
    playReplay(reread, *replayed, reread.endStep);
    CHECK(replayed->stateHash() == straight->stateHash());
}

TEST_CASE("Health: a save with an injury on a part the body doesn't have is refused") {
    const auto defs = healthTestDefs();
    auto sim = healthTestSim();
    const Entity pawn = healthTestPawn(*sim);
    healthTestHit(*sim, pawn, "Cut", 3.0f, "torso");
    SaveLoadResult ok = loadSim(saveSim(*sim), defs);
    CHECK(ok.sim != nullptr);
    sim->things().registry().get<Health>(pawn).injuries[0].part = 200;
    SaveLoadResult bad = loadSim(saveSim(*sim), defs);
    CHECK(bad.sim == nullptr);
    CHECK_FALSE(bad.error.empty());
}
