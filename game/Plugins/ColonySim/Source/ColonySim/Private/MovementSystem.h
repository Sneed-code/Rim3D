#pragma once

#include "ColonySim/Entity.h"

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <vector>

namespace colony {

class Map;

// Moves every pawn that has a Mover, once per step: along its path at the speed the ground and
// slope allow, steering around the pawns near it. See docs/design/movement.md.
//
// Every pawn decides from the positions and velocities at the start of the step and the new ones
// are written together at the end, so the order pawns are visited in can't change the result.
// Neighbours are always taken in the same order (nearest first, ties to the lower entity id).
class MovementSystem {
public:
    void step(entt::registry& registry, const Map& map);

    // Pawns whose walk was held up by other pawns this step, for the stats simrun prints.
    uint64_t pawnsHeldUp() const { return heldUp_; }

private:
    struct Agent {
        Entity entity = kNoEntity;
        float x = 0.0f;
        float y = 0.0f;
        int level = 0;
        float vx = 0.0f;
        float vy = 0.0f;
        float newVx = 0.0f;
        float newVy = 0.0f;
        int bucket = 0;
    };

    std::vector<Agent> agents_;
    std::vector<int> bucketHead_;  // first agent index in each bucket, or -1
    std::vector<int> nextInBucket_;
    uint64_t heldUp_ = 0;
};

}  // namespace colony
