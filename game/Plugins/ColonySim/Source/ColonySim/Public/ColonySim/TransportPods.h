#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Pathfinder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;
class FuelSystem;
class HealthSystem;
class JobSystem;
class Map;
class StateHasher;
class Things;
class World;
struct GameTime;

// Transport pods (docs/design/transport-pods.md): RimWorld's one-way drop pods. A pod stands beside
// a pod launcher that holds chemfuel. The player names pods, colonists and item stacks to load;
// haulers carry the stacks in (the LoadPod job), then the passengers walk to the pods and get in.
// A launch burns 2.25 chemfuel a tile from each pod's launcher, the pods are used up, and the load
// flies to a world tile: it lands on a map there, forms a caravan on the tile, or is a gift to a
// settlement. What is inside a pod or in the air is off the map (no Position) with its ids kept.
// Unreal does not include this header: it reads `Snapshot::podGroups` / `podFlights` and sends PodCommand.

enum class PodGroupState : uint8_t {
    Loading,   // haulers carry the named stacks into the pods
    Boarding,  // the stacks are in (or gone): the passengers walk to the pods
    Ready,     // everything that could get in is in
};

// What the pods do where they come down.
enum class PodArrival : uint8_t {
    Land,     // onto a map: the colony's, or another map a caravan has been to (not a settlement's: that is an attack)
    Caravan,  // the passengers form a caravan on the tile with the goods (W4); needs a passenger
    Gift,     // the goods are a gift to the tile's settlement (goodwill by their market value); no passengers
};

COLONY_API const char* podGroupStateName(PodGroupState state);
COLONY_API const char* podArrivalName(PodArrival arrival);
COLONY_API bool podArrivalFromName(const std::string& name, PodArrival& out);

// On a passenger walking to its pod (while its group boards): the job system leaves it alone.
// Rebuilt from the "pods" save section.
struct PodPassenger {
    int group = 0;
};

// One passenger of a group.
struct PodWalker {
    Entity pawn = kNoEntity;
    uint8_t walk = 0;   // 0 not called yet, 1 waiting for a path, 2 walking, 3 in a pod (off the map)
    uint8_t repaths = 0;
    PathTicket path;    // while `walk` is 1
    CellPos to;         // the cell beside the pod it walks to
};

struct PodGroup {
    int id = 0;
    PodGroupState state = PodGroupState::Loading;
    std::vector<Entity> pods;           // the transport pod buildings, in the order given
    std::vector<Entity> wanted;         // item stacks on the map still to load
    std::vector<PodWalker> passengers;
    std::vector<Entity> loaded;         // stacks inside (off the map)
    CellPos spot;                       // where the first pod stood: the load spills here if every pod is gone
};

// Pods in the air.
struct PodFlight {
    int id = 0;
    std::vector<Entity> pawns;  // off the map
    std::vector<Entity> items;  // off the map
    int pods = 0;               // how many pods (for drawing)
    int from = -1;              // the tile it left (-1 with no world)
    int to = -1;                // the tile it flies to (-1: the colony's map with no world)
    PodArrival arrival = PodArrival::Land;
    CellPos cell{-1, -1, 0};    // Land: where on the map (x -1: the middle)
    double progress = 0.0;      // game seconds flown
    double seconds = 0.0;       // game seconds the flight takes
};

// The numbers (TransportPodRules in data/defs/Rules).
struct TransportPodRules {
    double fuelPerTile = 2.25;      // chemfuel each launcher burns per tile of distance (wiki)
    double minFuel = 1.0;           // RimWorld burns at least 1 even for no distance (recalled)
    double ticksPerTile = 50.0;     // ours: RimWorld's pods are "very quick but not instant"
    double minFlightTicks = 250.0;  // ours: a flight to the same tile still takes a moment
    int maxRepaths = 5;             // ours: a passenger who can't get to its pod after this many tries stays
    static TransportPodRules read(const DefDatabase& defs);
};

class COLONY_API TransportPodSystem {
public:
    TransportPodSystem(Things& things, Map& map, Pathfinder& pathfinder, HealthSystem& health, FuelSystem& fuel,
                       const DefDatabase& defs, const GameTime& time, const World* world);

    const TransportPodRules& rules() const { return rules_; }
    const std::vector<PodGroup>& groups() const { return groups_; }
    const std::vector<PodFlight>& flights() const { return flights_; }
    const PodGroup* find(int id) const;
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    void setJobs(JobSystem* jobs) { jobs_ = jobs; }

    // ---- Commands. False, with a reason in plain words, when they can't be done. ----
    // Starts loading: `pods` (finished transport pods, not in another group) take the item stacks lying
    // on the map and the colonists named, no more than their mass capacity together (pawns count their own mass).
    bool load(const std::vector<Entity>& pods, const std::vector<Entity>& pawns, const std::vector<Entity>& items, int& id,
              std::string& error);
    // Gives the loading up: what is in the pods comes out beside them.
    bool cancel(int id, std::string& error);
    // Launches a group toward `tile` (-1: this map, which needs no world). The sim has checked the
    // arrival fits the place (Sim::launchPods); this checks the launchers, the fuel and the range,
    // burns the fuel, uses the pods up and puts the load in the air. Passengers not in yet stay.
    bool launch(int id, int tile, PodArrival arrival, CellPos cell, std::string& error);

    // ---- What the player sees before launching. ----
    // The launcher that fuels a pod (a finished pod launcher touching it along an edge; each launcher
    // fuels one pod, the first pods in id order taking the first launchers), or none.
    Entity launcherOf(Entity pod) const;
    // Tiles a group can fly: the least-fuelled launcher's fuel over fuelPerTile; -1 when a pod has no launcher.
    int rangeOf(const PodGroup& g) const;
    // Kilograms a group holds (and will hold: stacks still to load, passengers not in yet) and can hold.
    double massOf(const PodGroup& g) const;
    double capacityOf(const PodGroup& g) const;
    // Tiles between two tiles: steps on the world grid, diagonals counting one, east and west wrapping.
    int distance(int from, int to) const;
    // True if the group has anything aboard.
    bool hasLoad(const PodGroup& g) const;
    bool hasPassengersAboard(const PodGroup& g) const;

    // ---- The job system's side (LoadPod). ----
    // The pod a stack is wanted in (the group's pod nearest `near`), or none.
    Entity podWanting(Entity stack, CellPos near) const;
    // True if `pod` is in a group (loading or loaded).
    bool podInGroup(Entity pod) const { return groupOfPod(pod) != 0; }
    // A hauler put `carried` (off the map) into `pod`. False if that pod is not loading any more.
    bool putIn(Entity pod, Entity carried);

    // ---- The sim's side. ----
    void step();
    // Flights that came down this step, taken out of the system: the sim lands them (PodArrivals.cpp).
    std::vector<PodFlight> takeArrived();
    // Puts pawns and items (off the map, this sim's) down around `cell` on this map (x -1: the middle).
    // Returns the cell they came down at.
    CellPos landHere(const std::vector<Entity>& pawns, const std::vector<Entity>& items, CellPos cell);
    // Call before a thing is destroyed: it leaves the group or flight it was in. A group whose last pod
    // is gone spills its load beside it.
    void forget(Entity thing);

    // ---- Saves (section "pods"). ----
    int nextGroupId() const { return nextGroup_; }
    int nextFlightId() const { return nextFlight_; }
    bool restore(std::vector<PodGroup> groups, std::vector<PodFlight> flights, int nextGroup, int nextFlight, std::string& error);
    void hashState(StateHasher& h) const;

private:
    PodGroup* findMut(int id);
    bool isPod(Entity thing) const;
    bool isLauncher(Entity thing) const;
    bool canBoard(Entity pawn, std::string& error) const;
    double massOfThing(Entity thing) const;
    int groupOfPod(Entity pod) const;
    void stepGroup(PodGroup& g);
    void callPassengers(PodGroup& g);
    void dropPassenger(PodGroup& g, Entity pawn, const std::string& why);
    void spill(PodGroup& g, CellPos at);
    CellPos besidePod(const PodGroup& g, CellPos from) const;
    void emit(EventType type, int count, Entity thing, const std::string& message, float amount = 0.0f, CellPos cell = CellPos{}) const;

    Things& things_;
    Map& map_;
    Pathfinder& pathfinder_;
    HealthSystem& health_;
    FuelSystem& fuel_;
    const DefDatabase& defs_;
    const GameTime& time_;
    const World* world_;
    JobSystem* jobs_ = nullptr;
    TransportPodRules rules_;
    std::vector<PodGroup> groups_;
    std::vector<PodFlight> flights_;
    std::vector<PodFlight> arrived_;
    int nextGroup_ = 1;
    int nextFlight_ = 1;
    std::vector<Event>* events_ = nullptr;
};

}  // namespace colony
