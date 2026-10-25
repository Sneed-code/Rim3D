#pragma once

#include "ColonySim/Doors.h"
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
//
// It also runs the doors: a pawn can't walk into a closed door's cell, it waits at the edge and asks
// the door to open, and every door counts down to closing (docs/design/rooms.md).
class MovementSystem {
public:
    void setDoors(const DoorNumbers& doors) { doors_ = doors; }
    void step(entt::registry& registry, const Map& map);

    // Pawns whose walk was held up by other pawns this step, for the stats simrun prints.
    uint64_t pawnsHeldUp() const { return heldUp_; }

private:
    void movePawns(entt::registry& registry, const Map& map);
    void requestDoor(Entity door) { doorRequests_.push_back(door); }
    void updateDoors(entt::registry& registry);

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
    DoorNumbers doors_;
    std::vector<Entity> doorRequests_;  // doors a pawn is in, waiting at or about to walk into this step
};

}  // namespace colony
