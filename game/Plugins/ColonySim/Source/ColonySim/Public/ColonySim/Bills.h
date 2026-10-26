#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/BillMode.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class Things;

// Recipes and bills: what a work building (the stove) is asked to make, and for how long. See
// docs/design/bills.md.
//
// Unreal does not include this header: it reads bills through the snapshot and changes them
// through commands (docs/design/bridge.md).

// One line on a work building's bill list.
struct Bill {
    uint32_t id = 0;  // 1, 2, 3... per building, never reused
    DefId recipe;
    BillMode mode = BillMode::Count;
    int count = 1;  // Count: times left; UntilHave: the stock to reach; Forever: unused
    bool suspended = false;
    // The details RimWorld's bill dialog adds (task C14, docs/design/crafting.md).
    int skillMin = 0;        // only a worker whose skill in the recipe's skill is within [skillMin, skillMax] takes it
    int skillMax = 20;
    float radius = 0.0f;     // ingredients are taken from within this many metres of the building; 0 is anywhere
    uint32_t storeZone = 0;  // 0: products stay where the crafter made them; else haulers take them to this stockpile
    int unpauseAt = 0;       // UntilHave: once `count` is reached the bill waits until the stock is under this (0: restarts under `count`)
    bool satisfied = false;  // UntilHave with `unpauseAt`: the stock reached `count` and has not fallen under `unpauseAt` since
    std::vector<DefId> disallowed;  // item defs the bill may not use as ingredients, sorted by value
};

// On a building that has had a bill added. Bills are tried in list order: the first that should be
// done and whose ingredients a cook can bring wins.
struct BillList {
    std::vector<Bill> bills;
    uint32_t nextId = 1;
};

// One thing a recipe needs: items of any of `items`, `nutrition` worth (of their `stats.nutrition`)
// or `count` of them.
struct RecipeIngredient {
    std::vector<DefId> items;  // every Things item def the filter accepts, sorted
    bool byNutrition = false;
    double amount = 0.0;  // nutrition, or a count
    // The items to fetch to cover `amount` when each unit of `item` is worth `unitNutrition`.
    int unitsNeeded(double unitNutrition) const;
};

struct RecipeProduct {
    DefId item;
    int count = 1;
};

// A def of kind Recipes, read once (RimWorld's RecipeDef).
struct Recipe {
    DefId def;
    std::string defName;
    std::string label;
    std::vector<DefId> users;  // the building defs that can hold a bill for it
    float work = 0.0f;         // game seconds of work at cooking speed 1
    std::string skill;         // the Skills def that speeds it up
    int minSkill = 0;
    float skillXp = 0.0f;
    std::vector<RecipeIngredient> ingredients;
    std::vector<RecipeProduct> products;
    bool stuffFromIngredient = false;  // the product is made of the stuff the ingredients are (clothes, weapons)
    // Cuts up a corpse (N2, docs/design/hunting.md): the ingredient is a dead animal, not a stack of items, and the products
    // (meat and leather) come from the animal's def. "Until you have" counts the raw meat on the map.
    bool butcher = false;
    // How many things one run makes. A building that burns fuel per product burns this many times its `fuel.perUse`.
    int productCount() const {
        int made = 0;
        for (const RecipeProduct& product : products) {
            made += product.count;
        }
        return made;
    }
};

// The numbers in the BillRules def (with these values when the def is missing).
struct BillNumbers {
    int maxBillsPerBuilding = 15;
    int maxBillCount = 9999;
    double skillSpeedBase = 0.6;  // cooking speed = base + perLevel x skill level
    double skillSpeedPerLevel = 0.05;
    int defaultSkillLevel = 8;  // for a worker with no skills (a test pawn)
    int maxIngredientStacks = 8;
};

class COLONY_API BillSystem {
public:
    BillSystem(Things& things, const DefDatabase& defs);

    const BillNumbers& numbers() const { return numbers_; }
    const std::vector<Recipe>& recipes() const { return recipes_; }
    // The recipe of a Recipes def, or null.
    const Recipe* recipe(DefId def) const;

    // ---- Commands ----------------------------------------------------------------------------
    // Adds a bill for `recipe` (a Recipes defName) to a finished building that can make it. Returns
    // its id, or 0 with `error` in plain words. `count` is how many times (Count), the stock to
    // reach (UntilHave), and ignored for Forever.
    uint32_t addBill(Entity building, const std::string& recipe, BillMode mode, int count, std::string& error);
    bool removeBill(Entity building, uint32_t bill, std::string& error);
    // Changes a bill's mode, count and whether it is suspended (RimWorld's pause).
    bool setBill(Entity building, uint32_t bill, BillMode mode, int count, bool suspended, std::string& error);

    // The bill's details (everything but mode, count and suspended). `skillMin` to `skillMax` is 0 to 20 at the
    // widest; `radius` is 0 or more metres; `storeZone` is 0 or a stockpile (the caller checks it exists);
    // `disallowed` are Things item defNames, unknown names fail.
    struct Options {
        int skillMin = 0;
        int skillMax = 20;
        float radius = 0.0f;
        uint32_t storeZone = 0;
        int unpauseAt = 0;
        std::vector<std::string> disallowed;
    };
    bool setOptions(Entity building, uint32_t bill, const Options& options, std::string& error);
    // Moves a bill to place `index` in the building's list (0 is tried first; past the end is the last place).
    bool moveBill(Entity building, uint32_t bill, int index, std::string& error);

    // ---- Looking up --------------------------------------------------------------------------
    const BillList* billsOf(Entity building) const;
    const Bill* find(Entity building, uint32_t bill) const;
    // True if the building def is one of the recipe's users.
    bool canMake(DefId buildingDef, const Recipe& recipe) const;
    // True if the bill is not suspended and still has something to make: Count has times left,
    // UntilHave has fewer than `count` of the first product on the map, Forever always.
    bool shouldDo(const Bill& bill) const;
    // True if the bill's ingredient filter lets the item def through.
    static bool allowsIngredient(const Bill& bill, DefId item);
    // One game minute of upkeep: "until you have" bills with an `unpauseAt` pause at their stock and wake
    // when it falls.
    void step();
    // How many of an item def lie on the map (carried and delivered ones are not counted).
    int stockOf(DefId item) const;
    // The stock an "until you have" bill of the recipe counts: its first product, or for butchering all raw meat.
    // False if the recipe makes nothing countable.
    bool stockFor(const Recipe& recipe, int& stock) const;
    // Cooking speed of a pawn for a recipe: (base + perLevel x level of the recipe's skill), 1.0 at
    // level 8. Without a Colonist the default skill level counts.
    float workSpeed(Entity pawn, const Recipe& recipe) const;
    int skillLevel(Entity pawn, const std::string& skill) const;

    // ---- For the job system ------------------------------------------------------------------
    // A bill's times left or stock target after one more iteration is done: Count goes down by one
    // and the bill is removed when it reaches 0. Returns true if the bill was removed.
    bool iterationDone(Entity building, uint32_t bill);
    // The nutrition of one unit of an item def (0 if it is not food).
    double nutritionOf(DefId item) const;

private:
    Things& things_;
    const DefDatabase& defs_;
    BillNumbers numbers_;
    std::vector<Recipe> recipes_;  // in def id order
    std::vector<double> nutrition_;  // per def id
    std::vector<uint8_t> rawMeat_;   // per def id: an item in the MeatRaw category
};

}  // namespace colony
