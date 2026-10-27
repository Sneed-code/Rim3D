// Stairs and ladders (task M2, docs/design/levels.md): links between levels, regions and paths that
// climb them, climbing speed, upper floors built with work and materials, and stockpiles, doors and
// work on upper levels.

#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"
#include "ColonySim/UpperFloors.h"
#include "ColonySim/Zones.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
// The condition is worked out once, so it can spawn things.
#define STAIRS_REQUIRE(...)                                   \
    do {                                                      \
        const bool stairsOk = static_cast<bool>(__VA_ARGS__); \
        CHECK_MESSAGE(stairsOk, #__VA_ARGS__);                \
        if (!stairsOk) {                                      \
            return;                                           \
        }                                                     \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> stairsDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

std::unique_ptr<Sim> stairsSim(int size = 48) {
    return std::make_unique<Sim>(SimSettings{5, size, 0, 0, true}, stairsDefs());
}

// The house every test uses: a ring of walls round 10..16 by 10..16 with a gap at (13, 10), an
// upper floor over 9..18 by 9..17 held up by it, and stairs at x = 18 climbing north from (18, 9)
// on the ground to (18, 13) upstairs.
constexpr CellPos kStairsLow{18, 9, 0};
constexpr CellPos kStairsHigh{18, 13, 1};

Entity stairsHouse(Sim& sim, const char* climb = "Stairs") {
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if ((x == 10 || x == 16 || y == 10 || y == 16) && !(x == 13 && y == 10)) {
                CHECK(sim.things().spawn("Wall", CellPos{x, y, 0}) != kNoEntity);
            }
        }
    }
    sim.rooms().update();
    for (int y = 9; y <= 17; ++y) {
        for (int x = 9; x <= 18; ++x) {
            CHECK(sim.upperFloors().lay(CellPos{x, y, 1}));
        }
    }
    sim.rooms().update();
    const bool ladder = std::string(climb) == "Ladder";
    // A ladder is one cell: at (18, 12) it climbs from (18, 11) to (18, 13).
    return sim.things().spawn(climb, ladder ? CellPos{18, 12, 0} : CellPos{18, 10, 0}, 1, Rot4::North);
}

Entity stairsSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && e.cell == cell && sim.defs().get(e.def).defName() == def) {
            return e.thing;
        }
    }
    return kNoEntity;
}

// Sends a pawn with no job system along the path the pathfinder finds; false if it finds none.
bool stairsWalk(Sim& sim, Entity pawn, CellPos goal) {
    entt::registry& reg = sim.things().registry();
    const Position& pos = reg.get<Position>(pawn);
    std::vector<CellPos> path;
    if (!sim.pathfinder().findPath(cellOf(pos), goal, path)) {
        return false;
    }
    smoothPath(sim.map(), pos.x, pos.y, path, pos.level);
    reg.get<Mover>(pawn).walkTo(pos.x, pos.y, std::move(path));
    return true;
}

}  // namespace

TEST_CASE("Stairs: a staircase and a ladder link the cell in front of the foot to the cell beyond the head, one level up") {
    const std::vector<LevelLink> north = levelLinksOf(CellRect{18, 10, 1, 3, 0}, Rot4::North, kNoEntity, 0.6f);
    STAIRS_REQUIRE(north.size() == 1);
    CHECK(north[0].low == CellPos{18, 9, 0});
    CHECK(north[0].high == CellPos{18, 13, 1});
    CHECK(north[0].length == doctest::Approx(5.0f));  // 4 m along, 3 m up
    CHECK(north[0].cost() == doctest::Approx(5.0f / 0.6f));

    const std::vector<LevelLink> south = levelLinksOf(CellRect{5, 5, 1, 3, 1}, Rot4::South, kNoEntity, 0.6f);
    STAIRS_REQUIRE(south.size() == 1);
    CHECK(south[0].low == CellPos{5, 8, 1});
    CHECK(south[0].high == CellPos{5, 4, 2});
    // East and West: the footprint lies along x.
    const std::vector<LevelLink> east = levelLinksOf(footprintRect(CellPos{5, 5, 0}, 1, 3, Rot4::East), Rot4::East, kNoEntity, 0.6f);
    STAIRS_REQUIRE(east.size() == 1);
    CHECK(east[0].low == CellPos{4, 5, 0});
    CHECK(east[0].high == CellPos{8, 5, 1});
    const std::vector<LevelLink> west = levelLinksOf(footprintRect(CellPos{5, 5, 0}, 1, 3, Rot4::West), Rot4::West, kNoEntity, 0.6f);
    STAIRS_REQUIRE(west.size() == 1);
    CHECK(west[0].low == CellPos{8, 5, 0});
    CHECK(west[0].high == CellPos{4, 5, 1});

    const std::vector<LevelLink> ladder = levelLinksOf(CellRect{3, 3, 1, 1, 0}, Rot4::North, kNoEntity, 0.3f);
    STAIRS_REQUIRE(ladder.size() == 1);
    CHECK(ladder[0].low == CellPos{3, 2, 0});
    CHECK(ladder[0].high == CellPos{3, 4, 1});
    CHECK(ladder[0].length == doctest::Approx(std::sqrt(13.0f)));
    // Nothing above the top level to climb to.
    CHECK(levelLinksOf(CellRect{3, 3, 1, 1, Map::kLevels - 1}, Rot4::North, kNoEntity, 0.3f).empty());

    // The defs: stairs keep 60% of walking speed, a ladder 30% (ours).
    auto sim = stairsSim();
    const DefDatabase& defs = sim->defs();
    CHECK(defs.get(defs.idOf("Things", "Stairs")).number("building.climb.speedFactor") == doctest::Approx(0.6));
    CHECK(defs.get(defs.idOf("Things", "Ladder")).number("building.climb.speedFactor") == doctest::Approx(0.3));
}

TEST_CASE("Stairs: built stairs join the ground and the upper floor in one reachability group, and taking them away parts them") {
    auto sim = stairsSim();
    Regions& regions = sim->regions();
    // Without stairs the upper floor is a group of its own.
    stairsHouse(*sim, "Wall");  // a wall where the stairs go: no link
    CHECK_FALSE(regions.canReach(CellPos{20, 5, 0}, CellPos{12, 12, 1}));
    const size_t parted = regions.groupCount();
    sim->things().despawn(sim->map().buildingAt(CellPos{18, 10, 0}));

    const Entity stairs = sim->things().spawn("Stairs", CellPos{18, 10, 0}, 1, Rot4::North);
    STAIRS_REQUIRE(stairs != kNoEntity);
    STAIRS_REQUIRE(sim->map().levelLinks().size() == 1);
    CHECK(sim->map().levelLinks()[0].building == stairs);
    CHECK(sim->map().levelLinkBetween(kStairsHigh, kStairsLow) == &sim->map().levelLinks()[0]);
    CHECK_FALSE(sim->map().walkable(CellPos{18, 11, 0}));  // the stairs themselves are climbed, not walked
    CHECK(regions.canReach(CellPos{20, 5, 0}, CellPos{12, 12, 1}));
    CHECK(regions.canReach(CellPos{12, 12, 1}, CellPos{12, 12, 0}));  // and on into the house through its gap
    CHECK(regions.groupCount() == parted - 1);
    const RegionId low = regions.regionAt(kStairsLow);
    const RegionId high = regions.regionAt(kStairsHigh);
    STAIRS_REQUIRE(low != kNoRegion);
    STAIRS_REQUIRE(high != kNoRegion);
    CHECK(regions.region(low).climbs);
    CHECK(regions.region(high).climbs);
    const auto link = std::find_if(regions.region(low).links.begin(), regions.region(low).links.end(),
                                   [high](const RegionLink& l) { return l.other == high; });
    STAIRS_REQUIRE(link != regions.region(low).links.end());
    CHECK(link->x == doctest::Approx(18.5f));
    CHECK(link->y == doctest::Approx(11.5f));
    CHECK(link->extra == doctest::Approx(5.0f / 0.6f - 4.0f));

    sim->things().despawn(stairs);
    CHECK(sim->map().levelLinks().empty());
    CHECK_FALSE(regions.canReach(CellPos{20, 5, 0}, CellPos{12, 12, 1}));
    CHECK(regions.groupCount() == parted);
}

TEST_CASE("Stairs: paths climb stairs from the ground to the upper floor and down again, queued or found at once") {
    auto sim = stairsSim();
    STAIRS_REQUIRE(stairsHouse(*sim) != kNoEntity);
    std::vector<CellPos> path;
    STAIRS_REQUIRE(sim->pathfinder().findPath(CellPos{25, 3, 0}, CellPos{12, 12, 1}, path));
    // The path reaches the foot of the stairs on the ground and goes on from their head upstairs.
    const auto foot = std::find(path.begin(), path.end(), kStairsLow);
    STAIRS_REQUIRE(foot != path.end());
    STAIRS_REQUIRE(foot + 1 != path.end());
    CHECK(*(foot + 1) == kStairsHigh);
    for (auto it = path.begin(); it != path.end(); ++it) {
        CHECK(it->level == (it <= foot ? 0 : 1));
        CHECK(sim->map().walkable(*it));
    }
    CHECK(path.back() == CellPos{12, 12, 1});

    // The full-grid search agrees on the cost: the same number of cells either way here.
    std::vector<CellPos> full;
    STAIRS_REQUIRE(sim->pathfinder().findPathFullGrid(CellPos{25, 3, 0}, CellPos{12, 12, 1}, full));
    CHECK(std::find(full.begin(), full.end(), kStairsHigh) != full.end());

    // Upstairs to inside the house below: down the stairs and in through the gap.
    STAIRS_REQUIRE(sim->pathfinder().findPath(CellPos{11, 16, 1}, CellPos{13, 13, 0}, path));
    const auto head = std::find(path.begin(), path.end(), kStairsHigh);
    STAIRS_REQUIRE(head != path.end());
    STAIRS_REQUIRE(head + 1 != path.end());
    CHECK(*(head + 1) == kStairsLow);
    CHECK(std::find(path.begin(), path.end(), CellPos{13, 10, 0}) != path.end());

    // Through the queue too, within the step budget.
    const PathTicket ticket = sim->pathfinder().request(CellPos{25, 3, 0}, CellPos{12, 12, 1});
    CHECK(sim->pathfinder().status(ticket) == PathStatus::Queued);
    sim->pathfinder().update();
    CHECK(sim->pathfinder().status(ticket) == PathStatus::Found);
    CHECK(sim->pathfinder().takePath(ticket, path));
    CHECK(path.back() == CellPos{12, 12, 1});
    CHECK(sim->pathfinder().stats().failedSearches == 0);

    // Straightening keeps both ends of the climb.
    smoothPath(sim->map(), 25.5f, 3.5f, path, 0);
    const auto kept = std::find(path.begin(), path.end(), kStairsLow);
    STAIRS_REQUIRE(kept != path.end());
    STAIRS_REQUIRE(kept + 1 != path.end());
    CHECK(*(kept + 1) == kStairsHigh);
}

TEST_CASE("Stairs: a pawn walks up the stairs at their own speed, slower up a ladder, and the snapshot shows it on the way") {
    for (const std::string climb : {"Stairs", "Ladder"}) {
        CAPTURE(climb);
        auto sim = stairsSim();
        STAIRS_REQUIRE(stairsHouse(*sim, climb.c_str()) != kNoEntity);
        STAIRS_REQUIRE(sim->map().levelLinks().size() == 1);
        const LevelLink link = sim->map().levelLinks()[0];
        const Entity pawn = sim->things().spawn("Human", CellPos{link.low.x, link.low.y - 3, 0});
        STAIRS_REQUIRE(pawn != kNoEntity);
        STAIRS_REQUIRE(stairsWalk(*sim, pawn, CellPos{12, 12, 1}));
        entt::registry& reg = sim->things().registry();
        const float speed = reg.get<Mover>(pawn).metresPerStep;

        int climbStart = -1;
        int climbEnd = -1;
        bool sawMidway = false;
        for (int step = 0; step < 600 && climbEnd < 0; ++step) {
            sim->step();
            const Position& pos = reg.get<Position>(pawn);
            const Mover& mover = reg.get<Mover>(pawn);
            if (climbStart < 0 && mover.climbed > 0.0f) {
                climbStart = step;
                CHECK(cellOf(pos) == link.low);
            }
            if (climbStart >= 0 && pos.level == 1) {
                climbEnd = step;
                CHECK(cellOf(pos) == link.high);
            }
            if (!sawMidway && mover.climbed > 0.4f * link.length && mover.climbed < 0.6f * link.length) {
                sawMidway = true;
                Snapshot snap;
                sim->writeSnapshot(snap);
                for (const ThingView& view : snap.things) {
                    if (view.id == pawn) {
                        CHECK(view.climb > 0.3f);
                        CHECK(view.climb < 0.7f);
                        CHECK(view.level == 0);
                        CHECK(view.z > sim->map().heightAt(pos.x, pos.y) + 0.8f);
                        CHECK(view.z < sim->map().heightAt(pos.x, pos.y) + 2.2f);
                        CHECK(std::abs(view.y - pos.y) > 0.5f);  // on its way to the other end
                    }
                }
            }
        }
        STAIRS_REQUIRE(climbStart >= 0);
        STAIRS_REQUIRE(climbEnd >= 0);
        CHECK(sawMidway);
        // The climb takes its length at the link's share of the pawn's walking speed (a little
        // longer in the dark, which slows every walk, docs/design/light.md).
        const int expected = static_cast<int>(std::ceil(link.length / (speed * link.speedFactor)));
        CHECK(climbEnd - climbStart >= expected - 2);
        CHECK(climbEnd - climbStart <= expected * 5 / 4 + 2);

        // On to the goal upstairs.
        for (int step = 0; step < 300 && !reg.get<Mover>(pawn).arrived(); ++step) {
            sim->step();
        }
        CHECK(reg.get<Mover>(pawn).arrived());
        CHECK(cellOf(reg.get<Position>(pawn)) == CellPos{12, 12, 1});
    }
}

TEST_CASE("Stairs: a pawn whose stairs are taken away while it climbs stays at their foot and gives up the walk") {
    auto sim = stairsSim();
    const Entity stairs = stairsHouse(*sim);
    STAIRS_REQUIRE(stairs != kNoEntity);
    const Entity pawn = sim->things().spawn("Human", CellPos{18, 7, 0});
    STAIRS_REQUIRE(stairsWalk(*sim, pawn, CellPos{12, 12, 1}));
    entt::registry& reg = sim->things().registry();
    for (int step = 0; step < 300 && reg.get<Mover>(pawn).climbed <= 0.0f; ++step) {
        sim->step();
    }
    STAIRS_REQUIRE(reg.get<Mover>(pawn).climbed > 0.0f);
    sim->things().despawn(stairs);
    sim->step();
    const Mover& mover = reg.get<Mover>(pawn);
    CHECK(mover.gaveUp);
    CHECK(mover.arrived());
    CHECK(mover.climbed == 0.0f);
    CHECK(cellOf(reg.get<Position>(pawn)) == kStairsLow);
}

TEST_CASE("Stairs: a game saved halfway up the stairs loads and climbs on the same") {
    auto sim = stairsSim();
    STAIRS_REQUIRE(stairsHouse(*sim) != kNoEntity);
    const Entity pawn = sim->things().spawn("Human", CellPos{18, 6, 0});
    STAIRS_REQUIRE(stairsWalk(*sim, pawn, CellPos{12, 12, 1}));
    entt::registry& reg = sim->things().registry();
    for (int step = 0; step < 300 && reg.get<Mover>(pawn).climbed < 1.0f; ++step) {
        sim->step();
    }
    STAIRS_REQUIRE(reg.get<Mover>(pawn).climbed >= 1.0f);
    const SaveLoadResult loaded = loadSim(saveSim(*sim), stairsDefs());
    STAIRS_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.map().levelLinks().size() == 1);
    CHECK(copy.things().registry().get<Mover>(pawn).climbed == reg.get<Mover>(pawn).climbed);
    CHECK(copy.stateHash() == sim->stateHash());
    for (int i = 0; i < 120; ++i) {
        sim->step();
        copy.step();
    }
    CHECK(reg.get<Position>(pawn).level == 1);
    CHECK(copy.things().registry().get<Position>(pawn).level == 1);
    CHECK(copy.stateHash() == sim->stateHash());
}

TEST_CASE("Stairs: stairs and ladders are planned like any building, but not on the top level") {
    auto sim = stairsSim();
    std::string error;
    const DefDatabase& defs = sim->defs();
    const DefId stairs = defs.idOf("Things", "Stairs");
    const DefId wood = defs.idOf("Things", "WoodLog");
    CHECK(sim->construction().placeBlueprint(stairs, wood, CellPos{30, 30, 0}, Rot4::East, error) != kNoEntity);
    CHECK(sim->construction().costOf(stairs, wood).front().count == 30);
    // Up on the top level there is nowhere to climb to (a floor there needs walls under it first,
    // so ask the rule straight).
    CHECK(sim->construction().placeBlueprint(stairs, wood, CellPos{30, 30, Map::kLevels - 1}, Rot4::North, error) == kNoEntity);
    CHECK(error.find("no level above") != std::string::npos);
    // A ladder can't be made of stone.
    CHECK_FALSE(sim->construction().stuffFits(defs.idOf("Things", "Ladder"), defs.idOf("Things", "BlocksGranite")));
}

TEST_CASE("Stairs: a colonist builds an upper floor with materials and work, from the level below") {
    auto sim = stairsSim();
    // Walls round 10..16 hold up a floor; nothing upstairs yet, and no stairs.
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if ((x == 10 || x == 16 || y == 10 || y == 16) && !(x == 13 && y == 10)) {
                STAIRS_REQUIRE(sim->things().spawn("Wall", CellPos{x, y, 0}) != kNoEntity);
            }
        }
    }
    sim->rooms().update();
    const Entity pawn = stairsSpawn(*sim, "Human", CellPos{20, 5, 0});
    STAIRS_REQUIRE(pawn != kNoEntity);
    STAIRS_REQUIRE(stairsSpawn(*sim, "WoodLog", CellPos{22, 5, 0}, 20) != kNoEntity);

    // Unsupported floors can't be planned; a floor over the house can.
    sim->submit(PlaceBlueprintCommand{"UpperFloor", "WoodLog", CellPos{40, 40, 1}, Rot4::North});
    sim->step();
    bool refused = false;
    for (const Event& e : sim->events()) {
        refused = refused || (e.type == EventType::CommandFailed && e.message.find("holds it up") != std::string::npos);
    }
    CHECK(refused);
    sim->submit(PlaceBlueprintCommand{"UpperFloor", "WoodLog", CellPos{13, 13, 1}, Rot4::North});
    sim->step();
    const Entity site = sim->construction().siteAt(CellPos{13, 13, 1});
    STAIRS_REQUIRE(site != kNoEntity);
    CHECK(sim->construction().isUpperFloor(sim->things().registry().get<Thing>(site).def));
    // The colonist stands under it to work: the only cells beside it upstairs are air.
    CellPos stand;
    STAIRS_REQUIRE(sim->construction().workCell(sim->things().registry().get<ConstructionSite>(site).rect(), CellPos{20, 5, 0}, stand));
    CHECK(stand.level == 0);

    int steps = -1;
    for (int i = 0; i < 2000; ++i) {
        sim->step();
        if (sim->map().upperFloor(CellPos{13, 13, 1})) {
            steps = i;
            break;
        }
    }
    STAIRS_REQUIRE(steps >= 0);
    CHECK(steps > static_cast<int>(216.0 / 1.7));  // the work alone at construction speed 1
    CHECK(sim->construction().siteAt(CellPos{13, 13, 1}) == kNoEntity);
    CHECK(sim->map().roof(CellPos{13, 13, 0}) == Roof::Thin);
    // Four logs went into it.
    int logs = 0;
    const entt::registry& reg = sim->things().registry();
    for (const Entity item : reg.view<const Thing, const StackCount>()) {
        if (sim->defs().get(reg.get<Thing>(item).def).defName() == "WoodLog") {
            logs += reg.get<StackCount>(item).count;
        }
    }
    CHECK(logs == 16);
    CHECK(sim->stats().failedJobs == 0);
}

TEST_CASE("Stairs: colonists haul to a stockpile upstairs, up the stairs, and walk through a door up there") {
    auto sim = stairsSim();
    STAIRS_REQUIRE(stairsHouse(*sim) != kNoEntity);
    // An upstairs room behind a door: walls across the floor at x = 14 with a door at (14, 13).
    for (int y = 9; y <= 17; ++y) {
        STAIRS_REQUIRE(sim->things().spawn(y == 13 ? "Door" : "Wall", CellPos{14, y, 1}) != kNoEntity);
    }
    sim->rooms().update();
    CHECK(sim->regions().region(sim->regions().regionAt(CellPos{14, 13, 1})).kind == RegionKind::Door);
    // A stockpile on the far side of the door, upstairs.
    sim->submit(CreateStockpileCommand{CellRect{10, 11, 3, 3, 1}});
    sim->step();
    STAIRS_REQUIRE(sim->zones().stockpiles().size() == 1);
    const ZoneId zone = sim->zones().stockpiles()[0].id;
    CHECK(sim->zones().zoneAt(CellPos{11, 12, 1}) == zone);
    CHECK(sim->zones().zoneAt(CellPos{11, 12, 0}) == kNoZone);
    STAIRS_REQUIRE(stairsSpawn(*sim, "Human", CellPos{24, 5, 0}) != kNoEntity);
    STAIRS_REQUIRE(stairsSpawn(*sim, "Steel", CellPos{22, 4, 0}, 30) != kNoEntity);

    bool stored = false;
    for (int i = 0; i < 1500 && !stored; ++i) {
        sim->step();
        for (const CellPos& c : sim->zones().find(zone)->cells) {
            stored = stored || !sim->things().itemsAt(c).empty();
        }
    }
    CHECK(stored);
    CHECK(sim->stats().failedJobs == 0);

    // Zones on upper levels are saved and come back.
    const SaveLoadResult loaded = loadSim(saveSim(*sim), stairsDefs());
    STAIRS_REQUIRE(loaded.sim != nullptr);
    CHECK(loaded.sim->zones().zoneAt(CellPos{11, 12, 1}) == zone);
    CHECK(loaded.sim->stateHash() == sim->stateHash());
}

TEST_CASE("Stairs: an upper corner wall whose two sides stand is built from the ground beside it") {
    auto sim = stairsSim();
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            if (x == 10 || x == 16 || y == 10 || y == 16) {
                STAIRS_REQUIRE(sim->things().spawn("Wall", CellPos{x, y, 0}) != kNoEntity);
            }
        }
    }
    sim->rooms().update();
    for (int y = 10; y <= 16; ++y) {
        for (int x = 10; x <= 16; ++x) {
            STAIRS_REQUIRE(sim->upperFloors().lay(CellPos{x, y, 1}));
        }
    }
    STAIRS_REQUIRE(sim->things().spawn("Wall", CellPos{11, 10, 1}) != kNoEntity);
    STAIRS_REQUIRE(sim->things().spawn("Wall", CellPos{10, 11, 1}) != kNoEntity);
    std::string error;
    const DefDatabase& defs = sim->defs();
    STAIRS_REQUIRE(sim->construction().placeBlueprint(defs.idOf("Things", "Wall"), defs.idOf("Things", "WoodLog"),
                                                      CellPos{10, 10, 1}, Rot4::North, error) != kNoEntity);
    const Entity site = sim->construction().siteAt(CellPos{10, 10, 1});
    STAIRS_REQUIRE(site != kNoEntity);
    CellPos stand;
    STAIRS_REQUIRE(sim->construction().workCell(sim->things().registry().get<ConstructionSite>(site).rect(), CellPos{20, 5, 0}, stand));
    CHECK(stand.level == 0);
    CHECK(std::abs(stand.x - 10) <= 1);
    CHECK(std::abs(stand.y - 10) <= 1);
}
