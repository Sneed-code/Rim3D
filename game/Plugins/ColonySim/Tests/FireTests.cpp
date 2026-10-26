// Fire and firefighting (task E4, docs/design/fire.md).
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Filth.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define FIRE_REQUIRE(...)      \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> fireDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

// A flat map with no plants, clear weather for days and mild air, so the fire is what is tested.
struct FireFixture {
    std::unique_ptr<Sim> simPtr;
    Sim& sim;
    std::vector<Event> seen;

    explicit FireFixture(int size = 64)
        : simPtr(std::make_unique<Sim>(SimSettings{5, size, 0, 0, true, "Human", "Steel", 0}, fireDefs())), sim(*simPtr) {
        for (int y = 0; y < sim.map().height(); ++y) {
            for (int x = 0; x < sim.map().width(); ++x) {
                sim.plants().remove(CellPos{x, y, 0});
            }
        }
        sim.setOutdoorOverride(20.0f);
        sim.weather().force("Clear", 400.0);
    }

    Fire& fire() { return sim.fire(); }
    entt::registry& reg() { return sim.things().registry(); }

    void run(int64_t seconds) {
        for (int64_t i = 0; i < seconds; ++i) {
            sim.step();
            for (const Event& e : sim.events()) {
                if (e.type == EventType::FireStarted || e.type == EventType::FireOut || e.type == EventType::PawnCaughtFire ||
                    e.type == EventType::PawnFireOut || e.type == EventType::PlantRemoved || e.type == EventType::ThingDestroyed) {
                    seen.push_back(e);
                }
            }
        }
    }
    // Steps until the condition holds or `limit` seconds pass; true if it held.
    template <typename F>
    bool runUntil(int64_t limit, F done) {
        for (int64_t i = 0; i < limit; ++i) {
            run(1);
            if (done()) {
                return true;
            }
        }
        return done();
    }
    int seenOf(EventType type, const char* message = nullptr) const {
        int n = 0;
        for (const Event& e : seen) {
            n += e.type == type && (message == nullptr || e.message == message) ? 1 : 0;
        }
        return n;
    }

    void grass(CellPos c) { sim.plants().place(c, sim.defs().idOf("Things", "Plant_Grass"), 1.0f); }
    void grassPatch(int x0, int y0, int w, int h) {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                grass(CellPos{x, y, 0});
            }
        }
    }
    int grassLeft() { return static_cast<int>(sim.plants().count()); }

    Entity colonist(CellPos cell) {
        const Entity pawn = sim.things().spawn("Human", cell);
        Colonist c;
        reg().emplace<Colonist>(pawn, c);
        sim.jobs().addWorker(pawn, {}, false);
        sim.submit(SetWorkPriorityCommand{pawn, "Firefighter", 1});
        return pawn;
    }

    // A ring of walls round x0..x1 by y0..y1.
    void ring(int x0, int y0, int x1, int y1) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (x == x0 || x == x1 || y == y0 || y == y1) {
                    sim.things().spawn("Wall", CellPos{x, y, 0});
                }
            }
        }
        sim.rooms().update();
    }
};

}  // namespace

TEST_CASE("Fire: it starts only where something can burn") {
    FireFixture f;
    const CellPos bare{10, 10, 0};
    CHECK(!f.fire().ignite(bare));
    CHECK(f.fire().count() == 0);

    const CellPos grass{12, 10, 0};
    f.grass(grass);
    CHECK(f.fire().ignite(grass));
    CHECK(f.fire().burning(grass));
    CHECK(f.fire().sizeAt(grass) == doctest::Approx(f.fire().numbers().startSize));
    CHECK(!f.fire().ignite(grass));  // already burning

    // Steel, water and rock don't burn, however hard it is forced.
    const CellPos steel{14, 10, 0};
    f.sim.things().spawn("Steel", steel, 10);
    CHECK(f.fire().flammabilityAt(steel) == 0.0f);
    CHECK(!f.fire().ignite(steel));
    const_cast<Map&>(f.sim.map()).setTerrain(CellPos{16, 10, 0}, Terrain::Water);
    const_cast<Map&>(f.sim.map()).setTerrain(CellPos{18, 10, 0}, Terrain::Rock);
    CHECK(!f.fire().ignite(CellPos{16, 10, 0}, 0.0f, true));
    CHECK(!f.fire().ignite(CellPos{18, 10, 0}, 0.0f, true));
    CHECK(!f.fire().ignite(CellPos{-1, 10, 0}, 0.0f, true));

    // Wood does, and so does the table it makes.
    f.sim.things().spawn("WoodLog", CellPos{20, 10, 0}, 20);
    CHECK(f.fire().flammabilityAt(CellPos{20, 10, 0}) == doctest::Approx(1.0f));
    CHECK(f.fire().flammabilityAt(grass) == doctest::Approx(1.3f));
}

TEST_CASE("Fire: a command lights a cell or a pawn, and says why it could not") {
    FireFixture f;
    f.grass(CellPos{10, 10, 0});
    const Entity pawn = f.sim.things().spawn("Human", CellPos{30, 30, 0});
    const uint64_t good = f.sim.submit(IgniteCommand{CellPos{10, 10, 0}, 0.0f, kNoEntity, false});
    const uint64_t bad = f.sim.submit(IgniteCommand{CellPos{11, 11, 0}, 0.0f, kNoEntity, false});
    const uint64_t pawnCmd = f.sim.submit(IgniteCommand{CellPos{}, 0.0f, pawn, false});
    f.sim.step();
    bool badFailed = false;
    bool goodFailed = false;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::CommandFailed) {
            badFailed = badFailed || e.command == bad;
            goodFailed = goodFailed || e.command == good || e.command == pawnCmd;
        }
    }
    CHECK(badFailed);
    CHECK(!goodFailed);
    bool started = false;
    for (const Event& e : f.sim.events()) {
        started = started || (e.type == EventType::FireStarted && e.cell == CellPos{10, 10, 0});
    }
    CHECK(started);
    CHECK(f.fire().burning(CellPos{10, 10, 0}));
    CHECK(f.fire().pawnBurning(pawn));
}

TEST_CASE("Fire: grass burns up, the fire dies, and the ground is left burnt") {
    FireFixture f;
    const CellPos c{20, 20, 0};
    f.grass(c);
    f.fire().ignite(c);
    CHECK(f.runUntil(3 * kSecondsPerHour, [&] { return f.fire().count() == 0; }));
    CHECK(!f.sim.plants().has(c));
    CHECK(f.fire().burnt(c));
    CHECK(f.seenOf(EventType::PlantRemoved) == 1);
    CHECK(f.seenOf(EventType::FireOut) == 1);
    CHECK(f.fire().started() == 1);
    // The snapshot shows the burnt ground, and the ground fades back after the days the rules say.
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    CHECK(std::find(snap.burnt.begin(), snap.burnt.end(), c) != snap.burnt.end());
    f.run(8 * kSecondsPerDay);
    CHECK(!f.fire().burnt(c));
}

TEST_CASE("Fire: it spreads over grass and burns the patch") {
    FireFixture f;
    f.grassPatch(20, 20, 15, 15);
    CHECK(f.grassLeft() == 225);
    f.fire().ignite(CellPos{27, 27, 0});
    size_t peak = 0;
    CHECK(f.runUntil(8 * kSecondsPerHour, [&] {
        peak = std::max(peak, f.fire().count());
        return f.fire().count() == 0;
    }));
    CHECK(peak > 10);
    CHECK(f.grassLeft() < 10);
    // The bare ground round the patch never caught.
    CHECK(!f.fire().burnt(CellPos{15, 27, 0}));
    CHECK(f.fire().started() > 100);
}

TEST_CASE("Fire: a table burns down to ash") {
    FireFixture f;
    const Entity table = f.sim.things().spawn("Table1x2", CellPos{30, 30, 0});
    FIRE_REQUIRE(table != kNoEntity);
    const CellPos c = cellOf(f.reg().get<Position>(table));
    CHECK(f.fire().flammabilityAt(c) > 0.0f);
    FIRE_REQUIRE(f.fire().ignite(c));
    CHECK(f.runUntil(12 * kSecondsPerHour, [&] { return !f.sim.things().exists(table); }));
    CHECK(f.fire().burntUp() >= 1);
    CHECK(f.sim.filth().cellCount() > 0);
    CHECK(f.runUntil(2 * kSecondsPerHour, [&] { return f.fire().count() == 0; }));
}

TEST_CASE("Fire: rain keeps a fire from starting in the open and puts out one that is lit, but not under a roof") {
    FireFixture f;
    f.grass(CellPos{10, 10, 0});
    f.fire().ignite(CellPos{10, 10, 0});
    f.sim.weather().force("Rain", 100.0);
    f.run(3 * kSecondsPerHour);  // the rain takes over from the clear sky
    CHECK(f.sim.weather().effects().extinguishesFire);
    CHECK(f.sim.weather().effects().rainRate > 0.4f);

    f.grass(CellPos{40, 40, 0});
    CHECK(!f.fire().ignite(CellPos{40, 40, 0}));
    // Forced, it fades in a few minutes without eating the grass.
    CHECK(f.fire().ignite(CellPos{40, 40, 0}, 0.0f, true));
    CHECK(f.runUntil(kSecondsPerHour, [&] { return !f.fire().burning(CellPos{40, 40, 0}); }));
    CHECK(f.sim.plants().has(CellPos{40, 40, 0}));

    // In a roofed room the rain doesn't reach.
    f.ring(10, 30, 16, 36);
    f.grass(CellPos{13, 33, 0});
    CHECK(f.fire().ignite(CellPos{13, 33, 0}));
    f.run(5 * 60);
    CHECK(f.fire().burning(CellPos{13, 33, 0}));
}

TEST_CASE("Fire: deep snow stops a fire starting") {
    FireFixture f;
    f.sim.setOutdoorOverride(-5.0f);
    f.sim.weather().force("SnowHard", 100.0);
    f.grass(CellPos{40, 40, 0});
    CHECK(f.fire().ignite(CellPos{40, 40, 0}));  // a clear sky so far: it takes
    f.fire().extinguish(CellPos{40, 40, 0}, 10.0f);
    f.run(24 * kSecondsPerHour);
    CHECK(f.sim.weather().snowDepth(CellPos{40, 40, 0}) >= f.fire().numbers().snowBlocksAt);
    CHECK(!f.fire().ignite(CellPos{40, 40, 0}));
}

TEST_CASE("Fire: a pawn standing in a fire catches fire, is hurt and burns out") {
    FireFixture f;
    f.grassPatch(30, 30, 3, 3);
    const Entity pawn = f.sim.things().spawn("Human", CellPos{31, 31, 0});
    FIRE_REQUIRE(pawn != kNoEntity);
    f.fire().ignite(CellPos{31, 31, 0});
    CHECK(f.runUntil(3 * kSecondsPerHour, [&] { return f.fire().pawnBurning(pawn); }));
    CHECK(f.seenOf(EventType::PawnCaughtFire) == 1);
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    const ThingView* view = snap.find(pawn);
    FIRE_REQUIRE(view != nullptr);
    CHECK(view->onFire);
    f.run(20 * 60);
    CHECK(!f.reg().get<Health>(pawn).injuries.empty());
    // It burns out by itself within the time the rules give (up to 1.25 times 1,800 ticks).
    CHECK(f.runUntil(2 * kSecondsPerHour, [&] { return !f.fire().pawnBurning(pawn); }));
    CHECK(f.seenOf(EventType::PawnFireOut) == 1);
}

TEST_CASE("Fire: a pawn that cannot burn does not catch fire") {
    FireFixture f;
    f.fire().setPawnFactor([](Entity) { return 0.0f; });
    f.grass(CellPos{30, 30, 0});
    const Entity pawn = f.sim.things().spawn("Human", CellPos{30, 30, 0});
    f.fire().ignite(CellPos{30, 30, 0});
    f.run(2 * kSecondsPerHour);
    CHECK(!f.fire().pawnBurning(pawn));
    CHECK(!f.fire().setPawnAlight(pawn, f.sim.time().seconds));
    CHECK(f.fire().setPawnAlight(pawn, f.sim.time().seconds, true));
}

TEST_CASE("Fire: a colonist with the Firefighter work type beats out a fire in the home area, and ignores one outside") {
    FireFixture f;
    f.sim.submit(SetHomeAreaCommand{CellRect{8, 8, 20, 20, 0}, true});
    f.sim.step();
    CHECK(f.fire().home(CellPos{10, 10, 0}));
    CHECK(!f.fire().home(CellPos{40, 40, 0}));
    f.colonist(CellPos{16, 16, 0});

    f.grass(CellPos{12, 12, 0});
    f.grass(CellPos{45, 45, 0});
    f.fire().ignite(CellPos{12, 12, 0});
    f.fire().ignite(CellPos{45, 45, 0});
    CHECK(f.runUntil(1200, [&] { return !f.fire().burning(CellPos{12, 12, 0}); }));
    CHECK(f.seenOf(EventType::FireOut, "beaten") == 1);
    CHECK(f.sim.plants().has(CellPos{12, 12, 0}));  // put out before it ate the grass
    // The one outside the home area was left alone.
    CHECK(f.fire().burning(CellPos{45, 45, 0}));
}

TEST_CASE("Fire: a colonist puts out a burning friend wherever they are") {
    FireFixture f;
    f.sim.submit(SetHomeAreaCommand{CellRect{0, 0, 5, 5, 0}, true});
    const Entity victim = f.colonist(CellPos{40, 40, 0});
    f.colonist(CellPos{44, 40, 0});
    f.grass(CellPos{40, 40, 0});
    f.sim.submit(IgniteCommand{CellPos{}, 0.0f, victim, false});
    f.run(1);
    FIRE_REQUIRE(f.fire().pawnBurning(victim));
    CHECK(f.runUntil(20 * 60, [&] { return !f.fire().pawnBurning(victim); }));
    CHECK(f.seenOf(EventType::PawnFireOut) == 1);
    CHECK(f.reg().get<Health>(victim).dead == false);
}

TEST_CASE("Fire: a burning room warms up, and a room that gets hot enough sets everything in it alight") {
    FireFixture f;
    f.ring(10, 10, 16, 16);
    for (int y = 11; y <= 15; ++y) {
        for (int x = 11; x <= 15; ++x) {
            f.grass(CellPos{x, y, 0});
        }
    }
    const Room* room = f.sim.rooms().roomOf(CellPos{13, 13, 0});
    FIRE_REQUIRE(room != nullptr);
    FIRE_REQUIRE(room->enclosed() && !room->outdoors());
    f.fire().ignite(CellPos{13, 13, 0});
    const float before = f.sim.temperature().temperatureAt(CellPos{13, 13, 0});
    f.run(40 * 60);
    CHECK(f.sim.temperature().temperatureAt(CellPos{13, 13, 0}) > before + 3.0f);
    CHECK(f.sim.temperature().temperatureAt(CellPos{30, 30, 0}) == doctest::Approx(20.0f));  // outdoors didn't change

    // A room above 235 degrees: one fire is enough to light the table in the far corner.
    FireFixture g;
    g.ring(10, 10, 16, 16);
    g.grass(CellPos{11, 11, 0});
    const Entity table = g.sim.things().spawn("Table1x2", CellPos{15, 14, 0});
    FIRE_REQUIRE(table != kNoEntity);
    const Room* hot = g.sim.rooms().roomOf(CellPos{13, 13, 0});
    FIRE_REQUIRE(hot != nullptr);
    g.sim.temperature().restore({RoomTemperature{hot->anchor, 300.0f}});
    g.fire().ignite(CellPos{11, 11, 0});
    const CellPos tableCell = cellOf(g.reg().get<Position>(table));
    CHECK(g.runUntil(30 * 60, [&] { return g.fire().burning(tableCell); }));
}

TEST_CASE("Fire: a blast of burn damage starts fires and sets pawns alight, an ordinary one does not") {
    FireFixture f;
    f.grassPatch(30, 30, 7, 7);
    const Entity pawn = f.sim.things().spawn("Human", CellPos{33, 33, 0});
    f.sim.submit(ExplodeCommand{33.5f, 33.5f, 2.5f, 20.0f, "Bomb", 0.1f});
    f.run(2);
    CHECK(f.fire().count() == 0);
    CHECK(!f.fire().pawnBurning(pawn));

    FireFixture g;
    g.grassPatch(30, 30, 7, 7);
    const Entity victim = g.sim.things().spawn("Human", CellPos{33, 33, 0});
    g.sim.submit(ExplodeCommand{33.5f, 33.5f, 2.5f, 1.0f, "Burn", 0.1f});
    g.run(2);
    CHECK(g.fire().count() >= 5);
    CHECK(g.fire().burning(CellPos{33, 33, 0}));
    CHECK(g.fire().pawnBurning(victim));
}

TEST_CASE("Fire: the home area grows round new buildings until the player paints it") {
    FireFixture f;
    f.sim.things().spawn("Wall", CellPos{30, 30, 0});
    f.sim.step();
    CHECK(f.fire().home(CellPos{30, 30, 0}));
    CHECK(f.fire().home(CellPos{34, 30, 0}));
    CHECK(!f.fire().home(CellPos{40, 30, 0}));

    f.sim.submit(SpawnThingCommand{"Wall", CellPos{10, 10, 0}, 1, Rot4::North});
    f.sim.step();
    CHECK(f.fire().home(CellPos{10, 14, 0}));

    // Painting turns the automatic growth off.
    const uint64_t first = f.sim.submit(SetHomeAreaCommand{CellRect{50, 50, 3, 3, 0}, true});
    f.sim.submit(SpawnThingCommand{"Wall", CellPos{20, 50, 0}, 1, Rot4::North});
    f.sim.step();
    CHECK(f.fire().home(CellPos{51, 51, 0}));
    CHECK(!f.fire().home(CellPos{20, 50, 0}));
    CHECK(!f.fire().autoHome());
    (void)first;

    // Taking cells off, and a command that changes nothing fails.
    const uint64_t remove = f.sim.submit(SetHomeAreaCommand{CellRect{50, 50, 3, 3, 0}, false});
    const uint64_t again = f.sim.submit(SetHomeAreaCommand{CellRect{50, 50, 3, 3, 0}, false});
    f.sim.step();
    CHECK(!f.fire().home(CellPos{51, 51, 0}));
    bool removeFailed = false;
    bool againFailed = false;
    for (const Event& e : f.sim.events()) {
        if (e.type == EventType::CommandFailed) {
            removeFailed = removeFailed || e.command == remove;
            againFailed = againFailed || e.command == again;
        }
    }
    CHECK(!removeFailed);
    CHECK(againFailed);
    Snapshot snap;
    f.sim.writeSnapshot(snap);
    CHECK(snap.homeArea.size() == f.fire().homeCount());
}

TEST_CASE("Fire: saves keep fires, burnt ground, the home area and burning pawns, and the game goes on the same") {
    FireFixture f;
    f.grassPatch(20, 20, 9, 9);
    f.sim.submit(SetHomeAreaCommand{CellRect{10, 10, 30, 30, 0}, true});
    const Entity pawn = f.sim.things().spawn("Human", CellPos{40, 40, 0});
    f.fire().ignite(CellPos{24, 24, 0});
    f.sim.submit(IgniteCommand{CellPos{}, 0.0f, pawn, false});
    f.run(40 * 60);
    FIRE_REQUIRE(f.fire().count() > 3);
    FIRE_REQUIRE(f.fire().pawnBurning(pawn));

    const std::vector<uint8_t> bytes = saveSim(*f.simPtr);
    SaveLoadResult loaded = loadSim(bytes, fireDefs());
    FIRE_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.error.empty());
    CHECK(loaded.warnings.empty());
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == f.sim.stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(saveToJson(*f.simPtr).find("\"fire\"") != std::string::npos);
    CHECK(copy.fire().count() == f.fire().count());
    CHECK(copy.fire().pawnBurning(pawn));
    CHECK(copy.fire().homeCount() == f.fire().homeCount());
    CHECK(copy.fire().burntCells().size() == f.fire().burntCells().size());

    copy.setOutdoorOverride(20.0f);  // the override is a test aid and is not saved
    for (int i = 0; i < 90 * 60; ++i) {
        f.sim.step();
        copy.step();
    }
    CHECK(copy.stateHash() == f.sim.stateHash());
    const std::string a = saveToJson(copy);
    const std::string b = saveToJson(*f.simPtr);
    size_t at = 0;
    while (at < a.size() && at < b.size() && a[at] == b[at]) {
        ++at;
    }
    const bool same = a == b;
    if (!same) {
        FILE* out = fopen("/tmp/firediff.txt", "w");
        if (out != nullptr) {
            fprintf(out, "%s\n=====\n%s\n", a.substr(at > 600 ? at - 600 : 0, 1200).c_str(), b.substr(at > 600 ? at - 600 : 0, 1200).c_str());
            fclose(out);
        }
    }
    CHECK(same);
    CHECK(f.fire().started() > 5);
}

TEST_CASE("Fire: a game with nothing alight stays quiet") {
    FireFixture f;
    f.sim.things().spawn("Human", CellPos{20, 20, 0});
    f.run(3000);
    CHECK(f.fire().count() == 0);
    CHECK(f.fire().burningPawns() == 0);
    CHECK(f.fire().started() == 0);
}
