#pragma once

#include "ColonySim/Api.h"

#include <cstdint>
#include <vector>

namespace colony {

class Map;

// A* over the 8-connected cell grid, with no corner cutting. Buffers are sized
// to the map once and reused between searches. Region-level search, terrain
// and slope costs come later; for now every walkable cell costs the same.
class COLONY_API Pathfinder {
public:
    struct OpenNode {
        float f;
        float h;  // ties on f go to the node nearer the goal, which keeps open ground cheap
        int cell;
    };

    explicit Pathfinder(const Map& map);

    // Fills `out` with cell indices from just after `start` up to and including
    // `goal`. Returns false if `goal` can't be reached.
    bool findPath(int start, int goal, std::vector<int>& out);

    uint64_t searches() const { return searches_; }
    uint64_t cellsExpanded() const { return cellsExpanded_; }

private:
    const Map& map_;
    std::vector<float> g_;
    std::vector<int> parent_;
    std::vector<uint32_t> seen_;    // == generation_ when g_ and parent_ are valid
    std::vector<uint32_t> closed_;  // == generation_ when the cell is done
    std::vector<OpenNode> open_;
    uint32_t generation_ = 0;
    uint64_t searches_ = 0;
    uint64_t cellsExpanded_ = 0;
};

}  // namespace colony
