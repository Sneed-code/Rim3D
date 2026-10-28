#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Pathfinder.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;
class HealthSystem;
class JobSystem;
class Map;
class NeedsSystem;
class SpoilageSystem;
class StateHasher;
class Things;
class World;
struct GameTime;

// Caravans (W4, docs/design/caravans.md): colonists who leave the map with goods and travel the
// world map tile by tile, by RimWorld's speeds and costs, to a settlement and back home.
//
// A caravan is formed by command from colonists on the map and the item stacks it takes along.
// The members walk to an exit cell at the map edge facing the way they are going and step off the
// map there one by one; when the last is gone the items are packed and the caravan sets out from
// the start tile. Off the map its pawns and items keep their ids but have no Position. Their needs
// go on: they eat from the caravan's food and sleep while it rests at night. Arriving at the start
// tile, the caravan walks back onto the map. Unreal does not include this header: it reads
// `Snapshot::caravans` and sends CaravanCommand.

enum class CaravanState : uint8_t {
    Forming,     // members walk to the exit cell on the map
    Travelling,  // on the world, moving toward the next tile of its path
    Resting,     // on the world, stopped for the night (it moves from restToHour to restFromHour)
    Waiting,     // on the world, stopped: it arrived, or was told to stop
};

COLONY_API const char* caravanStateName(CaravanState state);

// On every pawn of a caravan, on the map while it forms and off it while it travels. The job
// system leaves such a pawn alone. Rebuilt from the "caravans" save section.
struct CaravanMember {
    int caravan = 0;
};

// One member walking to the exit cell (while the caravan forms).
struct CaravanWalker {
    Entity pawn = kNoEntity;
    uint8_t walk = 0;   // 0 not yet asked, 1 waiting for a path, 2 walking, 3 off the map
    uint8_t repaths = 0;
    PathTicket path;    // while `walk` is 1
};

struct Caravan {
    int id = 0;
    CaravanState state = CaravanState::Forming;
    std::vector<CaravanWalker> members;  // pawns, in the order given (dead ones leave the list)
    std::vector<Entity> items;           // stacks it carries (and corpses of members who died on the way)
    int tile = -1;                       // the world tile it is on
    int cameFrom = -1;                   // the tile it was on before (it walks onto the map from that side)
    int destination = -1;                // where it is going; -1 when it isn't
    std::vector<int> path;               // tiles still to enter, the next first
    double progress = 0.0;               // game seconds travelled toward the next tile
    double cost = 0.0;                   // game seconds the next tile takes (fixed when it set out for it)
    CellPos exit;                        // where the members leave the map while forming
    int64_t formedAt = 0;                // game second the command was given
};

// The numbers (CaravanRules in data/defs/Rules), RimWorld's from the wiki.
struct CaravanRules {
    double ticksPerTile = 4412.0;        // RimWorld ticks per tile of movement difficulty 1, fully loaded
    double zeroMassSpeedFactor = 2.0;    // speed with nothing to carry
    double fullMassSpeedFactor = 1.0;    // speed at full load
    double carryPerBodySize = 35.0;      // kg a pawn carries per body size
    double roadFactor = 0.5;             // a road between two tiles halves the cost
    std::array<double, 4> hillOffsets{0.0, 0.5, 1.5, 3.0};  // Flat, SmallHills, LargeHills, Mountainous
    double winterFrom = -17.1;           // the season's mean temperature where cold starts to slow it
    double winterTo = -22.5;             // and where it slows it most
    double winterOffset = 2.0;           // movement difficulty added at winterTo and colder
    int restFromHour = 22;
    int restToHour = 6;
    double diagonalFactor = 1.41421356;  // ours: a diagonal step on our square grid
    double formHours = 12.0;             // ours: members not at the exit by then are left behind
    int exitDepth = 2;                   // ours: cells in from the edge the exit cell is
    static CaravanRules read(const DefDatabase& defs);
};

class COLONY_API CaravanSystem {
public:
    CaravanSystem(Things& things, Map& map, Pathfinder& pathfinder, HealthSystem& health, NeedsSystem& needs,
                  const DefDatabase& defs, const GameTime& time, const World* world);

    const CaravanRules& rules() const { return rules_; }
    const std::vector<Caravan>& caravans() const { return caravans_; }
    const Caravan* find(int id) const;
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    void setJobs(JobSystem* jobs) { jobs_ = jobs; }
    void setSpoilage(SpoilageSystem* spoilage) { spoilage_ = spoilage; }

    // ---- Commands. False, with a reason in plain words, when they can't be done. ----
    // Colonists on the map (alive, up, not drafted, not in a mental break, not the one the player
    // walks) and the item stacks they take along walk off the map and set out for `destination`
    // (a tile, or -1 to stay on the start tile until told to move). The items must lie on the map
    // and weigh no more than the members can carry. `id` is the new caravan's.
    bool form(const std::vector<Entity>& pawns, const std::vector<Entity>& items, int destination, int& id,
              std::string& error);
    // Sets a caravan on the world off toward `tile` (by the cheapest path).
    bool move(int id, int tile, std::string& error);
    // Stops it where it is.
    bool stop(int id, std::string& error);
    // Brings a caravan on the start tile back onto the map (it does so by itself when it arrives there).
    bool enter(int id, std::string& error);

    void step();
    // Call before a thing is destroyed: it leaves the caravan it was in.
    void forget(Entity thing);

    // ---- The world as a caravan sees it. ----
    // Movement difficulty of a tile in a season (0..3): biome, hills and cold; 0 when it can't be crossed.
    double difficulty(int tile, int season) const;
    // Game seconds to go from `from` into its neighbour `to` in `season` at speed factor 1, or -1 if blocked.
    double stepSeconds(int from, int to, int season) const;
    // The cheapest tiles from `from` to `to` (the tiles entered, `to` last), by the season's costs.
    // Empty when there is no way, or from == to.
    std::vector<int> findPath(int from, int to, int season) const;
    // Kilograms a caravan carries and can carry, and its speed factor from its load (0 when it is overloaded).
    double massOf(const Caravan& c) const;
    double capacityOf(const Caravan& c) const;
    double speedFactor(const Caravan& c) const;
    // A pawn that sleeps right now because its caravan rests (the needs system asks).
    bool resting(Entity pawn) const;

    // ---- Saves (section "caravans"). ----
    int nextId() const { return nextId_; }
    // Puts the caravans back after loading. False if they don't fit the things.
    bool restore(std::vector<Caravan> caravans, int nextId, std::string& error);
    void hashState(StateHasher& h) const;

private:
    Caravan* findMut(int id);
    bool canJoin(Entity pawn, std::string& error) const;
    double massOfItem(Entity item) const;
    double capacityOfPawn(Entity pawn) const;
    bool restHour() const;
    CellPos exitCell(const Caravan& c, const CellPos& from) const;
    void stepForming(Caravan& c);
    void stepWorld(Caravan& c);
    void leaveMap(Caravan& c, CaravanWalker& w);
    void setOut(Caravan& c);
    void arrive(Caravan& c);
    void enterMap(Caravan& c);
    void feed(Caravan& c);
    void dropMember(Caravan& c, Entity pawn, const std::string& why);
    void startNextTile(Caravan& c);
    void emit(EventType type, const Caravan& c, Entity thing, const std::string& message, float amount = 0.0f) const;

    Things& things_;
    Map& map_;
    Pathfinder& pathfinder_;
    HealthSystem& health_;
    NeedsSystem& needs_;
    const DefDatabase& defs_;
    const GameTime& time_;
    const World* world_;
    JobSystem* jobs_ = nullptr;
    SpoilageSystem* spoilage_ = nullptr;
    CaravanRules rules_;
    std::vector<Caravan> caravans_;
    int nextId_ = 1;
    std::vector<float> nutrition_;      // per def id
    std::vector<int> foodRank_;         // per def id: higher is eaten first, 0 not food
    std::vector<uint64_t> roads_;       // road edges (both tiles in one key), sorted
    std::vector<double> biomeDifficulty_;                  // per world biome index
    std::vector<std::array<double, 4>> biomeSeasonShift_;  // per world biome index: season mean minus year mean
    std::vector<Event>* events_ = nullptr;
};

}  // namespace colony
