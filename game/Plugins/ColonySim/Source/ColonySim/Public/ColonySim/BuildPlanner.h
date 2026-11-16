#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class Sim;

// The build menu and placing plans (docs/design/build-menu.md). Plain answers about the sim for the
// overseer's architect menu and, later, placing things by hand in first person: what can be built,
// in which category, made of what, and which cells a drag plans with what cost. It only reads the
// sim; the plans themselves are the PlaceBlueprintCommands it hands back, so the sim checks them again.

// What a drag from one cell to another plans.
enum class DragShape : uint8_t {
    Single,   // one, where the drag ends
    Line,     // a row along the longer side of the drag (a fence, a line of walls)
    Outline,  // the edge of the dragged rectangle (a room's walls)
    Filled,   // every cell of the dragged rectangle (a floor)
};

COLONY_API const char* dragShapeName(DragShape shape);
// "Single", "Line", "Outline" or "Filled"; false for anything else.
COLONY_API bool dragShapeFromName(const std::string& name, DragShape& out);

// One button of the build menu.
struct BuildMenuEntry {
    std::string defName;  // a Things building, or a Terrain floor
    std::string label;
    bool floor = false;       // a floor (PlaceBlueprintCommand lays a floor plan)
    int sizeX = 1;            // footprint facing North
    int sizeY = 1;
    bool needsStuff = false;  // it is made of a stuff the player picks
    std::string locked;       // why it can't be planned yet (the research it needs), empty if it can
    DragShape drag = DragShape::Single;  // what a drag does unless the player picks another shape
};

struct BuildMenuCategory {
    std::string name;   // the defs' `designationCategory`
    std::string label;  // what the button says
    std::vector<BuildMenuEntry> entries;
};

// A stuff a building can be made of, and how much of it lies on the map.
struct StuffChoice {
    std::string defName;
    std::string label;
    int onMap = 0;
};

// One plan a drag would make: its origin cell (the footprint's lowest corner, as PlaceBlueprintCommand
// takes it) and why it can't be planned, empty if it can.
struct PlannedSpot {
    CellPos cell;
    std::string whyNot;
};

struct BuildCostLine {
    std::string defName;
    std::string label;
    int needed = 0;  // for all the spots that can be planned
    int onMap = 0;
};

struct BuildPreview {
    std::string whyNot;  // the whole order can't be given (unknown def, wrong stuff, research missing)
    std::vector<PlannedSpot> spots;
    int plannable = 0;
    std::vector<BuildCostLine> cost;
    bool affordable() const;  // the map holds everything it costs (colonists can still wait for more)
};

class COLONY_API BuildPlanner {
public:
    explicit BuildPlanner(const Sim& sim) : sim_(sim) {}

    // The architect menu: categories in ConstructionRules' `architectCategories` order (then any other in
    // name order), each with its buildings and floors in def order. Defs that can't be built are left out;
    // ones that need research are in with `locked` set.
    std::vector<BuildMenuCategory> menu() const;
    // One entry, by defName (a Things building first, else a Terrain floor); false if it isn't on the menu.
    bool entry(const std::string& defName, BuildMenuEntry& out) const;
    // The stuffs `defName` can be made of, the most on the map first (then def order). Empty for one that
    // takes none.
    std::vector<StuffChoice> stuffChoices(const std::string& defName) const;
    // The stuff to pick first: the one with the most on the map; "" if it takes none.
    std::string bestStuff(const std::string& defName) const;

    // The origins a drag from `from` to `to` plans for a footprint of `stepX` by `stepY` cells (as placed),
    // in drag order. Spots step by the footprint so they never overlap; `from` is always the first.
    static std::vector<CellPos> dragCells(CellPos from, CellPos to, DragShape shape, int stepX = 1, int stepY = 1);
    // What dragging `defName` made of `stuff`, facing `rot`, from `from` to `to` would plan.
    BuildPreview preview(const std::string& defName, const std::string& stuff, CellPos from, CellPos to, Rot4 rot,
                         DragShape shape) const;
    // The commands for the spots of a preview that can be planned.
    static std::vector<Command> commands(const BuildPreview& preview, const std::string& defName, const std::string& stuff,
                                         Rot4 rot);

    // How many of an item lie on the map (spawned stacks, not carried or packed away).
    int onMap(DefId item) const;

private:
    const Sim& sim_;
};

}  // namespace colony
