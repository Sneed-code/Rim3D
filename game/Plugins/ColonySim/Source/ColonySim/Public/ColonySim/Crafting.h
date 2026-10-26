#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <vector>

namespace colony {

struct BillUsed;
struct Recipe;
class ApparelSystem;
class StateHasher;
class Things;
class Zones;

// Work tables and crafting (task C14, docs/design/crafting.md). What a bill makes besides the product's
// count: the stuff it is made of, a quality rolled from the crafter's skill, the speed of the bench and
// of the stuff, and where the product is to be stored. The bills and the DoBill job themselves are
// Bills.h and Private/BillJobs.cpp.

// On a product that a bill sends to a stockpile: haulers take it to that zone first (any other
// stockpile if it has no room), and the mark goes when it lies in the zone.
struct StoreIn {
    uint32_t zone = 0;
};

// What is saved besides the items' components: the quality random stream.
struct CraftingState {
    uint64_t rngState = 0;
    uint64_t rngIncrement = 0;
};

class COLONY_API CraftingSystem {
public:
    static constexpr int kQualityLevels = 7;  // Awful to Legendary, as QualityRules.levels

    CraftingSystem(Things& things, ApparelSystem& apparel, const Zones& zones, const DefDatabase& defs, uint64_t seed);

    // The quality of what a crafter with this skill level makes: 0 awful to 5 masterwork, by the wiki's table
    // (CraftingRules.qualityChances). `shift` raises the result that many levels, to 6 legendary at most (an
    // inspiration would; nothing in the sim does yet).
    int rollQuality(int skillLevel, int shift = 0);
    // The chance of a quality level (0 to 6) at a skill level, from the table.
    double qualityChance(int skillLevel, int quality) const;
    // Whether things of this item def have a quality: apparel, melee weapons and anything with `hasQuality`.
    bool hasQuality(DefId item) const;
    // Whether things of this item def can be made of a stuff (`stuffCategories`).
    bool takesStuff(DefId item) const;
    // The stuff a product is made of: of the ingredients used, the item def with the most units that is a stuff
    // of one of the product's `stuffCategories` (the lower def id on a tie). Invalid if there is none.
    DefId dominantStuff(DefId product, const std::vector<BillUsed>& used) const;
    // How much longer or shorter work takes in this stuff: its `stuffProps.statFactors.workToMake` (1 if none).
    float stuffWorkFactor(DefId stuff) const;
    // A bench's work speed: its `stats.workTableWorkSpeedFactor` (1 if none), the hand tailor bench's 0.5.
    float benchSpeed(DefId buildingDef) const;

    // Makes a new product ready to be put down: made of its stuff when the recipe says so, and with a quality
    // when its def has one. `skillLevel` is the crafter's level in the recipe's skill. `storeZone` 0 leaves
    // it; a stockpile marks it for haulers (StoreIn).
    void finishProduct(Entity item, const Recipe& recipe, int skillLevel, const std::vector<BillUsed>& used,
                       uint32_t storeZone);

    // One game minute: a `StoreIn` mark goes when the product lies in its zone or the zone is gone.
    void step();

    CraftingState state() const { return CraftingState{rng_.state(), rng_.increment()}; }
    void restore(const CraftingState& state) { rng_.restore(state.rngState, state.rngIncrement); }
    void hashState(StateHasher& hasher) const;

private:
    Things& things_;
    ApparelSystem& apparel_;
    const Zones& zones_;
    const DefDatabase& defs_;
    Rng rng_;
    std::vector<std::vector<double>> chances_;  // by skill level, then quality 0..5
};

}  // namespace colony
