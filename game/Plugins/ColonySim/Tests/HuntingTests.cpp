// Hunting and butchering (task N2, docs/design/hunting.md).
#include "ColonySim/Animals.h"
#include "ColonySim/Bills.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Husbandry.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Zones.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define HUNT_REQUIRE(...)                  \
    do {                                    \
        const bool huntOk = (__VA_ARGS__); \
        CHECK(huntOk);                      \
        if (!huntOk) {                      \
            return;                         \
        }                                   \
    } while (0)

namespace {

// The game's real data.
std::shared_ptr<const DefDatabase> huntDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SimSettings huntSettings(uint64_t seed) {
    SimSettings s;
    s.seed = seed;
    s.mapSize = 64;
    s.wildAnimals = 0;
    s.flatMap = true;
    s.checkpointEvery = 0;
    return s;
}

struct HuntFixture {
    Sim sim;
    Entity colonist = kNoEntity;

    explicit HuntFixture(uint64_t seed = 5) : sim(huntSettings(seed), huntDefs()) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.stepMany(9 * kSecondsPerHour);  // daytime: animals are awake
    }

    Husbandry& hu() { return sim.husbandry(); }
    entt::registry& reg() { return sim.things().registry(); }
    DefId def(const char* name) const { return sim.defs().idOf("Things", name); }

    // A colonist who works, with the given Cooking and Shooting levels and a rifle if `gun` is set.
    Entity addColonist(CellPos cell, int cooking, bool gun) {
        const Entity pawn = sim.things().spawn("Human", cell);
        Colonist c;
        c.skills.push_back(SkillRecord{"Cooking", cooking, Passion::None, 0.0f, false});
        c.skills.push_back(SkillRecord{"Shooting", 10, Passion::None, 0.0f, false});
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn, {}, false);
        if (gun) {
            sim.combat().equip(pawn, sim.things().make(def("Gun_BoltActionRifle")));
        }
        colonist = pawn;
        return pawn;
    }
    // A finished building, through the command like the game does it.
    Entity building(const char* name, CellPos cell) {
        sim.submit(SpawnThingCommand{name, cell, 1});
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == name) {
                return e.thing;
            }
        }
        return kNoEntity;
    }
    Entity animal(const char* name, CellPos cell) {
        const Entity e = sim.things().spawn(name, cell);
        sim.stepMany(5);  // its first look sets it up
        return e;
    }
    bool dead(Entity e) { return reg().valid(e) && reg().get<Health>(e).dead; }
    void kill(Entity e) { sim.health().kill(e, DeathCause::BloodLoss); }
    int onMap(const char* name) { return sim.jobs().bills().stockOf(def(name)); }
    CellPos cellOfThing(Entity e) { return cellOf(reg().get<Position>(e)); }
    template <typename F>
    bool runUntil(int limit, F done) {
        for (int i = 0; i < limit; ++i) {
            if (done()) {
                return true;
            }
            sim.step();
        }
        return done();
    }
    bool sawEvent(EventType type) const {
        return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; });
    }
};

// RimWorld's butchery efficiency for a colonist: (75% + 2.5% per Cooking level) x (1 + 0.9 x (Manipulation - 1)).
double huntEfficiency(int cooking, double manipulation) { return (0.75 + 0.025 * cooking) * (1.0 + 0.9 * (manipulation - 1.0)); }

// Cooks a deer's corpse at a table with a colonist of the given Cooking level; returns the meat left on the map, or -1.
int huntButcherDeer(int cooking, int* leather = nullptr) {
    HuntFixture f;
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, cooking, false);
    (void)pawn;
    const Entity table = f.building("ButcherTable", CellPos{12, 10, 0});
    const Entity deer = f.animal("Deer", CellPos{18, 12, 0});
    if (table == kNoEntity || deer == kNoEntity) {
        return -1;
    }
    f.kill(deer);
    f.sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Count, 1});
    f.sim.step();
    const bool done = f.runUntil(5000, [&] { return !f.reg().valid(deer); });
    if (!done) {
        return -1;
    }
    f.sim.stepMany(5);
    if (leather != nullptr) {
        *leather = f.onMap("Leather_Plain");
    }
    return f.onMap("Meat_Deer");
}

}  // namespace

TEST_CASE("Hunting: a hunt mark is for living wild animals only, and replaces a tame mark") {
    HuntFixture f;
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, 8, true);
    const Entity deer = f.animal("Deer", CellPos{20, 20, 0});
    const Entity cow = f.animal("Cow", CellPos{25, 20, 0});
    HUNT_REQUIRE(deer != kNoEntity && cow != kNoEntity);
    std::string error;
    CHECK_FALSE(f.hu().markHunt(pawn, true, error));  // not an animal
    CHECK_FALSE(error.empty());
    for (int i = 0; i < 200 && !f.hu().isTame(cow); ++i) {
        f.hu().markTame(cow, true, error);
        f.hu().finishTame(pawn, cow, 20);
    }
    HUNT_REQUIRE(f.hu().isTame(cow));
    CHECK_FALSE(f.hu().markHunt(cow, true, error));  // tame: slaughter it
    HUNT_REQUIRE(f.hu().markTame(deer, true, error));
    HUNT_REQUIRE(f.hu().markHunt(deer, true, error));
    CHECK(f.hu().care(deer)->markHunt);
    CHECK_FALSE(f.hu().care(deer)->markTame);  // one or the other
    HUNT_REQUIRE(f.hu().markTame(deer, true, error));
    CHECK_FALSE(f.hu().care(deer)->markHunt);
    HUNT_REQUIRE(f.hu().markHunt(deer, true, error));
    HUNT_REQUIRE(f.hu().markHunt(deer, false, error));
    CHECK_FALSE(f.hu().care(deer)->markHunt);
    f.kill(deer);
    CHECK_FALSE(f.hu().markHunt(deer, true, error));  // dead
    // The command goes through the sim and shows in the snapshot.
    const Entity hare = f.animal("Hare", CellPos{30, 20, 0});
    f.sim.submit(MarkAnimalCommand{hare, "Hunt", true});
    f.sim.step();
    CHECK_FALSE(f.sawEvent(EventType::CommandFailed));
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    const ThingView* view = snap.find(hare);
    HUNT_REQUIRE(view != nullptr);
    CHECK(view->markHunt);
}

TEST_CASE("Hunting: a colonist with a gun shoots a marked animal and carries the corpse to a stockpile") {
    HuntFixture f;
    f.sim.submit(CreateStockpileCommand{CellRect{8, 14, 3, 3}});
    f.sim.step();
    const Entity hunter = f.addColonist(CellPos{10, 10, 0}, 8, true);
    const Entity deer = f.animal("Deer", CellPos{26, 12, 0});
    HUNT_REQUIRE(deer != kNoEntity);
    std::string error;

    // No mark, no hunt.
    f.sim.stepMany(300);
    CHECK_FALSE(f.dead(deer));
    CHECK(f.hu().stats().hunted == 0);

    HUNT_REQUIRE(f.hu().markHunt(deer, true, error));
    bool hunting = false;
    bool carried = false;
    const bool done = f.runUntil(20000, [&] {
        for (const Event& e : f.sim.events()) {
            hunting = hunting || (e.type == EventType::JobStarted && e.job == JobType::Hunt && e.thing == hunter);
            carried = carried || (e.type == EventType::JobStarted && e.job == JobType::HaulCorpse && e.thing == hunter);
        }
        return f.dead(deer) && f.sim.zones().stockpileAt(f.cellOfThing(deer)) != nullptr;
    });
    HUNT_REQUIRE(done);
    CHECK(hunting);
    CHECK(carried);
    CHECK(f.hu().stats().hunted == 1);
    CHECK_FALSE(f.hu().care(deer)->markHunt);
    CHECK(f.hu().care(deer)->collect);
    CHECK(f.hu().isCorpse(deer));
    // It stays in the stockpile and isn't carried again.
    const CellPos where = f.cellOfThing(deer);
    int haulJobs = 0;
    for (int i = 0; i < 600; ++i) {
        f.sim.step();
        for (const Event& e : f.sim.events()) {
            haulJobs += e.type == EventType::JobStarted && e.job == JobType::HaulCorpse ? 1 : 0;
        }
    }
    CHECK(haulJobs == 0);
    CHECK(f.cellOfThing(deer) == where);
}

TEST_CASE("Hunting: nobody hunts without a gun in their hands") {
    HuntFixture f;
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, 8, false);
    const Entity deer = f.animal("Deer", CellPos{16, 10, 0});
    HUNT_REQUIRE(deer != kNoEntity);
    std::string error;
    HUNT_REQUIRE(f.hu().markHunt(deer, true, error));
    bool hunting = false;
    for (int i = 0; i < 1500; ++i) {
        f.sim.step();
        for (const Event& e : f.sim.events()) {
            hunting = hunting || (e.type == EventType::JobStarted && e.job == JobType::Hunt);
        }
    }
    CHECK_FALSE(hunting);
    CHECK_FALSE(f.dead(deer));
    // Give it the rifle and the hunt starts.
    f.sim.combat().equip(pawn, f.sim.things().make(f.def("Gun_BoltActionRifle")));
    const bool started = f.runUntil(3000, [&] {
        for (const Event& e : f.sim.events()) {
            hunting = hunting || (e.type == EventType::JobStarted && e.job == JobType::Hunt);
        }
        return hunting;
    });
    CHECK(started);
}

TEST_CASE("Hunting: a hunted animal that is hurt can turn on the hunter") {
    // Wild boar fight back at whoever hurts them (N5): across a few games at least one of them does.
    uint64_t turned = 0;
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        HuntFixture f(seed);
        f.addColonist(CellPos{10, 10, 0}, 8, true);
        const Entity boar = f.animal("WildBoar", CellPos{24, 10, 0});
        if (boar == kNoEntity) {
            continue;
        }
        std::string error;
        f.hu().markHunt(boar, true, error);
        f.runUntil(6000, [&] { return f.dead(boar); });
        turned += f.sim.animals().stats().revenges + f.sim.animals().stats().manhunters;
    }
    CHECK(turned > 0);
}

TEST_CASE("Butchering: a bill carries a corpse to the table and cuts it up into meat and leather") {
    HuntFixture f;
    f.addColonist(CellPos{10, 10, 0}, 8, false);
    const Entity table = f.building("ButcherTable", CellPos{12, 10, 0});
    const Entity deer = f.animal("Deer", CellPos{18, 12, 0});
    HUNT_REQUIRE(table != kNoEntity && deer != kNoEntity);
    f.kill(deer);
    const float share = f.sim.animals().bodySize(deer) / 1.2f;  // the deer's body size against an adult's
    f.sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Count, 1});
    f.sim.step();
    CHECK_FALSE(f.sawEvent(EventType::CommandFailed));
    CHECK(f.onMap("Meat_Deer") == 0);

    bool butchered = false;
    int meat = -1;
    int leather = -1;
    const bool done = f.runUntil(5000, [&] {
        for (const Event& e : f.sim.events()) {
            if (e.type == EventType::AnimalButchered) {
                butchered = true;
                meat = static_cast<int>(e.amount);
                leather = e.part;
            }
        }
        return butchered;
    });
    HUNT_REQUIRE(done);
    // Cooking 8: 75% + 2.5% x 8 = 95%, times the hands, of the animal's 90 meat and 50 leather.
    const double efficiency = huntEfficiency(8, f.sim.health().capacityValue(f.colonist, "Manipulation"));
    CHECK(meat == static_cast<int>(std::lround(90.0 * efficiency * share)));
    CHECK(leather == static_cast<int>(std::lround(50.0 * efficiency * share)));
    CHECK_FALSE(f.reg().valid(deer));  // the corpse is gone
    f.sim.stepMany(5);
    CHECK(f.onMap("Meat_Deer") == meat);
    CHECK(f.onMap("Leather_Plain") == leather);
    // The one-time bill is finished.
    const BillList* list = f.sim.jobs().bills().billsOf(table);
    CHECK((list == nullptr || list->bills.empty()));
}

TEST_CASE("Butchering: the yield follows the butcher's Cooking skill") {
    int lowLeather = 0;
    int highLeather = 0;
    const int low = huntButcherDeer(0, &lowLeather);
    const int mid = huntButcherDeer(8);
    const int high = huntButcherDeer(20, &highLeather);
    HUNT_REQUIRE(low > 0 && mid > 0 && high > 0);
    CHECK(low < mid);
    CHECK(mid < high);
    // 75% at skill 0, 95% at 8 and 125% at skill 20 (RimWorld's butchery efficiency), whatever the hands: the shares are 3 : 3.8 : 5.
    CHECK(std::abs(high * 3 - low * 5) <= 5);
    CHECK(std::abs(mid * 3 - low * 19 / 5) <= 5);
    CHECK(std::abs(highLeather * 3 - lowLeather * 5) <= 4);
}

TEST_CASE("Butchering: a bill's radius and an until-you-have count decide which corpses it takes") {
    HuntFixture f;
    f.addColonist(CellPos{10, 10, 0}, 8, false);
    const Entity table = f.building("ButcherTable", CellPos{12, 10, 0});
    const Entity near1 = f.animal("Deer", CellPos{16, 12, 0});
    const Entity near2 = f.animal("Deer", CellPos{16, 14, 0});
    const Entity near3 = f.animal("Deer", CellPos{16, 16, 0});
    const Entity far = f.animal("Deer", CellPos{50, 50, 0});
    HUNT_REQUIRE(table != kNoEntity && near3 != kNoEntity && far != kNoEntity);
    for (const Entity e : {near1, near2, near3, far}) {
        f.kill(e);
    }
    f.sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::UntilHave, 150});
    f.sim.step();
    const BillList* list = f.sim.jobs().bills().billsOf(table);
    HUNT_REQUIRE(list != nullptr && !list->bills.empty());
    BillSystem::Options options;
    options.radius = 12.0f;  // the far corpse is out of reach for the bill
    std::string error;
    HUNT_REQUIRE(f.sim.jobs().bills().setOptions(table, list->bills.back().id, options, error));

    // One deer gives about 86 meat, two about 172: the bill stops with the second, and the third and the far one stay.
    f.runUntil(20000, [&] { return f.onMap("Meat_Deer") >= 150; });
    f.sim.stepMany(3000);
    const int valid = static_cast<int>(f.reg().valid(near1)) + static_cast<int>(f.reg().valid(near2)) +
                      static_cast<int>(f.reg().valid(near3));
    CHECK(valid == 1);
    CHECK(f.reg().valid(far));
    CHECK(f.onMap("Meat_Deer") >= 150);
    CHECK(f.onMap("Meat_Deer") < 250);
}

TEST_CASE("Butchering: a slaughtered animal is carried to the stockpile and can be butchered from there") {
    HuntFixture f;
    f.sim.submit(CreateStockpileCommand{CellRect{8, 14, 3, 3}});
    f.sim.step();
    const Entity pawn = f.addColonist(CellPos{10, 10, 0}, 8, false);
    const Entity cow = f.animal("Cow", CellPos{14, 12, 0});
    HUNT_REQUIRE(cow != kNoEntity);
    for (int i = 0; i < 200 && !f.hu().isTame(cow); ++i) {
        std::string error;
        f.hu().markTame(cow, true, error);
        f.hu().finishTame(pawn, cow, 20);
    }
    HUNT_REQUIRE(f.hu().isTame(cow));
    f.sim.submit(MarkAnimalCommand{cow, "Slaughter", true});
    f.sim.step();
    const bool stored = f.runUntil(20000, [&] { return f.dead(cow) && f.sim.zones().stockpileAt(f.cellOfThing(cow)) != nullptr; });
    HUNT_REQUIRE(stored);
    CHECK(f.hu().care(cow)->collect);

    const Entity table = f.building("ButcherTable", CellPos{12, 10, 0});
    HUNT_REQUIRE(table != kNoEntity);
    f.sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Count, 1});
    f.sim.step();
    int meat = 0;
    int leather = 0;
    const bool cut = f.runUntil(8000, [&] {
        for (const Event& e : f.sim.events()) {
            if (e.type == EventType::AnimalButchered) {
                meat = static_cast<int>(e.amount);
                leather = e.part;
            }
        }
        return !f.reg().valid(cow);
    });
    CHECK(cut);
    CHECK(meat > 0);  // a cow's 200 meat and 70 leather, about 90% of it
    CHECK(leather > 0);
}

TEST_CASE("Hunting: saving and loading in the middle of a hunt keeps the game going the same") {
    HuntFixture f;
    f.sim.submit(CreateStockpileCommand{CellRect{8, 14, 3, 3}});
    f.sim.step();
    f.addColonist(CellPos{10, 10, 0}, 8, true);
    const Entity table = f.building("ButcherTable", CellPos{12, 10, 0});
    const Entity deer = f.animal("Deer", CellPos{30, 12, 0});
    HUNT_REQUIRE(deer != kNoEntity && table != kNoEntity);
    std::string error;
    HUNT_REQUIRE(f.hu().markHunt(deer, true, error));
    f.sim.submit(AddBillCommand{table, "ButcherCorpse", BillMode::Forever, 0});
    // Stop while the hunter is out after the deer.
    bool hunting = false;
    f.runUntil(3000, [&] {
        for (const Event& e : f.sim.events()) {
            hunting = hunting || (e.type == EventType::JobStarted && e.job == JobType::Hunt);
        }
        return hunting;
    });
    HUNT_REQUIRE(hunting);
    f.sim.stepMany(20);

    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, huntDefs());
    HUNT_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.warnings.empty());
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(copy.husbandry().care(deer)->markHunt);

    // On from the save: the hunt, the haul and the butchering come out the same in both.
    f.sim.stepMany(12000);
    copy.stepMany(12000);
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(copy.husbandry().stats().hunted == f.hu().stats().hunted);
    CHECK(saveSim(copy) == saveSim(f.sim));
    CHECK(f.hu().stats().hunted == 1);
}

TEST_CASE("Hunting: the hunt mark is a command that goes into the replay file") {
    HuntFixture f;
    f.addColonist(CellPos{10, 10, 0}, 8, true);
    const Entity deer = f.animal("Deer", CellPos{30, 12, 0});
    HUNT_REQUIRE(deer != kNoEntity);
    f.sim.submit(MarkAnimalCommand{deer, "Hunt", true});
    f.sim.step();
    CHECK_FALSE(f.sawEvent(EventType::CommandFailed));
    f.sim.submit(MarkAnimalCommand{deer, "Dance", true});
    f.sim.step();
    CHECK(f.sawEvent(EventType::CommandFailed));
    const Replay recorded = recordReplay(f.sim);
    Replay loaded;
    std::string error;
    HUNT_REQUIRE(readReplay(writeReplay(recorded), loaded, error));
    CHECK(loaded.commands.size() == recorded.commands.size());
}
