#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;
class RaidSystem;
class StateHasher;
class Things;
struct GameTime;

// The spaceship ending (docs/design/ship.md). The colony researches the ship chain, builds a ship
// out of parts (a Things def with `ship.part`), starts the reactor, holds out for the 15 days it
// takes while raiders keep coming, and launches with the colonists it put in the cryptosleep
// caskets: the game is won. Unreal does not include this header: it reads `Snapshot::ship` and
// sends ShipCommand.

enum class ShipPhase : uint8_t {
    Idle,        // no reactor running
    Activating,  // the reactor is warming up; waves of raiders come
    Ready,       // the reactor is active (it stays so unless it is destroyed); the ship can launch
    Launched,    // the game is won
};

enum class ShipPart : uint8_t { None, Beam, Casket, Reactor, Engine, Core, Sensor };

// The numbers (ShipRules in data/defs/Rules).
struct ShipRules {
    double activationDays = 15.0;
    double waveFirstHours = 6.0;      // first wave this long after the reactor starts
    double waveIntervalHours = 36.0;  // then one wave this often
    float wavePointsFactor = 1.0f;    // of the colony's raid points
    std::vector<std::string> waveFactions{"Pirate", "Remnant"};  // in turn; a faction that can't raid is skipped
    int enginesNeeded = 3;
    static ShipRules read(const DefDatabase& defs);
};

// Everything the ship keeps between steps; saved as a whole (section "ship").
struct ShipState {
    ShipPhase phase = ShipPhase::Idle;
    Entity reactor = kNoEntity;    // the building that is activating or active
    int64_t startedAt = 0;         // game second the reactor was started
    int64_t nextWave = 0;
    int waves = 0;                 // waves sent so far
    // Who rides in which cryptosleep casket, in casket id order.
    std::vector<Entity> caskets;
    std::vector<Entity> riders;
    int escaped = 0;               // pawns that left on the ship
    int64_t launchedAt = 0;
};

// What a finished ship has, counted from its buildings.
struct ShipParts {
    int beams = 0;
    int caskets = 0;
    int reactors = 0;
    int engines = 0;
    int cores = 0;
    int sensors = 0;
    int ridersAboard = 0;       // living pawns in caskets that stand beside a beam
    int unconnectedCaskets = 0; // caskets not touching a beam
};

class COLONY_API ShipSystem {
public:
    ShipSystem(Things& things, RaidSystem& raids, const DefDatabase& defs, const GameTime& time);

    const ShipRules& rules() const { return rules_; }
    const ShipState& state() const { return state_; }
    ShipState& state() { return state_; }
    bool won() const { return state_.phase == ShipPhase::Launched; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }

    // What part a Things def is (None for most).
    ShipPart partOf(DefId def) const;
    // Counts the ship's finished parts (and the riders in connected caskets).
    ShipParts parts() const;
    // Why the ship can't launch now, in plain words; empty when it can.
    std::string launchProblem() const;
    // Game seconds of activation left (0 when not activating or done).
    int64_t activationLeft() const;

    // ---- Commands. False, with a reason in plain words, when they can't be done. ----
    // Starts the reactor (a finished ship reactor, not yet running).
    bool startReactor(Entity reactor, std::string& error);
    // Puts a pawn of the colony into a casket, or (pawn none) empties it. Takes the pawn out of any other casket.
    bool board(Entity casket, Entity pawn, std::string& error);
    // Launches: the riders leave the map and the game is won.
    bool launch(std::string& error);
    // Dev: the reactor that is warming up is active at the next step (RimWorld's dev-mode skip).
    bool finishWarmUp(std::string& error);

    // Waves of raiders while the reactor warms up, the reactor's end, losing a reactor.
    void step();
    // Call before a thing is destroyed.
    void forget(Entity thing);
    // After loading a save. False if it doesn't fit.
    bool restore(ShipState state, std::string& error);
    void hashState(StateHasher& h) const;

private:
    bool alive(Entity pawn) const;
    void emit(EventType type, Entity thing, float amount, const char* message) const;
    bool sendWave();

    Things& things_;
    RaidSystem& raids_;
    const DefDatabase& defs_;
    const GameTime& time_;
    ShipRules rules_;
    ShipState state_;
    std::vector<ShipPart> partByDef_;  // per def id
    std::vector<Event>* events_ = nullptr;
};

COLONY_API const char* shipPhaseName(ShipPhase phase);

}  // namespace colony
