#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Health.h"
#include "ColonySim/Inspirations.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define INSP_REQUIRE(...)       \
    do {                        \
        const bool inspOk = (__VA_ARGS__); \
        CHECK(inspOk);          \
        if (!inspOk) {          \
            return;             \
        }                       \
    } while (0)

namespace {

// The game's real data: the inspiration defs and rules are part of what is tested.
std::shared_ptr<const DefDatabase> inspDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim inspSim() { return Sim(SimSettings{5, 64, 0, 0, true}, inspDefs()); }

// Nobody snaps or is inspired by chance, unless a test turns it on again.
void inspQuiet(Sim& sim) {
    sim.mentalBreaks().setNatural(false);
    sim.inspirations().setNatural(false);
}

// A colonist with the given skill levels, after its first step so it has a mood.
Entity inspColonist(Sim& sim, CellPos cell, const std::vector<SkillRecord>& skills = {}) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.skills = skills;
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    sim.step();
    return pawn;
}

}  // namespace

TEST_CASE("Inspirations: the defs are all there, and a colonist can be given one once") {
    Sim sim = inspSim();
    inspQuiet(sim);
    InspirationSystem& insp = sim.inspirations();
    CHECK(insp.rules().present);
    const Entity pawn = inspColonist(sim, CellPos{10, 10, 0});
    INSP_REQUIRE(sim.things().registry().all_of<Mood>(pawn));
    CHECK_FALSE(insp.has(pawn));
    std::string error;
    CHECK_FALSE(insp.start(pawn, "NoSuchInspiration", error));
    CHECK_FALSE(error.empty());
    for (const char* def : {"InspiredCreativity", "InspiredSurgery", "InspiredTrade", "InspiredRecruitment", "FrenzyShoot"}) {
        INSP_REQUIRE(sim.defs().find("Inspirations", def) != nullptr);
    }
    CHECK(insp.start(pawn, "FrenzyShoot", error));
    CHECK(insp.defOf(pawn) == "FrenzyShoot");
    CHECK(insp.has(pawn));
    CHECK_FALSE(insp.start(pawn, "InspiredTrade", error));  // one at a time
    CHECK(error == "it is inspired already");
    CHECK(insp.end(pawn, "dismissed"));
    CHECK_FALSE(insp.has(pawn));
    CHECK_FALSE(insp.end(pawn, "dismissed"));
}

TEST_CASE("Inspirations: a pawn that can't do the work can't be inspired for it, and an animal can't be inspired at all") {
    Sim sim = inspSim();
    inspQuiet(sim);
    InspirationSystem& insp = sim.inspirations();
    SkillRecord noMedicine{"Medicine", 0, Passion::None, 0.0f, true};
    const Entity pawn = inspColonist(sim, CellPos{10, 10, 0}, {noMedicine});
    CHECK_FALSE(insp.eligible(pawn, "InspiredSurgery"));
    CHECK(insp.eligible(pawn, "FrenzyShoot"));
    std::string error;
    CHECK_FALSE(insp.start(pawn, "InspiredSurgery", error));
    CHECK(error == "it can't do the work this inspiration is for");
    const Entity animal = sim.things().spawn("Muffalo", CellPos{12, 12, 0});
    sim.step();
    CHECK_FALSE(insp.start(animal, "FrenzyShoot", error));
}

TEST_CASE("Inspirations: events say when one starts and ends, and it fades after its days") {
    Sim sim = inspSim();
    inspQuiet(sim);
    InspirationSystem& insp = sim.inspirations();
    std::vector<Event> log;
    insp.setEventLog(&log);
    const Entity pawn = inspColonist(sim, CellPos{10, 10, 0});
    std::string error;
    INSP_REQUIRE(insp.start(pawn, "FrenzyShoot", error));
    INSP_REQUIRE(log.size() == 1);
    CHECK(log[0].type == EventType::InspirationStarted);
    CHECK(log[0].thing == pawn);
    CHECK(log[0].message == "FrenzyShoot");
    CHECK(log[0].amount == doctest::Approx(2.5f));
    const Inspired inspired = sim.things().registry().get<Inspired>(pawn);
    CHECK(inspired.endsAt - inspired.startedAt == 216000);  // 2.5 days
    insp.step(inspired.endsAt - 1);
    CHECK(insp.has(pawn));
    insp.step(inspired.endsAt);
    CHECK_FALSE(insp.has(pawn));
    INSP_REQUIRE(log.size() == 2);
    CHECK(log[1].type == EventType::InspirationEnded);
    CHECK(log[1].message == "expired:FrenzyShoot");
}

TEST_CASE("Inspirations: only a good mood is ever inspired, and only for work it can do") {
    // Walks each pawn's own chances (one every 2,880 seconds, at a second that depends on the pawn) for 120 days.
    auto run = [](float moodLevel, std::vector<std::string>& seen) {
        Sim sim = inspSim();
        inspQuiet(sim);
        sim.inspirations().setNatural(true);
        SkillRecord medicine{"Medicine", 0, Passion::None, 0.0f, true};
        std::vector<Entity> pawns;
        for (int i = 0; i < 6; ++i) {
            const Entity pawn = inspColonist(sim, CellPos{10 + i * 2, 10, 0}, {medicine});
            sim.things().registry().get<Mood>(pawn).level = moodLevel;
            pawns.push_back(pawn);
        }
        const int64_t every = sim.inspirations().rules().checkEvery;
        std::vector<int64_t> turns;
        for (int64_t t = every; t < 120 * 86400; t += every) {
            for (const Entity pawn : pawns) {
                turns.push_back(t - static_cast<int64_t>(entt::to_integral(pawn)) % every);
            }
        }
        std::sort(turns.begin(), turns.end());
        for (const int64_t now : turns) {
            sim.inspirations().step(now);
            for (const Entity pawn : pawns) {
                if (sim.inspirations().has(pawn)) {
                    seen.push_back(sim.inspirations().defOf(pawn));
                    sim.inspirations().end(pawn, "dismissed");
                }
            }
        }
    };
    std::vector<std::string> sad;
    run(49.0f, sad);
    CHECK(sad.empty());
    std::vector<std::string> happy;
    run(100.0f, happy);
    CHECK(happy.size() >= 10);  // six pawns, a mean of 15 days, 120 days
    CHECK(happy.size() <= 150);
    CHECK(std::count(happy.begin(), happy.end(), "InspiredSurgery") == 0);  // no Medicine for them
    CHECK(std::count(happy.begin(), happy.end(), "FrenzyShoot") > 0);
    CHECK(std::count(happy.begin(), happy.end(), "InspiredCreativity") > 0);
}

TEST_CASE("Inspirations: the stat factors and one-use effects are used up by the thing they are for") {
    Sim sim = inspSim();
    inspQuiet(sim);
    InspirationSystem& insp = sim.inspirations();
    const Entity pawn = inspColonist(sim, CellPos{10, 10, 0});
    std::string error;
    CHECK(insp.statFactor(pawn, "aimSpread") == 1.0f);
    CHECK(insp.takeQualityShift(pawn) == 0);
    INSP_REQUIRE(insp.start(pawn, "FrenzyShoot", error));
    CHECK(insp.statFactor(pawn, "aimSpread") == doctest::Approx(0.7f));
    CHECK(insp.statFactor(pawn, "fireCooldown") == doctest::Approx(0.67f));
    CHECK(insp.statFactor(pawn, "tradePrice") == 1.0f);
    CHECK(insp.takeQualityShift(pawn) == 0);
    CHECK(insp.has(pawn));  // asking for what it doesn't give uses nothing up
    insp.end(pawn, "dismissed");

    INSP_REQUIRE(insp.start(pawn, "InspiredTrade", error));
    CHECK(insp.statFactor(pawn, "tradePrice") == doctest::Approx(0.85f));
    insp.end(pawn, "dismissed");

    INSP_REQUIRE(insp.start(pawn, "InspiredCreativity", error));
    CHECK(insp.takeQualityShift(pawn) == 2);
    CHECK_FALSE(insp.has(pawn));
    CHECK(insp.takeQualityShift(pawn) == 0);

    INSP_REQUIRE(insp.start(pawn, "InspiredRecruitment", error));
    CHECK(insp.takeRecruitFactor(pawn) == doctest::Approx(2.0f));
    CHECK_FALSE(insp.has(pawn));
    CHECK(insp.takeRecruitFactor(pawn) == 1.0f);

    INSP_REQUIRE(insp.start(pawn, "InspiredSurgery", error));
    CHECK(insp.takeFreeMedicine(pawn));
    CHECK_FALSE(insp.has(pawn));
    CHECK_FALSE(insp.takeFreeMedicine(pawn));
}

namespace {

// The quality of the first knife a smith of Crafting 12 makes, with or without an inspiration to start with.
int inspKnifeQuality(bool inspired, bool& used) {
    Sim sim = inspSim();
    inspQuiet(sim);
    sim.submit(UnlockResearchCommand{"Smithing"});
    sim.step();
    Entity smithy = kNoEntity;
    Entity lamp = kNoEntity;
    Entity steel = kNoEntity;
    auto spawn = [&](const char* def, CellPos cell, int count) {
        sim.submit(SpawnThingCommand{def, cell, count});
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ThingSpawned && e.cell == cell) {
                return testFueled(sim, e.thing);
            }
        }
        return kNoEntity;
    };
    smithy = spawn("FueledSmithy", CellPos{20, 20, 0}, 1);
    lamp = spawn("TorchLamp", CellPos{17, 17, 0}, 1);
    steel = spawn("Steel", CellPos{19, 19, 0}, 60);
    if (smithy == kNoEntity || lamp == kNoEntity || steel == kNoEntity) {
        return -2;
    }
    const Entity pawn = inspColonist(sim, CellPos{18, 18, 0}, {SkillRecord{"Crafting", 12, Passion::None, 0.0f, false}});
    std::string error;
    if (inspired && !sim.inspirations().start(pawn, "InspiredCreativity", error)) {
        return -3;
    }
    sim.submit(AddBillCommand{smithy, "MakeKnife", BillMode::Count, 1});
    const DefId knife = sim.defs().idOf("Things", "Melee_Knife");
    for (int i = 0; i < 12000 && sim.jobs().bills().stockOf(knife) < 1; ++i) {
        sim.step();
    }
    used = !sim.inspirations().has(pawn);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    for (const ThingView& view : snapshot.things) {
        if (view.def == knife) {
            return view.quality;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("Inspirations: an inspired crafter's next knife is two levels better, and the inspiration is used up") {
    bool plainUsed = false;
    bool inspiredUsed = false;
    const int plain = inspKnifeQuality(false, plainUsed);
    const int inspired = inspKnifeQuality(true, inspiredUsed);
    INSP_REQUIRE(plain >= 0);
    INSP_REQUIRE(inspired >= 0);
    CHECK(inspired == std::min(6, plain + 2));  // the same quality stream, shifted
    CHECK(inspiredUsed);
}

TEST_CASE("Inspirations: an inspired doctor's tend keeps its medicine") {
    auto tend = [](bool inspired, int& medicineLeft, bool& used) {
        Sim sim(SimSettings{7, 64, 0, 0, true}, inspDefs());
        sim.mentalBreaks().setNatural(false);
        sim.inspirations().setNatural(false);
        sim.things().spawn("HospitalBed", CellPos{20, 20, 0});
        sim.things().spawn("MedicineIndustrial", CellPos{12, 12, 0}, 5);
        sim.things().spawn("MealSurvivalPack", CellPos{14, 14, 0}, 20);
        const Entity doctor = sim.things().spawn("Human", CellPos{10, 10, 0});
        sim.jobs().addWorker(doctor);
        sim.things().registry().emplace<Colonist>(doctor);
        const Entity patient = sim.things().spawn("Human", CellPos{30, 30, 0});
        sim.jobs().addWorker(patient);
        sim.submit(SetWorkPriorityCommand{patient, "Doctor", 0});
        for (int h = 0; h < 24; ++h) {
            sim.submit(SetScheduleCommand{doctor, h, ScheduleKind::Work});
            sim.submit(SetScheduleCommand{patient, h, ScheduleKind::Work});
        }
        sim.step();
        sim.step();
        std::string error;
        if (inspired && !sim.inspirations().start(doctor, "InspiredSurgery", error)) {
            return false;
        }
        Rng rng(99);
        const std::vector<std::string> labels = sim.health().partLabels(sim.things().registry().get<Thing>(patient).def);
        auto part = [&](const std::string& label) {
            return static_cast<int>(std::find(labels.begin(), labels.end(), label) - labels.begin());
        };
        for (const char* p : {"torso", "left leg", "right leg", "left arm"}) {
            sim.health().applyDamage(patient, sim.health().damageTypeId("Blunt"), 20.0f, part(p), rng);
        }
        sim.health().applyDamage(patient, sim.health().damageTypeId("Cut"), 6.0f, part("right arm"), rng);
        bool tended = false;
        for (int i = 0; i < 3000 && !tended; ++i) {
            sim.step();
            for (const Event& e : sim.events()) {
                tended = tended || e.type == EventType::PawnTended;
            }
        }
        // Carry on a little so a kept medicine shows up lying on the ground.
        sim.stepMany(300);
        int total = 0;
        const DefId med = sim.defs().idOf("Things", "MedicineIndustrial");
        for (const auto [item, thing, stack] : sim.things().registry().view<const Thing, const StackCount>().each()) {
            (void)item;
            total += thing.def == med ? stack.count : 0;
        }
        medicineLeft = total;
        used = !sim.inspirations().has(doctor);
        return tended;
    };
    int plainLeft = 0;
    int keptLeft = 0;
    bool plainUsed = false;
    bool keptUsed = false;
    INSP_REQUIRE(tend(false, plainLeft, plainUsed));
    INSP_REQUIRE(tend(true, keptLeft, keptUsed));
    CHECK(plainLeft == 4);
    CHECK(keptLeft == 5);
    CHECK(keptUsed);
}

TEST_CASE("Inspirations: an inspired pawn saves and loads, and the loaded game goes on the same") {
    Sim original = inspSim();
    inspQuiet(original);
    const Entity pawn = inspColonist(original, CellPos{10, 10, 0});
    std::string error;
    INSP_REQUIRE(original.inspirations().start(pawn, "InspiredTrade", error));
    original.stepMany(100);
    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, inspDefs());
    INSP_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.sim->stateHash() == original.stateHash());
    CHECK(loaded.sim->inspirations().defOf(pawn) == "InspiredTrade");
    CHECK(saveSim(*loaded.sim) == bytes);
    loaded.sim->mentalBreaks().setNatural(false);
    loaded.sim->inspirations().setNatural(false);
    original.stepMany(300);
    loaded.sim->stepMany(300);
    CHECK(loaded.sim->stateHash() == original.stateHash());
}
