#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>

namespace colony {

class DefDatabase;
class Map;
class Things;

// Windows in walls. See docs/design/windows.md.
//
// A window is not a building of its own: it is a part of a wall (a def with `building.takesWindow`),
// cut in by a builder and filled in again the same way, for no materials (sneed, 2026-11-19). The wall
// still blocks walking and holds the roof, so rooms stay rooms. What changes is its height for sight,
// light and cover: the wall counts only up to the sill (`WindowRules.sillDm`), so pawns see and shoot
// through it, take cover behind the sill, lamps shine through it, and the sun lights the room behind it.
// Bullets above the lintel (`lintelDm`) still hit the wall. The glass breaks the first time a bullet, a
// thrown grenade or a blast goes through it; a broken window still lets light and sight through and is
// glazed again by the same order that cuts one.

// On a wall that has a window.
struct Window {
    bool broken = false;  // the glass is gone
};

struct COLONY_API WindowNumbers {
    uint8_t sillDm = 9;     // the wall below the window, decimetres: sight, light and cover stop here
    uint8_t lintelDm = 21;  // the top of the opening, decimetres: bullets above it hit the wall
    float lightRadius = 6.0f;  // metres of room the sun lights through a window
    float lightGlow = 0.8f;    // the share of the sky's light right inside the window, falling to 0 at the radius
    double addWork = 300.0;    // game seconds of construction work to cut a window or glaze a broken one
    double removeWork = 300.0;  // to fill one in

    static WindowNumbers read(const DefDatabase& defs);
};

// What a build-menu tool def (`buildTool`) does. Tools are Things defs with category BuildTool: they are
// on the architect menu but never become things.
enum class BuildTool : uint8_t { None, AddWindow, RemoveWindow };
COLONY_API BuildTool buildToolOf(const DefDatabase& defs, const std::string& defName);

class COLONY_API Windows {
public:
    Windows(Map& map, Things& things, const DefDatabase& defs);

    const WindowNumbers& numbers() const { return numbers_; }

    // The finished wall in a cell that can take a window (`building.takesWindow`, one cell), or none.
    Entity wallAt(CellPos c) const;
    // The window in a cell, or null.
    const Window* at(CellPos c) const;

    // Why a window can't be cut (or glazed again) in the cell; empty if it can.
    std::string whyNotAdd(CellPos c) const;
    // Why the window in the cell can't be filled in; empty if it can.
    std::string whyNotRemove(CellPos c) const;

    // Cuts a window in the wall in `c`, or glazes a broken one. False if whyNotAdd says no.
    bool add(CellPos c);
    // Fills the window in `c` in: the wall is whole again. False if there is none.
    bool remove(CellPos c);
    // Breaks the glass of a window wall. False if it has no window or the glass is already gone.
    static bool breakGlass(Things& things, Entity wall);

    // Puts the wall's height for sight and cover back to what its window says (sill or full height). The
    // footprint keeps it, so saves and loads keep it with no extra work.
    void apply(Entity wall);

private:
    Map& map_;
    Things& things_;
    const DefDatabase& defs_;
    WindowNumbers numbers_;
};

}  // namespace colony
