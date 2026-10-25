#pragma once

#include "ColonySim/Entity.h"
#include "ColonySim/Pathfinder.h"

#include <cstdint>

namespace colony {

// Stage 0 hauling placeholder: a pawn that only hauls items to one stockpile cell. The job
// system (task C1) replaces these components and the code that uses them.
// The waits for a path come last so saves from before S3 keep their stage numbers.
// WaitPathCarrying (S4): a new path to the stockpile after a pawn gave up on the first.
enum class HaulStage : uint8_t {
    Idle,
    GoToItem,
    CarryToStockpile,
    WaitPathToItem,
    WaitPathToStockpile,
    WaitPathCarrying
};

struct HaulJob {
    HaulStage stage = HaulStage::Idle;
    Entity item = kNoEntity;
    PathTicket path;  // while waiting for a path
};

// On an item a pawn has claimed, so nobody else goes for it.
struct HaulReservedBy {
    Entity pawn = kNoEntity;
};

// On an item while a pawn carries it (the item is off the map).
struct HaulCarriedBy {
    Entity pawn = kNoEntity;
};

// On an item that has reached the stockpile.
struct HaulStored {};

}  // namespace colony
