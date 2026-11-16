#include "ColonySim/BuildPlanner.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace colony {

namespace {

// A floor that can be laid from a plan: on the Floors menu and with something to pay (smoothed stone is
// made by smoothing rock, not laid).
bool buildPlanIsLaidFloor(const Sim& sim, const Def& def) {
    return sim.floors().isFloorDef(def.id()) && def.has("designationCategory") &&
           (def.count("costs") > 0 || def.has("costStuffCount"));
}

std::string buildPlanCapital(std::string text) {
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
    return text;
}

}  // namespace

const char* dragShapeName(DragShape shape) {
    switch (shape) {
        case DragShape::Single: return "Single";
        case DragShape::Line: return "Line";
        case DragShape::Outline: return "Outline";
        case DragShape::Filled: return "Filled";
    }
    return "Single";
}

bool dragShapeFromName(const std::string& name, DragShape& out) {
    for (const DragShape shape : {DragShape::Single, DragShape::Line, DragShape::Outline, DragShape::Filled}) {
        if (name == dragShapeName(shape)) {
            out = shape;
            return true;
        }
    }
    return false;
}

bool BuildPreview::affordable() const {
    for (const BuildCostLine& line : cost) {
        if (line.onMap < line.needed) {
            return false;
        }
    }
    return true;
}

// ---- The menu ---------------------------------------------------------------------------------

bool BuildPlanner::entry(const std::string& defName, BuildMenuEntry& out) const {
    const DefDatabase& defs = sim_.defs();
    const Construction& construction = sim_.construction();
    const Def* def = defs.find("Things", defName);
    bool floor = false;
    if (def != nullptr) {
        if (def->text("category") != "Building" || !def->has("designationCategory") || !construction.buildable(def->id())) {
            return false;
        }
    } else {
        def = defs.find("Terrain", defName);
        if (def == nullptr || !buildPlanIsLaidFloor(sim_, *def)) {
            return false;
        }
        floor = true;
    }
    out = BuildMenuEntry{};
    out.defName = defName;
    out.label = buildPlanCapital(def->label());
    out.floor = floor;
    if (!floor) {
        out.sizeX = std::max(1, static_cast<int>(def->numberAt("size", 0, 1.0)));
        out.sizeY = std::max(1, static_cast<int>(def->numberAt("size", 1, 1.0)));
    }
    out.needsStuff = def->count("stuffCategories") > 0;
    std::string why;
    if (!sim_.research().unlocked(*def, why)) {
        out.locked = why.empty() ? std::string("needs research") : why;
    }
    // A floor fills the dragged area, a one-cell building runs in a line, anything bigger is placed one at
    // a time, unless the def says otherwise (`building.dragShape`: the wall outlines a room).
    out.drag = floor ? DragShape::Filled : (out.sizeX == 1 && out.sizeY == 1 ? DragShape::Line : DragShape::Single);
    if (!floor && def->has("building.dragShape")) {
        dragShapeFromName(def->text("building.dragShape"), out.drag);
    }
    return true;
}

std::vector<BuildMenuCategory> BuildPlanner::menu() const {
    const DefDatabase& defs = sim_.defs();
    const Def* rules = defs.find("Rules", "ConstructionRules");
    std::vector<BuildMenuCategory> out;
    const auto categoryFor = [&](const std::string& name) -> BuildMenuCategory& {
        for (BuildMenuCategory& category : out) {
            if (category.name == name) {
                return category;
            }
        }
        BuildMenuCategory category;
        category.name = name;
        const std::string path = "architectLabels." + name;
        category.label = rules != nullptr && rules->has(path.c_str()) ? rules->text(path.c_str()) : name;
        out.push_back(std::move(category));
        return out.back();
    };
    // The usual order first, so the bar always reads the same.
    if (rules != nullptr) {
        for (size_t i = 0; i < rules->count("architectCategories"); ++i) {
            categoryFor(rules->textAt("architectCategories", i));
        }
    }
    const size_t usual = out.size();
    for (const char* kind : {"Things", "Terrain"}) {
        for (const DefId id : defs.idsOfKind(kind)) {
            const Def& def = defs.get(id);
            BuildMenuEntry item;
            if (!entry(def.defName(), item) || item.floor != (std::string(kind) == "Terrain")) {
                continue;
            }
            categoryFor(def.text("designationCategory")).entries.push_back(std::move(item));
        }
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(usual), out.end(),
              [](const BuildMenuCategory& a, const BuildMenuCategory& b) { return a.name < b.name; });
    out.erase(std::remove_if(out.begin(), out.end(), [](const BuildMenuCategory& c) { return c.entries.empty(); }), out.end());
    return out;
}

int BuildPlanner::onMap(DefId item) const {
    int total = 0;
    const Things& things = sim_.things();
    things.registry().view<const Thing, const StackCount>().each([&](const Entity e, const Thing& thing, const StackCount& stack) {
        if (thing.def == item && things.spawned(e)) {
            total += stack.count;
        }
    });
    return total;
}

std::vector<StuffChoice> BuildPlanner::stuffChoices(const std::string& defName) const {
    const DefDatabase& defs = sim_.defs();
    BuildMenuEntry item;
    std::vector<StuffChoice> out;
    if (!entry(defName, item) || !item.needsStuff) {
        return out;
    }
    const DefId def = defs.idOf(item.floor ? "Terrain" : "Things", defName);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& stuff = defs.get(id);
        if (stuff.text("category") != "Item" || stuff.count("stuffProps.categories") == 0 || !sim_.construction().stuffFits(def, id)) {
            continue;
        }
        out.push_back(StuffChoice{stuff.defName(), buildPlanCapital(stuff.label()), onMap(id)});
    }
    std::stable_sort(out.begin(), out.end(), [](const StuffChoice& a, const StuffChoice& b) { return a.onMap > b.onMap; });
    return out;
}

std::string BuildPlanner::bestStuff(const std::string& defName) const {
    const std::vector<StuffChoice> choices = stuffChoices(defName);
    return choices.empty() ? std::string() : choices.front().defName;
}

// ---- Dragging ---------------------------------------------------------------------------------

std::vector<CellPos> BuildPlanner::dragCells(CellPos from, CellPos to, DragShape shape, int stepX, int stepY) {
    stepX = std::max(1, stepX);
    stepY = std::max(1, stepY);
    std::vector<CellPos> out;
    // How many footprints fit from `from` towards `to` along each side (at least one), and which way.
    const int dx = to.x - from.x;
    const int dy = to.y - from.y;
    const int dirX = dx < 0 ? -1 : 1;
    const int dirY = dy < 0 ? -1 : 1;
    // A footprint going the negative way starts its own size before the last one.
    const int countX = std::abs(dx) / stepX + 1;
    const int countY = std::abs(dy) / stepY + 1;
    const auto at = [&](int i, int j) { return CellPos{from.x + dirX * i * stepX, from.y + dirY * j * stepY, from.level}; };
    switch (shape) {
        case DragShape::Single:
            out.push_back(to.level == from.level ? to : from);
            break;
        case DragShape::Line:
            if (std::abs(dx) >= std::abs(dy)) {
                for (int i = 0; i < countX; ++i) {
                    out.push_back(at(i, 0));
                }
            } else {
                for (int j = 0; j < countY; ++j) {
                    out.push_back(at(0, j));
                }
            }
            break;
        case DragShape::Outline:
            for (int j = 0; j < countY; ++j) {
                for (int i = 0; i < countX; ++i) {
                    if (i == 0 || j == 0 || i == countX - 1 || j == countY - 1) {
                        out.push_back(at(i, j));
                    }
                }
            }
            break;
        case DragShape::Filled:
            for (int j = 0; j < countY; ++j) {
                for (int i = 0; i < countX; ++i) {
                    out.push_back(at(i, j));
                }
            }
            break;
    }
    return out;
}

BuildPreview BuildPlanner::preview(const std::string& defName, const std::string& stuff, CellPos from, CellPos to, Rot4 rot,
                                   DragShape shape) const {
    BuildPreview out;
    const DefDatabase& defs = sim_.defs();
    const Construction& construction = sim_.construction();
    BuildMenuEntry item;
    if (!entry(defName, item)) {
        out.whyNot = "that can't be built";
        return out;
    }
    if (!item.locked.empty()) {
        out.whyNot = item.locked;
        return out;
    }
    const DefId def = defs.idOf(item.floor ? "Terrain" : "Things", defName);
    const DefId stuffId = stuff.empty() ? DefId{} : defs.idOf("Things", stuff);
    if (!stuff.empty() && !stuffId.valid()) {
        out.whyNot = "no such material";
        return out;
    }
    if (!construction.stuffFits(def, stuffId)) {
        out.whyNot = item.needsStuff ? "it can't be made of that" : "it is not made of anything";
        return out;
    }
    const CellRect footprint = footprintRect(CellPos{0, 0, 0}, item.sizeX, item.sizeY, rot);
    std::set<std::pair<int, int>> taken;  // cells of the spots planned so far in this drag
    for (const CellPos cell : dragCells(from, to, shape, footprint.sizeX, footprint.sizeY)) {
        // A footprint dragged the negative way grows back towards the drag's start, so it covers the cursor.
        CellPos origin = cell;
        if (to.x < from.x && footprint.sizeX > 1) {
            origin.x -= footprint.sizeX - 1;
        }
        if (to.y < from.y && footprint.sizeY > 1) {
            origin.y -= footprint.sizeY - 1;
        }
        PlannedSpot spot{origin, {}};
        spot.whyNot = item.floor ? construction.whyNotFloorPlan(def, stuffId, origin) : construction.whyNotBlueprint(def, stuffId, origin, rot);
        const CellRect rect = footprintRect(origin, item.sizeX, item.sizeY, rot);
        for (int y = rect.minY; spot.whyNot.empty() && y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                if (taken.count({x, y}) != 0) {
                    spot.whyNot = "it overlaps another plan of this order";
                    break;
                }
            }
        }
        if (spot.whyNot.empty()) {
            for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
                for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                    taken.insert({x, y});
                }
            }
            ++out.plannable;
        }
        out.spots.push_back(std::move(spot));
    }
    for (const Construction::Cost& cost : construction.costOf(def, stuffId)) {
        out.cost.push_back(BuildCostLine{defs.get(cost.item).defName(), buildPlanCapital(defs.get(cost.item).label()),
                                         cost.count * out.plannable, onMap(cost.item)});
    }
    return out;
}

std::vector<Command> BuildPlanner::commands(const BuildPreview& preview, const std::string& defName, const std::string& stuff,
                                            Rot4 rot) {
    std::vector<Command> out;
    if (!preview.whyNot.empty()) {
        return out;
    }
    for (const PlannedSpot& spot : preview.spots) {
        if (spot.whyNot.empty()) {
            out.push_back(PlaceBlueprintCommand{defName, stuff, spot.cell, rot});
        }
    }
    return out;
}

}  // namespace colony
