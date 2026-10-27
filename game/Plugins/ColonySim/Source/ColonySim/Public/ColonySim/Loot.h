#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class Map;
class Things;
struct RaidState;

// Loot (docs/design/loot.md): what raiders carry and drop when they die, what a cargo pod holds, what
// an ancient danger or a ruin stash hides. All of it is data: a `LootTables` def says what a table
// can give, and anything that wants loot (the raid, the narrator's cargo pod, quests) rolls a table
// by name and drops what came out.
//
// A table has two parts, either of which may be missing:
//   items:  [{thing, chance (1), count [min,max], perHundredPoints}]  each rolled on its own
//   stacks: {rolls [min,max], value [lo,hi], pointsScaled, from: [{thing, weight, count [min,max]}]}
//           each roll picks one thing by weight and makes a stack worth `value` silver (its count
//           follows its `stats.marketValue`, up to its stack limit; `count` overrides that)
// With `pointsScaled`, the stack value grows with the raid points (points / referencePoints), and an
// item's count grows by `perHundredPoints` per hundred points. Without points (0) the scale is 1.
struct LootItem {
    DefId def;
    int count = 0;
};

// The numbers of the LootRules def (these values when it is missing).
struct COLONY_API LootNumbers {
    float referencePoints = 100.0f;  // raid points at which a points-scaled table gives its plain amounts
    float minScale = 0.5f;           // points scale is kept between these
    float maxScale = 4.0f;
    int dropRadius = 6;              // loot is put down this many cells from where it falls
    int podMinDistance = 10;         // a cargo pod lands between these distances from the colony (cells)
    int podMaxDistance = 40;

    static LootNumbers read(const DefDatabase& defs);
};

class COLONY_API LootSystem {
public:
    LootSystem(Things& things, Map& map, const DefDatabase& defs, uint64_t seed);

    const LootNumbers& numbers() const { return numbers_; }
    bool hasTable(const std::string& table) const;

    // What a table gives this time. `points` is the raid's points (0: no scaling). Unknown table: empty.
    std::vector<LootItem> roll(const std::string& table, float points, Rng& rng) const;
    // The table a pawn kind carries (its `lootTable`, empty if none).
    std::string tableOfKind(const std::string& pawnKind) const;
    // A stream of its own for a roll that has to be the same on every run: from the seed, a salt
    // (a thing's id, say) and the game second.
    Rng rngFor(uint64_t salt, int64_t second) const;

    // Puts the items on the ground near `cell` (not on a stack of another def, one stack to a cell
    // where it can, stacks cut at the stack limit). Returns the items made.
    std::vector<Entity> drop(const std::vector<LootItem>& items, CellPos cell);

    // A cargo pod: rolls `table` and drops it at a free open-air cell `podMinDistance` to
    // `podMaxDistance` cells from `centre`. False if there is no such cell. `where` and `items` say
    // what happened.
    bool cargoPod(const std::string& table, CellPos centre, Rng& rng, CellPos& where, std::vector<LootItem>& items);

    // Raiders that died in the events since `from` drop their kind's table where they lie, scaled by
    // their squad's points. Call once a step.
    void step(const std::vector<Event>& events, size_t from, const RaidState& raids, int64_t second);

    // "3 steel, 1 packaged survival meal": for letters and logs.
    std::string describe(const std::vector<LootItem>& items) const;

private:
    struct Entry {
        DefId thing;
        float chance = 1.0f;
        int countMin = 1;
        int countMax = 1;
        bool hasCount = false;
        float weight = 1.0f;
        float perHundredPoints = 0.0f;
    };
    struct Table {
        std::string name;
        std::vector<Entry> items;
        int rollsMin = 0;
        int rollsMax = 0;
        float valueLo = 0.0f;
        float valueHi = 0.0f;
        bool pointsScaled = false;
        std::vector<Entry> from;
    };

    const Table* find(const std::string& name) const;
    int stackLimit(DefId def) const;
    bool freeCell(CellPos cell) const;

    Things& things_;
    Map& map_;
    const DefDatabase& defs_;
    uint64_t seed_;
    LootNumbers numbers_;
    std::vector<Table> tables_;
};

}  // namespace colony
