#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define CONTROL_REQUIRE(...)   \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// A colonist as the scenario makes one: a pawn with a colonist record that works.
Entity controlColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.things().registry().emplace<Colonist>(pawn);
    sim.jobs().addWorker(pawn);
    return pawn;
}

// A flat 32 m map with test defs, no hauling test, and one colonist at (10.5, 10.5).
struct ControlFixture {
    Sim sim{SimSettings{3, 32, 0, 0, true}, testDefs()};
    Entity colonist = kNoEntity;

    ControlFixture() { colonist = controlColonist(sim, CellPos{10, 10, 0}); }

    bool failed(uint64_t command) const {
        for (const Event& e : sim.events()) {
            if (e.type == EventType::CommandFailed && e.command == command) {
                return true;
            }
        }
        return false;
    }
    bool controlled(Entity pawn) const { return sim.things().registry().all_of<PlayerControl>(pawn); }
    const Position& position(Entity pawn) const { return sim.things().registry().get<Position>(pawn); }
    const Mover& mover(Entity pawn) const { return sim.things().registry().get<Mover>(pawn); }
};

// Steps until the colonist carries something (a haul in progress), at most 3000 steps.
bool jobsRunUntilCarrying(Sim& sim, Entity pawn) {
    for (int i = 0; i < 3000; ++i) {
        if (sim.things().registry().all_of<Carrying>(pawn)) {
            return true;
        }
        sim.step();
    }
    return false;
}

// Steps until the colonist has a job that isn't waiting, at most 3000 steps.
bool jobsRunUntilWorking(Sim& sim, Entity pawn) {
    for (int i = 0; i < 3000; ++i) {
        sim.step();
        if (sim.things().registry().get<Worker>(pawn).job.type == JobType::HaulToCell) {
            return true;
        }
    }
    return false;
}

// The same walk in every test that needs one: a wiggly line east, then north, in 0.2 m strides.
void controlWalk(Sim& sim, Entity pawn, int64_t from, int64_t to) {
    for (int64_t i = from; i < to; ++i) {
        if (i == from) {
            sim.submit(ControlPawnCommand{pawn});
        }
        const float t = static_cast<float>(i - from);
        sim.submit(DirectMoveCommand{pawn, 10.5f + 0.2f * t, 10.5f + 3.0f * std::sin(t * 0.05f)});
        sim.step();
    }
}

}  // namespace

TEST_CASE("Control: only a colonist on the map can be taken over, and taking another lets the first go") {
    ControlFixture f;
    const Entity second = controlColonist(f.sim, CellPos{14, 14, 0});
    const Entity item = f.sim.things().spawn("Steel", CellPos{20, 20, 0});
    const Entity plain = f.sim.things().spawn("Human", CellPos{16, 16, 0});  // a pawn, not a colonist

    const uint64_t onItem = f.sim.submit(ControlPawnCommand{item});
    const uint64_t onPlain = f.sim.submit(ControlPawnCommand{plain});
    const uint64_t onNothing = f.sim.submit(ControlPawnCommand{static_cast<Entity>(12345)});
    f.sim.step();
    CHECK(f.failed(onItem));
    CHECK(f.failed(onPlain));
    CHECK(f.failed(onNothing));
    CHECK_FALSE(f.controlled(f.colonist));

    const uint64_t ok = f.sim.submit(ControlPawnCommand{f.colonist});
    f.sim.step();
    CHECK_FALSE(f.failed(ok));
    CHECK(f.controlled(f.colonist));

    // Taking the second one lets the first go.
    f.sim.submit(ControlPawnCommand{second});
    f.sim.step();
    CHECK_FALSE(f.controlled(f.colonist));
    CHECK(f.controlled(second));

    // kNoEntity lets go of whoever is controlled; it never fails.
    const uint64_t letGo = f.sim.submit(ControlPawnCommand{kNoEntity});
    f.sim.step();
    CHECK_FALSE(f.failed(letGo));
    CHECK_FALSE(f.controlled(second));
}

TEST_CASE("Control: taking a colonist over ends its job, the job system leaves it alone, and letting go gives it work again") {
    ControlFixture f;
    // A stockpile and an item to haul, so the colonist has work to be interrupted in.
    f.sim.submit(CreateStockpileCommand{CellRect{2, 2, 3, 3, 0}});
    f.sim.submit(SpawnThingCommand{"Steel", CellPos{20, 20, 0}, 10});
    CONTROL_REQUIRE(jobsRunUntilCarrying(f.sim, f.colonist));
    CONTROL_REQUIRE(f.sim.things().registry().get<Worker>(f.colonist).job.type == JobType::HaulToCell);

    // Taking over ends the job (what it carried is put down) and starts no other.
    f.sim.submit(ControlPawnCommand{f.colonist});
    f.sim.step();
    bool ended = false;
    for (const Event& e : f.sim.events()) {
        ended = ended || (e.type == EventType::JobEnded && e.thing == f.colonist && e.message.find("taken over") != std::string::npos);
    }
    CHECK(ended);
    const float heldX = f.position(f.colonist).x;
    for (int i = 0; i < 300; ++i) {
        f.sim.step();
        CONTROL_REQUIRE(f.sim.things().registry().get<Worker>(f.colonist).job.type == JobType::None);
    }
    CHECK(f.position(f.colonist).x == doctest::Approx(heldX));
    CHECK_FALSE(f.sim.things().registry().all_of<Carrying>(f.colonist));

    // An order is refused while the player has the pawn.
    CHECK(f.sim.things().registry().view<Thing, StackCount>().size_hint() > 0);
    Entity steel = kNoEntity;
    for (const auto [e, thing, stack] : f.sim.things().registry().view<const Thing, const StackCount>().each()) {
        steel = e;
    }
    std::string error;
    CHECK_FALSE(f.sim.jobs().orderHaul(f.colonist, steel, error));
    CHECK(error.find("controlling") != std::string::npos);

    // Letting go: the colonist thinks again and takes work.
    f.sim.submit(ControlPawnCommand{kNoEntity});
    CHECK(jobsRunUntilWorking(f.sim, f.colonist));
}

TEST_CASE("Control: DirectMove moves the pawn, at most kDirectMaxStep a step, and sets its velocity") {
    ControlFixture f;
    f.sim.submit(ControlPawnCommand{f.colonist});
    f.sim.step();

    const uint64_t near = f.sim.submit(DirectMoveCommand{f.colonist, 10.5f + 0.2f, 10.5f});
    f.sim.step();
    CHECK_FALSE(f.failed(near));
    CHECK(f.position(f.colonist).x == doctest::Approx(10.7f));
    CHECK(f.position(f.colonist).y == doctest::Approx(10.5f));
    CHECK(f.mover(f.colonist).vx == doctest::Approx(0.2f));
    CHECK(f.mover(f.colonist).vy == doctest::Approx(0.0f));

    // A far target is cut to one step's worth, in the direction of the target.
    f.sim.submit(DirectMoveCommand{f.colonist, 10.7f, 20.5f});
    f.sim.step();
    CHECK(f.position(f.colonist).x == doctest::Approx(10.7f));
    CHECK(f.position(f.colonist).y == doctest::Approx(10.5f + kDirectMaxStep));
    CHECK(f.mover(f.colonist).vy == doctest::Approx(kDirectMaxStep));

    // Keeps going while the target stays far away.
    f.sim.submit(DirectMoveCommand{f.colonist, 10.7f, 20.5f});
    f.sim.step();
    CHECK(f.position(f.colonist).y == doctest::Approx(10.5f + 2.0f * kDirectMaxStep));

    // No command this step: the pawn stands still and has no velocity.
    f.sim.step();
    CHECK(f.position(f.colonist).y == doctest::Approx(10.5f + 2.0f * kDirectMaxStep));
    CHECK(f.mover(f.colonist).vx == 0.0f);
    CHECK(f.mover(f.colonist).vy == 0.0f);

    // It shows in the snapshot, which is what Unreal animates from.
    f.sim.submit(DirectMoveCommand{f.colonist, 10.7f, 11.9f});
    f.sim.step();
    Snapshot snapshot;
    f.sim.writeSnapshot(snapshot);
    const ThingView* view = snapshot.find(f.colonist);
    CONTROL_REQUIRE(view != nullptr);
    CHECK(view->vy > 0.0f);
    CHECK(view->waypointCount == 0);
}

TEST_CASE("Control: walls stop the pawn, or let it slide along them") {
    ControlFixture f;
    for (int y = 0; y < 32; ++y) {
        f.sim.submit(SpawnThingCommand{"Wall", CellPos{14, y, 0}});
    }
    f.sim.submit(ControlPawnCommand{f.colonist});
    f.sim.step();
    f.sim.submit(DirectMoveCommand{f.colonist, 10.5f, 10.5f});
    f.sim.step();

    // Walk east into the wall: stops a pawn radius short of cell 14, never inside it.
    for (int i = 0; i < 40; ++i) {
        f.sim.submit(DirectMoveCommand{f.colonist, f.position(f.colonist).x + 0.3f, 10.5f});
        f.sim.step();
    }
    CHECK(f.position(f.colonist).x < 14.0f - kPawnRadius + 0.001f);
    CHECK(f.position(f.colonist).x > 13.0f);
    const float against = f.position(f.colonist).x;

    // Diagonally into it: it slides north along the wall and stays where it was in x.
    for (int i = 0; i < 10; ++i) {
        f.sim.submit(DirectMoveCommand{f.colonist, against + 0.2f, f.position(f.colonist).y + 0.2f});
        f.sim.step();
    }
    CHECK(f.position(f.colonist).x == doctest::Approx(against));
    CHECK(f.position(f.colonist).y > 11.5f);
    CHECK(f.mover(f.colonist).vy > 0.0f);
}

TEST_CASE("Control: DirectMove needs the controlled pawn and a real position") {
    ControlFixture f;
    const uint64_t notControlled = f.sim.submit(DirectMoveCommand{f.colonist, 11.0f, 11.0f});
    f.sim.step();
    CHECK(f.failed(notControlled));
    CHECK(f.position(f.colonist).x == doctest::Approx(10.5f));

    f.sim.submit(ControlPawnCommand{f.colonist});
    const uint64_t nan = f.sim.submit(DirectMoveCommand{f.colonist, std::numeric_limits<float>::quiet_NaN(), 11.0f});
    const uint64_t inf = f.sim.submit(DirectMoveCommand{f.colonist, 11.0f, std::numeric_limits<float>::infinity()});
    const uint64_t gone = f.sim.submit(DirectMoveCommand{static_cast<Entity>(777), 11.0f, 11.0f});
    f.sim.step();
    CHECK(f.failed(nan));
    CHECK(f.failed(inf));
    CHECK(f.failed(gone));
    CHECK(f.position(f.colonist).x == doctest::Approx(10.5f));

    // Let go: moves are refused again.
    f.sim.submit(ControlPawnCommand{kNoEntity});
    f.sim.step();
    const uint64_t afterLetGo = f.sim.submit(DirectMoveCommand{f.colonist, 11.0f, 11.0f});
    f.sim.step();
    CHECK(f.failed(afterLetGo));
}

TEST_CASE("Control: the movement system leaves the controlled pawn alone and others walk around it") {
    ControlFixture f;
    // A pawn walking along y = 10.5 from the west, past the controlled one standing still.
    const Entity walker = f.sim.things().spawn("Human", CellPos{2, 10, 0});
    f.sim.submit(ControlPawnCommand{f.colonist});
    f.sim.step();
    Mover& mover = f.sim.things().registry().get<Mover>(walker);
    mover.walkTo(2.5f, 10.5f, {CellPos{20, 10, 0}});
    for (int i = 0; i < 400; ++i) {
        f.sim.step();
    }
    CHECK(f.position(f.colonist).x == doctest::Approx(10.5f));
    CHECK(f.position(f.colonist).y == doctest::Approx(10.5f));
    CHECK(f.position(walker).x > 15.0f);  // got past without being stuck behind it
}

TEST_CASE("Control: a controlled pawn survives a save and load, and the game goes on the same") {
    const auto defs = testDefs();
    ControlFixture f;
    controlWalk(f.sim, f.colonist, 0, 150);
    CHECK(f.controlled(f.colonist));
    // Commands still queued at the moment of the save apply after loading.
    f.sim.submit(DirectMoveCommand{f.colonist, 20.0f, 12.0f});

    const std::vector<uint8_t> bytes = saveSim(f.sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    CONTROL_REQUIRE(loaded.error.empty());
    CONTROL_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(copy.things().registry().all_of<PlayerControl>(f.colonist));
    CHECK(saveSim(copy) == bytes);
    const std::string json = saveToJson(copy);
    CHECK(json.find("\"playerControlled\"") != std::string::npos);
    CHECK(json.find("\"DirectMove\"") != std::string::npos);

    for (int64_t i = 150; i < 400; ++i) {
        for (Sim* sim : {&f.sim, &copy}) {
            sim->submit(DirectMoveCommand{f.colonist, 12.0f + 0.01f * static_cast<float>(i - 150), 14.0f});
            sim->step();
        }
    }
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(saveSim(copy) == saveSim(f.sim));
}

TEST_CASE("Control: being controlled is part of the state hash") {
    ControlFixture a;
    ControlFixture b;
    a.sim.step();
    b.sim.step();
    CHECK(a.sim.stateHash() == b.sim.stateHash());
    b.sim.submit(ControlPawnCommand{b.colonist});
    b.sim.step();
    a.sim.step();
    CHECK(a.sim.stateHash() != b.sim.stateHash());
}

TEST_CASE("Control: a save with two controlled pawns, or one that isn't a colonist, is refused") {
    const auto defs = testDefs();
    ControlFixture f;
    const Entity second = controlColonist(f.sim, CellPos{14, 14, 0});
    f.sim.submit(ControlPawnCommand{f.colonist});
    f.sim.step();
    // Break the rules from outside, as a damaged file would.
    f.sim.things().registry().emplace<PlayerControl>(second);
    SaveLoadResult two = loadSim(saveSim(f.sim), defs);
    CHECK_FALSE(two.error.empty());
    CHECK(two.sim == nullptr);

    f.sim.things().registry().remove<PlayerControl>(second);
    f.sim.things().registry().remove<Colonist>(f.colonist);
    SaveLoadResult notColonist = loadSim(saveSim(f.sim), defs);
    CHECK_FALSE(notColonist.error.empty());
}

TEST_CASE("Control: a replay with control and direct moves plays back to the same hashes") {
    const auto defs = testDefs();
    SimSettings settings{3, 32, 0, 0, true};
    settings.checkpointEvery = 100;
    settings.scenario = "";
    // The replay can't make a colonist from test defs, so the colonist is made by hand in both
    // sims the same way (a hand-made entity is the first id handed out in a fresh sim).
    const auto makeSim = [&] {
        auto sim = std::make_unique<Sim>(settings, defs);
        const Entity colonist = controlColonist(*sim, CellPos{10, 10, 0});
        return std::make_pair(std::move(sim), colonist);
    };
    auto [original, colonist] = makeSim();
    controlWalk(*original, colonist, 0, 350);

    const Replay recorded = recordReplay(*original);
    CHECK(recorded.commands.size() == 351);
    std::string text = writeReplay(recorded);
    CHECK(text.find("\"ControlPawn\"") != std::string::npos);
    CHECK(text.find("\"DirectMove\"") != std::string::npos);
    Replay reread;
    std::string error;
    CONTROL_REQUIRE(readReplay(text, reread, error));
    CHECK(writeReplay(reread) == text);  // floats come back exactly

    auto [replayed, colonist2] = makeSim();
    CHECK(colonist2 == colonist);
    playReplay(reread, *replayed, reread.endStep);
    CHECK(replayed->stateHash() == original->stateHash());
    const ReplayCheck check = checkReplay(reread, *replayed);
    CHECK(check.compared >= 4);
    CHECK(check.matched());
}

TEST_CASE("Control: replay files with a bad control or move command are refused") {
    const std::string head = R"("format": "rim3d-replay", "version": 1, "settings": {"seed": 1, "mapSize": 32})";
    Replay replay;
    std::string error;
    CHECK_FALSE(readReplay("{" + head + R"(, "commands": [{"step": 0, "type": "DirectMove", "pawn": 0, "x": "a", "y": 1}]})", replay, error));
    CHECK_FALSE(readReplay("{" + head + R"(, "commands": [{"step": 0, "type": "DirectMove", "pawn": 0, "x": 1}]})", replay, error));
    CHECK_FALSE(readReplay("{" + head + R"(, "commands": [{"step": 0, "type": "ControlPawn", "pawn": "no"}]})", replay, error));
    CHECK(readReplay("{" + head + R"(, "commands": [{"step": 0, "type": "ControlPawn", "pawn": 0}, {"step": 1, "type": "DirectMove", "pawn": 0, "x": 1.5, "y": 2}]})", replay, error));
    CHECK(replay.commands.size() == 2);
}

TEST_CASE("Control: a real Crashlanded start can be controlled by the protagonist") {
    auto defs = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    CONTROL_REQUIRE(DefDatabase::loadDirectory(COLONY_DATA_DIR, *defs, errors));
    SimSettings settings;
    settings.seed = 9;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    Sim sim(settings, defs);
    CONTROL_REQUIRE(sim.setupErrors().empty());

    Entity protagonist = kNoEntity;
    for (const auto [e, colonist] : sim.things().registry().view<const Colonist>().each()) {
        if (colonist.protagonist) {
            protagonist = e;
        }
    }
    CONTROL_REQUIRE(protagonist != kNoEntity);
    const Position start = sim.things().registry().get<Position>(protagonist);
    sim.submit(ControlPawnCommand{protagonist});
    sim.step();
    for (int i = 0; i < 20; ++i) {
        sim.submit(DirectMoveCommand{protagonist, start.x + 0.1f * static_cast<float>(i + 1), start.y});
        sim.step();
    }
    CHECK(sim.things().registry().get<Position>(protagonist).x == doctest::Approx(start.x + 2.0f));
    CHECK(sim.commandLog().size() == 21);
}
