// Windows in walls (docs/design/windows.md): the build-menu tools, sight, cover and light through a window, glass that
// breaks under bullets, grenades and blasts, builders cutting, glazing and filling windows in, and saves.

#include "ColonySim/BuildPlanner.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/Windows.h"
#include "FuelTestHelper.h"

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

using namespace colony;

#define WINDOW_REQUIRE(...)    \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> windowDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> windowSim(int size = 64) {
    SimSettings settings;
    settings.seed = 7;
    settings.mapSize = size;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, windowDefs());
}

void windowRing(Sim& sim, int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (x == x0 || x == x1 || y == y0 || y == y1) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
}

void windowGoTo(Sim& sim, double hour) {
    if (sim.time().seconds == 0) {
        sim.submit(SetWeatherCommand{"Clear", 1000.0f});
    }
    sim.stepMany(static_cast<int64_t>(hour * static_cast<double>(kSecondsPerHour)) - sim.time().seconds);
}

int windowCount(const Sim& sim, EventType type) {
    int n = 0;
    for (const Event& e : sim.events()) {
        n += e.type == type ? 1 : 0;
    }
    return n;
}

// Steps until no projectile is left and returns the events of every step.
std::vector<Event> windowFly(Sim& sim, int limit = 200) {
    std::vector<Event> out;
    for (int i = 0; i < limit; ++i) {
        sim.step();
        out.insert(out.end(), sim.events().begin(), sim.events().end());
        if (sim.things().registry().view<Projectile>().empty()) {
            break;
        }
    }
    return out;
}

Entity windowPawn(Sim& sim, int x, int y, const char* gun = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

// The map's cover and sight; the sim only hands out a const map, and cover queries fill a cache as they go.
Cover& windowCover(Sim& sim) { return const_cast<Map&>(sim.map()).cover(); }

// What the snapshot says about a wall's window.
WindowState windowShown(const Sim& sim, Entity e) {
    Snapshot snap;
    sim.writeSnapshot(snap);
    for (const ThingView& v : snap.things) {
        if (v.id == e) {
            return v.window;
        }
    }
    return WindowState::None;
}

}  // namespace

TEST_CASE("Windows: the rules, the walls that take them and the build-menu tools load from the data") {
    auto sim = windowSim();
    const WindowNumbers& n = sim->windows().numbers();
    CHECK(n.sillDm == 9);
    CHECK(n.lintelDm == 21);
    CHECK(n.lightRadius == doctest::Approx(6.0f));
    CHECK(buildToolOf(sim->defs(), "WindowTool") == BuildTool::AddWindow);
    CHECK(buildToolOf(sim->defs(), "RemoveWindowTool") == BuildTool::RemoveWindow);
    CHECK(buildToolOf(sim->defs(), "Wall") == BuildTool::None);

    const BuildPlanner planner(*sim);
    bool found = false;
    for (const BuildMenuCategory& c : planner.menu()) {
        for (const BuildMenuEntry& e : c.entries) {
            if (e.defName == "WindowTool") {
                found = true;
                CHECK(c.name == "Structure");
                CHECK(e.tool);
                CHECK_FALSE(e.needsStuff);
                CHECK(e.drag == DragShape::Line);
                CHECK(e.locked.empty());
            }
        }
    }
    CHECK(found);
}

TEST_CASE("Windows: one goes only in a finished wall, and lowers it to the sill for sight and cover") {
    auto sim = windowSim();
    Windows& windows = sim->windows();
    const CellPos at{20, 20, 0};
    CHECK_FALSE(windows.whyNotAdd(at).empty());  // open ground
    CHECK_FALSE(windows.add(at));
    sim->things().spawn("Sandbags", CellPos{22, 20, 0});
    CHECK_FALSE(windows.whyNotAdd(CellPos{22, 20, 0}).empty());  // sandbags don't take windows

    const Entity wall = sim->things().spawn("Wall", at);
    WINDOW_REQUIRE(wall != kNoEntity);
    CHECK(sim->map().obstacleDm(at) == kObstacleTallDm);
    const Viewpoint west = Viewpoint::at(CellPos{17, 20, 0});
    const Viewpoint east = Viewpoint::at(CellPos{23, 20, 0});
    CHECK_FALSE(windowCover(*sim).lineOfSight(west, east));
    CHECK(windows.whyNotRemove(at) == "there is no window there");

    CHECK(windows.whyNotAdd(at).empty());
    CHECK(windows.add(at));
    CHECK(windows.at(at) != nullptr);
    CHECK_FALSE(windows.at(at)->broken);
    CHECK_FALSE(windows.whyNotAdd(at).empty());  // it has one already
    CHECK(sim->map().obstacleDm(at) == 9);
    CHECK(sim->map().passage(at) == Passage::Blocked);  // still a wall to walk into
    CHECK(windowCover(*sim).lineOfSight(west, east));  // seen through, standing
    // Crouched right behind it, the sill hides most of a pawn; seen standing over it, most of a pawn shows.
    CHECK(windowCover(*sim).coverHeight(CellPos{21, 20, 0}, Dir8::W) == doctest::Approx(0.9f));
    CHECK(windowCover(*sim).coverFraction(CellPos{21, 20, 0}, Dir8::W, Stance::Crouching) >
          windowCover(*sim).coverFraction(CellPos{21, 20, 0}, Dir8::W, Stance::Standing));

    sim->step();
    CHECK(windowShown(*sim, wall) == WindowState::Glass);

    CHECK(windows.remove(at));
    CHECK(windows.at(at) == nullptr);
    CHECK(sim->map().obstacleDm(at) == kObstacleTallDm);
    CHECK_FALSE(windowCover(*sim).lineOfSight(west, east));
    CHECK_FALSE(windows.remove(at));
}

TEST_CASE("Windows: the sun lights the room behind a window, by the time of day") {
    auto sim = windowSim();
    windowRing(*sim, 20, 20, 30, 28);
    windowGoTo(*sim, 12.0);
    const CellPos near{25, 21, 0};
    const CellPos far{25, 25, 0};
    WINDOW_REQUIRE(sim->rooms().indoors(near));
    CHECK(sim->lights().lightAt(near) == 0.0f);

    WINDOW_REQUIRE(sim->windows().add(CellPos{25, 20, 0}));  // the south wall: open sky outside, the room inside
    sim->step();
    CHECK(sim->lights().windowShareAt(near) == doctest::Approx(0.8f));
    CHECK(sim->lights().lightAt(near) == doctest::Approx(0.8f));
    CHECK(sim->lights().lightAt(far) < sim->lights().lightAt(near));
    CHECK(sim->lights().lightAt(far) > 0.0f);  // 5 m in, inside the 6 m reach
    CHECK(sim->lights().lightAt(CellPos{29, 27, 0}) == 0.0f);  // the far corner is past the reach
    CHECK(sim->lights().workSpeedAt(near) == doctest::Approx(1.0f));
    CHECK(sim->rooms().indoors(near));  // the glass keeps the room a room

    windowGoTo(*sim, 23.0);
    CHECK(sim->lights().lightAt(near) == doctest::Approx(0.0f));  // no sun, no light through the window

    sim->windows().remove(CellPos{25, 20, 0});
    windowGoTo(*sim, 36.0);
    CHECK(sim->lights().windowShareAt(near) == 0.0f);
    CHECK(sim->lights().lightAt(near) == 0.0f);
}

TEST_CASE("Windows: a lamp shines out through a window") {
    auto sim = windowSim();
    windowRing(*sim, 20, 20, 26, 26);
    testFueled(*sim, sim->things().spawn("TorchLamp", CellPos{23, 22, 0}));
    sim->step();
    WINDOW_REQUIRE(sim->lights().lampLightAt(CellPos{23, 22, 0}) > 0.0f);
    const CellPos outside{23, 19, 0};
    CHECK(sim->lights().lampLightAt(outside) == 0.0f);  // the wall shades it
    WINDOW_REQUIRE(sim->windows().add(CellPos{23, 20, 0}));
    sim->step();
    CHECK(sim->lights().lampLightAt(outside) > 0.0f);
}

TEST_CASE("Windows: a bullet breaks the glass and flies on; above the lintel it hits the wall") {
    auto sim = windowSim(96);
    const Entity shooter = windowPawn(*sim, 10, 30, "Gun_BoltActionRifle");
    const Entity target = windowPawn(*sim, 30, 30);
    const Entity wall = sim->things().spawn("Wall", CellPos{20, 30, 0});
    WINDOW_REQUIRE(sim->windows().add(CellPos{20, 30, 0}));
    Rng rng(3);

    float x = 0.0f, y = 0.0f, z = 0.0f;
    sim->combat().aimPoint(target, x, y, z);
    sim->combat().fire(shooter, x, y, z, 0.0f, rng);
    std::vector<Event> events = windowFly(*sim);
    int broken = 0;
    bool hitPawn = false;
    for (const Event& e : events) {
        if (e.type == EventType::WindowBroken) {
            ++broken;
            CHECK(e.thing == wall);
            CHECK(e.target == shooter);
            CHECK(e.x == doctest::Approx(20.0f).epsilon(0.01));
        }
        hitPawn = hitPawn || (e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Pawn && e.target == target);
    }
    CHECK(broken == 1);
    CHECK(hitPawn);
    WINDOW_REQUIRE(sim->windows().at(CellPos{20, 30, 0}) != nullptr);
    CHECK(sim->windows().at(CellPos{20, 30, 0})->broken);
    CHECK(sim->windows().whyNotAdd(CellPos{20, 30, 0}).empty());  // a broken one can be glazed again
    sim->step();
    CHECK(windowShown(*sim, wall) == WindowState::Broken);

    // The glass is gone: the next shot breaks nothing.
    sim->combat().fire(shooter, x, y, z, 0.0f, rng);
    events = windowFly(*sim);
    for (const Event& e : events) {
        CHECK(e.type != EventType::WindowBroken);
    }

    // Aimed high over the opening, at 2.6 m: the wall above the window stops it.
    const int hpBefore = sim->things().registry().get<HitPoints>(wall).current;
    sim->combat().fire(shooter, x, y, sim->map().heightAt(x, y) + 2.6f + (2.6f - 1.4f), 0.0f, rng);
    events = windowFly(*sim);
    bool hitWall = false;
    for (const Event& e : events) {
        hitWall = hitWall || (e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Building && e.target == wall);
    }
    CHECK(hitWall);
    CHECK(sim->things().registry().get<HitPoints>(wall).current < hpBefore);
}

TEST_CASE("Windows: a blast blows the glass out") {
    auto sim = windowSim();
    const Entity wall = sim->things().spawn("Wall", CellPos{20, 20, 0});
    WINDOW_REQUIRE(sim->windows().add(CellPos{20, 20, 0}));
    Explosion ex;
    ex.x = 21.5f;
    ex.y = 20.5f;
    ex.z = sim->map().heightAt(21.5f, 20.5f) + 0.2f;
    ex.radius = 2.0f;
    ex.damage = 10.0f;
    ex.damageType = sim->defs().idOf("DamageTypes", "Bomb");
    sim->combat().explode(ex);
    WINDOW_REQUIRE(sim->windows().at(CellPos{20, 20, 0}) != nullptr);
    CHECK(sim->windows().at(CellPos{20, 20, 0})->broken);
    CHECK(sim->things().exists(wall));
}

TEST_CASE("Windows: builders cut one from the build menu, glaze it after it breaks and fill it in") {
    auto sim = windowSim();
    const CellPos at{20, 20, 0};
    const Entity wall = sim->things().spawn("Wall", at);
    WINDOW_REQUIRE(wall != kNoEntity);

    // The tool refuses cells with no wall and marks the wall.
    const BuildPlanner planner(*sim);
    const BuildPreview preview = planner.preview("WindowTool", "", CellPos{18, 20, 0}, CellPos{22, 20, 0}, Rot4::North, DragShape::Line);
    CHECK(preview.whyNot.empty());
    CHECK(preview.spots.size() == 5);
    CHECK(preview.plannable == 1);
    CHECK(preview.cost.empty());
    for (const Command& c : BuildPlanner::commands(preview, "WindowTool", "", Rot4::North)) {
        sim->submit(c);
    }
    sim->submit(PlaceBlueprintCommand{"WindowTool", "", CellPos{30, 30, 0}, Rot4::North});
    sim->step();
    CHECK(windowCount(*sim, EventType::CommandFailed) == 1);
    CHECK(sim->designations().at(at) == DesignationKind::AddWindow);
    CHECK(sim->windows().at(at) == nullptr);  // not yet: a builder does it

    sim->submit(SpawnThingCommand{"Human", CellPos{15, 15, 0}, 1});  // a colonist, with work to do
    sim->step();
    const auto runUntil = [&](auto done) {
        int steps = 0;
        while (steps < 3600 && !done()) {
            sim->step();
            ++steps;
        }
        return steps;
    };
    const int cutSteps = runUntil([&] { return sim->windows().at(at) != nullptr; });
    CHECK(cutSteps < 3600);
    CHECK(cutSteps > 60);  // real work, not instant
    CHECK(sim->designations().at(at) == DesignationKind::None);
    CHECK(sim->map().obstacleDm(at) == 9);

    // Break it, and glaze it again with the same tool.
    WINDOW_REQUIRE(sim->combat().breakWindow(wall, 20.5f, 20.5f, 1.4f));
    CHECK_FALSE(sim->combat().breakWindow(wall, 20.5f, 20.5f, 1.4f));
    sim->submit(PlaceBlueprintCommand{"WindowTool", "", at, Rot4::North});
    sim->step();
    CHECK(sim->designations().at(at) == DesignationKind::AddWindow);
    runUntil([&] { return !sim->windows().at(at)->broken; });
    CHECK_FALSE(sim->windows().at(at)->broken);

    // Fill it in.
    sim->submit(PlaceBlueprintCommand{"RemoveWindowTool", "", at, Rot4::North});
    sim->step();
    CHECK(sim->designations().at(at) == DesignationKind::RemoveWindow);
    runUntil([&] { return sim->windows().at(at) == nullptr; });
    CHECK(sim->windows().at(at) == nullptr);
    CHECK(sim->map().obstacleDm(at) == kObstacleTallDm);
    CHECK(sim->stats().failedJobs == 0);
}

TEST_CASE("Windows: a save keeps the window, its broken glass and the wall's height") {
    auto sim = windowSim();
    const Entity a = sim->things().spawn("Wall", CellPos{20, 20, 0});
    const Entity b = sim->things().spawn("Wall", CellPos{21, 20, 0});
    sim->things().spawn("Wall", CellPos{22, 20, 0});
    WINDOW_REQUIRE(sim->windows().add(CellPos{20, 20, 0}));
    WINDOW_REQUIRE(sim->windows().add(CellPos{21, 20, 0}));
    sim->combat().breakWindow(b, 21.5f, 20.5f, 1.4f);
    sim->step();
    SaveLoadResult loaded = loadSim(saveSim(*sim), windowDefs());
    WINDOW_REQUIRE(loaded.sim != nullptr);
    Sim& again = *loaded.sim;
    CHECK(again.stateHash() == sim->stateHash());
    WINDOW_REQUIRE(again.windows().at(CellPos{20, 20, 0}) != nullptr);
    CHECK_FALSE(again.windows().at(CellPos{20, 20, 0})->broken);
    WINDOW_REQUIRE(again.windows().at(CellPos{21, 20, 0}) != nullptr);
    CHECK(again.windows().at(CellPos{21, 20, 0})->broken);
    CHECK(again.windows().at(CellPos{22, 20, 0}) == nullptr);
    CHECK(again.map().obstacleDm(CellPos{20, 20, 0}) == 9);
    CHECK(again.map().obstacleDm(CellPos{22, 20, 0}) == kObstacleTallDm);
    (void)a;
    again.stepMany(50);
    sim->stepMany(50);
    CHECK(again.stateHash() == sim->stateHash());
}
