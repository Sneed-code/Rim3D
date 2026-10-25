#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace colony {

class DefDatabase;
class Map;
class Rooms;
class Things;

// Light and darkness. See docs/design/light.md.
//
// Every cell has a light level from 0 (pitch dark) to 1 (full light): the larger of the sun's and the
// lamps'. The sun lights every cell with no roof (thin or thick) by the time of day (`LightRules`
// `skyGlowByHour`). A building with `building.light` (radius in metres, glow 0 to 1) lights the cells
// round it, falling off in a straight line to 0 at the radius and stopped by tall things (walls, rock)
// in the way. The lamp layer is rebuilt when a building or a roof changes; it is derived, not saved
// and not in the state hash. Dark cells slow work and walking (`workSpeedByGlow`, `moveSpeedByGlow`).
struct COLONY_API LightNumbers {
    // Pairs (x, factor) of a curve, x ascending; the factor is held flat outside the ends.
    using Curve = std::vector<std::pair<float, float>>;

    Curve skyGlowByHour;      // hour of day (0 to 24) -> sun glow
    Curve workSpeedByGlow;    // cell glow -> work speed factor
    Curve moveSpeedByGlow;    // cell glow -> walking speed factor
    float blockingHeightDm = 15;  // a thing this tall (decimetres) stops a lamp's light

    static LightNumbers read(const DefDatabase& defs);
    static float eval(const Curve& curve, float x);
};

// A building that gives light.
struct COLONY_API LightSource {
    Entity building = kNoEntity;
    CellPos cell;        // the cell its position is in
    float radius = 0.0f;  // metres
    float glow = 1.0f;    // light at the source
};

class COLONY_API Lights {
public:
    Lights(Map& map, Things& things, const Rooms& rooms, const DefDatabase& defs, const GameTime& time);

    const LightNumbers& numbers() const { return numbers_; }

    // Rebuilds the lamp layer if a building or a roof changed since the last call (or after
    // markDirty). Cheap when nothing changed. The sim calls it at the end of every step, after
    // Rooms::update.
    void update();
    // Makes the next update rebuild the lamps: for a light that is switched on or off by something
    // that is not a building change (power, fuel, fire).
    void markDirty() { dirty_ = true; }

    // The sun's glow now, from the time of day. 0 at night, 1 by day.
    float skyGlow() const;
    // Light at a cell, 0 to 1; 0 outside the map. The cell's roof decides whether the sun reaches it.
    float lightAt(CellPos c) const;
    // Only the lamps' share, and whether the sun reaches the cell.
    float lampLightAt(CellPos c) const;
    bool underOpenSky(CellPos c) const;

    // Work speed and walking speed factors in a cell's light.
    float workSpeedAt(CellPos c) const;
    float moveSpeedAt(CellPos c) const;

    // Every building that gives light, in entity order, as of the last update.
    const std::vector<LightSource>& sources() const { return sources_; }
    uint64_t rebuilds() const { return rebuilds_; }

private:
    void rebuild();
    bool blocked(int x0, int y0, int x1, int y1) const;

    Map& map_;
    Things& things_;
    const Rooms& rooms_;
    const GameTime& time_;
    LightNumbers numbers_;
    std::vector<uint8_t> lightDef_;  // per def id: 1 if it has building.light
    std::vector<float> lamp_;        // per cell
    std::vector<int> touched_;       // cells lamp_ is non-zero in, to clear
    std::vector<LightSource> sources_;
    bool built_ = false;
    bool dirty_ = false;
    uint64_t buildingChanges_ = 0;
    uint64_t roofChanges_ = 0;
    uint64_t regionChanges_ = 0;
    uint64_t rebuilds_ = 0;
};

}  // namespace colony
