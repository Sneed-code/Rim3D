#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace colony {

struct BillUsed;
class Map;
class RoomStats;
class StateHasher;
class TemperatureSystem;
class Things;

// Rot and storage (task C16, docs/design/spoilage.md). Food rots by the temperature of the cell it
// lies in: not at all below freezing, slowly up to 10 degrees, at full speed above (so a cold room
// or a freezer keeps it). Rotted food is gone. Items with a `stats.deteriorationRate` that lie under
// the open sky lose hit points and are gone at 0. A meal remembers what it was cooked from, and a
// kitchen that is dirty, or a cook who cannot cook, may poison it; whoever eats a poisoned meal may
// get food poisoning.
//
// The per-item data is the `Spoilage` component (Things.h).

// The numbers from the SpoilageRules def (these values when it is missing).
struct COLONY_API SpoilageNumbers {
    int intervalSeconds = 60;       // items are worked out this often (game seconds)
    float rotFullAboveC = 10.0f;    // at or above this, food rots at full speed
    float rotNoneBelowC = 0.0f;     // at or below this, it does not rot; in between the speed is linear
    float outdoorFactor = 1.0f;     // deterioration under the open sky; E1 raises it in rain
    float poisonOnIngest = 0.35f;   // chance that eating a poisoned meal gives food poisoning
    float dirtyChance = 0.15f;      // chance a meal is poisoned in the dirtiest kitchen
    float cleanAtLeast = 0.4f;      // kitchens this clean (RoomStats "clean") add no chance
    float dirtiest = -1.1f;         // kitchens this dirty (RoomStats "very dirty") add all of it
    float unskilledChance = 0.04f;  // chance a meal is poisoned by a cook of skill 0
    int skillSafe = 8;              // cooks of this skill or better add none

    static SpoilageNumbers read(const DefDatabase& defs);
};

// What is saved besides the items' components: the random stream.
struct SpoilageState {
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
};

class COLONY_API SpoilageSystem {
public:
    SpoilageSystem(Map& map, Things& things, TemperatureSystem& temperature, RoomStats& roomStats, const DefDatabase& defs,
             uint64_t seed);

    const SpoilageNumbers& numbers() const { return numbers_; }

    // E1 sets this to more than 1 in rain: how much faster items deteriorate under the open sky.
    void setOutdoorFactor(float factor) { numbers_.outdoorFactor = factor; }
    // Where ItemSpoiled and PawnPoisoned events go; null (the default) reports nothing.
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // H2 installs this to give the pawn the FoodPoisoning hediff. Without it the event is all that happens.
    void setPoisonHandler(std::function<void(Entity pawn)> handler) { poisoned_ = std::move(handler); }

    // How fast food rots in this temperature: 0 to 1.
    float rotSpeedAt(float celsius) const;

    // A new item (not yet put down) was cooked by `cook` standing at `cell` from `used`: it
    // remembers the ingredients, and is poisoned by chance if it is a meal.
    void madeBy(Entity item, const std::vector<BillUsed>& used, Entity cook, CellPos cell);
    // The chance a meal cooked by `cook` at `cell` is poisoned (0 to 1).
    float poisonChance(Entity cook, CellPos cell);
    // `pawn` ate `item` (before it is destroyed): if the stack was poisoned, rolls for food poisoning.
    // Returns true if the pawn got it.
    bool eaten(Entity pawn, Entity item);

    // One game second. Every `intervalSeconds` the rot and wear of every item are brought up to date.
    void step(int64_t now);

    SpoilageState state() const { return SpoilageState{rng_.state(), rng_.increment()}; }
    void restore(const SpoilageState& state) { rng_.restore(state.rngState, state.rngIncrement); }
    void hashState(StateHasher& hasher) const;

private:
    bool sheltered(CellPos cell) const;
    void report(Entity item, DefId def, CellPos cell, const char* how);

    Map& map_;
    Things& things_;
    TemperatureSystem& temperature_;
    RoomStats& roomStats_;
    const DefDatabase& defs_;
    SpoilageNumbers numbers_;
    Rng rng_;
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity)> poisoned_;
    std::vector<float> rotDays_;         // by def id; 0 if the def does not rot
    std::vector<float> deterioration_;   // by def id: hit points a game day
    std::vector<uint8_t> isMeal_;
    std::vector<uint8_t> shelters_;      // by def id: a building that keeps what lies on it dry
    std::vector<Entity> scratch_;
};

}  // namespace colony
