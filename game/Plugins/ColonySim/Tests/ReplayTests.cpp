#include "ColonySim/Replay.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Version.h"

#include "TestDefs.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace colony;

namespace {

// A session as Unreal would drive it: haulers at work while commands arrive at odd steps,
// including ones that fail and ones that name things by the id the sim gave them.
void replayTestSession(Sim& sim, int64_t steps) {
    for (int64_t i = 0; i < steps; ++i) {
        if (i == 7) {
            sim.submit(SpawnThingCommand{"Wall", CellPos{10, 10, 0}});
            sim.submit(SpawnThingCommand{"Wall", CellPos{11, 10, 0}});
            sim.submit(SpawnThingCommand{"Wall", CellPos{10, 10, 0}});  // fails: occupied
        }
        if (i == 500) {
            sim.submit(SpawnThingCommand{"Steel", CellPos{60, 70, 0}, 30});
            sim.submit(SpawnThingCommand{"Bed", CellPos{20, 30, 0}, 1, Rot4::West});
        }
        if (i == 2400) {
            sim.submit(DestroyThingCommand{sim.map().buildingAt(CellPos{11, 10, 0})});
        }
        sim.step();
    }
}

}  // namespace

TEST_CASE("Replay: playing a recorded session gives the same state, checkpoint for checkpoint") {
    const auto defs = testDefs();
    SimSettings settings{31, 128, 6, 60, true};
    settings.checkpointEvery = 600;
    settings.pathBudget = 150;  // small, so path searches are still half done at checkpoints
    Sim original(settings, defs);
    replayTestSession(original, 5000);
    const Replay recorded = recordReplay(original);
    CHECK(recorded.commands.size() == 6);
    CHECK(recorded.checkpoints.size() == 8);
    CHECK(recorded.endStep == 5000);
    CHECK(recorded.simVersion == kSimVersion);
    CHECK(recorded.defsHash == defs->contentHash());

    // Through text, as a file would go.
    Replay loaded;
    std::string error;
    CHECK(readReplay(writeReplay(recorded), loaded, error));
    CHECK(error.empty());
    CHECK(loaded.commands.size() == recorded.commands.size());
    CHECK(loaded.endHash == recorded.endHash);
    CHECK(loaded.settings.pathBudget == 150);
    CHECK(writeReplay(loaded) == writeReplay(recorded));

    Sim replayed(loaded.settings, defs);
    playReplay(loaded, replayed, loaded.endStep);
    CHECK(replayed.stateHash() == original.stateHash());
    CHECK(replayed.stats().hauls == original.stats().hauls);
    const ReplayCheck check = checkReplay(loaded, replayed);
    CHECK(check.compared == 9);  // 8 checkpoints and the end
    CHECK(check.matched());
}

TEST_CASE("Replay: a run that went differently is caught at the first checkpoint after it") {
    const auto defs = testDefs();
    SimSettings settings{31, 128, 6, 60, true};
    settings.checkpointEvery = 600;
    Sim original(settings, defs);
    replayTestSession(original, 3000);
    Replay recorded = recordReplay(original);

    // Move the step-500 commands to step 1300: the first checkpoint after 500 differs.
    for (LoggedCommand& logged : recorded.commands) {
        if (logged.step == 500) {
            logged.step = 1300;
        }
    }
    Sim replayed(recorded.settings, defs);
    playReplay(recorded, replayed, recorded.endStep);
    const ReplayCheck check = checkReplay(recorded, replayed);
    CHECK_FALSE(check.matched());
    CHECK(check.firstMismatch == 600);
    CHECK(check.compared == 1);
}

TEST_CASE("Replay: a short scenario file with defaults and no hashes plays and records") {
    const std::string scenario = R"({
        "format": "rim3d-replay", "version": 1,
        "settings": {"seed": 5, "mapSize": 48, "flatMap": true},
        "endStep": 30,
        "commands": [
            {"step": 0, "type": "SpawnThing", "def": "Wall", "cell": [3, 4, 0]},
            {"step": 10, "type": "SpawnThing", "def": "Bed", "cell": [8, 8, 0], "rot": "South"},
            {"step": 20, "type": "DestroyThing", "thing": 0}
        ]
    })";
    Replay replay;
    std::string error;
    if (!readReplay(scenario, replay, error)) {
        FAIL_CHECK(error);
        return;
    }
    CHECK(replay.settings.mapSize == 48);
    CHECK(replay.settings.pawns == 0);
    CHECK(replay.settings.pawnDef == "Human");
    CHECK(replay.settings.checkpointEvery == kSecondsPerHour);
    CHECK(replay.settings.pathBudget == Pathfinder::kDefaultBudget);
    CHECK_FALSE(replay.hasEndHash);
    CHECK(replay.checkpoints.empty());
    CHECK(std::get<SpawnThingCommand>(replay.commands[0].command).count == 1);
    CHECK(std::get<SpawnThingCommand>(replay.commands[1].command).rot == Rot4::South);

    Sim sim(replay.settings, testDefs());
    playReplay(replay, sim, replay.endStep);
    CHECK(sim.time().seconds == 30);
    CHECK(sim.things().count() == 1);  // thing 0 is the wall, destroyed at step 20
    CHECK(sim.map().buildingAt(CellPos{8, 8, 0}) != kNoEntity);
    CHECK(checkReplay(replay, sim).compared == 0);
}

TEST_CASE("Replay: broken files are refused with a reason") {
    const auto refused = [](const std::string& text) {
        Replay replay;
        std::string error;
        const bool ok = readReplay(text, replay, error);
        CHECK_FALSE(error.empty());
        return !ok;
    };
    const std::string head = R"("format": "rim3d-replay", "version": 1, "settings": {})";
    CHECK(refused("not json"));
    CHECK(refused("[]"));
    CHECK(refused(R"({"format": "something", "version": 1, "settings": {}})"));
    CHECK(refused(R"({"format": "rim3d-replay", "version": 2, "settings": {}})"));
    CHECK(refused(R"({"format": "rim3d-replay", "version": 1})"));
    CHECK(refused("{" + head + R"(, "endHash": "xyz"})"));
    CHECK(refused(R"({"format": "rim3d-replay", "version": 1, "settings": {"seed": -1}})"));
    CHECK(refused(R"({"format": "rim3d-replay", "version": 1, "settings": {"mapSize": "big"}})"));
    CHECK(refused("{" + head + R"(, "commands": [{"step": 1, "type": "Teleport"}]})"));
    CHECK(refused("{" + head + R"(, "commands": [{"step": 1, "type": "SpawnThing", "def": "Wall", "cell": [1, 2]}]})"));
    CHECK(refused("{" + head + R"(, "commands": [{"step": 1, "type": "SpawnThing", "def": "Wall", "cell": [1, 2, 0], "rot": "Up"}]})"));
    CHECK(refused("{" + head + R"(, "commands": [{"step": 5, "type": "DestroyThing", "thing": 1}, {"step": 4, "type": "DestroyThing", "thing": 1}]})"));
    CHECK(refused("{" + head + R"(, "checkpoints": [{"step": 5}]})"));
    CHECK(refused("{" + head + R"(, "checkpoints": [{"step": 5, "hash": "01"}, {"step": 5, "hash": "02"}]})"));

    Replay fine;
    std::string error;
    CHECK(readReplay("{" + head + "}", fine, error));
}
