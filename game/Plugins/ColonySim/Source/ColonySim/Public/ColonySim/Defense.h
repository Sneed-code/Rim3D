#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"
#include "ColonySim/ThreatMap.h"
#include "ColonySim/Things.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace colony {

class CombatSystem;
class FuelSystem;
class HealthSystem;
class Map;
class PowerGrid;
class StateHasher;
class Things;

// Turrets and traps. See docs/design/defense.md.
//
// A turret is a powered building whose def has a `turret` block: while its net has power it looks
// for the nearest able enemy it can see within its gun's range, winds up, fires a burst of real
// projectiles (the same flight, spread and armor as any gun's, `CombatSystem::fireFrom`) and
// cools down. When it is shot to pieces it blows up (`CombatSystem::explode`).
//
// A trap is a building whose def has a `trap` block. An enemy that walks onto an armed one springs it:
// a spike trap stabs, an IED explodes. Everyone on the trap's side knows where it is and never
// springs it; an enemy side learns of it when one of its pawns springs it, or when one of its fighters
// walks close enough to notice, and from then on its fighters plan their spots and their walks around it.
//
// Unreal does not include this header: it reads `turretPhase`, `turretTarget` and `trapState` from the
// snapshot, ShotFired, TrapSprung, TrapRearmed and Explosion from the event list, and sends
// RearmTrapCommand.

enum class TurretPhase : uint8_t {
    Idle,     // no target (or no power)
    Warming,  // aiming at `target` before the burst
    Firing,   // between the shots of a burst
    Cooling,  // after the burst, until it can look again
};

// On every building whose def has a `turret` block (Things.cpp adds it). Every field is saved and hashed.
struct Turret {
    TurretPhase phase = TurretPhase::Idle;
    int stepsLeft = 0;  // of the phase
    int shotsLeft = 0;  // in the burst, counting the next
    Entity target = kNoEntity;
};

// On every building whose def has a `trap` block. Every field is saved and hashed.
struct Trap {
    bool sprung = false;               // a sprung spike trap waits to be rearmed
    std::vector<FactionId> known;      // enemy sides that know where it is (sorted)
};

// The numbers in the DefenseRules def (with these values when the def is missing).
struct COLONY_API DefenseNumbers {
    int scanEvery = 10;           // steps between a turret's looks for a target (staggered by building id)
    float lead = 1.0f;            // how far turrets lead a moving target (1 perfect, 0 none)
    int noticeEvery = 15;         // steps between enemy fighters' looks for traps nearby
    float noticeRadius = 4.0f;    // metres from which an enemy fighter may notice an armed trap
    float noticeChance = 0.35f;   // per look, per fighter and trap
    float knownSpringChance = 0.0f;  // an enemy that knows the trap still springs it this often (it walks over it by accident)
    float avoidMargin = 0.15f;    // metres a walk keeps outside a known trap's cell
    int detourTries = 4;          // detours made on one walk before it gives up and goes through

    static DefenseNumbers read(const DefDatabase& defs);
};

struct DefenseModel;

class COLONY_API DefenseSystem {
public:
    DefenseSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, PowerGrid& power,
                  const DefDatabase& defs, const GameTime& time);
    ~DefenseSystem();
    DefenseSystem(const DefenseSystem&) = delete;
    DefenseSystem& operator=(const DefenseSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Ammunition: a turret whose def has a `fuel` block with `perUse` uses that much of it for every shot, and fires only
    // while it has it (colonists refuel it like any building, docs/design/fuel.md). Null: turrets never run dry.
    void setFuel(FuelSystem* fuel) { fuel_ = fuel; }
    // Called before the system destroys a thing itself (a trap that went off), so jobs can let go of it.
    void setBeforeDestroy(std::function<void(Entity)> hook) { beforeDestroy_ = std::move(hook); }
    // Called for every fighter of a side that has just noticed a trap, so it plans its spot and walk again round it
    // (the fighter system's `replan`).
    void setOnLearn(std::function<void(Entity)> hook) { onLearn_ = std::move(hook); }
    const DefenseNumbers& numbers() const { return numbers_; }
    // For tests and tools: other numbers than the data files'.
    void setNumbers(const DefenseNumbers& numbers) { numbers_ = numbers; }

    bool isTurret(DefId def) const;
    bool isTrap(DefId def) const;

    // Rearms a sprung trap. False, with a reason, if it is not a trap, is armed already, or can't be used again
    // (an IED is gone once it goes off).
    bool rearm(Entity trap, std::string* why = nullptr);
    // A sprung trap that can be used again: what the Rearm job (colonists, Construction work type) looks for.
    bool needsRearm(Entity trap) const;
    // Seconds of work the trap's rearming takes (0 for a thing that is not a trap).
    float rearmSeconds(Entity trap) const;

    // Whether a pawn knows where a trap is: pawns of its side, and pawns of a side that learned of it.
    bool knows(Entity pawn, Entity trap) const;
    // The side `faction` knows an armed trap in `cell`: its fighters keep out of that cell and walk round it.
    bool avoidsCell(FactionId faction, CellPos cell) const;
    // Bends a walk (cells after the start, as the pathfinder and smoothPath give them) round the cells of armed traps
    // the side knows, with the detours `DefenseNumbers::detourTries` allows. The last cell is never changed.
    void detour(FactionId faction, float startX, float startY, std::vector<CellPos>& route) const;

    // The hook for buildings shot to pieces (CombatSystem::setBeforeDestroy): a building with an
    // `explosion` that `explodeWhenDestroyed` goes up in the next step.
    void onDestroy(Entity building);

    // One step: turrets look, aim and fire, enemies step on traps, blasts queued by destroyed buildings go off.
    void step(Rng& rng);

    // Hashes the turret and trap components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    uint64_t shotsFired() const { return shots_; }
    uint64_t trapsSprung() const { return sprung_; }

private:
    struct PawnInfo {
        Entity pawn = kNoEntity;
        FactionId faction = 1;  // kPlayerFaction
        float x = 0.0f;
        float y = 0.0f;
        int level = 0;
        bool able = false;     // alive and not downed
        bool animal = false;
        bool fighter = false;
    };
    struct ArmedTrap {
        Entity trap = kNoEntity;
        CellPos cell;
    };
    struct Blast {
        Explosion explosion;
    };

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    PowerGrid& power_;
    FuelSystem* fuel_ = nullptr;
    const GameTime& time_;
    DefenseNumbers numbers_;
    std::unique_ptr<DefenseModel> model_;
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity)> beforeDestroy_;
    std::function<void(Entity)> onLearn_;
    std::vector<PawnInfo> pawns_;  // scratch: every living pawn on the map, by id
    mutable std::vector<ArmedTrap> armed_;  // armed traps by id, for the step `armedAt_`
    mutable int64_t armedAt_ = -1;
    mutable size_t armedCount_ = 0;
    std::vector<Entity> order_;
    std::vector<Blast> blasts_;    // queued by onDestroy
    uint64_t shots_ = 0;
    uint64_t sprung_ = 0;

    void collectPawns();
    const std::vector<ArmedTrap>& armedTraps() const;
    void stepTurret(Entity building, Turret& turret, Rng& rng);
    Entity pickTarget(const Position& pos, float range, float minRange, float height) const;
    bool canSee(const Position& pos, float height, Entity target, float range) const;
    void stepTraps(Rng& rng);
    void spring(Entity trap, Entity victim, Rng& rng);
    void learn(Trap& trap, FactionId faction);
    FactionId factionOf(Entity pawn) const;
    bool segmentHits(float x0, float y0, float x1, float y1, CellPos cell) const;
    void queueBlast(Entity building, const Position& pos);
};

}  // namespace colony
