#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class AnimalSystem;
class CombatSystem;
class DefDatabase;
class FighterSystem;
class HealthSystem;
class Map;
class RaidSystem;
class Rng;
class Things;
struct GameTime;

// Mechanoids (the Remnant) and chitinid hives (task T3 part, docs/design/monsters.md).
//
// Two incidents the narrator or a dev command can call:
//  - `infest`: brood mounds (`Hive`) grow in a roofed place near the colony and their insects (brood
//    scarabs, delvers, stalkers) come out hunting. The mounds then make jelly, breed insects, and spread.
//  - `remnantCluster`: mechanoids walk in at a map edge, bought with points by `race.combatPower`, and
//    fight with the fighter AI until they are destroyed (they do not rout).
//
// The system keeps no state of its own and so adds nothing to the save: a mound's jelly, brood and
// spread are due at times worked out from its entity id and the clock, and an insect is an animal that
// is enraged again whenever a colonist is near.
class COLONY_API MonsterSystem {
public:
    MonsterSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, FighterSystem& fighters,
                  RaidSystem& raids, AnimalSystem& animals, const DefDatabase& defs, const GameTime& time, Rng& rng);
    ~MonsterSystem();
    MonsterSystem(const MonsterSystem&) = delete;
    MonsterSystem& operator=(const MonsterSystem&) = delete;

    // `points` 0: what the colony is worth now (the raid rule). False, with a reason, when it can't be done.
    bool infest(float points, std::string& error);
    bool remnantCluster(float points, int edge, std::string& error);
    // A single mound at `cell` (a dev tool and test hook).
    Entity spawnHive(CellPos cell);

    // Once a game second, with the events of the step (a destroyed mound leaves jelly behind).
    void step(const std::vector<Event>& events, size_t from);

    size_t hiveCount() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace colony
