// Hands-on work (task U10, docs/design/handson.md): the controlled colonist does the sim's own jobs
// in place while the player holds the interact key.
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Apparel.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define HANDS_REQUIRE(...)     \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data, as the construction tests use it.
std::shared_ptr<const DefDatabase> handDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

SimSettings handSettings() {
    SimSettings settings{5, 48, 0, 0, true};
    settings.checkpointEvery = 100;
    return settings;
}

// A colonist made by hand (the first thing in a fresh sim, so a replay makes the same one), with
// the map around it cleared of plants.
Entity handColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    std::string why;  // a parka, so the weather doesn't hurt the hands (C15)
    sim.apparel().putOn(pawn, sim.apparel().make(sim.defs().idOf("Things", "Apparel_Parka"), sim.defs().idOf("Things", "Cloth"), -1, cell, &why), why);
    sim.things().registry().emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn, {}, false);
    for (int y = 4; y < 24; ++y) {
        for (int x = 4; x < 24; ++x) {
            sim.plants().remove(CellPos{x, y, 0});
        }
    }
    return pawn;
}

struct HandFixture {
    Sim sim{handSettings(), handDefs()};
    Entity pawn = kNoEntity;

    // The pawn is taken over by the player with a ControlPawn command.
    HandFixture() {
        pawn = handColonist(sim, CellPos{10, 10, 0});
        sim.submit(ControlPawnCommand{pawn});
        sim.step();
    }

    DefId def(const char* name) const { return sim.defs().idOf("Things", name); }
    const Worker& worker() const { return sim.things().registry().get<Worker>(pawn); }
    const Position& position() const { return sim.things().registry().get<Position>(pawn); }
    bool carries() const { return sim.things().registry().all_of<Carrying>(pawn); }

    // Sends a hands-on command, steps once and returns the failure message, empty if it worked.
    std::string hand(Entity thing, CellPos cell, HandsOnMode mode = HandsOnMode::Work) {
        const uint64_t id = sim.submit(HandsOnCommand{pawn, mode, thing, cell});
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::CommandFailed && e.command == id) {
                return e.message.empty() ? "failed" : e.message;
            }
        }
        return std::string();
    }
    std::string work(CellPos cell) { return hand(kNoEntity, cell); }

    Entity spawn(const char* def, CellPos cell, int count = 1) {
        sim.submit(SpawnThingCommand{def, cell, count});
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
                return e.thing;
            }
        }
        return kNoEntity;
    }
    int itemsOnMap(const char* def) const {
        const DefId id = this->def(def);
        int total = 0;
        sim.things().registry().view<const Thing, const StackCount, const Position>().each(
            [&](const Thing& thing, const StackCount& stack, const Position&) { total += thing.def == id ? stack.count : 0; });
        return total;
    }
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
};

bool handEnded(const Sim& sim, const std::string& message) {
    return std::any_of(sim.events().begin(), sim.events().end(),
                       [&](const Event& e) { return e.type == EventType::JobEnded && e.message == message; });
}

}  // namespace

TEST_CASE("Hands-on: holding interact mines a rock in place, and the mark it made is gone afterwards") {
    HandFixture f;
    const CellPos rock{12, 10, 0};
    f.sim.mineables().place(rock, f.def("Rock_Sandstone"));
    const Position before = f.position();

    CHECK(f.work(rock).empty());
    CHECK(f.worker().job.type == JobType::Mine);
    CHECK(f.worker().job.handsOn == 2);
    CHECK(f.sim.designations().at(rock) == DesignationKind::Mine);
    HANDS_REQUIRE(f.runUntil(3000, [&] { return !f.sim.mineables().has(rock); }));
    f.sim.step();
    CHECK(f.worker().job.type == JobType::None);
    CHECK(f.sim.designations().count() == 0);
    CHECK(f.sim.map().terrain(rock) == Terrain::RoughStone);  // dug out (a chunk is left some of the time)
    CHECK(f.sim.stats().failedJobs == 0);
    CHECK(f.position().x == before.x);  // it worked where it stood, no walking
    CHECK(f.position().y == before.y);
    CHECK(f.sim.things().registry().all_of<PlayerControl>(f.pawn));
}

TEST_CASE("Hands-on: chopping a tree gives its wood, and an existing mark is used and not removed early") {
    HandFixture f;
    const CellPos tree{12, 10, 0};
    f.sim.plants().remove(tree);
    f.sim.plants().place(tree, f.sim.defs().idOf("Things", "Plant_TreeOak"), 1.0f);
    f.sim.submit(DesignateCommand{CellRect{12, 10, 1, 1, 0}, DesignationKind::CutPlant});
    f.sim.step();

    CHECK(f.work(tree).empty());
    CHECK(f.worker().job.type == JobType::CutPlant);
    CHECK(f.worker().job.handsOn == 1);  // the mark was there already, so it is not the job's to take away
    f.sim.submit(HandsOnCommand{f.pawn, HandsOnMode::Stop, kNoEntity, CellPos{}});
    f.sim.step();
    CHECK(f.sim.designations().at(tree) == DesignationKind::CutPlant);
    CHECK(f.work(tree).empty());
    HANDS_REQUIRE(f.runUntil(3000, [&] { return !f.sim.plants().has(tree); }));
    CHECK(f.itemsOnMap("WoodLog") > 0);
    CHECK(f.sim.designations().count() == 0);
}

TEST_CASE("Hands-on: far targets, empty ground and a busy rock are refused") {
    HandFixture f;
    f.sim.mineables().place(CellPos{30, 10, 0}, f.def("Rock_Sandstone"));
    f.sim.mineables().place(CellPos{12, 10, 0}, f.def("Rock_Sandstone"));
    CHECK(f.work(CellPos{30, 10, 0}).find("too far") != std::string::npos);
    CHECK(f.work(CellPos{14, 14, 0}) == "nothing to work on there");
    CHECK(f.work(CellPos{-4, 3, 0}) == "nothing there");
    CHECK(f.worker().job.type == JobType::None);

    // Another colonist already digs the rock: the player doesn't take it.
    const Entity other = f.sim.things().spawn("Human", CellPos{13, 10, 0});
    f.sim.jobs().addWorker(other, {}, false);
    f.sim.submit(DesignateCommand{CellRect{12, 10, 1, 1, 0}, DesignationKind::Mine});
    CHECK(f.runUntil(200, [&] { return f.sim.jobs().reservedBy(JobTarget{kNoEntity, CellPos{12, 10, 0}}) == other; }));
    CHECK(f.work(CellPos{12, 10, 0}) == "another colonist is working on it");
}

TEST_CASE("Hands-on: letting go stops the job, takes the mark with it, and the rock keeps its damage") {
    HandFixture f;
    const CellPos rock{12, 10, 0};
    f.sim.mineables().place(rock, f.def("Rock_Sandstone"));
    f.sim.things().spawn("TorchLamp", CellPos{11, 9, 0});  // the sim starts at midnight; the swing counts at full speed
    f.sim.step();
    const int full = f.sim.mineables().hitPoints(rock);
    CHECK(f.work(rock).empty());
    for (int i = 0; i < 200; ++i) {
        f.sim.step();
    }
    CHECK(f.sim.mineables().hitPoints(rock) < full);  // one swing landed after 144 seconds
    CHECK(f.hand(kNoEntity, CellPos{}, HandsOnMode::Stop).empty());
    CHECK(f.worker().job.type == JobType::None);
    CHECK(f.sim.designations().at(rock) == DesignationKind::None);
    CHECK(handEnded(f.sim, "interrupted: the player let go"));
    CHECK(f.sim.mineables().has(rock));
    CHECK(f.sim.stats().failedJobs == 0);
}

TEST_CASE("Hands-on: stepping out of reach ends the job without a failure") {
    HandFixture f;
    const CellPos rock{12, 10, 0};
    f.sim.mineables().place(rock, f.def("Rock_Sandstone"));
    CHECK(f.work(rock).empty());
    float x = f.position().x;
    bool ended = false;
    for (int i = 0; i < 40 && !ended; ++i) {
        x -= 0.4f;
        f.sim.submit(DirectMoveCommand{f.pawn, x, 10.5f});
        f.sim.step();
        ended = handEnded(f.sim, "interrupted: out of reach");
    }
    CHECK(ended);
    CHECK(f.worker().job.type == JobType::None);
    CHECK(f.sim.designations().count() == 0);
    CHECK(f.sim.stats().failedJobs == 0);
}

TEST_CASE("Hands-on: pick up steel, bring it to a plan, build it, put the rest down") {
    HandFixture f;
    const Entity steel = f.spawn("Steel", CellPos{11, 10, 0}, 10);
    HANDS_REQUIRE(steel != kNoEntity);
    const Entity plan = [&] {
        f.sim.submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{12, 10, 0}, Rot4::North});
        f.sim.step();
        return f.sim.construction().siteAt(CellPos{12, 10, 0});
    }();
    HANDS_REQUIRE(plan != kNoEntity);

    // A plan with no materials can't be built; the player is told so.
    CHECK(f.hand(plan, CellPos{}).find("needs materials") != std::string::npos);
    CHECK(f.hand(steel, CellPos{}).empty());
    CHECK(f.carries());
    CHECK(f.itemsOnMap("Steel") == 0);
    // Hands full: nothing else can be picked up or worked.
    const Entity more = f.spawn("Steel", CellPos{11, 11, 0}, 3);
    CHECK(f.hand(more, CellPos{}) == "your hands are full");
    f.sim.mineables().place(CellPos{12, 11, 0}, f.def("Rock_Sandstone"));
    CHECK(f.work(CellPos{12, 11, 0}) == "your hands are full");

    // Into the plan: it takes 5, the rest is put down.
    CHECK(f.hand(plan, CellPos{}).empty());
    const ConstructionSite* site = f.sim.things().registry().try_get<ConstructionSite>(plan);
    HANDS_REQUIRE(site != nullptr);
    CHECK(site->hasAllMaterials());
    CHECK(!f.carries());
    CHECK(f.itemsOnMap("Steel") == 5 + 3);
    CHECK(f.worker().job.type == JobType::None);

    // Build it.
    CHECK(f.hand(plan, CellPos{}).empty());
    CHECK(f.worker().job.type == JobType::BuildFrame);
    HANDS_REQUIRE(f.runUntil(3000, [&] { return f.sim.map().passage(CellPos{12, 10, 0}) == Passage::Blocked; }));
    CHECK(f.sim.stats().failedJobs == 0);
}

TEST_CASE("Hands-on: putting down uses the aimed cell if it is near, else the feet; letting go drops what it carries") {
    HandFixture f;
    const Entity steel = f.spawn("Steel", CellPos{11, 10, 0}, 10);
    HANDS_REQUIRE(steel != kNoEntity);
    CHECK(f.hand(steel, CellPos{}).empty());
    CHECK(f.hand(kNoEntity, CellPos{11, 11, 0}, HandsOnMode::PutDown).empty());
    CHECK(!f.carries());
    CHECK(f.sim.things().itemsAt(CellPos{11, 11, 0}).size() == 1);

    const Entity again = f.sim.things().itemsAt(CellPos{11, 11, 0}).front();
    CHECK(f.hand(again, CellPos{}).empty());
    CHECK(f.hand(kNoEntity, CellPos{40, 40, 0}, HandsOnMode::PutDown).empty());  // out of reach: near the feet
    CHECK(!f.carries());
    CHECK(f.itemsOnMap("Steel") == 10);
    CHECK(f.hand(kNoEntity, CellPos{11, 11, 0}, HandsOnMode::PutDown) == "you carry nothing");

    // Taking something up and then being let go puts it down on the ground.
    const Entity pick = f.spawn("Steel", CellPos{11, 10, 0}, 4);
    HANDS_REQUIRE(pick != kNoEntity);
    CHECK(f.hand(pick, CellPos{}).empty());
    CHECK(f.carries());
    f.sim.submit(ControlPawnCommand{kNoEntity});
    f.sim.step();
    CHECK(!f.carries());
    CHECK(f.itemsOnMap("Steel") == 14);
    CHECK(!f.sim.things().registry().all_of<PlayerControl>(f.pawn));
}

TEST_CASE("Hands-on: only the controlled pawn can use its hands, and the plan names what a look would do") {
    HandFixture f;
    const CellPos rock{12, 10, 0};
    f.sim.mineables().place(rock, f.def("Rock_Sandstone"));
    const HandsOnPlan plan = f.sim.jobs().planHandsOn(f.pawn, kNoEntity, rock);
    CHECK(plan.kind == HandsOnKind::Mine);
    CHECK(plan.label == "mine sandstone");
    CHECK(plan.inReach);
    CHECK(plan.ok());
    CHECK(!f.sim.jobs().planHandsOn(f.pawn, kNoEntity, CellPos{30, 10, 0}).inReach);

    const Entity other = f.sim.things().spawn("Human", CellPos{20, 20, 0});
    f.sim.things().registry().emplace<Colonist>(other);
    f.sim.jobs().addWorker(other, {}, false);
    const uint64_t id = f.sim.submit(HandsOnCommand{other, HandsOnMode::Work, kNoEntity, rock});
    f.sim.step();
    bool failed = false;
    for (const Event& e : f.sim.events()) {
        failed = failed || (e.type == EventType::CommandFailed && e.command == id);
    }
    CHECK(failed);
    CHECK(f.sim.jobs().planHandsOn(other, kNoEntity, rock).error == "you aren't controlling a colonist");
}

TEST_CASE("Hands-on: a hands-on job survives a save and load, and the game goes on the same") {
    HandFixture f;
    const CellPos rock{12, 10, 0};
    f.sim.mineables().place(rock, f.def("Rock_Sandstone"));
    CHECK(f.work(rock).empty());
    for (int i = 0; i < 60; ++i) {
        f.sim.step();
    }
    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, handDefs());
    HANDS_REQUIRE(loaded.error.empty());
    HANDS_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(copy.things().registry().get<Worker>(f.pawn).job.handsOn == 2);
    CHECK(saveSim(copy) == bytes);
    CHECK(saveToJson(copy).find("\"handsOn\"") != std::string::npos);
    for (int i = 0; i < 400; ++i) {
        f.sim.step();
        copy.step();
    }
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(saveSim(copy) == saveSim(f.sim));
}

TEST_CASE("Hands-on: a replay with hands-on commands plays back to the same hashes") {
    const auto makeSim = [] {
        auto sim = std::make_unique<Sim>(handSettings(), handDefs());
        const Entity pawn = handColonist(*sim, CellPos{10, 10, 0});
        return std::make_pair(std::move(sim), pawn);
    };
    auto made = makeSim();
    std::unique_ptr<Sim>& original = made.first;
    const Entity pawn = made.second;
    original->submit(ControlPawnCommand{pawn});
    original->submit(SpawnThingCommand{"Steel", CellPos{11, 10, 0}, 10});
    original->submit(PlaceBlueprintCommand{"Wall", "Steel", CellPos{12, 10, 0}, Rot4::North});
    original->step();
    Entity steel = kNoEntity;
    for (const Event& e : original->events()) {
        if (e.type == EventType::ThingSpawned && e.cell == CellPos{11, 10, 0}) {
            steel = e.thing;
        }
    }
    const Entity plan = original->construction().siteAt(CellPos{12, 10, 0});
    HANDS_REQUIRE(steel != kNoEntity);
    HANDS_REQUIRE(plan != kNoEntity);
    original->submit(HandsOnCommand{pawn, HandsOnMode::Work, steel, CellPos{}});
    original->step();
    original->submit(HandsOnCommand{pawn, HandsOnMode::Work, plan, CellPos{}});
    original->step();
    original->submit(HandsOnCommand{pawn, HandsOnMode::Work, plan, CellPos{}});
    for (int i = 0; i < 250; ++i) {
        original->step();
    }
    original->submit(HandsOnCommand{pawn, HandsOnMode::Stop, kNoEntity, CellPos{}});
    original->submit(HandsOnCommand{pawn, HandsOnMode::PutDown, kNoEntity, CellPos{11, 11, 0}});
    for (int i = 0; i < 50; ++i) {
        original->step();
    }

    const Replay recorded = recordReplay(*original);
    std::string text = writeReplay(recorded);
    CHECK(text.find("\"HandsOn\"") != std::string::npos);
    Replay reread;
    std::string error;
    HANDS_REQUIRE(readReplay(text, reread, error));
    CHECK(writeReplay(reread) == text);

    auto remade = makeSim();
    CHECK(remade.second == pawn);
    playReplay(reread, *remade.first, reread.endStep);
    CHECK(remade.first->stateHash() == original->stateHash());
    const ReplayCheck check = checkReplay(reread, *remade.first);
    CHECK(check.compared >= 2);
    CHECK(check.matched());
}
