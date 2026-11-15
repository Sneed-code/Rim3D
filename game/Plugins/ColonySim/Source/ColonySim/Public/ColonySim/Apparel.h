#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <entt/entity/registry.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class CombatSystem;
class HealthSystem;
class MoodSystem;
class Rng;
class StateHasher;
class Things;
struct Event;

// Clothes and armor (task C15). See docs/design/apparel.md.
//
// A piece of apparel is an item with `Thing`, `HitPoints` and, when it is made of a stuff, `MadeOf`
// and `Quality`. A pawn wears it through `Gear::apparel` (Combat.h); a worn piece is off the map.
// What a piece is worth (armor, insulation) follows from its def, its stuff and its quality
// (ApparelStats); what a pawn wears and why (outfits, wearing, wear and tear, taint, thoughts) is
// ApparelSystem. Unreal reads worn pieces and outfits from the snapshot and sends the commands.

// sneed's damage model (2026-11-14, docs/design/armour.md). Every hit is a mix of three classes; an
// elemental hit also names its subtype (fire, cold...). Every worn piece and a creature's own hide has
// a % against each class and an extra % against each subtype; damage is divided by (1 + %) for every
// piece, so it never reaches 0. A negative % multiplies the damage by (1 + |%|) instead.
enum class DamageClass : uint8_t { Blunt, Sharp, Elemental };
enum class DamageSub : uint8_t { None, Fire, Cold, Toxic, Electric, Psionic };
constexpr int kDamageClasses = 3;
constexpr int kMitigationFields = 8;  // blunt, sharp, elemental, fire, cold, toxic, electric, psionic

// Percentages, by field: the three classes, then the five subtypes (field 3 + subtype - 1).
struct COLONY_API Mitigation {
    float pct[kMitigationFields] = {0, 0, 0, 0, 0, 0, 0, 0};

    // Reads {"blunt": 15, "sharp": 30, "elemental": 15, "fire": 10, ...} at `path`; missing fields are 0.
    static Mitigation read(const Def& def, const std::string& path);
    static const char* fieldName(int field);
    bool any() const;
};

// What a hit is made of: shares of the three classes (summing to 1 for a normal hit) and the subtype of its
// elemental share. Read from a DamageTypes def's `mix` ({"sharp": 0.75, "blunt": 0.25},
// {"elemental": 1, "sub": "Fire"}).
struct COLONY_API DamageMix {
    float share[kDamageClasses] = {0, 0, 0};
    DamageSub sub = DamageSub::None;

    static DamageMix read(const Def& def, const std::string& path);
    float total() const { return share[0] + share[1] + share[2]; }
};

// The divisor one % gives: 1 + % / 100, or 1 / (1 + |%| / 100) for a negative %.
COLONY_API float mitigationDivisor(float pct);
// The divisor a set of %s gives against one class (an elemental hit adds its subtype's), after
// armour penetration (0 to 1) takes that share off each positive %.
COLONY_API float mitigationFactor(const Mitigation& m, DamageClass c, DamageSub sub, float penetration);

// Which layer a worn piece is (docs/design/armour.md): clothing (shirt, pants, hat) or armour (vest,
// suit, helmet); other gear (belts, packs) gives no mitigation. A piece fills slots; two pieces of the
// same kind can't share a slot. Pants and a shirt count under armour; a hat and a helmet: the better.
enum class GearKind : uint8_t { None, Clothing, Armour, Other };
constexpr uint8_t kGearHead = 1;
constexpr uint8_t kGearChest = 2;
constexpr uint8_t kGearLegs = 4;
constexpr uint8_t kGearArm = 8;  // a shield (docs/design/shields.md): armour kind, its own slot

// sneed's shields (2026-11-15, docs/design/shields.md). The numbers from the ShieldRules def.
struct COLONY_API ShieldRules {
    float blockCut = 0.4f;          // a hit that lands on a raised shield loses this share...
    float floorBaseDamage = 6.0f;   // ...then the material's flat block; a hit of at least this much...
    float floorDamage = 1.0f;       // ...keeps at least this much
    float arcDegrees = 60.0f;       // the raised shield catches hits from within this angle either side of its facing
    int aiHoldSteps = 15;           // a fighter keeps its shield up this long after the last attack it saw coming

    static ShieldRules read(const DefDatabase& defs);
};

// On a pawn holding its shield up (docs/design/shields.md): it can walk and reload but not shoot or swing. A hit
// from the front (within `arcDegrees` of where it faces) lands on the shield with the shield's catch chance. The
// fighter AI raises it at an attack it sees coming (`ai`) and faces `threat` while it is up; it lowers it once
// `holdSteps` run out with no attack coming, or to attack. The player's block faces where the pawn faces, or the
// way the command gave (`hasDir`).
struct Blocking {
    Entity threat = kNoEntity;
    bool ai = false;
    int holdSteps = 0;
    bool hasDir = false;
    float dirX = 1.0f;
    float dirY = 0.0f;
};

// An item made of a stuff (cloth, leather...): clothes from tailoring. Its armor and insulation come
// from the stuff (Textiles.json, `stuffProps`); without one a `stuffRequired` garment is worth nothing.
struct MadeOf {
    DefId stuff;
};

// An item's quality: a place in `QualityRules.levels` (0 awful to 6 legendary). Items without the
// component are normal quality (`QualityRules.defaultLevel`).
struct Quality {
    uint8_t level = 2;
};

// How worn a piece of apparel is, apart from its hit points (docs/design/apparel-durability.md): a share of full,
// 0 to 1, capped by the item's hit-point share (`ApparelStats::condition`). Wearing it and the hits its armor
// takes wear this down, not the hit points; at 0 the piece is tattered and needs repair. No component: full.
struct Durability {
    float share = 1.0f;
};

// Worn by a human as they died: taints the clothes for good (RimWorld wiki, "Tainted").
struct Tainted {};

// On a downed or dead pawn the player wants stripped of its clothes and weapon; a colonist with Hauling
// work does it.
struct StripMark {};

// On a pawn that thinks: the outfit it follows, and the game second before which it doesn't look for
// better clothes again (not set while it keeps finding some).
struct PawnOutfit {
    uint32_t outfit = 1;
    int64_t nextCheck = 0;
};

// An outfit policy: which apparel a pawn may wear. A def is allowed unless it is listed; the durability
// (ApparelStats::condition; the fields keep their old names) and quality must be in range. Ids are 1, 2, 3... in the order made.
struct OutfitPolicy {
    uint32_t id = 0;
    std::string name;
    std::vector<std::string> disallowed;  // Things defNames, sorted
    float hitPointsMin = 0.0f;
    float hitPointsMax = 1.0f;
    uint8_t qualityMin = 0;
    uint8_t qualityMax = 6;
    // Fight gear (docs/design/armour.md): armour / clothes (not hats) the outfit allows are kept in the bag and put on
    // only when the pawn fights, then go back in the bag. Clothes in the bag are for nudists.
    bool armourInBag = false;
    bool clothesInBag = false;
};

// On a fighting pawn putting on gear from its bag (docs/design/armour.md): `item` goes on when `stepsLeft` runs out;
// meanwhile it can't shoot or swing. A new injury (more than `injuries`) makes it give up for the rest of the fight
// (`gaveUp`). Removed when it stops fighting.
struct Dressing {
    Entity item = kNoEntity;
    int stepsLeft = 0;
    int injuries = 0;
    bool gaveUp = false;
};

inline bool isDressing(const entt::registry& reg, Entity pawn) {
    const Dressing* dressing = reg.valid(pawn) ? reg.try_get<Dressing>(pawn) : nullptr;
    return dressing != nullptr && dressing->stepsLeft > 0;
}

// On a pawn whose clothes and armour are not drawn (a look-only switch, docs/design/armour.md); they still count.
struct HideGear {};

// The numbers from the ApparelRules def (RimWorld's where the wiki gives them, the rest ours).
struct COLONY_API ApparelRules {
    float absorbDamageDivisor = 4.0f;       // apparel wears 1 hit point's worth of durability per this much damage it absorbs
    float wearHpLossChancePerDay = 0.4f;    // each worn piece: hit points' worth of durability worn per day (RimWorld's chance)
    float wearFactor = 1.0f;                // times that (ours, to tune)
    float deathHpLossMin = 0.15f;           // worn apparel loses this share of its durability when the wearer dies...
    float deathHpLossMax = 0.40f;           // ...up to this share
    float tatteredBenefitFactor = 0.8f;     // a piece at 0 durability gives this share of its insulation (ours)
    float tatteredMitigationFactor = 0.67f; // ...and this share of its mitigation % (sneed)
    float clothingInsulationCold = 10.0f;   // every regular garment, degrees (sneed)
    float clothingInsulationHeat = 2.0f;
    int taintedStageCap = 4;
    std::string defaultOutfit = "Anything";
    int wearSteps = 60;                     // putting on or taking off a weapon or a piece of other gear (belts)
    int clothingWearSteps = 1;              // a piece of clothing goes on at once (sneed)
    int armourWearSteps = 120;              // a piece of armour: 4 seconds at 1x (sneed)
    float weightKgPerSlowdown = 100.0f;     // speed = 1 / (1 + kg / this) (sneed)
    float workWeightShare = 0.25f;          // work counts this share of armour kg (sneed)
    int stripStepsPerPiece = 60;
    int checkEverySteps = 1200;             // between looks for better clothes, once a pawn found none
    float scoreBase = 0.1f;
    float scoreSharp = 1.0f;
    float scoreBlunt = 0.5f;
    float scoreHeat = 0.25f;
    float scoreInsulationPerDegree = 0.01f;
    float scoreHpFloor = 0.4f;              // a piece at no hit points scores this share of its full score
    float taintedScorePenalty = 0.2f;
    float minGain = 0.05f;                  // a change of clothes must score this much better
    float maxDistance = 50.0f;              // metres a pawn walks for clothes (ours)

    static ApparelRules read(const DefDatabase& defs);
};

// What a piece of apparel is worth, from its def, stuff and quality, worked out once from the defs.
class COLONY_API ApparelStats {
public:
    explicit ApparelStats(const DefDatabase& defs);

    bool isApparel(DefId def) const { return def.valid() && def.value < apparel_.size() && apparel_[def.value].valid; }
    const std::vector<std::string>& groups(DefId def) const;
    const std::vector<std::string>& layers(DefId def) const;
    bool covers(DefId def, const std::string& group) const;

    // Which layer and slots a def fills (GearKind::None for a def that is not apparel or says nothing).
    GearKind gearKind(DefId def) const { return isApparel(def) ? apparel_[def.value].kind : GearKind::None; }
    uint8_t gearSlots(DefId def) const { return isApparel(def) ? apparel_[def.value].slots : 0; }
    // The piece's mitigation %s (docs/design/armour.md): clothing takes its material's `clothing.mitigation`
    // times the garment's share (a hat half); armour its own `gear.mitigation`, times its material's
    // `armourStuff.factor` plus `armourStuff.offset` when it is made of one. Positive %s are then scaled
    // by the quality's factor and, worn down, by `tatteredMitigationFactor`.
    Mitigation mitigation(const entt::registry& reg, Entity item) const;
    // Kilograms the piece weighs on the wearer: clothing its material's `clothing.kg` times the share,
    // other apparel its def's mass.
    float weightKg(const entt::registry& reg, Entity item) const;
    // What gear weighs on a pawn (docs/design/armour.md): `armourKg` its worn armour; `wornKg` all it wears
    // (a hat under a helmet weighs nothing) plus the weapon in its hands.
    float armourKg(const entt::registry& reg, Entity pawn) const;
    float wornKg(const entt::registry& reg, Entity pawn) const;
    // Movement: 1 / (1 + wornKg / 100). Attack (aim, cooldown, reload, swings): their times are multiplied by
    // 1 + armourKg / 100. Work: 1 / (1 + 0.25 x armourKg / 100).
    float moveFactor(const entt::registry& reg, Entity pawn) const;
    float attackTimeFactor(const entt::registry& reg, Entity pawn) const;
    float workFactor(const entt::registry& reg, Entity pawn) const;
    // Shields (docs/design/shields.md). A shield is armour in the Arm slot with a catch chance; its material gives
    // the flat block (`shieldStuff.block`) and a weight factor (`shieldStuff.mass`). A two-handed weapon in the hands
    // (`twoHanded` on the weapon's def) slings it on the back: it still weighs but neither protects nor blocks.
    bool isShield(DefId def) const { return isApparel(def) && apparel_[def.value].shield; }
    float catchChance(DefId def) const { return isShield(def) ? apparel_[def.value].catchChance : 0.0f; }
    float shieldBlock(const entt::registry& reg, Entity item) const;
    bool twoHanded(DefId weaponDef) const {
        return weaponDef.valid() && weaponDef.value < twoHanded_.size() && twoHanded_[weaponDef.value];
    }
    // The shield the pawn wears (kNoEntity for none) / the same, only when it is on its arm (not slung).
    Entity shieldWorn(const entt::registry& reg, Entity pawn) const;
    Entity activeShield(const entt::registry& reg, Entity pawn) const;
    const ShieldRules& shieldRules() const { return shieldRules_; }
    // Degrees Celsius the comfortable range widens: cold lowers its minimum, heat raises its maximum.
    float insulationCold(const entt::registry& reg, Entity item) const;
    float insulationHeat(const entt::registry& reg, Entity item) const;
    // The hit points a new item of `def` made of `stuff` has (the def's, times the stuff's factor).
    int maxHitPoints(DefId def, DefId stuff) const;
    // A share of its maximum, 1 when it has no hit points.
    static float hitPointShare(const entt::registry& reg, Entity item);
    // The item's durability, 0 to 1: its own wear, capped by its hit-point share (an item at 70% of its hit points
    // is at most 70% durable). 1 for an item with neither.
    static float condition(const entt::registry& reg, Entity item);
    // At 0 durability: tattered, worth `tatteredMitigationFactor` of its mitigation and `tatteredBenefitFactor` of its insulation.
    static bool tattered(const entt::registry& reg, Entity item);
    // Basic clothes (`apparel.fragile`): worn at 0 durability, further wear takes hit points and can destroy them.
    bool fragile(DefId def) const { return isApparel(def) && apparel_[def.value].fragile; }
    float tatteredFactor() const { return tatteredFactor_; }

    int qualityLevels() const { return static_cast<int>(qualityArmor_.size()); }
    int defaultQuality() const { return defaultQuality_; }
    int qualityOf(const entt::registry& reg, Entity item) const;
    const std::string& qualityName(int level) const;

private:
    struct Apparel {
        bool valid = false;
        bool stuffRequired = false;
        bool fragile = false;
        GearKind kind = GearKind::None;
        uint8_t slots = 0;
        float share = 1.0f;     // clothing: share of the material's mitigation and weight (hats 0.5)
        Mitigation fixed;       // armour: its own %s (steel's for armour made of a material)
        bool shield = false;    // a shield (gear.shield)
        float catchChance = 0.0f;
        float mass = 0.0f;
        float coldMultiplier = 0.0f;
        float heatMultiplier = 0.0f;
        float fixedCold = 0.0f;
        float fixedHeat = 0.0f;
        int maxHitPoints = 0;
        std::vector<std::string> groups;
        std::vector<std::string> layers;
    };
    struct Stuff {
        bool valid = false;
        bool clothing = false;  // has a `clothing` block
        Mitigation clothingMit;
        float clothingKg = 0.0f;
        float armourFactor[kDamageClasses] = {1.0f, 1.0f, 1.0f};
        Mitigation armourOffset;
        float cold = 0.0f;
        float heat = 0.0f;
        float hitPoints = 1.0f;
        float shieldBlock = 0.0f;  // shieldStuff.block: flat damage a raised shield of it stops
        float shieldMass = 1.0f;   // shieldStuff.mass: times the shield def's mass
    };

    std::vector<Apparel> apparel_;  // by Things DefId value
    std::vector<Stuff> stuff_;      // by Things DefId value
    std::vector<float> qualityArmor_;
    std::vector<float> qualityInsulation_;
    std::vector<std::string> qualityNames_;
    int defaultQuality_ = 2;
    float tatteredFactor_ = 0.8f;
    float tatteredMitigation_ = 0.67f;
    float clothingCold_ = 10.0f;
    float clothingHeat_ = 2.0f;
    float kgPerSlowdown_ = 100.0f;
    float workWeightShare_ = 0.25f;
    std::vector<float> weaponKg_;  // by Things DefId value: stats.mass of anything else (the weapon in hand)
    std::vector<bool> twoHanded_;  // by Things DefId value: a weapon held in both hands
    ShieldRules shieldRules_;
    std::vector<std::string> none_;
    std::string noName_;
};

// Outfits, wearing and taking off, wear and tear, death, taint and the thoughts about clothes. Runs
// once per step after combat (so it sees every death of the step).
class COLONY_API ApparelSystem {
public:
    ApparelSystem(Things& things, CombatSystem& combat, HealthSystem& health, const DefDatabase& defs);
    ~ApparelSystem();
    ApparelSystem(const ApparelSystem&) = delete;
    ApparelSystem& operator=(const ApparelSystem&) = delete;

    const ApparelRules& rules() const { return rules_; }
    const ApparelStats& stats() const;

    // ---- Items --------------------------------------------------------------------------------
    // Makes a piece of apparel made of `stuff` (invalid: none) of `quality` (-1: normal) in a cell,
    // with its hit points. kNoEntity, with the reason in `why`, if it can't be put there.
    Entity make(DefId def, DefId stuff, int quality, CellPos cell, std::string* why = nullptr);
    // Makes an existing item of an apparel def made of `stuff` with this quality, its hit points full.
    bool setMadeOf(Entity item, DefId stuff, int quality, std::string* why = nullptr);

    // ---- Outfits ------------------------------------------------------------------------------
    const std::vector<OutfitPolicy>& outfits() const { return outfits_; }
    const OutfitPolicy* outfit(uint32_t id) const;
    uint32_t nextOutfitId() const { return nextOutfitId_; }
    // The outfit a pawn follows: its own, else the default.
    uint32_t outfitOf(Entity pawn) const;
    bool setOutfit(Entity pawn, uint32_t outfit, std::string& error);
    bool editOutfit(uint32_t outfit, const std::vector<std::string>& disallowed, float hitPointsMin, float hitPointsMax,
                    int qualityMin, int qualityMax, std::string& error);
    // A new outfit that allows everything. Returns its id, or 0 with the reason.
    uint32_t addOutfit(const std::string& name, std::string& error);
    // Pawns that followed it go back to the default outfit; the first outfit can't be removed.
    bool removeOutfit(uint32_t outfit, std::string& error);
    // Replaces the outfits (loading a save).
    void restoreOutfits(std::vector<OutfitPolicy> outfits, uint32_t nextId);
    bool allowed(const OutfitPolicy& outfit, Entity item) const;
    // Sets an outfit's fight gear: armour and/or clothes kept in the bag and put on for fights.
    bool setFightGear(uint32_t outfit, bool armourInBag, bool clothesInBag, std::string& error);
    // Whether the pawn's outfit keeps pieces of this def in the bag outside fights.
    bool bagged(Entity pawn, DefId def) const;
    // Whether the pawn is in a fight (drafted, called up, or an enemy), so it wears its fight gear.
    bool fighting(Entity pawn) const;

    // ---- Wearing ------------------------------------------------------------------------------
    // How good a piece is to wear (ours: armor, insulation, hit points, taint).
    float score(Entity item) const;
    // A worn piece the pawn's outfit doesn't allow, or kNoEntity.
    Entity wornToRemove(Entity pawn) const;
    // The apparel on the map the pawn would gain most by wearing, among those `usable` accepts
    // (not held by another, reachable), within maxDistance; kNoEntity if none gains `minGain`.
    Entity bestToWear(Entity pawn, const std::function<bool(Entity)>& usable) const;
    // Puts it on (the pieces it replaces drop where the pawn stands) / takes it off and drops it there.
    bool putOn(Entity pawn, Entity item, std::string& error);
    bool takeOff(Entity pawn, Entity item, std::string& error);
    // Fight gear: a worn piece the outfit keeps in the bag (while not fighting), or kNoEntity; a piece in the bag to
    // put on now (fighting: clothes first, then armour legs, chest, helmet), or kNoEntity.
    Entity wornToBag(Entity pawn) const;
    Entity bagToWear(Entity pawn) const;
    // Worn into the bag / from the ground into the bag (a bagged piece it replaces drops there) / from the bag on.
    bool stow(Entity pawn, Entity item, std::string& error);
    bool pickUp(Entity pawn, Entity item, std::string& error);
    bool wearFromBag(Entity pawn, Entity item, std::string& error);
    // The look-only switch: hide the pawn's clothes and armour (they still count).
    bool setHideGear(Entity pawn, bool hide, std::string& error);
    bool gearHidden(Entity pawn) const;
    // Apparel and weapon off a downed or dead pawn, on the ground where it lies.
    bool strip(Entity pawn);
    // Whether the pawn wears anything over a body part group, and the pieces it wears.
    bool covered(Entity pawn, const std::string& group) const;
    const std::vector<Entity>& worn(Entity pawn) const;
    // Total insulation of everything worn, degrees (E2 adds it to the comfortable range).
    void insulation(Entity pawn, float& cold, float& heat) const;
    // Steps it takes to put on or take off this item (clothing at once, armour 4 seconds, other gear wearSteps).
    int wearStepsFor(Entity item) const;
    // Worn pieces at 0 durability (the tattered thought's stage, up to 3).
    int tatteredWorn(Entity pawn) const;
    // The durability (ApparelStats::condition) of the worst worn piece; 1 when it wears nothing.
    float worstCondition(Entity pawn) const;
    // Wears a piece down by a share of its full durability (never below 0; hit points are not touched).
    void wearDown(Entity item, float share);

    // ---- Strip marks --------------------------------------------------------------------------
    bool markStrip(Entity target, bool on, std::string& error);
    bool stripMarked(Entity pawn) const;
    // Whether stripping would give anything: the pawn wears apparel or holds a weapon.
    bool hasGear(Entity pawn) const;

    // ---- Thoughts (the situations MoodSystem asks about; see Mood.h) ----------------------------
    bool naked(Entity pawn) const;
    bool nudistClothed(Entity pawn) const;
    int taintedWorn(Entity pawn) const;
    // Worn pieces made of a stuff with `humanlike` true (human leather).
    int humanLeatherWorn(Entity pawn) const;
    // Registers the situations "noApparel", "apparelOnNudist", "apparelTattered", "apparelTainted" and "apparelHumanLeather".
    void registerSituations(MoodSystem& mood) const;

    // One step, after combat: the dead wear their clothes down and taint them, worn clothes wear
    // (once an hour), marks on pawns that need no stripping go, fighters put on their fight gear from the bag.
    // `now` is the game second of the step.
    void step(int64_t now, Rng& rng, const std::vector<Event>& events);

    // The state hash and the save read these.
    void hashThing(Entity e, StateHasher& h) const;
    void hashState(StateHasher& h) const;

private:
    bool hasTrait(Entity pawn, const char* trait) const;
    bool isFemale(Entity pawn) const;
    bool conflicts(Entity item, Entity worn) const;
    int wearRank(DefId def) const;
    void stepDressing();

    Things& things_;
    CombatSystem& combat_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    ApparelRules rules_;
    std::vector<OutfitPolicy> outfits_;
    uint32_t nextOutfitId_ = 1;
    std::vector<Entity> none_;
    std::vector<Entity> scratch_;
};

}  // namespace colony
