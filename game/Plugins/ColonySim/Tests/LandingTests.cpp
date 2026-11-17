// Shuttles and the landing zone (docs/design/landing.md).
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Landing.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"

#include <doctest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define LND_REQUIRE(...)       \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> lndDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim lndSim() { return Sim(SimSettings{7, 128, 0, 0, true}, lndDefs()); }

Entity lndColonist(Sim& sim, CellPos cell) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    return pawn;
}

// Takes the wild plants off a rectangle (the flat test map has trees scattered on it).
void lndClear(Sim& sim, CellRect r) {
    for (int y = r.minY; y < r.minY + r.sizeY; ++y) {
        for (int x = r.minX; x < r.minX + r.sizeX; ++x) {
            sim.plants().remove(CellPos{x, y, 0});
        }
    }
}

// Whether the command failed in the last step.
bool lndFailed(const Sim& sim, uint64_t command) {
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed && e.command == command) {
            return true;
        }
    }
    return false;
}

// Steps until the shuttle has come down (or is gone).
void lndUntilLanded(Sim& sim, uint32_t id) {
    for (int i = 0; i < 2000; ++i) {
        const Shuttle* s = sim.landing().find(id);
        if (s == nullptr || s->phase != ShuttlePhase::Descending) {
            return;
        }
        sim.step();
    }
}

}  // namespace

TEST_CASE("Landing: the rules and the shuttle def are in the data") {
    Sim sim = lndSim();
    const LandingSystem& landing = sim.landing();
    CHECK(landing.rules().present);
    CHECK(landing.available());
    CHECK(landing.rules().shuttleDef == "Shuttle_VTOL");
    const Def* def = lndDefs()->find("Things", "Shuttle_VTOL");
    LND_REQUIRE(def != nullptr);
    CHECK(def->number("size.0") == 6);   // 6 m wide
    CHECK(def->number("size.1") == 12);  // 12 m long
    CHECK(def->number("shuttle.passengerSeats") == 8);
    CHECK(def->number("shuttle.pilotSeats") == 2);
    CHECK(def->number("shuttle.liftThrusters") == 8);
    CHECK(def->number("shuttle.rearThrusters") == 2);
}

TEST_CASE("Landing: the zone is painted on and off; the command fails when nothing changes") {
    Sim sim = lndSim();
    const uint64_t first = sim.submit(SetLandingZoneCommand{CellRect{20, 20, 10, 16, 0}, true});
    sim.step();
    CHECK_FALSE(lndFailed(sim, first));
    CHECK(sim.landing().inZone(CellPos{25, 30, 0}));
    CHECK_FALSE(sim.landing().inZone(CellPos{31, 30, 0}));
    CHECK(sim.landing().zoneCells().size() == 160);
    const uint64_t again = sim.submit(SetLandingZoneCommand{CellRect{20, 20, 10, 16, 0}, true});
    sim.step();
    CHECK(lndFailed(sim, again));
    const uint64_t off = sim.submit(SetLandingZoneCommand{CellRect{20, 20, 10, 4, 0}, false});
    sim.step();
    CHECK_FALSE(lndFailed(sim, off));
    CHECK(sim.landing().zoneCells().size() == 120);
    // Cells off the map are left out.
    CHECK(sim.landing().paintZone(CellRect{-5, -5, 3, 3, 0}, true) == 0);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    CHECK(snapshot.landingZone.size() == 120);
}

TEST_CASE("Landing: with no zone a shuttle picks a clear spot near the colony, comes down and flattens small plants") {
    Sim sim = lndSim();
    // Grass everywhere near the colony, so the shuttle must flatten some.
    const DefId grass = lndDefs()->idOf("Things", "Plant_Grass");
    LND_REQUIRE(grass.valid());
    for (int y = 20; y < 108; ++y) {
        for (int x = 20; x < 108; ++x) {
            if (!sim.plants().has(CellPos{x, y, 0})) {
                sim.plants().place(CellPos{x, y, 0}, grass, 0.5f);
            }
        }
    }
    uint32_t id = 0;
    std::string error;
    const bool landed = sim.landing().land(0, {}, 64.0f, 64.0f, id, error);
    if (!landed) MESSAGE(error);
    LND_REQUIRE(landed);
    const Shuttle* s = sim.landing().find(id);
    LND_REQUIRE(s != nullptr);
    CHECK(s->phase == ShuttlePhase::Descending);
    const CellRect rect = sim.landing().footprint(*s);
    CHECK(rect.sizeX * rect.sizeY == 72);
    // Within the search ring round the colony's middle.
    const float dx = sim.landing().centreX(*s) - 64.0f;
    const float dy = sim.landing().centreY(*s) - 64.0f;
    CHECK(std::sqrt(dx * dx + dy * dy) <= 60.0f + 8.0f);
    // It claims the footprint at once: an impassable building.
    CHECK(sim.map().buildingAt(CellPos{rect.minX + 1, rect.minY + 1, 0}) == s->building);
    CHECK_FALSE(sim.map().walkable(CellPos{rect.minX + 1, rect.minY + 1, 0}));
    // The snapshot shows it on its way down.
    sim.stepMany(100);
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    LND_REQUIRE(snapshot.shuttles.size() == 1);
    CHECK(snapshot.shuttles[0].phase == static_cast<uint8_t>(ShuttlePhase::Descending));
    CHECK(snapshot.shuttles[0].progress > 0.2f);
    CHECK(snapshot.shuttles[0].progress < 0.4f);
    bool grassUnder = false;
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            grassUnder = grassUnder || sim.plants().has(CellPos{x, y, 0});
        }
    }
    CHECK(grassUnder);
    lndUntilLanded(sim, id);
    s = sim.landing().find(id);
    LND_REQUIRE(s != nullptr);
    CHECK(s->phase == ShuttlePhase::Landed);
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            CHECK_FALSE(sim.plants().has(CellPos{x, y, 0}));
        }
    }
    // Outside the footprint the grass stands.
    CHECK(sim.plants().has(CellPos{rect.minX - 2, rect.minY - 2, 0}));
    // Up and away.
    CHECK(sim.landing().depart(id));
    CHECK_FALSE(sim.landing().depart(id));
    sim.stepMany(sim.landing().rules().ascendSteps + 2);
    CHECK(sim.landing().find(id) == nullptr);
    CHECK(sim.map().buildingAt(CellPos{rect.minX + 1, rect.minY + 1, 0}) == kNoEntity);
}

TEST_CASE("Landing: hard things stop a landing: buildings, trees, items, rock, deep water and steep ground") {
    Sim sim = lndSim();
    const LandingSystem& landing = sim.landing();
    const CellPos at{40, 40, 0};
    lndClear(sim, CellRect{38, 38, 10, 16, 0});
    CHECK(landing.siteClear(at, Rot4::North, false));
    // A wall in the footprint.
    const Entity wall = sim.things().spawn("Wall", CellPos{43, 45, 0});
    LND_REQUIRE(wall != kNoEntity);
    CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
    sim.things().destroy(wall);
    CHECK(landing.siteClear(at, Rot4::North, false));
    // A tree.
    const DefId tree = lndDefs()->idOf("Things", "Plant_TreeOak");
    if (tree.valid()) {
        sim.plants().remove(CellPos{42, 42, 0});
        CHECK(sim.plants().place(CellPos{42, 42, 0}, tree, 1.0f));
        CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
        sim.plants().remove(CellPos{42, 42, 0});
        CHECK(landing.siteClear(at, Rot4::North, false));
    }
    // An item.
    const Entity steel = sim.things().spawn("Steel", CellPos{41, 50, 0}, 10);
    CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
    sim.things().destroy(steel);
    // Deep water and rock.
    sim.editMap().setTerrain(CellPos{44, 41, 0}, Terrain::Water);
    CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
    sim.editMap().setTerrain(CellPos{44, 41, 0}, Terrain::Rock);
    CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
    sim.editMap().setTerrain(CellPos{44, 41, 0}, Terrain::Soil);
    // Marsh and shallow water are fine.
    sim.editMap().setTerrain(CellPos{44, 41, 0}, Terrain::Marsh);
    CHECK(landing.siteClear(at, Rot4::North, false));
    // A slope over 1.5 m across the footprint.
    sim.editMap().setGroundHeight(CellPos{45, 51, 0}, 2.5f);
    CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
    sim.editMap().setGroundHeight(CellPos{45, 51, 0}, 1.0f);
    CHECK(landing.siteClear(at, Rot4::North, false));
    // A pawn counts against a random spot but not against the zone.
    const Entity pawn = lndColonist(sim, CellPos{42, 46, 0});
    CHECK_FALSE(landing.siteClear(at, Rot4::North, false));
    CHECK(landing.siteClear(at, Rot4::North, true));
    (void)pawn;
}

TEST_CASE("Landing: a shuttle sets down in the landing zone, and anyone standing there steps aside") {
    Sim sim = lndSim();
    lndClear(sim, CellRect{88, 18, 14, 20, 0});
    sim.submit(SetLandingZoneCommand{CellRect{90, 20, 10, 16, 0}, true});
    sim.step();
    const Entity pawn = lndColonist(sim, CellPos{95, 28, 0});
    uint32_t id = 0;
    std::string error;
    const bool landed = sim.landing().land(0, {}, 30.0f, 100.0f, id, error);
    LND_REQUIRE(landed);
    const Shuttle* s = sim.landing().find(id);
    LND_REQUIRE(s != nullptr);
    const CellRect rect = sim.landing().footprint(*s);
    for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
        for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
            CHECK(sim.landing().inZone(CellPos{x, y, 0}));
        }
    }
    const Position& pos = sim.things().registry().get<Position>(pawn);
    CHECK_FALSE(rect.contains(cellOf(pos)));
    CHECK(sim.map().walkable(cellOf(pos)));
    // The door is outside the footprint and can be walked to.
    const CellPos door = sim.landing().door(*s);
    CHECK_FALSE(rect.contains(door));
    CHECK(sim.map().walkable(door));
}

TEST_CASE("Landing: a zone with no room sends the shuttle to a clear spot elsewhere") {
    Sim sim = lndSim();
    // A zone too small for a 12 by 6 footprint.
    sim.submit(SetLandingZoneCommand{CellRect{90, 20, 5, 5, 0}, true});
    sim.step();
    uint32_t id = 0;
    std::string error;
    const bool landed = sim.landing().land(0, {}, 64.0f, 64.0f, id, error);
    LND_REQUIRE(landed);
    const Shuttle* s = sim.landing().find(id);
    LND_REQUIRE(s != nullptr);
    CHECK_FALSE(sim.landing().inZone(s->origin));
}

TEST_CASE("Landing: a caravan comes by shuttle, steps out at the door, trades and flies away") {
    Sim sim = lndSim();
    const Entity colonist = lndColonist(sim, CellPos{64, 64, 0});
    (void)colonist;
    uint32_t id = 0;
    std::string error;
    const bool started = sim.trade().startVisit("FrayCompact", VisitKind::Caravan, "BulkGoods", kShuttleEdge, id, error);
    if (!started) MESSAGE(error);
    LND_REQUIRE(started);
    const Shuttle* s = sim.landing().ofVisit(id);
    LND_REQUIRE(s != nullptr);
    const uint32_t shuttle = s->id;
    CHECK(s->passengers.size() >= 3);
    // Nobody is out while it comes down, and the visit waits for them.
    const Visit* caravan = sim.trade().find(id);
    LND_REQUIRE(caravan != nullptr);
    CHECK(caravan->members.empty());
    CHECK(sim.landing().awaiting(id));
    sim.stepMany(200);
    LND_REQUIRE(sim.trade().find(id) != nullptr);
    CHECK(sim.trade().find(id)->members.empty());
    lndUntilLanded(sim, shuttle);
    s = sim.landing().find(shuttle);
    LND_REQUIRE(s != nullptr);
    CHECK(s->phase == ShuttlePhase::Landed);
    CHECK(s->passengers.empty());
    caravan = sim.trade().find(id);
    LND_REQUIRE(caravan != nullptr);
    CHECK(caravan->members.size() >= 3);
    CHECK(caravan->trader != kNoEntity);
    // They stand by the door.
    const CellPos door = sim.landing().door(*s);
    for (const Entity m : caravan->members) {
        const Position& p = sim.things().registry().get<Position>(m);
        CHECK(std::abs(p.x - (door.x + 0.5f)) < 6.0f);
        CHECK(std::abs(p.y - (door.y + 0.5f)) < 6.0f);
    }
    // Their stay counts from the landing.
    CHECK(caravan->leaveAt - sim.time().seconds >= 12 * 3600 - 30);
    const Entity trader = caravan->trader;

    // Time is up: they walk back to the shuttle, climb aboard and it lifts off.
    TradeState state = sim.trade().state();
    state.visits[0].leaveAt = sim.time().seconds + 30;
    const bool restored = sim.trade().restore(state, error);
    LND_REQUIRE(restored);
    bool ascending = false;
    for (int i = 0; i < 6000 && sim.landing().find(shuttle) != nullptr; ++i) {
        sim.step();
        const Shuttle* now = sim.landing().find(shuttle);
        ascending = ascending || (now != nullptr && now->phase == ShuttlePhase::Ascending);
    }
    CHECK(ascending);
    CHECK(sim.trade().find(id) == nullptr);
    CHECK(sim.landing().find(shuttle) == nullptr);
    CHECK_FALSE(sim.things().registry().valid(trader));
}

TEST_CASE("Landing: a shuttle and its zone save and load, and the loaded game goes on the same") {
    Sim sim = lndSim();
    lndColonist(sim, CellPos{64, 64, 0});
    sim.submit(SetLandingZoneCommand{CellRect{80, 70, 14, 14, 0}, true});
    sim.step();
    uint32_t id = 0;
    std::string error;
    const bool started = sim.trade().startVisit("FrayCompact", VisitKind::Visitors, "", kShuttleEdge, id, error);
    LND_REQUIRE(started);
    sim.stepMany(120);  // mid-descent
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, lndDefs());
    if (!loaded.error.empty()) MESSAGE(loaded.error);
    LND_REQUIRE(loaded.error.empty());
    LND_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.landing().zoneCells().size() == 196);
    LND_REQUIRE(copy.landing().ofVisit(id) != nullptr);
    CHECK(copy.landing().ofVisit(id)->passengers.size() == sim.landing().ofVisit(id)->passengers.size());
    CHECK(saveSim(copy) == bytes);
    sim.stepMany(600);
    copy.stepMany(600);
    CHECK(copy.stateHash() == sim.stateHash());
    CHECK(copy.trade().find(id)->members.size() == sim.trade().find(id)->members.size());
    CHECK_FALSE(sim.trade().find(id)->members.empty());
}

TEST_CASE("Landing: the landing zone command is written into a replay and read back") {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = 128;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    settings.checkpointEvery = 300;
    Sim recorded(settings, lndDefs());
    recorded.stepMany(5);
    recorded.submit(SetLandingZoneCommand{CellRect{10, 10, 14, 14, 0}, true});
    recorded.submit(StartVisitCommand{"FrayCompact", "Visitors", "", kShuttleEdge});
    recorded.stepMany(600);
    const Replay replay = recordReplay(recorded);
    const std::string text = writeReplay(replay);
    CHECK(text.find("SetLandingZone") != std::string::npos);
    Replay parsed;
    std::string error;
    const bool read = readReplay(text, parsed, error);
    LND_REQUIRE(read);
    Sim again(parsed.settings, lndDefs());
    playReplay(parsed, again, replay.endStep);
    CHECK(checkReplay(parsed, again).matched());
    CHECK(again.stateHash() == recorded.stateHash());
    CHECK(again.landing().zoneCells().size() == 196);
    CHECK(again.landing().shuttles().size() == 1);
}
