// The build menu and placing plans (docs/design/build-menu.md): BuildPlanner's menu, drag shapes and
// previews, and the remove-floor order.

#include "ColonySim/BuildPlanner.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Events.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Map.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace colony;

#define BUILDPLAN_REQUIRE(...) \
    do {                       \
        CHECK((__VA_ARGS__));  \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

std::shared_ptr<const DefDatabase> buildPlanDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim buildPlanSim() { return Sim(SimSettings{11, 48, 0, 0, true}, buildPlanDefs()); }

void buildPlanSpawn(Sim& sim, const std::string& def, CellPos cell, int count = 1) {
    sim.submit(SpawnThingCommand{def, cell, count});
    sim.step();
}

const BuildMenuEntry* buildPlanFind(const std::vector<BuildMenuCategory>& menu, const std::string& defName, std::string* category = nullptr) {
    for (const BuildMenuCategory& c : menu) {
        for (const BuildMenuEntry& e : c.entries) {
            if (e.defName == defName) {
                if (category != nullptr) {
                    *category = c.name;
                }
                return &e;
            }
        }
    }
    return nullptr;
}

int buildPlanItems(const Sim& sim, const std::string& def) {
    const DefId id = sim.defs().idOf("Things", def);
    int total = 0;
    sim.things().registry().view<const Thing, const StackCount>().each(
        [&](const Thing& thing, const StackCount& stack) { total += thing.def == id ? stack.count : 0; });
    return total;
}

}  // namespace

TEST_CASE("Build planner: drag shapes") {
    const CellPos a{10, 10, 0};
    // One cell: only where the drag ends.
    CHECK(BuildPlanner::dragCells(a, CellPos{14, 12, 0}, DragShape::Single) == std::vector<CellPos>{CellPos{14, 12, 0}});
    // A line runs the longer way from the start.
    const std::vector<CellPos> line = BuildPlanner::dragCells(a, CellPos{14, 12, 0}, DragShape::Line);
    BUILDPLAN_REQUIRE(line.size() == 5);
    CHECK(line.front() == a);
    CHECK(line.back() == CellPos{14, 10, 0});
    CHECK(BuildPlanner::dragCells(a, CellPos{9, 4, 0}, DragShape::Line).back() == CellPos{10, 4, 0});
    // A 5 x 4 rectangle: 14 cells round the edge, 20 filled; dragged either way.
    CHECK(BuildPlanner::dragCells(a, CellPos{14, 13, 0}, DragShape::Outline).size() == 14);
    CHECK(BuildPlanner::dragCells(a, CellPos{6, 7, 0}, DragShape::Outline).size() == 14);
    CHECK(BuildPlanner::dragCells(a, CellPos{14, 13, 0}, DragShape::Filled).size() == 20);
    // Nothing doubled, even on a one-cell-wide outline.
    const std::vector<CellPos> thin = BuildPlanner::dragCells(a, CellPos{10, 15, 0}, DragShape::Outline);
    CHECK(thin.size() == 6);
    CHECK(std::set<std::pair<int, int>>([&] {
              std::set<std::pair<int, int>> s;
              for (const CellPos& c : thin) {
                  s.insert({c.x, c.y});
              }
              return s;
          }()).size() == 6);
    // A 2 x 1 footprint steps two cells at a time.
    const std::vector<CellPos> beds = BuildPlanner::dragCells(a, CellPos{15, 10, 0}, DragShape::Line, 2, 1);
    BUILDPLAN_REQUIRE(beds.size() == 3);
    CHECK(beds[1] == CellPos{12, 10, 0});
}

TEST_CASE("Build planner: the menu") {
    Sim sim = buildPlanSim();
    const BuildPlanner planner(sim);
    const std::vector<BuildMenuCategory> menu = planner.menu();
    BUILDPLAN_REQUIRE(!menu.empty());
    CHECK(menu.front().name == "Structure");
    for (const BuildMenuCategory& c : menu) {
        CHECK(!c.entries.empty());
        if (c.name == "Joy") {
            CHECK(c.label == "Recreation");
        }
    }
    std::string category;
    const BuildMenuEntry* wall = buildPlanFind(menu, "Wall", &category);
    BUILDPLAN_REQUIRE(wall != nullptr);
    CHECK(category == "Structure");
    CHECK(wall->label == "Wall");
    CHECK(wall->drag == DragShape::Outline);
    CHECK(wall->needsStuff);
    CHECK(wall->locked.empty());
    CHECK(!wall->floor);
    const BuildMenuEntry* bed = buildPlanFind(menu, "Bed");
    BUILDPLAN_REQUIRE(bed != nullptr);
    CHECK(bed->drag == DragShape::Single);
    CHECK(bed->sizeY == 2);
    const BuildMenuEntry* floor = buildPlanFind(menu, "WoodFloor", &category);
    BUILDPLAN_REQUIRE(floor != nullptr);
    CHECK(category == "Floors");
    CHECK(floor->floor);
    CHECK(floor->drag == DragShape::Filled);
    CHECK(!floor->needsStuff);
    // Smoothed stone is made by smoothing, not laid; a ruin's wall is never built.
    CHECK(buildPlanFind(menu, "SmoothedStone") == nullptr);
    CHECK(buildPlanFind(menu, "AncientWall") == nullptr);
    // Stone tiles wait for Stonecutting, and say so.
    const BuildMenuEntry* tiles = buildPlanFind(menu, "StoneTile");
    BUILDPLAN_REQUIRE(tiles != nullptr);
    if (!sim.research().unlocked(sim.defs().get(sim.defs().idOf("Terrain", "StoneTile")))) {
        CHECK(!tiles->locked.empty());
        CHECK(!planner.preview("StoneTile", "BlocksGranite", CellPos{5, 5, 0}, CellPos{6, 6, 0}, Rot4::North, DragShape::Filled).whyNot.empty());
        sim.submit(UnlockResearchCommand{"Stonecutting"});
        sim.step();
        const BuildMenuEntry* now = buildPlanFind(planner.menu(), "StoneTile");
        BUILDPLAN_REQUIRE(now != nullptr);
        CHECK(now->locked.empty());
    }
}

TEST_CASE("Build planner: materials, the most on the map first") {
    Sim sim = buildPlanSim();
    buildPlanSpawn(sim, "WoodLog", CellPos{5, 5, 0}, 30);
    buildPlanSpawn(sim, "Steel", CellPos{6, 5, 0}, 70);
    buildPlanSpawn(sim, "Steel", CellPos{7, 5, 0}, 50);
    const BuildPlanner planner(sim);
    const std::vector<StuffChoice> choices = planner.stuffChoices("Wall");
    BUILDPLAN_REQUIRE(choices.size() >= 2);
    CHECK(choices[0].defName == "Steel");
    CHECK(choices[0].onMap == 120);
    CHECK(choices[1].defName == "WoodLog");
    CHECK(choices[1].onMap == 30);
    CHECK(planner.bestStuff("Wall") == "Steel");
    CHECK(planner.stuffChoices("FueledStove").empty());
    CHECK(planner.bestStuff("FueledStove").empty());
}

TEST_CASE("Build planner: a room's walls, with a blocked cell skipped, are planned and built") {
    Sim sim = buildPlanSim();
    buildPlanSpawn(sim, "Human", CellPos{8, 8, 0});
    buildPlanSpawn(sim, "Steel", CellPos{5, 5, 0}, 75);
    // A wall already standing in the way (built at once by a dev spawn).
    buildPlanSpawn(sim, "Wall", CellPos{24, 20, 0});
    const BuildPlanner planner(sim);
    const BuildPreview preview = planner.preview("Wall", "Steel", CellPos{20, 20, 0}, CellPos{24, 24, 0}, Rot4::North, DragShape::Outline);
    CHECK(preview.whyNot.empty());
    CHECK(preview.spots.size() == 16);
    CHECK(preview.plannable == 15);
    int blocked = 0;
    for (const PlannedSpot& spot : preview.spots) {
        if (!spot.whyNot.empty()) {
            ++blocked;
            CHECK(spot.cell == CellPos{24, 20, 0});
        }
    }
    CHECK(blocked == 1);
    BUILDPLAN_REQUIRE(preview.cost.size() == 1);
    CHECK(preview.cost[0].defName == "Steel");
    CHECK(preview.cost[0].needed == 75);
    CHECK(preview.cost[0].onMap == 75);
    CHECK(preview.affordable());

    for (Command& command : BuildPlanner::commands(preview, "Wall", "Steel", Rot4::North)) {
        sim.submit(std::move(command));
    }
    sim.step();
    for (const Event& e : sim.events()) {
        CHECK(e.type != EventType::CommandFailed);
    }
    CHECK(sim.construction().siteAt(CellPos{20, 20, 0}) != kNoEntity);
    CHECK(sim.construction().siteAt(CellPos{22, 22, 0}) == kNoEntity);  // the middle stays open
    // The same drag again plans nothing: every cell has a plan or a wall now.
    CHECK(planner.preview("Wall", "Steel", CellPos{20, 20, 0}, CellPos{24, 24, 0}, Rot4::North, DragShape::Outline).plannable == 0);

    int steps = 0;
    while (steps < 4 * 3600 && sim.construction().siteAt(CellPos{20, 24, 0}) != kNoEntity) {
        sim.step();
        ++steps;
    }
    CHECK(sim.map().buildingAt(CellPos{20, 24, 0}) != kNoEntity);
}

TEST_CASE("Build planner: beds dragged out never overlap, and a locked or wrong order plans nothing") {
    Sim sim = buildPlanSim();
    const BuildPlanner planner(sim);
    // Facing East a bed is 2 x 1: a 6-cell line holds three.
    const BuildPreview beds = planner.preview("Bed", "WoodLog", CellPos{10, 10, 0}, CellPos{15, 10, 0}, Rot4::East, DragShape::Line);
    CHECK(beds.plannable == 3);
    // Dragged back the other way they cover the cursor and still don't overlap.
    const BuildPreview back = planner.preview("Bed", "WoodLog", CellPos{30, 10, 0}, CellPos{25, 10, 0}, Rot4::East, DragShape::Line);
    CHECK(back.plannable == 3);
    CHECK(back.spots.front().cell == CellPos{29, 10, 0});
    // A filled area of beds facing North (1 x 2): rows two cells apart.
    CHECK(planner.preview("Bed", "WoodLog", CellPos{10, 20, 0}, CellPos{12, 23, 0}, Rot4::North, DragShape::Filled).plannable == 6);
    // Wrong material, unknown building, a building that takes none given one.
    CHECK(!planner.preview("Wall", "Silver", CellPos{1, 1, 0}, CellPos{1, 1, 0}, Rot4::North, DragShape::Single).whyNot.empty());
    CHECK(!planner.preview("NoSuchThing", "", CellPos{1, 1, 0}, CellPos{1, 1, 0}, Rot4::North, DragShape::Single).whyNot.empty());
    CHECK(BuildPlanner::commands(planner.preview("FueledStove", "Steel", CellPos{1, 1, 0}, CellPos{1, 1, 0}, Rot4::North, DragShape::Single),
                                 "FueledStove", "Steel", Rot4::North)
              .empty());
}

TEST_CASE("Build planner: a floor area is planned cell by cell and laid") {
    Sim sim = buildPlanSim();
    buildPlanSpawn(sim, "Human", CellPos{8, 8, 0});
    buildPlanSpawn(sim, "WoodLog", CellPos{5, 5, 0}, 40);
    const BuildPlanner planner(sim);
    const BuildPreview preview = planner.preview("WoodFloor", "", CellPos{20, 20, 0}, CellPos{22, 21, 0}, Rot4::North, DragShape::Filled);
    CHECK(preview.plannable == 6);
    BUILDPLAN_REQUIRE(preview.cost.size() == 1);
    CHECK(preview.cost[0].needed == 18);
    for (Command& command : BuildPlanner::commands(preview, "WoodFloor", "", Rot4::North)) {
        sim.submit(std::move(command));
    }
    sim.step();
    const DefId wood = sim.defs().idOf("Terrain", "WoodFloor");
    int steps = 0;
    while (steps < 4 * 3600 && sim.floors().at(CellPos{22, 21, 0}) != wood) {
        sim.step();
        ++steps;
    }
    CHECK(sim.floors().at(CellPos{20, 20, 0}) == wood);
    CHECK(sim.floors().at(CellPos{22, 21, 0}) == wood);
    // Laid already: dragging the same floor again plans nothing.
    CHECK(planner.preview("WoodFloor", "", CellPos{20, 20, 0}, CellPos{22, 21, 0}, Rot4::North, DragShape::Filled).plannable == 0);
}

TEST_CASE("Remove floor: a colonist takes a marked floor up and gets some of it back") {
    Sim sim = buildPlanSim();
    sim.submit(PlaceFloorCommand{CellRect{20, 20, 2, 1, 0}, "WoodFloor"});
    sim.step();
    const DefId wood = sim.defs().idOf("Terrain", "WoodFloor");
    BUILDPLAN_REQUIRE(sim.floors().at(CellPos{20, 20, 0}) == wood);
    // Only cells with a floor take the mark.
    sim.submit(DesignateCommand{CellRect{30, 30, 2, 2, 0}, DesignationKind::RemoveFloor});
    sim.step();
    bool refused = false;
    for (const Event& e : sim.events()) {
        refused = refused || e.type == EventType::CommandFailed;
    }
    CHECK(refused);
    sim.submit(DesignateCommand{CellRect{19, 19, 4, 3, 0}, DesignationKind::RemoveFloor});
    sim.step();
    CHECK(sim.designations().countOf(DesignationKind::RemoveFloor) == 2);

    buildPlanSpawn(sim, "Human", CellPos{8, 8, 0});
    int steps = 0;
    while (steps < 2 * 3600 && sim.designations().countOf(DesignationKind::RemoveFloor) > 0) {
        sim.step();
        ++steps;
    }
    CHECK(sim.designations().countOf(DesignationKind::RemoveFloor) == 0);
    CHECK(!sim.floors().at(CellPos{20, 20, 0}).valid());
    CHECK(!sim.floors().at(CellPos{21, 20, 0}).valid());
    // Half of 3 logs a floor, rounded at random: 1 or 2 each.
    const int back = buildPlanItems(sim, "WoodLog");
    CHECK(back >= 2);
    CHECK(back <= 4);
    CHECK(sim.stats().failedJobs == 0);
}

TEST_CASE("Remove floor: cancelling the mark stops the work and keeps the floor") {
    Sim sim = buildPlanSim();
    sim.submit(PlaceFloorCommand{CellRect{20, 20, 1, 1, 0}, "WoodFloor"});
    sim.submit(DesignateCommand{CellRect{20, 20, 1, 1, 0}, DesignationKind::RemoveFloor});
    sim.step();
    buildPlanSpawn(sim, "Human", CellPos{18, 20, 0});
    for (int i = 0; i < 60; ++i) {
        sim.step();
    }
    sim.submit(CancelDesignationsCommand{CellRect{20, 20, 1, 1, 0}, DesignationKind::RemoveFloor});
    for (int i = 0; i < 600; ++i) {
        sim.step();
    }
    CHECK(sim.floors().at(CellPos{20, 20, 0}).valid());
    CHECK(sim.designations().countOf(DesignationKind::RemoveFloor) == 0);
}
