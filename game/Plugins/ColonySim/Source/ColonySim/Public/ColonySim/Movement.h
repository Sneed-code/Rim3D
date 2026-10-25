#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Map.h"

#include <cstdint>
#include <vector>

namespace colony {

// Smooth movement (task S4, docs/design/movement.md): how fast a pawn walks over ground, how a
// cell path is straightened into long lines, and how pawns steer around each other.

// How wide a pawn is when it must fit between walls, in metres.
inline constexpr float kPawnRadius = 0.3f;

// ---- Ground speed ----------------------------------------------------------------------------

// RimWorld's path cost of the ground, in the units of its wiki: every point slows the walk by
// 7.7% (`1 / (1 + cost * 0.077)`). Soil 2 and sand 4 give the wiki's 87% and 76%. These move into
// the Terrain defs (`pathCost`, data/defs/Terrain) when the map reads terrain from data.
COLONY_API float terrainPathCost(Terrain t);
// Share of a pawn's `moveSpeed` it keeps on this ground: 0.87 on soil, 0.76 on sand.
COLONY_API float terrainSpeedFactor(Terrain t);
// Share of its speed a pawn keeps walking along a slope (rise over run, up is positive): Tobler's
// hiking function scaled to 1 on flat ground. A little faster on a gentle downhill, slower on the
// steep and on any uphill. Slopes beyond +-0.5 count as +-0.5.
COLONY_API float slopeSpeedFactor(float slope);
// Speed factor (ground and slope) for a pawn at (x, y) metres heading along the unit vector
// (dirX, dirY). Slope is measured over the metre around the pawn.
COLONY_API float walkSpeedFactor(const Map& map, float x, float y, float dirX, float dirY);
// Effort of walking the straight line: its metres divided by the speed factor, summed along it.
// Divide by a pawn's `moveSpeed` for game seconds.
COLONY_API float walkEffort(const Map& map, float x0, float y0, float x1, float y1);

// ---- Straight lines --------------------------------------------------------------------------

// True when a pawn of `radius` can stand with its centre at (x, y): every cell it overlaps is
// walkable.
COLONY_API bool pawnFitsAt(const Map& map, float x, float y, float radius, int level = 0);
// True when a pawn can walk the straight line from (x0, y0) to (x1, y1) without touching a cell
// it can't walk. The start itself isn't tested (a pawn already hugging a wall may leave it).
COLONY_API bool straightWalkIsClear(const Map& map, float x0, float y0, float x1, float y1, float radius, int level = 0);

// Straightens a cell path: drops every cell the pawn can skip by walking straight to a later one
// where the ground allows (clear of walls for a pawn's width, and not slower than the cells it
// replaces over mud or hills). `path` runs from the cell after the start to the goal, as the
// pathfinder returns it, and so does the result, a subset of its cells. The walk starts at
// (startX, startY) in metres.
COLONY_API void smoothPath(const Map& map, float startX, float startY, std::vector<CellPos>& path);

}  // namespace colony
