#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Random.h"
#include "ColonySim/WeaponSlot.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class ApparelStats;
class HealthSystem;
class InspirationSystem;
class TalentSystem;
class Map;
class StateHasher;
class Things;
class Weather;

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
    Entity weapon = kNoEntity;    // in the hands
    std::vector<Entity> apparel;  // in the order put on
    // Weapons carried on the body but not in the hands (P1): at most one per slot, counting the one in
    // the hands, so at most two here. They add to the load the pawn carries.
    std::vector<Entity> stowed;
};

// On a pawn switching weapons (P1): the weapon in `to` (a stowed one) comes to the hands when the steps run out,
// and the one in the hands goes to the belt. Until then it can't shoot, reload or swing. Added by the first
// switch and kept.
struct WeaponSwitch {
    Entity to = kNoEntity;
    int stepsLeft = 0;  // 0: not switching
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

// The player's gun kick (P2, docs/design/shooter-feel.md): the shots just fired lift the aim by
// the weapon's list of kicks, shot by shot; it wears off by itself. Added to a pawn the first time
// its player trigger fires a gun that has a kick list, and kept. Degrees: `up` raises the aim,
// `side` turns it to the right (left when negative). Only player shots follow it; fighters get
// their spread from skill.
struct RecoilKick {
    float up = 0.0f;
    float side = 0.0f;
    int shots = 0;      // shots in the current spray: the next one reads this place in the kick list
    int sinceShot = 0;  // steps since the last shot; the kick holds, then wears off, and the spray restarts
};

// A dodge roll (P2): start-up, a roll in which bullets pass through the pawn for a short window,
// recovery, then a wait before the next. Added by the first DodgeRollCommand and kept.
enum class RollPhase : uint8_t {
    Idle,
    StartUp,   // crouching into it: not moving yet, can be hit
    Rolling,   // moving; hit-proof for the first part of it
    Recovery,  // getting up: not moving, can be hit
};

struct DodgeRoll {
    RollPhase phase = RollPhase::Idle;
    int stepsLeft = 0;   // of the phase
    int rolled = 0;      // steps rolled so far, while Rolling
    float dirX = 1.0f;   // unit vector on the ground
    float dirY = 0.0f;
    int cooldown = 0;    // Idle: steps before the next roll may start
    bool proof = false;  // bullets passed through the pawn in the step just run
};

// On a pawn thrown to the ground by a blast (F9): prone and unable to act or walk until the steps run
// out, then it gets back to the stance it had.
struct KnockedDown {
    int stepsLeft = 0;
    Stance before = Stance::Standing;
};

// An explosion (CombatSystem::explode): a blast of `damage` that falls off to `edgeFactor` of it at
// `radius`, is stopped by walls and cover, and reaches pawns as 2 to 4 fragments each.
struct Explosion {
    float x = 0.0f;  // metres; z above sea level
    float y = 0.0f;
    float z = 0.0f;
    int level = 0;
    float radius = 0.0f;
    float damage = 0.0f;
    DefId damageType;           // a DamageTypes def, normally Bomb
    float penetration = 0.0f;   // armor penetration of each fragment
    Entity source = kNoEntity;      // the grenade entity, for the event
    Entity instigator = kNoEntity;  // who threw or planted it
    uint64_t seed = 0;          // where its random choices come from
};

struct ExplosionResult {
    int pawnsHurt = 0;
    int pawnsKnocked = 0;
    int buildingsHit = 0;
    int buildingsDestroyed = 0;
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
    // Worn pieces that stopped or softened the hit and the hit points each loses (C15); the caller
    // passes the result to CombatSystem::wearApparel.
    std::vector<std::pair<Entity, int>> wear;
};

// What a gun's def says, for fixed guns that fire without a pawn's hands (turrets, F11): range in
// metres, steps of warm-up, of cooldown after a burst and between the shots of a burst, shots in a burst.
struct GunStats {
    float range = 0.0f;
    int warmup = 0;
    int cooldown = 0;
    int burst = 1;
    int burstGap = 0;
};

struct CombatModel;

class COLONY_API CombatSystem {
public:
    CombatSystem(Things& things, Map& map, HealthSystem& health);
    ~CombatSystem();
    CombatSystem(const CombatSystem&) = delete;
    CombatSystem& operator=(const CombatSystem&) = delete;

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Weather and snow cut a shooter's hit chance under the open sky (docs/design/weather.md).
    void setWeather(const Weather* weather) { weather_ = weather; }
    // The protagonist's talents make the spread of its shots smaller (docs/design/talents.md).
    void setTalents(const TalentSystem* talents) { talents_ = talents; }
    // A shooting frenzy narrows the spread and shortens the cooldown (docs/design/inspirations.md).
    void setInspirations(const InspirationSystem* inspirations) { inspirations_ = inspirations; }
    // Called before combat destroys a thing (a building shot to pieces), so jobs can let go of it.
    void setBeforeDestroy(std::function<void(Entity)> hook) { beforeDestroy_ = std::move(hook); }
    // Called when a bullet or a melee blow hurt a pawn (`victim`, `attacker`): animals react (N5, docs/design/animal-fights.md).
    void setHurtHook(std::function<void(Entity, Entity)> hook) { hurtHook_ = std::move(hook); }
    void noteHurt(Entity victim, Entity attacker) const {
        if (hurtHook_) {
            hurtHook_(victim, attacker);
        }
    }

    // Puts a weapon (a gun, or a melee weapon since F8) in the pawn's hands (the one held before is dropped where the pawn stands) or
    // apparel on its body (one piece per layer and body part group: what it replaces is dropped).
    // The item may be on the map (it is picked up) or not. False, changing nothing, if the pawn
    // has no body, is dead, or the item is neither a gun nor apparel, or is someone else's.
    bool equip(Entity pawn, Entity item, std::string* why = nullptr);
    // Takes one worn piece (or a carried weapon) off and puts it on the ground where the pawn stands.
    // False if the pawn doesn't have it.
    bool unequip(Entity pawn, Entity item);

    // Several weapons (P1, docs/design/weapons.md). The slot a weapon def goes in; false if it is no weapon.
    bool weaponSlot(DefId def, WeaponSlot& out) const;
    // Adds a weapon to what the pawn carries: into the hands if they are empty, else on the body. The
    // weapon already carried in its slot (hands or body) is dropped where the pawn stands. The item may lie on the
    // map. False, changing nothing, if the pawn has no body or is dead, or the item is no weapon or is someone else's.
    bool carryWeapon(Entity pawn, Entity item, std::string* why = nullptr);
    // Starts a switch to the weapon in `slot`: a time that depends on the slot (CombatRules `weaponSwitchSteps`)
    // in which the pawn can't shoot, reload or swing. False, changing nothing, if the pawn carries nothing in
    // that slot, has it in the hands already, is downed, rolling or knocked down, is mid-shot or reloading, or is
    // switching already.
    bool switchWeapon(Entity pawn, WeaponSlot slot, std::string* why = nullptr);
    bool switching(Entity pawn) const;
    int switchStepsLeft(Entity pawn) const;
    // The weapon in a slot, in the hands or on the body (kNoEntity if none), and which slot the hands hold.
    Entity weaponIn(Entity pawn, WeaponSlot slot) const;
    bool handsSlot(Entity pawn, WeaponSlot& out) const;
    // The slot a switch is going to (false if the pawn is not switching).
    bool switchTarget(Entity pawn, WeaponSlot& out) const;
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
    // `accuracyFactor` multiplies the hit chance (the weather's, for a shooter outdoors).
    float spread(DefId weapon, int shootingSkill, float distance, bool player, float accuracyFactor = 1.0f) const;
    int shootingSkill(Entity pawn) const;

    // Fires one projectile from the pawn's muzzle toward (x, y, z) with this spread. Returns it,
    // or kNoEntity if the pawn has no gun. The ShotFired event comes with it. `kickUp` and `kickSide`
    // (slopes) turn the shot up and to the right of the aim, the player's gun kick.
    Entity fire(Entity pawn, float x, float y, float z, float spreadSlope, Rng& rng, float kickUp = 0.0f,
                float kickSide = 0.0f);

    // A shot from a gun that no pawn holds (a turret, F11): from (ox, oy, oz) at `level` toward (x, y, z)
    // with this spread slope, as `fire` does. `launcher` (the turret) is never hit by it, nor is a building
    // in `startCell`. Returns the projectile, or kNoEntity if `weapon` is not a gun. ShotFired comes with it.
    Entity fireFrom(Entity launcher, DefId weapon, float ox, float oy, float oz, int level, CellPos startCell, float x,
                    float y, float z, float spreadSlope, Rng& rng);
    // `leadPoint` for a gun at (fromX, fromY) that no pawn holds.
    bool leadPointFrom(float fromX, float fromY, DefId weapon, Entity target, float lead, float& x, float& y,
                       float& z) const;
    // The numbers of a gun def; false if it is not a gun.
    bool gunStats(DefId weapon, GunStats& out) const;

    // Gun kick (P2): where the player's shots are going above and to the right of the aim now, in
    // degrees. Zero for a pawn that has not fired a gun with a kick list.
    void kickOf(Entity pawn, float& up, float& side) const;

    // Dodge roll (P2, docs/design/shooter-feel.md). Starts a roll of the pawn in the direction
    // (dx, dy): a start-up, then a roll in which bullets pass through it, then a recovery. Not a
    // number or zero means a backstep away from where it faces. Fails, changing nothing, if the pawn
    // isn't a colonist under direct control, is downed, lies prone, or is rolling or waiting out
    // the last roll. A roll ends a shot being aimed or fired (a reload goes on). While it lasts the
    // pawn can't shoot or reload, and the sim moves it: DirectMove is ignored (rollLocked).
    bool dodgeRoll(Entity pawn, float dx, float dy, std::string* why = nullptr);
    RollPhase rollPhase(Entity pawn) const;
    // True from the command to the end of the recovery: the pawn is not its player's to move.
    bool rollLocked(Entity pawn) const { return rollPhase(pawn) != RollPhase::Idle; }
    int rollStepsLeft(Entity pawn) const;
    // Whether bullets pass through the pawn right now.
    bool hitProof(Entity pawn) const;
    bool rollDirection(Entity pawn, float& x, float& y) const;

    // What the straight stretch from a to b meets first: ground, rock, a building (not in
    // `skipCell`'s cell) or a pawn's hitbox (not `skipPawn`). Kind None if nothing.
    // With `pawns` false only the ground, rock and buildings count (blast lines of sight).
    TraceHit trace(float ax, float ay, float az, float bx, float by, float bz, int level, Entity skipPawn,
                   CellPos skipCell, bool pawns = true) const;
    // The point other pawns aim at on this pawn (its chest), world metres. False if not a pawn
    // on the map.
    bool aimPoint(Entity pawn, float& x, float& y, float& z) const;
    // The pawn's hitboxes as they stand now; false if it has none (no Hitboxes def, dead).
    bool hitboxesOf(Entity pawn, std::vector<HitboxView>& out) const;

    // An explosion (F9, docs/design/explosives.md). Every living pawn in the radius is hurt by the share of
    // its body the blast can see (ground, rock and buildings stop it), less with distance and for a
    // pawn that crouches or lies, in 2 to 4 fragments that go through armor like bullets; a hard
    // enough hit knocks the pawn down and pushes it away; buildings in the radius lose hit points
    // (walls the most). Events: PawnInjured... and one Explosion.
    ExplosionResult explode(const Explosion& explosion);
    // Takes `damage` hit points off a building (a raider digging through a wall, F12); at none it goes through the
    // before-destroy hook and is destroyed. True if it was destroyed; false for a thing without hit points or one
    // that still stands.
    bool damageBuilding(Entity building, int damage);
    // Steps left of a pawn's knock-down (0: not knocked down).
    int knockedDownFor(Entity pawn) const;

    // Hashes the combat components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

    // A hit on the hitbox of `hitboxPart`: picks the part under it (a hand or a finger...), goes in
    // to an organ or bone if the damage type reaches inside, then through the pawn's armor, outside
    // layer first (RimWorld's rule). Every random choice comes from `seed`. Doesn't apply the damage:
    // the caller does (HealthSystem::applyHit), so bullets and melee strikes land the same way (F8).
    PawnHitResult hitPawn(Entity pawn, int hitboxPart, DefId damageType, float amount, float penetration,
                          uint64_t seed) const;
    // Damage of a kind with no aim behind it (flames): a random outside part, through the armor that
    // covers it (heat armor for Burn), worn pieces losing hit points. False if the pawn can't be hurt.
    bool hurtPawn(Entity pawn, DefId damageType, float amount, Rng& rng);
    // Takes the hit points off the pieces `result.wear` names; a piece at none is destroyed.
    void wearApparel(Entity pawn, const PawnHitResult& result);
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
    const Weather* weather_ = nullptr;
    const TalentSystem* talents_ = nullptr;
    const InspirationSystem* inspirations_ = nullptr;
    std::function<void(Entity)> beforeDestroy_;
    std::function<void(Entity, Entity)> hurtHook_;
    std::vector<Entity> scratch_;

    void emitHit(Entity projectile, const Projectile& p, const TraceHit& hit, float amount);
    void impact(Entity projectile, Projectile& p, const TraceHit& hit);
    void updateShooter(Entity pawn, Shooter& s, Rng& rng);
    void startReload(Entity pawn, Shooter& s, Entity weapon, int extraSteps);
    void drop(Entity pawn, Entity item);
    // The armor rolls of a hit on `part` (shared by bullets, blows and blast fragments): the damage
    // left (deflected if the armor turned it away), the damage type (sharp may become blunt) and the
    // wear on the pieces. Uses `rng`.
    PawnHitResult armorPass(Entity pawn, int part, DefId damageType, float damage, float penetration, Rng& rng) const;
    int pickOutsidePart(Entity pawn, Rng& rng) const;
    Entity launch(Entity launcher, DefId weaponDef, float ox, float oy, float oz, int level, CellPos startCell, float x,
                  float y, float z, float spreadSlope, Rng& rng, float kickUp, float kickSide, Posture* posture);
    void stepRolls();
    void stepKicks();
    void stepSwitches();
};

}  // namespace colony
