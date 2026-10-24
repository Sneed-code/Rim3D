#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Pawn.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <vector>

namespace colony {

struct SimSettings {
    uint64_t seed = 0;
    int mapSize = 512;  // cells per side
    int pawns = 0;      // Stage 0 hauling test: pawns start at a stockpile in the middle
    int items = 0;      // and haul items scattered over the reachable map
};

struct SimStats {
    uint64_t hauls = 0;
    uint64_t failedJobs = 0;
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

    const std::vector<Pawn>& pawns() const { return pawns_; }
    const std::vector<Item>& items() const { return items_; }
    int stockpileCell() const { return stockpileCell_; }  // -1 if the map had no walkable cell
    int itemsLeft() const;
    const SimStats& stats() const { return stats_; }
    const Pathfinder& pathfinder() const { return pathfinder_; }

private:
    void placeStockpileAndThings();
    void updatePawn(Pawn& pawn);
    void startNextHaul(Pawn& pawn);

    SimSettings settings_;
    Map map_;
    Pathfinder pathfinder_;
    GameTime time_;
    Rng rng_;
    int stockpileCell_ = -1;
    std::vector<Pawn> pawns_;
    std::vector<Item> items_;
    SimStats stats_;
};

}  // namespace colony
