#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class ApparelStats;
class HealthSystem;
class Map;
class StateHasher;
class Things;

// Guns, bullets and hitboxes. See docs/design/combat.md.
//
// Every shot is a real projectile: it leaves the muzzle with spread, flies with speed and drop,
// and stops at the first thing it touches: the ground, rock, a building, or a pawn's hitbox. A hit
// on a pawn goes through its armor to the body part that was hit. There is no hit-chance roll.
//
// Unreal does not include this header: it reads projectiles, stances and weapons from the
// snapshot, ShotFired and ProjectileHit events from the event list, and sends ShootCommand,
// EquipCommand and SetStanceCommand.

// How a pawn holds its body and which way it faces (a unit vector on the ground; +x is east).
// Every pawn has one. Downed pawns are treated as prone whatever `stance` says.
struct Posture {
    Stance stance = Stance::Standing;
    float facingX = 1.0f;
    float facingY = 0.0f;
};

// What a pawn carries and wears: things that are off the map (no Position), each with an
// EquippedBy naming the pawn.
struct Gear {
    Entity weapon = kNoEntity;
    std::vector<Entity> apparel;  // in the order put on
};

struct EquippedBy {
    Entity pawn = kNoEntity;
};

// A pawn's trigger: a shot it was ordered to fire, then the gun's cooldown. Added the first time
// a pawn shoots and kept.
enum class ShotPhase : uint8_t {
    Ready,    // nothing to do
    Aiming,   // the gun's warm-up before the first shot of a burst
    Firing,   // between the shots of a burst
    Cooling,  // the gun's cooldown after a burst
    Reloading,  // the cooldown, then a new magazine (F5); the gun is full when it ends
};

struct Shooter {
    ShotPhase phase = ShotPhase::Ready;
    int stepsLeft = 0;  // of the phase
    int shotsLeft = 0;  // in the burst, counting the next
    Entity target = kNoEntity;  // aim at this pawn's aim point at each shot, if it still stands
    float aimX = 0.0f;          // otherwise (or if it is gone) at this point, metres
    float aimY = 0.0f;
    float aimZ = 0.0f;
    bool player = false;  // the player's own trigger: no aiming time, the gun's own spread only
    float recoil = 0.0f;  // extra spread from the shots just fired, as a slope; wears off
};

// On a gun item once it has been fired: the rounds left in its magazine (F5). A gun without one
// is full. It stays with the gun when the gun is dropped and picked up again.
struct Magazine {
    int rounds = 0;
};

// A bullet in flight. Not a Thing: it has no def of kind Things and is not in the snapshot's
// things, but in Snapshot::projectiles. Destroyed when it hits something or flies out.
struct Projectile {
    DefId def;              // Projectiles def
    DefId weapon;           // the gun that fired it (Things def)
    Entity launcher = kNoEntity;  // never hit by its own shots
    float x = 0.0f;         // metres; z is height above sea level, like the ground's
    float y = 0.0f;
    float z = 0.0f;
    float vx = 0.0f;        // metres per step
    float vy = 0.0f;
    float vz = 0.0f;
    int level = 0;
    int age = 0;            // steps in flight
    CellPos startCell;      // the shooter's cell: buildings there never stop the shot (a doorway)
    uint64_t seed = 0;      // where the hit's choices (part, armor) come from
};

// A hitbox in the world, for the debug view (CombatSystem::hitboxesOf).
struct HitboxView {
    int part = -1;     // body part index
    bool box = false;  // a box (axis aligned with the pawn's facing) or a capsule
    // Capsule: the two end points and the radius. Box: the centre in a, the half sizes along the
    // pawn's forward, left and up in b, radius 0. World metres, z above sea level.
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    float bx = 0.0f, by = 0.0f, bz = 0.0f;
    float radius = 0.0f;
    float forwardX = 1.0f, forwardY = 0.0f;  // the pawn's facing
};

// What a shot found along a stretch of its flight (CombatSystem::trace).
struct TraceHit {
    ProjectileHitKind kind = ProjectileHitKind::None;
    float t = 1.0f;  // 0 to 1 along the stretch
    float x = 0.0f, y = 0.0f, z = 0.0f;
    Entity thing = kNoEntity;  // the pawn or building hit
    int part = -1;             // the body part whose hitbox was hit (outside part)
};

// What a hit on a pawn came to (CombatSystem::hitPawn): the part it lands on, and the damage left
// after armor (none if the armor deflected it).
struct PawnHitResult {
    bool deflected = false;
    int part = -1;     // -1: the pawn has no hitboxes, so the caller lets health pick a part
    DefId damageType;  // Blunt if armor turned a sharp hit blunt
    float amount = 0.0f;
};

struct CombatModel;

class COLONY_API CombatSystem {
public:
    CombatSystem(Things& things, Map& map, HealthSystem& health);
    ~CombatSystem();
    CombatSystem(const CombatSystem&) = delete;
    CombatSystem& operator=(const CombatSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Called before combat destroys a thing (a building shot to pieces), so jobs can let go of it.
    void setBeforeDestroy(std::function<void(Entity)> hook) { beforeDestroy_ = std::move(hook); }

    // Puts a weapon (a gun, or a melee weapon since F8) in the pawn's hands (the one held before is dropped where the pawn stands) or
    // apparel on its body (one piece per layer and body part group: what it replaces is dropped).
    // The item may be on the map (it is picked up) or not. False, changing nothing, if the pawn
    // has no body, is dead, or the item is neither a gun nor apparel, or is someone else's.
    bool equip(Entity pawn, Entity item, std::string* why = nullptr);
    // Takes one worn piece (or the gun) off and puts it on the ground where the pawn stands. False if
    // the pawn doesn't have it.
    bool unequip(Entity pawn, Entity item);
    // Takes `hitPoints` off a worn piece of apparel; at none it is destroyed (an ApparelDestroyed
    // event). Used when armor takes a hit and by wear and tear (C15).
    void damageApparel(Entity pawn, Entity item, int hitPoints);
    // What the defs and the item's stuff and quality make of a piece of apparel (Apparel.h).
    const ApparelStats& apparelStats() const { return *apparelStats_; }
    // Takes all of a pawn's gear off and puts it on the ground where it stands (or destroys it if
    // the pawn is off the map). For pawns about to be destroyed.
    void dropGear(Entity pawn);

    // Orders a pawn to shoot: at `target`'s aim point if it is a pawn on the map, else at the
    // point (x, y, z). An aimed shot waits the gun's warm-up and its spread follows the shooter's
    // Shooting skill (RimWorld's hit chance); a player shot fires at once with the gun's own spread.
    // During warm-up a new order changes the aim. False if the pawn can't shoot now (no gun,
    // downed, mid-burst or cooling down).
    bool shoot(Entity pawn, Entity target, float x, float y, float z, bool player, std::string* why = nullptr);

    bool setStance(Entity pawn, Stance stance);
    // Steps until the pawn can start a new shot (0: ready now), a reload included.
    int shotReadyIn(Entity pawn) const;

    // Magazines (F5). A gun's `magazineSize` rounds, then a reload of `reloadTime` steps (faster with
    // Shooting skill) that starts by itself after the shot that empties it. reload() starts one early:
    // false, changing nothing, if the pawn has no gun with a magazine, it is full, or the pawn is
    // downed or mid-shot. Size 0: the gun never reloads (and roundsLeft is 0).
    bool reload(Entity pawn, std::string* why = nullptr);
    int roundsLeft(Entity pawn) const;
    int magazineSize(Entity pawn) const;
    bool reloading(Entity pawn) const;
    // Ends an aimed shot still in its warm-up (a fighter that has to dodge). Nothing mid-burst.
    void cancelAim(Entity pawn);

    // Aiming like a player (F5), all by Shooting skill (CombatRules). Steps from seeing a new target
    // to starting to aim at it (the fighter AI waits them) and to noticing a projectile coming.
    int reactionSteps(Entity pawn) const;
    // Where an aimed shot at a moving pawn should go so the bullet meets it: its aim point moved on
    // by its velocity times the bullet's flight time times `lead` (1 a perfect lead, 0 none).
    // False if the target isn't a pawn on the map or the shooter has no gun.
    bool leadPoint(Entity shooter, Entity target, float lead, float& x, float& y, float& z) const;
    float gravity() const;

    // One step: pawns face where they walk or aim, triggers count down and fire, then every
    // projectile flies one step. Runs after movement, so it sees where pawns are now.
    void step(Rng& rng);

    // The spread of a shot as a slope (sideways metres per metre of flight): the radius of the
    // circle its direction is picked in. An aimed shot at `distance` is as likely to land on a
    // standing man in the open as RimWorld's hit chance (shooter accuracy ^ distance x the gun's
    // accuracy at that distance); a player's shot uses the gun's accuracy at the medium distance.
    // Recoil is not included.
    float spread(DefId weapon, int shootingSkill, float distance, bool player) const;
    int shootingSkill(Entity pawn) const;

    // Fires one projectile from the pawn's muzzle toward (x, y, z) with this spread. Returns it,
    // or kNoEntity if the pawn has no gun. The ShotFired event comes with it.
    Entity fire(Entity pawn, float x, float y, float z, float spreadSlope, Rng& rng);

    // What the straight stretch from a to b meets first: ground, rock, a building (not in
    // `skipCell`'s cell) or a pawn's hitbox (not `skipPawn`). Kind None if nothing.
    TraceHit trace(float ax, float ay, float az, float bx, float by, float bz, int level, Entity skipPawn,
                   CellPos skipCell) const;
    // The point other pawns aim at on this pawn (its chest), world metres. False if not a pawn
    // on the map.
    bool aimPoint(Entity pawn, float& x, float& y, float& z) const;
    // The pawn's hitboxes as they stand now; false if it has none (no Hitboxes def, dead).
    bool hitboxesOf(Entity pawn, std::vector<HitboxView>& out) const;

    // Hashes the combat components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

    // A hit on the hitbox of `hitboxPart`: picks the part under it (a hand or a finger...), goes in
    // to an organ or bone if the damage type reaches inside, then through the pawn's armor, outside
    // layer first (RimWorld's rule). Every random choice comes from `seed`. Doesn't apply the damage:
    // the caller does (HealthSystem::applyHit), so bullets and melee strikes land the same way (F8).
    PawnHitResult hitPawn(Entity pawn, int hitboxPart, DefId damageType, float amount, float penetration,
                          uint64_t seed) const;
    // The radius around a standing man's chest, seen from the front, in which a point picked evenly
    // lands on him with this chance (the table aimed shots use). Melee strikes use it too (F8).
    float spreadRadiusFor(float chance) const;
    // Things defs that go in a pawn's hands: a melee weapon (an item with `tools`, F8) or a gun.
    bool isMeleeWeapon(DefId def) const;
    bool isGun(DefId def) const;

private:
    Things& things_;
    Map& map_;
    HealthSystem& health_;
    std::unique_ptr<CombatModel> model_;
    std::unique_ptr<ApparelStats> apparelStats_;
    std::vector<Event>* events_ = nullptr;
    std::function<void(Entity)> beforeDestroy_;
    std::vector<Entity> scratch_;

    void emitHit(Entity projectile, const Projectile& p, const TraceHit& hit, float amount);
    void impact(Entity projectile, Projectile& p, const TraceHit& hit);
    void updateShooter(Entity pawn, Shooter& s, Rng& rng);
    void startReload(Entity pawn, Shooter& s, Entity weapon, int extraSteps);
    void drop(Entity pawn, Entity item);
};

}  // namespace colony
