#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace colony {

class CombatSystem;
class DefDatabase;
class HealthSystem;
class Map;
class StateHasher;
class Things;

// Thrown explosives. See docs/design/explosives.md. The explosion itself (radius, falloff, cover,
// fragments, armor, buildings, knock-down) is `CombatSystem::explode`; this is what throws things
// that end in one.
//
// A grenade is an item (a Things def with a `thrown` block). A pawn carries them on its belt
// (`Belt`), throws one with ThrowCommand (a wind-up, then the grenade flies in an arc, lands, and
// explodes when its fuse runs out). A thrown grenade is an entity with a `Grenade` component and no
// `Thing`; it is in the snapshot's projectiles. Unreal draws it and the explosion's effects; it
// never decides who is hurt.
//
// Unreal does not include this header: it reads the belt, throw timing and the grenades in flight
// from the snapshot, GrenadeThrown, GrenadeLanded and Explosion from the event list, and sends
// ThrowCommand and EquipCommand (to put a grenade stack on a belt).

// A stack of one kind of grenade on a pawn's belt (a Things def).
struct BeltStack {
    DefId item;
    int count = 0;
};

// On a pawn that carries explosives. Stacks are in the order they were first taken.
struct Belt {
    std::vector<BeltStack> stacks;
};

enum class ThrowPhase : uint8_t {
    Ready,    // nothing to do
    Winding,  // the wind-up before the release
    Cooling,  // after the release, until it can throw again
};

// A pawn's throwing arm: an order to throw, then the cooldown. Added the first time a pawn throws
// and kept. It has its own timing, apart from the gun's `Shooter`.
struct Thrower {
    ThrowPhase phase = ThrowPhase::Ready;
    int stepsLeft = 0;
    DefId item;                 // the Things def being thrown
    Entity target = kNoEntity;  // aim at the ground under this pawn at the release, if it is still there
    float aimX = 0.0f;          // otherwise at this point
    float aimY = 0.0f;
    bool player = false;
};

// A thrown explosive in flight or lying on the ground waiting for its fuse. Not a Thing.
struct Grenade {
    DefId def;                    // Projectiles def (has an `explosion` and a `thrown` block)
    DefId item;                   // the Things def it was thrown as (invalid if made by a command)
    Entity thrower = kNoEntity;   // never hit by its own flight
    float x = 0.0f;               // metres; z above sea level
    float y = 0.0f;
    float z = 0.0f;
    float vx = 0.0f;              // metres per step
    float vy = 0.0f;
    float vz = 0.0f;
    int level = 0;
    int age = 0;                  // steps since the throw
    int fuse = -1;                // -1 while it flies; steps left once it has landed
    CellPos startCell;            // the thrower's cell: a building there never stops the throw
    uint64_t seed = 0;            // where the blast's choices (fragments, parts, armor) come from
};

// A grenade that is, or will be, a danger: where it lands and how long until it goes off, for the
// fighter AI's dive (ExplosiveSystem::dangers).
struct GrenadeDanger {
    Entity grenade = kNoEntity;
    float x = 0.0f;         // where it explodes, metres
    float y = 0.0f;
    float z = 0.0f;
    int level = 0;
    float radius = 0.0f;
    int stepsToBlast = 0;   // from now
    int age = 0;            // steps since the throw (a fighter notices it after its reaction time)
    Entity thrower = kNoEntity;
};

// What a plan for a throw found (ExplosiveSystem::planThrow).
struct ThrowPlan {
    bool reaches = false;   // the grenade lands within `landingTolerance` of the aimed point
    float vx = 0.0f;        // the release velocity, metres per step
    float vy = 0.0f;
    float vz = 0.0f;
    float landX = 0.0f;     // where the grenade comes to rest if thrown so
    float landY = 0.0f;
    int flightSteps = 0;
};

struct ExplosiveModel;

class COLONY_API ExplosiveSystem {
public:
    ExplosiveSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, const DefDatabase& defs);
    ~ExplosiveSystem();
    ExplosiveSystem(const ExplosiveSystem&) = delete;
    ExplosiveSystem& operator=(const ExplosiveSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }

    // True if the Things def is a grenade (has a `thrown` block).
    bool isThrown(DefId item) const;
    // Metres a pawn can throw it (the def's `thrown.range`); 0 if it is not a grenade.
    float throwRange(DefId item) const;
    // Blast radius of what the item throws, metres; 0 if it is not a grenade.
    float blastRadius(DefId item) const;

    // Puts an item stack on a pawn's belt, up to the belt's limit (ExplosiveRules.beltLimit); what
    // does not fit stays where it is. The item may lie on the map. False, changing nothing, if the
    // pawn has no body or is dead, the item is not a grenade or is someone else's, or the belt has
    // no room.
    bool take(Entity pawn, Entity item, std::string* why = nullptr);
    // Puts `count` grenades of a Things def on the belt without an item (a raider's kit, tests).
    // Returns how many fitted.
    int give(Entity pawn, DefId item, int count);
    // Puts everything on the belt on the ground where the pawn stands (or loses it if the pawn is off the
    // map). For pawns about to be destroyed.
    void dropBelt(Entity pawn);
    // Grenades of that def on the belt (any def if invalid).
    int count(Entity pawn, DefId item = DefId{}) const;
    // The Things def the next throw uses: the first stack that has one. Invalid if none.
    DefId firstItem(Entity pawn) const;

    // Orders a throw: the pawn winds up, then throws one grenade of `item` (the first stack if
    // invalid) at the ground under `target` if it is a pawn on the map, else at (x, y). A player's
    // throw has no wind-up (ExplosiveRules.playerWarmup). During the wind-up a new order only
    // changes the aim. False, with a reason, if the pawn can't: no such grenade, downed, knocked
    // down, mid-shot, or still cooling down from the last throw.
    bool throwAt(Entity pawn, DefId item, Entity target, float x, float y, bool player, std::string* why = nullptr);
    // The pawn is winding up a throw (hold still), or the arm is not free for a new one.
    bool throwing(Entity pawn) const;
    // Steps until it can start a new throw (0: ready now).
    int throwReadyIn(Entity pawn) const;

    // How a pawn would throw at a point on the ground: the release velocity of an arc that lands
    // there, taking the steeper arc if the flatter one is blocked. `reaches` says whether the
    // grenade really comes to rest within `landingTolerance` of the point (walls, cover and pawns
    // in the way stop it short). Reads the pawn's position and stance. False if the pawn has no
    // such grenade.
    bool planThrow(Entity pawn, DefId item, float tx, float ty, ThrowPlan& out) const;

    // Every grenade that will explode, with where and when (a flying one at its predicted landing).
    void dangers(std::vector<GrenadeDanger>& out) const;

    // Makes a grenade at a point already lying on the ground, fuse running (dev command and tests).
    Entity place(DefId projectile, float x, float y, int level, Entity thrower, Rng& rng);

    // One step: throws wind up and are released, grenades fly, land and explode.
    void step(Rng& rng);

    // Hashes the explosive components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

private:
    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    std::unique_ptr<ExplosiveModel> model_;
    std::vector<Event>* events_ = nullptr;
    std::vector<Entity> scratch_;

    void release(Entity pawn, Thrower& t, Rng& rng);
    void flyOrBlow(Entity grenade, Rng& rng);
    bool simulate(Grenade g, float& endX, float& endY, float& endZ, int& steps) const;
    float releaseHeight(Entity pawn) const;
};

}  // namespace colony
