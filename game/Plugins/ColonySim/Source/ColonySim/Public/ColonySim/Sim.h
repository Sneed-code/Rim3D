#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Doors.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Pathfinder.h"
#include "ColonySim/Random.h"
#include "ColonySim/ThreatMap.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class Construction;
class DefDatabase;
struct Colonist;
struct PawnLooks;
class Designations;
class JobSystem;
class Mineables;
class MovementSystem;
class MoodSystem;
class NeedsSystem;
class Plants;
class Regions;
class Rooms;
class Things;
class Zones;
struct Snapshot;
class SimSaveAccess;

struct SimSettings {
    uint64_t seed = 0;
    int mapSize = 512;  // cells per side
    int pawns = 0;      // Hauling test: pawns start at a stockpile zone in the middle
    int items = 0;      // and haul items scattered over the reachable map to it
    bool flatMap = false;               // all soil at height 0, for tests
    std::string pawnDef = "Human";      // Things defs the hauling test spawns
    std::string haulItemDef = "Steel";
    // Record a state hash every this many game seconds (0: never), for replays to compare.
    int64_t checkpointEvery = kSecondsPerHour;
    int pathBudget = Pathfinder::kDefaultBudget;  // path search work per step (docs/design/pathfinding.md)
    // A Scenarios def to start (e.g. "Crashlanded"): colonists made from the seed, with starting
    // items, near the middle of the map (docs/design/colonists.md). Empty: no scenario.
    std::string scenario{};
};

struct SimStats {
    uint64_t hauls = 0;
    uint64_t failedJobs = 0;
    uint64_t repaths = 0;  // paths asked for again because a pawn gave up on its walk
};

// The state hash after a step, recorded every SimSettings::checkpointEvery game seconds.
struct Checkpoint {
    int64_t step = 0;  // game seconds
    uint64_t hash = 0;
};

// The whole game state and its rules. Unreal sends commands, reads events and snapshots, and
// never changes sim state directly (docs/design/bridge.md).
class COLONY_API Sim {
public:
    // A sim with no defs: a map but no things. Enough for terrain work and the Stage 0 tests.
    explicit Sim(const SimSettings& settings);
    // Things are made from `defs`, which every sim made from it shares. The hauling test needs the
    // Things defs it names and the WorkTypes def Hauling.
    Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs);
    ~Sim();
    Sim(const Sim&) = delete;
    Sim& operator=(const Sim&) = delete;

    // Advances one game second: applies the commands submitted since the last step, in order,
    // then runs every system.
    void step();
    void stepMany(int64_t steps);

    // Queues a command for the start of the next step. Returns its id: 1, 2, 3... in submit
    // order, which a CommandFailed event names.
    uint64_t submit(Command command);
    // Every command applied so far, failed ones included.
    const std::vector<LoggedCommand>& commandLog() const { return commandLog_; }

    // What happened during the last step (before the first step: while setting up the map).
    // Cleared when the next step starts, so read it after every step.
    const std::vector<Event>& events() const { return events_; }

    // Fills `out` with what Unreal draws, reusing its memory.
    void writeSnapshot(Snapshot& out) const;

    // Hash of the whole game state: same seed and commands, same hash. Every piece of sim state
    // must go in it (docs/design/bridge.md). Costs about a microsecond per thing.
    uint64_t stateHash() const;
    const std::vector<Checkpoint>& checkpoints() const { return checkpoints_; }

    const SimSettings& settings() const { return settings_; }
    const DefDatabase& defs() const { return *defs_; }
    const GameTime& time() const { return time_; }
    const Map& map() const { return map_; }
    // Regions and reachability (docs/design/regions.md). Queries may rebuild, so not const.
    Regions& regions() { return map_.regions(); }
    // Cover from the eight sides and line of sight (docs/design/cover.md).
    Cover& cover() { return map_.cover(); }
    // The threat map of a faction, made empty the first time it is asked for.
    ThreatMap& threatMap(FactionId faction);
    uint64_t seed() const { return settings_.seed; }

    // Every pawn, item and building (docs/design/things.md).
    Things& things() { return *things_; }
    const Things& things() const { return *things_; }
    // Body parts, injuries, bleeding, downed and death (docs/design/health.md). Damage comes in
    // through `health().applyDamage` from other sim systems, or the DamagePawnCommand.
    HealthSystem& health() { return *health_; }
    const HealthSystem& health() const { return *health_; }
    // Food and rest (docs/design/needs.md). Eating and sleeping are jobs, see jobs().
    NeedsSystem& needs() { return *needs_; }
    const NeedsSystem& needs() const { return *needs_; }
    // Mood and thoughts (docs/design/mood.md).
    MoodSystem& mood() { return *mood_; }
    const MoodSystem& mood() const { return *mood_; }
    // Guns, projectiles, hitboxes and armor (docs/design/combat.md).
    CombatSystem& combat() { return *combat_; }
    const CombatSystem& combat() const { return *combat_; }
    // What a health panel shows for a pawn; false if it has no body.
    bool healthReport(Entity pawn, HealthReport& out) const { return health_->report(pawn, out); }

    // The colonist the player controls directly (ControlPawnCommand), or kNoEntity.
    Entity controlledPawn() const;
    // A pawn's colonist record (name, skills...), or null if it is not a colonist.
    const Colonist* colonistOf(Entity pawn) const;
    // How a humanlike pawn looks (docs/design/looks.md), or null for other things.
    const PawnLooks* looksOf(Entity pawn) const;

    // The plant layer: wild plants from map generation, growing hourly (docs/design/plants.md).
    Plants& plants() { return *plants_; }
    const Plants& plants() const { return *plants_; }

    // The rock and ore layer, mined down by jobs (docs/design/mining.md), and the marks the player
    // has put on rock and plants for colonists to work on.
    Mineables& mineables() { return *mineables_; }
    const Mineables& mineables() const { return *mineables_; }
    Designations& designations() { return *designations_; }
    const Designations& designations() const { return *designations_; }

    // Problems setting up the start (e.g. a def the settings name is missing). Empty when fine.
    const std::vector<std::string>& setupErrors() const { return setupErrors_; }

    // Rooms, roofs and roles (docs/design/rooms.md). Brought up to date at the end of every step.
    Rooms& rooms() { return *rooms_; }
    const Rooms& rooms() const { return *rooms_; }

    // Stockpile zones (docs/design/jobs.md).
    const Zones& zones() const { return *zones_; }
    // Work types, the think list, jobs and reservations (docs/design/jobs.md).
    JobSystem& jobs() { return *jobs_; }
    const JobSystem& jobs() const { return *jobs_; }

    // Blueprints, frames and taking buildings down (docs/design/construction.md).
    Construction& construction() { return *construction_; }
    const Construction& construction() const { return *construction_; }

    // Hauling test: the middle of its stockpile zone (empty if no cell is walkable) and its pawns.
    const std::optional<CellPos>& stockpileCell() const { return stockpileCell_; }
    const std::vector<Entity>& haulers() const { return haulers_; }
    // Items not in a stockpile that takes them, carried ones included.
    int itemsLeft() const;
    const SimStats& stats() const { return stats_; }
    // Path requests go through its queue, which runs once per step (docs/design/pathfinding.md).
    Pathfinder& pathfinder() { return pathfinder_; }
    const Pathfinder& pathfinder() const { return pathfinder_; }

private:
    friend class SimSaveAccess;  // saves read and restore the private state (Save.h)

    struct EmptyStart {};
    // A sim with its map and nothing else: no hauling test. Sim(settings, defs) adds the test's
    // stockpile, items and pawns; loading a save restores them from the file instead.
    Sim(const SimSettings& settings, std::shared_ptr<const DefDatabase> defs, EmptyStart);

    void applyCommand(const Command& command, uint64_t id);
    void commandFailed(uint64_t id, std::string message);
    void placeStockpileAndThings();
    void pruneDesignations();
    void startScenario();
    // Gives a new pawn its looks if it is humanlike (its def names no other intelligence).
    void giveLooks(Entity pawn, uint64_t seed, bool female, const std::string& lineage);

    SimSettings settings_;
    std::shared_ptr<const DefDatabase> defs_;
    Map map_;
    Pathfinder pathfinder_;
    std::unique_ptr<Zones> zones_;
    std::unique_ptr<Things> things_;
    std::unique_ptr<Plants> plants_;
    std::unique_ptr<Mineables> mineables_;
    std::unique_ptr<Designations> designations_;
    std::unique_ptr<MovementSystem> movement_;
    std::unique_ptr<Rooms> rooms_;
    DoorNumbers doorNumbers_;  // for the snapshot's door animation
    std::unique_ptr<HealthSystem> health_;
    std::unique_ptr<NeedsSystem> needs_;
    std::unique_ptr<MoodSystem> mood_;
    std::unique_ptr<CombatSystem> combat_;
    std::map<FactionId, std::unique_ptr<ThreatMap>> threatMaps_;
    GameTime time_;
    Rng rng_;
    uint64_t groundHash_;  // map content hash before anything was built
    std::optional<CellPos> stockpileCell_;
    std::vector<Entity> haulers_;
    SimStats stats_;
    std::unique_ptr<Construction> construction_;
    std::unique_ptr<JobSystem> jobs_;  // after everything it holds a reference to
    std::vector<std::string> setupErrors_;
    std::vector<std::pair<uint64_t, Command>> pending_;  // id, command
    uint64_t submitted_ = 0;
    std::vector<LoggedCommand> commandLog_;
    std::vector<Event> events_;
    std::vector<Checkpoint> checkpoints_;
};

}  // namespace colony
