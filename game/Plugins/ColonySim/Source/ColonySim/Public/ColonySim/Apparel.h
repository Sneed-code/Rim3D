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

enum class ArmorType : uint8_t { Sharp, Blunt, Heat };

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

// An outfit policy: which apparel a pawn may wear. A def is allowed unless it is listed; the hit points
// (as a share of the maximum) and quality must be in range. Ids are 1, 2, 3... in the order made.
struct OutfitPolicy {
    uint32_t id = 0;
    std::string name;
    std::vector<std::string> disallowed;  // Things defNames, sorted
    float hitPointsMin = 0.0f;
    float hitPointsMax = 1.0f;
    uint8_t qualityMin = 0;
    uint8_t qualityMax = 6;
};

// The numbers from the ApparelRules def (RimWorld's where the wiki gives them, the rest ours).
struct COLONY_API ApparelRules {
    float absorbDamageDivisor = 4.0f;       // apparel loses 1 hit point per this much damage it absorbs
    float wearHpLossChancePerDay = 0.4f;    // each worn piece: chance per day to lose 1 hit point
    float deathHpLossMin = 0.15f;           // worn apparel loses this share of its hit points when the wearer dies...
    float deathHpLossMax = 0.40f;           // ...up to this share
    float rattyBelow = 0.5f;                // a worn piece at or under this share of its hit points: ratty
    float tatteredBelow = 0.25f;            // under this share: tattered
    int taintedStageCap = 4;
    std::string defaultOutfit = "Anything";
    int wearSteps = 60;                     // putting on or taking off one piece
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

    // Armor rating 0 to more than 1 (1 stops everything a rating of 1 can). A garment made of a
    // stuff: the stuff's factor times the garment's multiplier; other apparel: its own rating. Times
    // the quality's factor.
    float armor(const entt::registry& reg, Entity item, ArmorType type) const;
    // Degrees Celsius the comfortable range widens: cold lowers its minimum, heat raises its maximum.
    float insulationCold(const entt::registry& reg, Entity item) const;
    float insulationHeat(const entt::registry& reg, Entity item) const;
    // The hit points a new item of `def` made of `stuff` has (the def's, times the stuff's factor).
    int maxHitPoints(DefId def, DefId stuff) const;
    // A share of its maximum, 1 when it has no hit points.
    static float hitPointShare(const entt::registry& reg, Entity item);

    int qualityLevels() const { return static_cast<int>(qualityArmor_.size()); }
    int defaultQuality() const { return defaultQuality_; }
    int qualityOf(const entt::registry& reg, Entity item) const;
    const std::string& qualityName(int level) const;

private:
    struct Apparel {
        bool valid = false;
        bool stuffRequired = false;
        float fixedArmor[3] = {0.0f, 0.0f, 0.0f};
        float armorMultiplier = 1.0f;
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
        float armor[3] = {0.0f, 0.0f, 0.0f};
        float cold = 0.0f;
        float heat = 0.0f;
        float hitPoints = 1.0f;
    };

    std::vector<Apparel> apparel_;  // by Things DefId value
    std::vector<Stuff> stuff_;      // by Things DefId value
    std::vector<float> qualityArmor_;
    std::vector<float> qualityInsulation_;
    std::vector<std::string> qualityNames_;
    int defaultQuality_ = 2;
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
    // Apparel and weapon off a downed or dead pawn, on the ground where it lies.
    bool strip(Entity pawn);
    // Whether the pawn wears anything over a body part group, and the pieces it wears.
    bool covered(Entity pawn, const std::string& group) const;
    const std::vector<Entity>& worn(Entity pawn) const;
    // Total insulation of everything worn, degrees (E2 adds it to the comfortable range).
    void insulation(Entity pawn, float& cold, float& heat) const;
    // The share of its hit points the worst worn piece has left; 1 when it wears nothing.
    float worstCondition(Entity pawn) const;

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

    // One step, after combat: the dead lose hit points on their clothes and taint it, worn clothes wear
    // (once an hour), marks on pawns that need no stripping go. `now` is the game second of the step.
    void step(int64_t now, Rng& rng, const std::vector<Event>& events);

    // The state hash and the save read these.
    void hashThing(Entity e, StateHasher& h) const;
    void hashState(StateHasher& h) const;

private:
    bool hasTrait(Entity pawn, const char* trait) const;
    bool isFemale(Entity pawn) const;
    bool conflicts(Entity item, Entity worn) const;

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
