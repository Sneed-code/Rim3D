#pragma once

#include "ColonySim/Clock.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace colony {

// Colonists jog at about 4 m/s (game design doc), in real seconds at 1x.
inline constexpr float kPawnJogSpeed = 4.0f;
inline constexpr float kPawnMetresPerStep = kPawnJogSpeed / static_cast<float>(kStepsPerRealSecondAt1x);

// Stage 0 placeholders for the sim speed test: a haulable item and a pawn that
// only hauls. Both become EnTT entities with defs and a real job system in Stage 1.
struct Item {
    int cell = 0;
    bool reserved = false;
    bool stored = false;
};

enum class PawnJob : uint8_t { Idle, GoToItem, CarryToStockpile };

struct Pawn {
    float x = 0.0f;  // metres
    float y = 0.0f;
    PawnJob job = PawnJob::Idle;
    int item = -1;
    std::vector<int> path;
    size_t pathPos = 0;
};

}  // namespace colony
