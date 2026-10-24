#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"

#include <cstdint>

namespace colony {

struct SimSettings {
    uint64_t seed = 0;
    int mapSize = 512;  // cells per side
};

// The whole game state and its rules. Unreal reads it and sends commands;
// it never changes sim state directly.
class COLONY_API Sim {
public:
    explicit Sim(const SimSettings& settings);

    // Advances one game second.
    void step();
    void stepMany(int64_t steps);

    const GameTime& time() const { return time_; }
    const Map& map() const { return map_; }
    uint64_t seed() const { return settings_.seed; }

private:
    SimSettings settings_;
    Map map_;
    GameTime time_;
};

}  // namespace colony
