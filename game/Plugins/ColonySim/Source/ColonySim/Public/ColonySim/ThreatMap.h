#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Grid.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace colony {

class Map;

// Which side a threat map is for. Factions themselves come with raids (F6).
using FactionId = uint32_t;

// Ground a faction's enemies can shoot at. See docs/design/cover.md.
//
// Every enemy that is known to the faction adds itself with `addSource`: each walkable cell within
// weapon range that the enemy has line of sight to gets the source's weight. Overlapping sources
// add up. The map is rebuilt by its owner (clear, then add every known enemy), a few times a second
// at most; it is a cache of what the sim knows, so it isn't saved.
class COLONY_API ThreatMap {
public:
    explicit ThreatMap(Map& map);

    // Removes every source.
    void clear();
    // Adds one enemy at `from`. Cells up to `range` metres away count if `from` can see a pawn of
    // `targetStance` standing there (Standing means "anywhere it could be hit").
    void addSource(const Viewpoint& from, float range, float weight, Stance targetStance = Stance::Standing);

    // Total weight of the sources that can see `c`; 0 off the map and on unwalkable cells.
    float threatAt(CellPos c) const;
    size_t sourceCount() const { return sources_; }

private:
    Map& map_;
    std::vector<float> threat_;  // allocated by the first source
    // Rectangle of cells (inclusive) that may hold a non-zero value, so clear() stays cheap.
    int minX_ = 0;
    int minY_ = 0;
    int maxX_ = -1;
    int maxY_ = -1;
    size_t sources_ = 0;
};

}  // namespace colony
