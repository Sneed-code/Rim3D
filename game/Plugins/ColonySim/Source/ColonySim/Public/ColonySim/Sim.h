#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;
class Things;

struct SimSettings {
    uint64_t seed = 0;
    int mapSize = 512;  // cells per side
    int pawns = 0;      // Stage 0 hauling test: pawns start at a stockpile in the middle
    int items = 0;      // and haul items scattered over the reachable map
    bool flatMap = false;               // all soil at height 0, for tests
    std::string pawnDef = "Human";      // Things defs the hauling test spawns
    std::string haulItemDef = "Steel";
};

struct SimStats {
    uint64_t hauls = 0;
    uint64_t failedJobs = 0;
};

// The whole game state and its rules. Unreal reads it and sends commands;
// it never changes sim state directly.
class COLONY_API Sim {
public:
    // A sim with no defs: a map but no things. Enough for terrain work and the Stage 0 tests.
    explicit Sim(const SimSettings& settings);
    // Things are made from `defs`, which every sim made from it shares.
    Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs);
    ~Sim();
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    // Advances one game second.
    void step();
    void stepMany(int64_t steps);

    const GameTime& time() const { return time_; }
    const Map& map() const { return map_; }
    uint64_t seed() const { return settings_.seed; }

    // Every pawn, item and building (docs/design/things.md).
    Things& things() { return *things_; }
    const Things& things() const { return *things_; }

    // Problems setting up the start (e.g. a def the settings name is missing). Empty when fine.
    const std::vector<std::string>& setupErrors() const { return setupErrors_; }

    // Stage 0 hauling test.
    const std::optional<CellPos>& stockpileCell() const { return stockpileCell_; }  // empty if no walkable cell
    const std::vector<Entity>& haulers() const { return haulers_; }
    int itemsLeft() const;
    const SimStats& stats() const { return stats_; }
    const Pathfinder& pathfinder() const { return pathfinder_; }

private:
    void placeStockpileAndThings();
    void updateHauler(Entity pawn);
    void startNextHaul(Entity pawn);

    SimSettings settings_;
    std::shared_ptr<const DefDatabase> defs_;
    Map map_;
    Pathfinder pathfinder_;
    std::unique_ptr<Things> things_;
    GameTime time_;
    Rng rng_;
    std::optional<CellPos> stockpileCell_;
    std::vector<Entity> haulers_;
    SimStats stats_;
    std::vector<std::string> setupErrors_;
};

}  // namespace colony
