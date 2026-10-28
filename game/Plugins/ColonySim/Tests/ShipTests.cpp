#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Research.h"
#include "ColonySim/Save.h"
#include "ColonySim/Ship.h"
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
#define SHIP_REQUIRE(...)      \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// The game's real data: the numbers in docs/design/ship.md are what is tested.
std::shared_ptr<const DefDatabase> shipDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Snapshot shipSnap(const Sim& sim) {
    Snapshot snap;
    sim.writeSnapshot(snap);
    return snap;
}

struct ShipYard {
    Sim sim{SimSettings{7, 64, 0, 0, true}, shipDefs()};
    Entity reactor = kNoEntity;
    Entity casket = kNoEntity;
    Entity pawn = kNoEntity;
    int engines = 0;

    // A whole ship on flat ground: reactor, three engines, core, sensor cluster, a beam with a casket against it.
    explicit ShipYard(int enginesBuilt = 3, bool casketTouchesBeam = true) {
        Things& things = sim.things();
        reactor = things.spawn("ShipReactor", CellPos{5, 5, 0});
        for (int i = 0; i < enginesBuilt; ++i) {
            if (things.spawn("ShipEngine", CellPos{15 + 4 * i, 5, 0}) != kNoEntity) {
                ++engines;
            }
        }
        things.spawn("ShipComputerCore", CellPos{5, 15, 0});
        things.spawn("ShipSensorCluster", CellPos{9, 15, 0});
        things.spawn("ShipStructuralBeam", CellPos{30, 5, 0});
        casket = things.spawn("ShipCryptosleepCasket", CellPos{casketTouchesBeam ? 32 : 36, 5, 0});
        pawn = things.spawn("Human", CellPos{40, 40, 0});
    }

    // Skips the reactor's 15 days.
    void warmUp() {
        sim.ship().state().startedAt -= 16 * kSecondsPerDay;
    }
    bool sawEvent(EventType type) const {
        return std::any_of(sim.events().begin(), sim.events().end(), [&](const Event& e) { return e.type == type; });
    }
    std::string lastFailure() const {
        for (auto it = sim.events().rbegin(); it != sim.events().rend(); ++it) {
            if (it->type == EventType::CommandFailed) {
                return it->message;
            }
        }
        return std::string();
    }
};

}  // namespace

TEST_CASE("Ship: the parts are in the data and the research chain gates them") {
    const DefDatabase& defs = *shipDefs();
    Sim sim(SimSettings{7, 64, 0, 0, true}, shipDefs());
    for (const char* name : {"ShipStructuralBeam", "ShipCryptosleepCasket", "ShipReactor", "ShipEngine", "ShipComputerCore", "ShipSensorCluster"}) {
        const Def* def = defs.find("Things", name);
        SHIP_REQUIRE(def != nullptr);
        CHECK(sim.ship().partOf(defs.idOf("Things", name)) != ShipPart::None);
        std::string error;
        CHECK_FALSE(sim.research().unlocked(*def, error));
    }
    const Def* reactor = defs.find("Things", "ShipReactor");
    SHIP_REQUIRE(reactor != nullptr);
    CHECK(reactor->integer("size.0") == 6);
    CHECK(reactor->integer("size.1") == 7);
    CHECK(reactor->integer("costs.Steel") == 350);
    CHECK(reactor->integer("costs.Plasteel") == 280);
    CHECK(reactor->integer("costs.Uranium") == 70);
    CHECK(reactor->integer("costs.ComponentSpacer") == 8);
    // 65,000 ticks at 1.44 game seconds a tick.
    CHECK(reactor->number("stats.workToBuild") == doctest::Approx(93600.0));
    sim.submit(UnlockResearchCommand{"StarshipReactor"});
    sim.step();
    std::string error;
    CHECK(sim.research().unlocked(*reactor, error));
    CHECK(sim.ship().rules().activationDays == doctest::Approx(15.0));
    CHECK(sim.ship().rules().enginesNeeded == 3);
}

TEST_CASE("Ship: a ship is counted from its finished parts and the launch problems are named") {
    ShipYard yard(2);
    SHIP_REQUIRE(yard.reactor != kNoEntity);
    SHIP_REQUIRE(yard.casket != kNoEntity);
    const ShipParts parts = yard.sim.ship().parts();
    CHECK(parts.reactors == 1);
    CHECK(parts.engines == 2);
    CHECK(parts.cores == 1);
    CHECK(parts.sensors == 1);
    CHECK(parts.beams == 1);
    CHECK(parts.caskets == 1);
    CHECK(parts.unconnectedCaskets == 0);
    CHECK(yard.sim.ship().launchProblem().find("reactor") != std::string::npos);
    yard.sim.submit(ShipCommand{"Launch", kNoEntity, kNoEntity});
    yard.sim.step();
    CHECK(yard.lastFailure().find("reactor") != std::string::npos);
    CHECK_FALSE(yard.sim.ship().won());
}

TEST_CASE("Ship: the reactor takes 15 days, sends waves of raiders and then the ship can launch") {
    ShipYard yard;
    SHIP_REQUIRE(yard.reactor != kNoEntity);
    Sim& sim = yard.sim;
    sim.submit(ShipCommand{"StartReactor", yard.pawn, kNoEntity});
    sim.step();
    CHECK(yard.lastFailure().find("reactor") != std::string::npos);
    sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
    sim.step();
    CHECK(yard.sawEvent(EventType::ShipReactorStarted));
    CHECK(sim.ship().state().phase == ShipPhase::Activating);
    CHECK(shipSnap(sim).ship.activationLeft > 14 * kSecondsPerDay);
    sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
    sim.step();
    CHECK(yard.lastFailure().find("already") != std::string::npos);

    // The first wave comes after the first wait, then one every 36 hours.
    CHECK(sim.ship().state().waves == 0);
    sim.ship().state().nextWave = sim.time().seconds;
    sim.step();
    CHECK(yard.sawEvent(EventType::ShipWave));
    CHECK(sim.ship().state().waves == 1);
    CHECK(sim.raids().state().squads.size() >= 1);
    CHECK(sim.ship().state().nextWave >= sim.time().seconds + 36 * kSecondsPerHour - 1);

    // Not active yet: no launch.
    CHECK(sim.ship().launchProblem().find("not active") != std::string::npos);
    yard.warmUp();
    sim.step();
    CHECK(sim.ship().state().phase == ShipPhase::Ready);
    CHECK(yard.sawEvent(EventType::ShipReactorReady));
    const int wavesAtReady = sim.ship().state().waves;
    sim.ship().state().nextWave = sim.time().seconds;
    sim.stepMany(5);
    CHECK(sim.ship().state().waves == wavesAtReady);  // the raids stop once it is active

    // No one is in a casket yet.
    CHECK(sim.ship().launchProblem().find("casket") != std::string::npos);
    sim.submit(ShipCommand{"Board", yard.casket, yard.pawn});
    sim.step();
    CHECK(sim.ship().launchProblem().empty());
    CHECK(shipSnap(sim).ship.canLaunch);
    sim.submit(ShipCommand{"Launch", kNoEntity, kNoEntity});
    sim.step();
    CHECK(sim.ship().won());
    CHECK(shipSnap(sim).ship.won);
    CHECK(shipSnap(sim).ship.escaped == 1);
    CHECK(yard.sawEvent(EventType::ShipLaunched));
    CHECK_FALSE(sim.things().exists(yard.pawn));
}

TEST_CASE("Ship: a launch needs three engines and a casket that touches a beam") {
    {
        ShipYard yard(2);
        yard.sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
        yard.sim.step();
        yard.warmUp();
        yard.sim.submit(ShipCommand{"Board", yard.casket, yard.pawn});
        yard.sim.step();
        CHECK(yard.sim.ship().state().phase == ShipPhase::Ready);
        CHECK(yard.sim.ship().launchProblem().find("engines") != std::string::npos);
        yard.sim.submit(ShipCommand{"Launch", kNoEntity, kNoEntity});
        yard.sim.step();
        CHECK_FALSE(yard.sim.ship().won());
    }
    {
        ShipYard yard(3, false);
        yard.sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
        yard.sim.step();
        yard.warmUp();
        yard.sim.submit(ShipCommand{"Board", yard.casket, yard.pawn});
        yard.sim.step();
        CHECK(yard.sim.ship().parts().unconnectedCaskets == 1);
        CHECK(yard.sim.ship().launchProblem().find("beam") != std::string::npos);
    }
}

TEST_CASE("Ship: losing the reactor loses the activation, and a rider who dies leaves the casket") {
    ShipYard yard;
    Sim& sim = yard.sim;
    sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
    sim.submit(ShipCommand{"Board", yard.casket, yard.pawn});
    sim.step();
    CHECK(sim.ship().state().phase == ShipPhase::Activating);
    CHECK(sim.ship().state().riders.size() == 1);
    sim.things().destroy(yard.reactor);
    sim.step();
    CHECK(sim.ship().state().phase == ShipPhase::Idle);
    CHECK(yard.sawEvent(EventType::ShipReactorLost));
    sim.submit(ShipCommand{"Board", yard.casket, kNoEntity});
    sim.step();
    CHECK(sim.ship().state().riders.empty());
    sim.submit(ShipCommand{"Board", yard.reactor, yard.pawn});
    sim.step();
    CHECK(yard.lastFailure().find("casket") != std::string::npos);
    sim.submit(ShipCommand{"Fly", kNoEntity, kNoEntity});
    sim.step();
    CHECK(yard.lastFailure().find("Fly") != std::string::npos);
}

TEST_CASE("Ship: the state is saved, loads back the same and replays") {
    ShipYard yard;
    Sim& sim = yard.sim;
    sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
    sim.submit(ShipCommand{"Board", yard.casket, yard.pawn});
    sim.stepMany(100);
    sim.ship().state().nextWave = sim.time().seconds + 5;
    sim.stepMany(10);
    CHECK(sim.ship().state().waves == 1);
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, shipDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    SHIP_REQUIRE(loaded.error.empty());
    SHIP_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.ship().state().phase == ShipPhase::Activating);
    CHECK(copy.ship().state().waves == 1);
    CHECK(copy.ship().state().riders.size() == 1);
    CHECK(saveSim(copy) == bytes);
    sim.stepMany(2000);
    copy.stepMany(2000);
    CHECK(copy.stateHash() == sim.stateHash());

    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 600;
    Sim recorded(settings, shipDefs());
    recorded.stepMany(5);
    recorded.submit(ShipCommand{"Launch", kNoEntity, kNoEntity});
    recorded.stepMany(600);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("\"Ship\"") != std::string::npos);
    Replay parsed;
    std::string error;
    SHIP_REQUIRE(readReplay(text, parsed, error));
    Sim again(parsed.settings, shipDefs());
    playReplay(parsed, again, replay.endStep);
    CHECK(checkReplay(parsed, again).matched());
    CHECK(again.stateHash() == recorded.stateHash());
}

TEST_CASE("Ship: the snapshot names the reactor and who rides where; the dev skip ends the warm-up") {
    ShipYard yard;
    Sim& sim = yard.sim;
    CHECK(shipSnap(sim).ship.reactor == kNoEntity);
    sim.submit(ShipCommand{"FinishWarmUp", kNoEntity, kNoEntity});
    sim.step();
    CHECK(yard.lastFailure() == "the reactor is not warming up");
    sim.submit(ShipCommand{"StartReactor", yard.reactor, kNoEntity});
    sim.submit(ShipCommand{"Board", yard.casket, yard.pawn});
    sim.step();
    Snapshot snap = shipSnap(sim);
    CHECK(snap.ship.reactor == yard.reactor);
    SHIP_REQUIRE(snap.ship.aboard.size() == 1);
    CHECK(snap.ship.aboard[0].first == yard.casket);
    CHECK(snap.ship.aboard[0].second == yard.pawn);
    sim.submit(ShipCommand{"FinishWarmUp", kNoEntity, kNoEntity});
    sim.step();
    sim.step();
    snap = shipSnap(sim);
    CHECK(snap.ship.phase == static_cast<uint8_t>(ShipPhase::Ready));
    CHECK(snap.ship.canLaunch);
    sim.submit(ShipCommand{"Launch", kNoEntity, kNoEntity});
    sim.step();
    snap = shipSnap(sim);
    CHECK(snap.ship.won);
    CHECK(snap.ship.aboard.empty());
}