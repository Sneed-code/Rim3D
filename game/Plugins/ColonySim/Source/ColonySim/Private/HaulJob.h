#pragma once

#include "ColonySim/Entity.h"

#include <cstdint>

namespace colony {

// Stage 0 hauling placeholder: a pawn that only hauls items to one stockpile cell. The job
// system (task C1) replaces these components and the code that uses them.
enum class HaulStage : uint8_t { Idle, GoToItem, CarryToStockpile };

struct HaulJob {
    HaulStage stage = HaulStage::Idle;
    Entity item = kNoEntity;
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
