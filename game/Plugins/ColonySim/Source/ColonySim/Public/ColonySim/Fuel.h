#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class Lights;
class Things;

// Fuel for buildings (task C11, docs/design/fuel.md): the stove burns wood to cook, a campfire and a
// torch lamp burn it all the time, a wood-fired generator will (E5). A refuel job under Hauling
// (RimWorld's WorkGiver_Refuel) brings the wood; a building out of fuel stops.
//
// Unreal does not include this header: it reads fuel from the snapshot and changes the refuel
// settings through SetRefuelCommand (docs/design/bridge.md).

// On a building whose Things def has a `fuel` field.
struct Fuel {
    float amount = 0.0f;       // in items of the def's fuel item (a campfire burns 12 a day, not whole items)
    bool autoRefuel = true;    // colonists refuel it when it is low (RimWorld's "allow auto refuel")
    bool fillOrdered = false;  // the player asked for it to be filled now; cleared when it is full
};

// A building def's `fuel` field, read once (RimWorld's CompProperties_Refuelable).
struct FuelSpec {
    bool present = false;
    DefId item;                    // the Things item def that burns
    float capacity = 0.0f;         // most it holds, in items
    float perSecond = 0.0f;        // burnt all the time while it has any (campfire, torch lamp); 0 for none
    float perUse = 0.0f;           // burnt per product of a bill finished on it (the stove)
    float autoRefuelBelow = 0.3f;  // colonists refuel at or below this fraction of capacity
    float initial = 0.0f;          // fraction of capacity a new building starts with (RimWorld: empty)
};

// Reads a Things def's `fuel` field. `present` is false for a def without one.
COLONY_API FuelSpec readFuelSpec(const DefDatabase& defs, const Def& def);

// Burns, counts and refills fuel. Owned by the job system, which asks it before it hands out a
// job that needs fuel; stepped once a game second.
class COLONY_API FuelSystem {
public:
    // Game seconds of work to put fuel in (RimWorld: 240 ticks).
    static constexpr float kRefuelWorkSeconds = 345.6f;

    FuelSystem(Things& things, const DefDatabase& defs, std::vector<Event>& events);

    // The spec of a Things def, or an empty one (`present` false).
    const FuelSpec& spec(DefId def) const;
    const FuelSpec& specOf(Entity building) const;

    // A building with a `glower` goes dark when its fuel runs out and lights again when refuelled; the
    // light layer is told (E3, docs/design/light.md).
    void setLights(Lights* lights) { lights_ = lights; }

    // Which buildings burn their fuel at the moment (the default: all). The sim says a generator burns
    // only while it makes power (E5); campfires and torch lamps always do.
    void setBurning(std::function<bool(Entity)> burning) { burning_ = std::move(burning); }

    // ---- Burning -----------------------------------------------------------------------------
    // One game second: buildings that burn all the time use some of theirs. Sends OutOfFuel when a
    // building's fuel reaches 0.
    void step();
    // True if the building needs no fuel, or has any.
    bool lit(Entity building) const;
    // True if the building needs no fuel, or has at least `amount`.
    bool canUse(Entity building, float amount) const;
    // Burns `amount` (at most what is left). Sends OutOfFuel when that empties it.
    void use(Entity building, float amount);

    // ---- Refuelling --------------------------------------------------------------------------
    // 0 (empty) to 1 (full); -1 for a building with no fuel.
    float fraction(Entity building) const;
    // Whole items of the fuel item the building has room for.
    int roomFor(Entity building) const;
    // True if a colonist should refuel it: auto refuel is on and it is at or below its threshold, or
    // the player ordered a fill; and there is room for an item.
    bool wantsFuel(Entity building) const;
    // Puts `count` of the fuel item in (clamped to the room). Returns how many went in. Sends Refueled.
    int add(Entity building, int count);

    // ---- Commands ----------------------------------------------------------------------------
    // SetRefuelCommand: whether colonists refuel the building by themselves, and a fill order.
    bool setRefuel(Entity building, bool autoRefuel, bool fillNow, std::string& error);

private:
    Things& things_;
    const DefDatabase& defs_;
    std::vector<Event>& events_;
    Lights* lights_ = nullptr;
    std::function<bool(Entity)> burning_;
    std::vector<FuelSpec> specs_;  // per def id
    FuelSpec none_;
};

}  // namespace colony
