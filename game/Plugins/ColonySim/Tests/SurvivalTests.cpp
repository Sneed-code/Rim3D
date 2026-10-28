// Tests for the year run's survival fixes on main: the expectations mood (docs/design/mood.md "Expectations") and doctors
// feeding downed colonists (docs/design/medicine.md "Feeding patients").
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define SURVIVAL_REQUIRE(...)  \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> survivalDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim survivalSim() { return Sim(SimSettings{11, 64, 0, 0, true}, survivalDefs()); }

Entity survivalColonist(Sim& sim, CellPos cell, bool doctor) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    sim.things().registry().emplace<Colonist>(pawn, Colonist{});
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    if (!doctor) {
        sim.submit(SetWorkPriorityCommand{pawn, "Doctor", 0});
    }
    return pawn;
}

const ThoughtView* survivalThought(const std::vector<ThoughtView>& thoughts, const std::string& def) {
    const auto it = std::find_if(thoughts.begin(), thoughts.end(), [&](const ThoughtView& t) { return t.def == def; });
    return it == thoughts.end() ? nullptr : &*it;
}

int survivalPart(const Sim& sim, Entity pawn, const std::string& label) {
    const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(pawn).def);
    const auto it = std::find(labels.begin(), labels.end(), label);
    return it == labels.end() ? -1 : static_cast<int>(it - labels.begin());
}

}  // namespace

TEST_CASE("Expectations: a poor new colony is content with little, and a richer one expects more an hour later") {
    Sim sim = survivalSim();
    const Entity pawn = survivalColonist(sim, CellPos{10, 10, 0}, true);
    sim.step();
    const std::vector<ThoughtView> poor = sim.mood().thoughts(pawn, sim.time().seconds);
    const ThoughtView* low = survivalThought(poor, "Expectations");
    SURVIVAL_REQUIRE(low != nullptr);
    CHECK(low->offset == doctest::Approx(30.0f));
    CHECK(sim.mood().wealth() < 15000.0f);

    // 20,000 silver (market value 1 each, RimWorld's) lifts the wealth over 15,000: very low expectations, +24, once measured.
    for (int i = 0; i < 40; ++i) {
        sim.things().spawn("Silver", CellPos{20 + i % 10, 20 + i / 10, 0}, 500);
    }
    sim.step();
    CHECK(survivalThought(sim.mood().thoughts(pawn, sim.time().seconds), "Expectations")->offset == doctest::Approx(30.0f));
    sim.stepMany(3600);
    CHECK(sim.mood().wealth() > 15000.0f);
    CHECK(survivalThought(sim.mood().thoughts(pawn, sim.time().seconds), "Expectations")->offset == doctest::Approx(24.0f));

    // The measured wealth is saved: a loaded game goes on with the same number and the same hash.
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, survivalDefs());
    SURVIVAL_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->mood().wealth() == sim.mood().wealth());
    CHECK(loaded.sim->mood().wealthAt() == sim.mood().wealthAt());
    sim.stepMany(600);
    loaded.sim->stepMany(600);
    CHECK(loaded.sim->stateHash() == sim.stateHash());
}

TEST_CASE("Expectations: prisoners and pawns that are not colonists have none") {
    Sim sim = survivalSim();
    const Entity animal = sim.things().spawn("Human", CellPos{10, 10, 0});
    sim.jobs().addWorker(animal);  // a pawn with needs but no Colonist part
    sim.step();
    CHECK(survivalThought(sim.mood().thoughts(animal, sim.time().seconds), "Expectations") == nullptr);
}

TEST_CASE("Feeding patients: a doctor brings food to a hungry colonist who is down, and not to one who can walk") {
    Sim sim = survivalSim();
    sim.things().spawn("MealSimple", CellPos{14, 14, 0}, 10);
    const Entity doctor = survivalColonist(sim, CellPos{10, 10, 0}, true);
    const Entity patient = survivalColonist(sim, CellPos{30, 30, 0}, false);
    const Entity walker = survivalColonist(sim, CellPos{40, 40, 0}, false);
    sim.submit(SetWorkPriorityCommand{walker, "Hauling", 0});
    sim.step();
    // Bruises on the legs and body: down with pain and broken legs, for days.
    Rng rng(99);
    for (const char* part : {"torso", "left leg", "right leg", "left arm"}) {
        sim.health().applyDamage(patient, sim.health().damageTypeId("Blunt"), 20.0f, survivalPart(sim, patient, part), rng);
    }
    sim.step();
    SURVIVAL_REQUIRE(sim.things().registry().get<Health>(patient).downed);
    sim.things().registry().get<Needs>(patient).food = 0.2f;
    CHECK(sim.jobs().feedablePatient(patient));
    CHECK_FALSE(sim.jobs().feedablePatient(walker));
    CHECK_FALSE(sim.jobs().feedablePatient(doctor));

    bool fed = false;
    for (int i = 0; i < 4 * 3600 && !fed; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            fed = fed || (e.type == EventType::JobEnded && e.job == JobType::FeedPatient && e.thing == doctor && e.message == "done");
        }
    }
    CHECK(fed);
    CHECK(sim.things().registry().get<Needs>(patient).food > 0.5f);
}

TEST_CASE("Jobs: a dead colonist butchered by another colonist's job in the same step is skipped, not touched") {
    SimSettings settings;
    settings.seed = 5;
    settings.mapSize = 64;
    settings.wildAnimals = 0;
    settings.flatMap = true;
    settings.checkpointEvery = 0;
    Sim sim(settings, survivalDefs());
    entt::registry& reg = sim.things().registry();
    for (int y = 0; y < sim.map().height(); ++y) {
        for (int x = 0; x < sim.map().width(); ++x) {
            sim.plants().remove(CellPos{x, y, 0});
        }
    }
    sim.stepMany(9 * 3600);  // daytime
    // The butcher comes first in the job system's order, the dead colonist after it (it still has its Worker).
    const Entity butcher = survivalColonist(sim, CellPos{10, 10, 0}, true);
    sim.submit(SpawnThingCommand{"ButcherTable", CellPos{12, 10, 0}, 1});
    sim.step();
    Entity table = kNoEntity;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && sim.defs().get(e.def).defName() == "ButcherTable") {
            table = e.thing;
        }
    }
    SURVIVAL_REQUIRE(table != kNoEntity);
    const Entity victim = survivalColonist(sim, CellPos{18, 12, 0}, false);
    SURVIVAL_REQUIRE(butcher < victim);
    sim.step();
    sim.health().kill(victim, DeathCause::BloodLoss);
    sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Count, 1});
    // The bill leaves people out by default: let them in.
    sim.submit(SetBillOptionsCommand{table, 1, 0, 20, 0.0f, 0, 0, 0, {}});
    for (int i = 0; i < 6000 && reg.valid(victim); ++i) {
        sim.step();
    }
    CHECK_FALSE(reg.valid(victim));
    sim.stepMany(10);
    CHECK(reg.valid(butcher));
}
