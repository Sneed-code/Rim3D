#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defense.h"
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
class FuelSystem;
class HealthSystem;
class JobSystem;
class Map;
class PowerGrid;
class StateHasher;
class Things;

// Manned artillery: the mortar and the rocketswarm launcher. See docs/design/artillery.md.
//
// A building whose def has an `artillery` block fires only while a colonist mans it: one ordered there (the
// ManArtillery job, standing beside it) or the player's own colonist standing within reach. It fires at a point or
// a pawn the player picks (ArtilleryCommand "Aim"), or, for a mortar, at the nearest enemy anywhere in range. Every
// shell lands at a random point within the miss radius of its aim (for a mortar, scaled by the crew's Shooting,
// Manipulation and Sight). A mortar's shells fly over walls and land (an `ArtilleryShell`, not a projectile); only a
// mountain's thick roof stops them, and a mortar under any roof can't fire. Rocketswarm rockets are ordinary
// projectiles that burst where they hit. Ammunition is the building's fuel (fuel.md): shells, chemfuel.
//
// Unreal does not include this header: it reads `turretPhase`, `artilleryCrew`, `artilleryManned`, the aim and
// `holdFire` from the snapshot, shells in flight from the snapshot's projectiles, ShotFired, ProjectileHit and
// Explosion from the event list, and sends ArtilleryCommand.

// On every building whose def has an `artillery` block (Things.cpp adds it). Every field is saved and hashed.
struct Artillery {
    TurretPhase phase = TurretPhase::Idle;
    int stepsLeft = 0;              // of the phase; warm-up and cooldown count only while it is manned
    int shotsLeft = 0;              // in the volley, counting the next
    Entity crew = kNoEntity;        // the colonist ordered to man it (on its way or at its post)
    bool holdFire = false;          // the player's hold fire: no shots, not even at its aim
    bool hasAim = false;            // the player picked a point or a pawn
    float aimX = 0.0f;              // the picked point, metres (or where the picked pawn was last)
    float aimY = 0.0f;
    Entity aimPawn = kNoEntity;     // the picked pawn, followed while it is on the map
    Entity target = kNoEntity;      // the pawn this volley is at (the aim's, or one it picked itself), else none
    float volleyX = 0.0f;           // the volley's point, fixed when it is aimed at a point
    float volleyY = 0.0f;
    std::vector<CellPos> hitCells;  // the rocketswarm's: cells this volley's rockets went for (each takes a new one)
};

// A mortar shell in flight: from the muzzle to where it lands, over everything. Not a Thing. Every field is saved
// and hashed.
struct ArtilleryShell {
    DefId def;                     // Projectiles def (its `explosion` is set off where it lands)
    DefId gun;                     // the gun's Things def
    Entity launcher = kNoEntity;   // the mortar
    float fromX = 0.0f;            // metres; z above sea level
    float fromY = 0.0f;
    float fromZ = 0.0f;
    float toX = 0.0f;              // where it lands
    float toY = 0.0f;
    int level = 0;
    int steps = 1;                 // of the flight
    int age = 0;                   // steps flown
    uint64_t seed = 0;             // where the blast's choices come from
};

// Where a shell is on its arc now, and its velocity in metres per step (for the snapshot): a straight line over the
// ground from the muzzle to where it lands, lifted by a parabola whose top is `apexShare` of the distance.
COLONY_API void artilleryShellAt(const ArtilleryShell& shell, const Map& map, float apexShare, float& x, float& y, float& z,
                                 float& vx, float& vy, float& vz);

// The numbers in the ArtilleryRules def (with these values when the def is missing). All ours: RimWorld has a
// manning interaction cell, and its shells' arcs are drawn, not simulated.
struct COLONY_API ArtilleryNumbers {
    int scanEvery = 10;         // steps between a manned mortar's looks for an enemy (staggered by building id)
    float reach = 1.5f;         // metres from the building within which the player's colonist mans it
    float apexShare = 0.25f;    // a shell's arc rises this share of the distance it flies (for the drawing)
    int spreadTries = 8;        // tries for a rocketswarm rocket to find a cell no other rocket of the volley took
    // RimWorld's Mortar Miss Radius Multiplier: 1 + base - perShootingLevel x Shooting - (1 - Sight, Sight at most 1)
    // - manipulationFactor x (Manipulation - 1), never under 0.
    float missBase = 0.2f;
    float missPerShooting = 0.025f;
    float missManipulation = 0.5f;

    static ArtilleryNumbers read(const DefDatabase& defs);
};

struct ArtilleryModel;

class COLONY_API ArtillerySystem {
public:
    ArtillerySystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, PowerGrid& power,
                    JobSystem& jobs, const DefDatabase& defs, const GameTime& time);
    ~ArtillerySystem();
    ArtillerySystem(const ArtillerySystem&) = delete;
    ArtillerySystem& operator=(const ArtillerySystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Ammunition (the building's fuel). Null: artillery never runs dry.
    void setFuel(FuelSystem* fuel) { fuel_ = fuel; }
    const ArtilleryNumbers& numbers() const { return numbers_; }
    void setNumbers(const ArtilleryNumbers& numbers) { numbers_ = numbers; }

    bool isArtillery(DefId def) const;
    // The gun's range in metres, and its minimum (0, 0 for a thing that is not artillery).
    float range(Entity building) const;
    float minRange(Entity building) const;
    // Metres a shell aimed by this crew may land from its aim: the def's `missRadius`, times the crew's miss radius
    // factor for a mortar. The def's radius for kNoEntity.
    float missRadius(Entity building, Entity crew) const;
    // RimWorld's Mortar Miss Radius Multiplier of a pawn (1 for a pawn without skills or health).
    float missFactor(Entity pawn) const;

    // ---- Commands (ArtilleryCommand). False, with a reason in plain words, when they can't be done. ----------------
    // Who mans it: `pawn` takes the post (the job system walks a colonist there; the player's colonist must be within
    // reach). Refused for a pawn that isn't the colony's, can't fight (incapable of violence, downed) or is busy fighting.
    bool man(Entity building, Entity pawn, std::string& error);
    // The crew leaves its post.
    bool stopManning(Entity building, std::string& error);
    // Aims at a pawn (`target`), else at the point (x, y). Refused out of range, under a mountain's roof, or off the map.
    bool aim(Entity building, Entity target, float x, float y, std::string& error);
    bool clearAim(Entity building, std::string& error);
    bool setHoldFire(Entity building, bool hold, std::string& error);

    // Whether its crew is at its post now.
    bool manned(Entity building) const;

    // One step: crews are checked, manned artillery aims and fires, shells fly and land.
    void step(Rng& rng);

    // Hashes the artillery components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    uint64_t shotsFired() const { return shots_; }
    uint64_t shellsLanded() const { return landed_; }

private:
    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    PowerGrid& power_;
    JobSystem& jobs_;
    FuelSystem* fuel_ = nullptr;
    const GameTime& time_;
    ArtilleryNumbers numbers_;
    std::unique_ptr<ArtilleryModel> model_;
    std::vector<Event>* events_ = nullptr;
    std::vector<Entity> order_;
    uint64_t shots_ = 0;
    uint64_t landed_ = 0;

    bool checkBuilding(Entity building, std::string& error) const;
    bool atPost(Entity building, const Artillery& a) const;
    bool crewGone(Entity building, const Artillery& a) const;
    bool coveredFromAbove(Entity building) const;
    bool targetAllowed(Entity building, float x, float y, std::string* why) const;
    bool aimPoint(Entity building, Artillery& a, float& x, float& y) const;
    Entity pickTarget(Entity building) const;
    bool hasAmmo(Entity building, float amount) const;
    void stepBuilding(Entity building, Artillery& a, Rng& rng);
    void fire(Entity building, Artillery& a, float x, float y, Rng& rng);
    void stepShells();
};

}  // namespace colony
