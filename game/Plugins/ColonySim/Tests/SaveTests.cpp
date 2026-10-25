#include "ColonySim/Defs.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define SAVE_REQUIRE(...)      \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// Commands as Unreal would send them while the haulers work: some fail, some name things by id.
void saveTestSession(Sim& sim, int64_t from, int64_t to) {
    for (int64_t i = from; i < to; ++i) {
        if (i == 7) {
            sim.submit(SpawnThingCommand{"Wall", CellPos{10, 10, 0}});
            sim.submit(SpawnThingCommand{"Wall", CellPos{11, 10, 0}});
            sim.submit(SpawnThingCommand{"Wall", CellPos{10, 10, 0}});  // fails: occupied
        }
        if (i == 500) {
            sim.submit(SpawnThingCommand{"Steel", CellPos{60, 70, 0}, 30});
            sim.submit(SpawnThingCommand{"Bed", CellPos{20, 30, 0}, 1, Rot4::West});
        }
        if (i == 1200) {
            sim.submit(DestroyThingCommand{sim.map().buildingAt(CellPos{11, 10, 0})});
        }
        if (i == 2500) {
            sim.submit(SpawnThingCommand{"Door", CellPos{12, 10, 0}});
            sim.submit(SpawnThingCommand{"Wall", CellPos{11, 10, 0}});
        }
        sim.step();
    }
}

SimSettings saveTestSettings() {
    SimSettings settings{31, 128, 6, 60, true};
    settings.checkpointEvery = 600;
    return settings;
}

bool saveTestCarrying(const Sim& sim) {
    // An item with no Position is in someone's hands.
    return sim.things().registry().view<const Thing>(entt::exclude<Position>).size_hint() > 0;
}

// Defs for a database that lacks one of the test defs.
std::shared_ptr<const DefDatabase> saveTestDefsWithout(const std::string& removed) {
    std::string items =
        R"([{"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}},
            {"defName": "Steel", "category": "Item", "stackLimit": 75, "stats": {"maxHitPoints": 100}},
            {"defName": "Wall", "category": "Building", "passability": "Impassable", "stats": {"maxHitPoints": 300}},
            {"defName": "Door", "category": "Building", "thingClass": "Building_Door", "passability": "PassThroughOnly"}])";
    const std::string bed = R"(,{"defName": "Bed", "category": "Building", "size": [1, 2]}])";
    if (removed != "Bed") {
        items.pop_back();
        items += bed;
    }
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build({{"Things", "test.json", items}}, "", *db, errors);
    return db;
}

}  // namespace

TEST_CASE("Save: load and run on gives the same game as never saving") {
    const auto defs = testDefs();
    Sim original(saveTestSettings(), defs);
    saveTestSession(original, 0, 2000);
    SAVE_REQUIRE(saveTestCarrying(original));  // the save catches pawns mid-job, items in their hands
    // Commands still queued at the moment of the save apply after loading.
    original.submit(SpawnThingCommand{"Steel", CellPos{40, 40, 0}, 5});

    const std::vector<uint8_t> bytes = saveSim(original);
    SaveLoadResult loaded = loadSim(bytes, defs);
    SAVE_REQUIRE(loaded.error.empty());
    SAVE_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.warnings.empty());
    Sim& copy = *loaded.sim;

    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.time().seconds == 2000);
    CHECK(copy.things().count() == original.things().count());
    CHECK(copy.commandLog().size() == original.commandLog().size());
    CHECK(copy.events().empty());
    CHECK(saveSim(copy) == bytes);  // saving what was loaded gives the same file

    // Run both on, with the same later commands, and compare at every checkpoint.
    saveTestSession(original, 2000, 6000);
    saveTestSession(copy, 2000, 6000);
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.stats().hauls == original.stats().hauls);
    CHECK(copy.itemsLeft() == original.itemsLeft());
    SAVE_REQUIRE(copy.checkpoints().size() == original.checkpoints().size());
    for (size_t i = 0; i < copy.checkpoints().size(); ++i) {
        CHECK(copy.checkpoints()[i].step == original.checkpoints()[i].step);
        CHECK(copy.checkpoints()[i].hash == original.checkpoints()[i].hash);
    }
    CHECK(saveSim(copy) == saveSim(original));
}

TEST_CASE("Save: saving and loading again and again changes nothing") {
    const auto defs = testDefs();
    Sim original(saveTestSettings(), defs);
    Sim* current = &original;
    std::unique_ptr<Sim> holder;
    for (int round = 0; round < 6; ++round) {
        saveTestSession(*current, current->time().seconds, current->time().seconds + 700);
        SaveLoadResult loaded = loadSim(saveSim(*current), defs);
        SAVE_REQUIRE(loaded.sim != nullptr);
        holder = std::move(loaded.sim);
        current = holder.get();
    }
    Sim straight(saveTestSettings(), defs);
    // The straight run has the same commands only if they land at the same game seconds, so
    // compare against a run that doesn't save: same session pieces, no loads.
    for (int round = 0; round < 6; ++round) {
        saveTestSession(straight, straight.time().seconds, straight.time().seconds + 700);
    }
    CHECK(current->time().seconds == straight.time().seconds);
    CHECK(current->stateHash() == straight.stateHash());
}

TEST_CASE("Save: a loaded sim hands out the same thing ids as the one that was saved") {
    const auto defs = testDefs();
    Sim original(SimSettings{5, 64, 0, 0, true}, defs);
    // Make and destroy things so some ids have been used several times and the free list is
    // in a particular order.
    std::vector<Entity> made;
    for (int round = 0; round < 5; ++round) {
        made.clear();
        for (int i = 0; i < 8; ++i) {
            made.push_back(original.things().spawn("Steel", CellPos{i, round, 0}, 3));
        }
        for (const int i : {3, 0, 6, 1, 5}) {
            original.things().destroy(made[static_cast<size_t>(i)]);
        }
    }
    SaveLoadResult loaded = loadSim(saveSim(original), defs);
    SAVE_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    for (int i = 0; i < 12; ++i) {
        const Entity a = original.things().spawn("Wall", CellPos{20 + i, 20, 0});
        const Entity b = copy.things().spawn("Wall", CellPos{20 + i, 20, 0});
        CHECK(a == b);
    }
    CHECK(copy.stateHash() == original.stateHash());
}

TEST_CASE("Save: buildings, doors and reachability come back") {
    const auto defs = testDefs();
    Sim original(SimSettings{5, 64, 0, 0, true}, defs);
    for (int y = 0; y < 64; ++y) {
        original.things().spawn("Wall", CellPos{30, y, 0});
    }
    original.things().spawn("Bed", CellPos{10, 10, 0}, 1, Rot4::East);
    original.things().destroy(original.map().buildingAt(CellPos{30, 20, 0}));
    original.things().spawn("Door", CellPos{30, 20, 0});

    SaveLoadResult loaded = loadSim(saveSim(original), defs);
    SAVE_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.map().buildingAt(CellPos{30, 5, 0}) == original.map().buildingAt(CellPos{30, 5, 0}));
    CHECK_FALSE(copy.map().walkable(CellPos{30, 5, 0}));
    CHECK(copy.map().obstacleDm(CellPos{30, 5, 0}) == original.map().obstacleDm(CellPos{30, 5, 0}));
    CHECK(copy.map().obstacleDm(CellPos{30, 5, 0}) > 0);  // a wall blocks sight; its height came back
    CHECK(copy.map().isDoor(CellPos{30, 20, 0}));
    // The Bed faces east: 2 wide, 1 deep.
    CHECK(copy.map().buildingAt(CellPos{11, 10, 0}) == original.map().buildingAt(CellPos{10, 10, 0}));
    CHECK(copy.regions().canReach(CellPos{5, 5, 0}, CellPos{50, 5, 0}));  // through the door
    CHECK(copy.regions().regionCount() == original.regions().regionCount());

    Snapshot a;
    Snapshot b;
    original.writeSnapshot(a);
    copy.writeSnapshot(b);
    SAVE_REQUIRE(a.things.size() == b.things.size());
    for (size_t i = 0; i < a.things.size(); ++i) {
        CHECK(a.things[i].id == b.things[i].id);
        CHECK(a.things[i].def == b.things[i].def);
        CHECK(a.things[i].x == b.things[i].x);
        CHECK(a.things[i].sizeX == b.things[i].sizeX);
    }
}

TEST_CASE("Save: a loaded sim records the same replay as the original") {
    const auto defs = testDefs();
    Sim original(saveTestSettings(), defs);
    saveTestSession(original, 0, 3000);
    SaveLoadResult loaded = loadSim(saveSim(original), defs);
    SAVE_REQUIRE(loaded.sim != nullptr);
    saveTestSession(original, 3000, 4000);
    saveTestSession(*loaded.sim, 3000, 4000);

    const Replay a = recordReplay(original);
    const Replay b = recordReplay(*loaded.sim);
    CHECK(writeReplay(a) == writeReplay(b));
    // And the replay of the whole session, from step 0, reproduces both.
    Sim replayed(a.settings, defs);
    playReplay(a, replayed, a.endStep);
    CHECK(replayed.stateHash() == loaded.sim->stateHash());
}

TEST_CASE("Save: damaged, wrong and out of date files are refused with a reason") {
    const auto defs = testDefs();
    Sim original(saveTestSettings(), defs);
    saveTestSession(original, 0, 1000);
    const std::vector<uint8_t> good = saveSim(original);

    SUBCASE("not a save") {
        CHECK(loadSim({}, defs).error.find("not a Rim3D save") != std::string::npos);
        CHECK(loadSim(std::vector<uint8_t>(100, 7), defs).error.find("not a Rim3D save") != std::string::npos);
    }
    SUBCASE("cut short") {
        std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
        const SaveLoadResult r = loadSim(cut, defs);
        CHECK(r.sim == nullptr);
        CHECK(r.error.find("damaged") != std::string::npos);
    }
    SUBCASE("one flipped byte, anywhere") {
        for (const size_t at : {size_t{9}, good.size() / 3, good.size() / 2, good.size() - 12}) {
            std::vector<uint8_t> bad = good;
            bad[at] ^= 0x10;
            const SaveLoadResult r = loadSim(bad, defs);
            CHECK(r.sim == nullptr);
            CHECK_FALSE(r.error.empty());
        }
    }
    SUBCASE("a newer format") {
        std::vector<uint8_t> newer = good;
        newer[8] = 99;  // format version, little-endian
        const SaveLoadResult r = loadSim(newer, defs);
        CHECK(r.sim == nullptr);
        CHECK(r.error.find("newer version") != std::string::npos);
    }
    SUBCASE("a def the data files no longer have") {
        const SaveLoadResult r = loadSim(good, saveTestDefsWithout("Bed"));
        CHECK(r.sim == nullptr);
        CHECK(r.error.find("'Bed'") != std::string::npos);
    }
    SUBCASE("no defs at all") {
        CHECK(loadSim(good, nullptr).sim == nullptr);
    }
}

TEST_CASE("Save: other data files load with a warning when every def is still there") {
    const auto defs = testDefs();
    Sim original(saveTestSettings(), defs);
    saveTestSession(original, 0, 1000);
    const SaveLoadResult r = loadSim(saveSim(original), saveTestDefsWithout("nothing"));
    SAVE_REQUIRE(r.sim != nullptr);
    SAVE_REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("data files differ") != std::string::npos);
}

TEST_CASE("Save: a sim with no defs and no things saves and loads") {
    Sim original(SimSettings{9, 64, 0, 0, false});
    original.stepMany(5000);
    SaveLoadResult r = loadSim(saveSim(original), nullptr);
    SAVE_REQUIRE(r.sim != nullptr);
    CHECK(r.sim->stateHash() == original.stateHash());
    r.sim->stepMany(100);
    original.stepMany(100);
    CHECK(r.sim->stateHash() == original.stateHash());
}

TEST_CASE("Save: the JSON dump names defs and matches the save") {
    const auto defs = testDefs();
    Sim sim(saveTestSettings(), defs);
    saveTestSession(sim, 0, 1500);
    const std::string json = saveToJson(sim);
    CHECK(json.find("\"format\": \"rim3d-save\"") != std::string::npos);
    CHECK(json.find("\"def\": \"Human\"") != std::string::npos);
    CHECK(json.find("\"def\": \"Wall\"") != std::string::npos);
    CHECK(json.find("\"name\": \"SpawnThing\"") != std::string::npos);

    // The same dump comes out of the file's bytes, with no sim and no defs.
    std::string fromFile;
    std::string error;
    SAVE_REQUIRE(saveFileToJson(saveSim(sim), fromFile, error));
    CHECK(fromFile == json);
    CHECK_FALSE(saveFileToJson(std::vector<uint8_t>(20, 0), fromFile, error));
}
