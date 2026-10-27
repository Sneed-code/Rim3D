#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace colony {

class HealthModel;
class MoodSystem;
class Things;

// Health of pawns: a body of parts from the Bodies defs, injuries on parts, bleeding, pain,
// downed and death, slow natural healing. See docs/design/health.md.
//
// Unreal does not include this header: it reads `downed` and `dead` from the snapshot, health
// events from the event list, and a full report from Sim::healthReport.

// Where a pawn is resting, which speeds up healing (HealthRules.restHealBonusPerDay). Whatever
// puts a pawn in a bed sets it (C4/C7); the default is the ground.
enum class RestKind : uint8_t { Ground, Bed, HospitalBed };

enum class DeathCause : uint8_t {
    None,
    BloodLoss,      // blood loss reached its lethal severity
    VitalPartLost,  // a part with `lethalIfDestroyed` was destroyed (torso, head, brain, heart...)
    VitalCapacity,  // a capacity with `lethalAtZero` fell to zero (breathing, blood pumping...)
    Starvation,     // malnutrition reached its lethal severity (Needs.h)
    Hypothermia,    // an Environment hediff reached its lethal severity (Temperature.h)
    Heatstroke,
    Environment,    // any other Environment hediff
    Slaughtered,    // a colonist killed a tame animal (Husbandry.h)
    Disease,        // a disease reached its lethal severity (H2, docs/design/diseases.md)
    Surgery,        // an operation went wrong (Surgery.h)
    Executed,       // a warden executed a prisoner (Prisoners.h)
    Overdose,       // the Overdose hediff reached its lethal severity (Drugs.h)
    Withdrawal      // a withdrawal hediff did (luciferium)
};

COLONY_API const char* deathCauseName(DeathCause cause);

// One injury: hit points of damage on a body part. Part index is the position in the Bodies def,
// depth-first, torso 0.
struct Injury {
    uint16_t part = 0;
    DefId hediff;             // Hediffs def: Cut, Gunshot, Bruise...
    float severity = 0.0f;    // hit points lost on the part
    float tendQuality = -1.0f;  // below 0: untended. A tended injury stops bleeding and heals faster
    float peak = 0.0f;          // the most severity it has had; set when healing starts (H1: scars)
    int64_t infectAt = -1;      // game second the wound turns infected (H2); below 0: it won't. Tending can take it away
    bool tended() const { return tendQuality >= 0.0f; }
};

// An artificial part fitted to the body (task H3, docs/design/surgery.md): a Hediffs def of class AddedPart (peg leg, bionic
// arm...) on a body part. It stands for the whole part and everything under it, with its own efficiency instead of the
// natural one (damage on the part still lowers it).
struct AddedPart {
    uint16_t part = 0;
    DefId hediff;
};

// What a body part is, for surgery (HealthSystem::partCondition).
enum class PartCondition : uint8_t {
    Whole,    // there, with or without wounds
    Missing,  // lost; a prosthetic can be fitted here
    Under,    // under a lost part
    Covered,  // under a fitted artificial part, which stands for it
    Fitted    // an artificial part is on it
};

// A severity-based illness from the surroundings (hypothermia, heatstroke...): a Hediffs def of class
// Environment, which has stages with capacity offsets like blood loss. TemperatureSystem moves the
// severity (docs/design/temperature.md); the stage shows in the capacities.
struct EnvironmentHediff {
    DefId def;
    float severity = 0.0f;
};

// A sickness on a pawn (H2, docs/design/diseases.md): a Hediffs def of class Disease. Severity rises or falls in a race
// against the pawn's immunity; the stage (from the def) says what it does to the capacities.
struct Disease {
    DefId def;
    float severity = 0.0f;
    float tendQuality = -1.0f;  // quality of the last tend, kept while it lasts; below 0 once it has run out
    int64_t tendedUntil = 0;    // game second the last tend runs out
    float tendTotal = 0.0f;     // tend quality added up over every tend; gut worms and muscle parasites go at 3
    int32_t part = -1;          // an infection: the wound's body part; -1 for a sickness of the whole body
};

// How immune a pawn is to one disease def, 0 to 1. It rises while the pawn has the disease and at 1 the severity falls.
struct Immunity {
    DefId def;
    float level = 0.0f;
};

// A destroyed body part. Its children are gone with it (they are not listed).
struct MissingPart {
    uint16_t part = 0;
    float tendQuality = -1.0f;  // below 0: untended, so it bleeds
    bool tended() const { return tendQuality >= 0.0f; }
};

// Every pawn whose def names a body (`race.body`) has one. What is saved and hashed is the first
// block; the second is worked out from it again after any change (and after loading).
struct Health {
    bool protagonist = false;  // 2x hit points on every part (HealthRules.protagonistPartHitPointsFactor);
                               // set before the pawn is hurt
    RestKind rest = RestKind::Ground;
    std::vector<Injury> injuries;    // in the order they happened
    std::vector<MissingPart> missing;  // in the order they were lost
    std::vector<AddedPart> added;      // artificial parts, in the order they were fitted (H3)
    float bloodLoss = 0.0f;          // severity of the BloodLoss hediff, 0 to 1 (1 is death)
    std::vector<EnvironmentHediff> environment;  // by def id, only those above 0
    std::vector<Disease> diseases;       // by def id, then part (H2)
    std::vector<Immunity> immunities;    // by def id, only those above 0 or with the disease (H2)
    bool dead = false;
    DeathCause cause = DeathCause::None;

    // ---- Derived. Never saved or hashed.
    float stageHp = 1.0f;        // young animals: the life stage's healthScaleFactor, set by AnimalSystem (N4 leftovers)
    bool downed = false;
    float pain = 0.0f;
    float bleedPerDay = 0.0f;    // total bleed rate, blood loss per day
    float movingFactor = 1.0f;   // the Moving capacity, 0 while downed or dead; scales walking speed
    std::vector<float> capacities;  // by position in the Capacities defs (by id)
    bool dirty = true;           // injuries changed: work the derived values out again
    int bloodStage = -1;         // current BloodLoss stage, -1 for none

    bool incapacitated() const { return downed || dead; }
};

// Changes to pain and capacities from outside the body (resonance's numb and burden, X2): pain is
// multiplied by `painFactor`, the Moving capacity is capped at `movingMax`. Not saved: the system
// that causes them is asked again (HealthSystem::setMods) whenever health is worked out.
struct HealthMods {
    float painFactor = 1.0f;
    float movingMax = 1.0f;
};

// Everything a health panel shows, with names instead of ids. Filled by HealthSystem::report.
struct HealthReport {
    struct InjuryLine {
        std::string part;    // "left arm"
        std::string injury;  // "gunshot"
        float severity = 0.0f;
        bool tended = false;
        float tendQuality = -1.0f;  // below 0 while untended
        bool permanent = false;     // a scar or old wound: it never heals
    };
    struct CapacityLine {
        std::string name;  // def name, "Consciousness"
        float value = 1.0f;
    };
    struct EnvironmentLine {
        std::string name;   // "hypothermia"
        std::string stage;  // "shivering", "extreme"...
        float severity = 0.0f;
        bool lifeThreatening = false;
    };
    struct DiseaseLine {
        std::string name;   // "flu"
        std::string part;   // an infection's wound: "left arm"; empty for the whole body
        std::string stage;  // "minor", "extreme"...
        float severity = 0.0f;
        float immunity = 0.0f;
        bool tended = false;
        bool lifeThreatening = false;
        bool visible = true;  // false while it is too early to notice
    };
    bool dead = false;
    bool downed = false;
    DeathCause cause = DeathCause::None;
    bool protagonist = false;
    float pain = 0.0f;
    float bleedPerDay = 0.0f;
    float bloodLoss = 0.0f;
    std::vector<InjuryLine> injuries;
    std::vector<std::string> missingParts;  // "left hand"; one with an artificial part on it is not listed
    struct AddedLine {
        std::string part;     // "left leg"
        std::string label;    // "peg leg"
        float efficiency = 1.0f;
    };
    std::vector<AddedLine> addedParts;
    std::vector<EnvironmentLine> environment;
    std::vector<DiseaseLine> diseases;
    std::vector<CapacityLine> capacities;
};

// What combat needs to know about one body part (HealthSystem::bodyParts).
struct BodyPartInfo {
    std::string label;  // "left arm"
    int parent = -1;
    bool inside = false;     // an organ or bone under another part
    float ownOutside = 0.0f;  // its share of hits from outside (its area minus its outside children's)
    std::vector<std::string> groups;  // BodyPartGroups defs, which apparel covers
    std::string def;         // its BodyParts def name: "Arm", "Hand"...
};

// The rules. Owned by the sim, which calls `step` once per game second; damage comes in through
// `applyDamage` (from commands now, from projectiles and melee later).
class COLONY_API HealthSystem {
public:
    explicit HealthSystem(Things& things);
    ~HealthSystem();
    HealthSystem(const HealthSystem&) = delete;
    HealthSystem& operator=(const HealthSystem&) = delete;

    // Where events go; null (the default) reports none.
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Where HealthMods come from for a pawn (resonance). Without it there are none. Set `Health::dirty` on a pawn
    // when its mods change, so the next step works it out again (and it may get up or go down).
    void setMods(std::function<void(Entity, HealthMods&)> mods) { mods_ = std::move(mods); }

    // True if the pawn has a Health the system understands (its body def is known).
    bool tracks(Entity pawn) const;
    // Number of parts in the body of a pawn def (0 if it has none). Saves check part numbers
    // against it.
    int partCount(DefId pawnDef) const;
    // Pawn def's body part names, in index order; empty if it has no body.
    std::vector<std::string> partLabels(DefId pawnDef) const;

    // Damage by a Damage types def (Bullet, Cut, Blunt...). `part` is the exact part to hit, or -1
    // to pick one at random as a hit would: by how much of the body each outside part covers, from
    // `rng`; penetrating types can then go in to an organ or bone under it (and the outside part
    // over it is hurt too). Returns false, and changes nothing, if the pawn has no tracked health,
    // is dead, the type is unknown, or the part is missing or doesn't exist.
    bool applyDamage(Entity pawn, DefId damageType, float amount, int part, Rng& rng);
    DefId damageTypeId(const std::string& name) const;

    // Damage to exactly this part, after the hit has been aimed (combat picks the part from a
    // hitbox and goInside): as a random hit there would be, an inside part also hurts the outside
    // part over it. Same failures as applyDamage.
    bool applyHit(Entity pawn, DefId damageType, float amount, int part);
    // Where a hit on `part` ends up if the damage type reaches inside: an organ or bone under it,
    // by coverage, as applyDamage's random hits do. `part` itself if not.
    int goInside(Entity pawn, DefId damageType, int part, Rng& rng) const;
    // The parts of a pawn def's body, in index order; empty if it has none.
    std::vector<BodyPartInfo> bodyParts(DefId pawnDef) const;

    // Tends every untended injury and lost part with this quality (0 is a rag, 1 is good
    // medicine), so they stop bleeding and heal faster. Returns how many it tended.
    int tendAll(Entity pawn, float quality);
    // What doctors and patients ask (task H1, docs/design/medicine.md). `needsTending`: an injury that
    // can be tended (its Hediffs def's `tendable`, not a scar) and isn't, or a lost part that isn't.
    // `needsRest`: a tended injury still healing, or any bleeding: the pawn belongs in a bed.
    // `urgentlyNeedsTending`: it needs tending and bleeds (so is losing blood).
    bool needsTending(Entity pawn) const;
    bool needsRest(Entity pawn) const;
    bool urgentlyNeedsTending(Entity pawn) const;

    // One game second: bleeding, blood loss, natural healing, death and downing. `seed` is the
    // game's seed, which healing's choices are made from (not the shared random stream, so the
    // order pawns are visited in can't matter).
    void step(int64_t seconds, uint64_t seed);

    // Works the derived values out for one pawn or all of them, without events. Called after
    // loading a save.
    void refresh(Entity pawn);
    // `exact`: the save held each pawn's downed and dirty flags, so they stay as saved.
    void refreshAll(bool exact = false);

    bool incapacitated(Entity pawn) const;
    // Kills a pawn for a cause that is not an injury (starvation). False if it has no tracked
    // health or is dead already.
    bool kill(Entity pawn, DeathCause cause);
    bool report(Entity pawn, HealthReport& out) const;
    // A capacity of the pawn (a Capacities defName like "Manipulation") as it is now, 1 for a
    // pawn with no body or a capacity the data doesn't have. Work speed uses it.
    float capacityValue(Entity pawn, const std::string& capacity) const;

    // Environment hediffs (hypothermia...): the severity of a Hediffs def of class Environment on a
    // pawn (0 if it has none), and setting it (clamped to 0 and up; at 0 the hediff goes). A change
    // of stage works the capacities out again. Returns false, and changes nothing, if the pawn has no
    // tracked health, is dead, or the def is not an Environment hediff.
    float environmentSeverity(Entity pawn, DefId hediff) const;
    bool setEnvironmentSeverity(Entity pawn, DefId hediff, float severity);
    // The severity at which the hediff kills (its `lethalSeverity`, 1 by default), 0 if the def is not one.
    float environmentLethalSeverity(DefId hediff) const;

    // Puts an injury of `hediff` (a Hediffs def of class Injury) on a body part without a damage type,
    // for things like frostbite. Returns false if the pawn has no tracked health, is dead, or the
    // part is missing or doesn't exist.
    bool addInjury(Entity pawn, int part, DefId hediff, float severity);
    // ---- Infections and diseases (task H2, docs/design/diseases.md)
    // The clock (game seconds) and the game's seed, which an infection's rolls are made from. Without them wounds
    // never turn infected.
    void setWorld(const int64_t* clock, uint64_t seed) {
        clock_ = clock;
        seed_ = seed;
    }
    // The cleanliness of the cell a pawn stands in (RoomStats), which scales a fresh wound's chance of infection:
    // dirt raises it, clean floors lower it (MedicineRules.infection.cleanlinessSlope). Without it the factor is 1.
    void setCleanliness(std::function<float(Entity pawn)> cleanliness) { cleanliness_ = std::move(cleanliness); }
    // Gives the pawn the disease (a Hediffs def of class Disease) at `severity` (below 0: the def's
    // `initialSeverity`). An infection needs the wound's `part`. False, and nothing changes, if the pawn has no
    // tracked health or is dead, the def is not a disease, the pawn already has it (an infection: on that part) or
    // is fully immune. Event `PawnSick`.
    bool contract(Entity pawn, DefId disease, float severity = -1.0f, int part = -1);
    // An outbreak (the hook for T3's events): gives the disease to up to `victims` of the candidates that could
    // catch it, picked with `rng`. Returns who caught it, in the order picked.
    std::vector<Entity> outbreak(DefId disease, std::vector<Entity> candidates, int victims, Rng& rng);
    DefId diseaseId(const std::string& name) const;   // a Hediffs def of class Disease; invalid otherwise
    bool isDisease(DefId hediff) const;
    // The disease's severity on the pawn (the worst one if it is an infection in several wounds), 0 if it has none.
    float diseaseSeverity(Entity pawn, DefId disease) const;
    // Immunity to the disease, 0 to 1.
    float immunity(Entity pawn, DefId disease) const;
    // Sets the immunity (clamped to 0 and 1); false if the pawn has no tracked health or the def is not a disease.
    bool setImmunity(Entity pawn, DefId disease, float level);
    // The pawn has a disease past its first, unnoticed stage.
    bool sick(Entity pawn) const;
    // Multipliers from the diseases the pawn has: gut worms make it hungrier, sleeping sickness tires it faster.
    float foodRateFactor(Entity pawn) const;
    float restFallFactor(Entity pawn) const;
    // Days between each pawn's chance of the disease (MedicineRules.diseaseOnsetMtbDays), 0 for one with none:
    // what an event system picks outbreaks by.
    float onsetMtbDays(DefId disease) const;
    // The mood situation "illness" (the Sick thought).
    void registerSituations(MoodSystem& mood) const;

    // The parts of a pawn's body in a body part group (e.g. "Fingers", "Toes" when the data has
    // them), by part index, in body order.
    std::vector<int> partsOfDef(DefId pawnDef, const std::string& partDef) const;

    // ---- Surgery (task H3, docs/design/surgery.md) ---------------------------------------------------
    // What a part is now. `ok` (if given) is false for a pawn without a body or a part number the body doesn't have.
    PartCondition partCondition(Entity pawn, int part, bool* ok = nullptr) const;
    // The part's Bodies part def ("Leg"), "" if it has none.
    std::string partDef(Entity pawn, int part) const;
    // True for the parts the limb capacities are built from (arm, hand, fingers, leg, foot, toes).
    bool partIsLimb(Entity pawn, int part) const;
    // Whether an operation may take the natural part off: it is Whole, can be destroyed, and neither it nor anything
    // under it keeps the pawn alive.
    bool canRemovePart(Entity pawn, int part) const;
    // The artificial part on `part` (an invalid id if none), and its Hediffs def's numbers.
    DefId addedAt(Entity pawn, int part) const;
    // The Hediffs def of an AddedPart: the Bodies part def it goes on, the item it comes from and goes back to, and
    // its efficiency. False if the def is not an AddedPart.
    bool addedInfo(DefId hediff, std::string& partDef, DefId& item, float& efficiency) const;
    // Fits an artificial part on a Whole or Missing part with none: wounds on the part go (it is replaced), a lost part
    // is tended with `tendQuality` (so it stops bleeding). False, changing nothing, if that can't be done.
    bool installPart(Entity pawn, int part, DefId hediff, float tendQuality);
    // Takes the artificial part off `part`: returns its Hediffs def (invalid if there was none). The part under it is
    // then lost (if it wasn't already), tended with `tendQuality`.
    DefId uninstallPart(Entity pawn, int part, float tendQuality);
    // Takes a natural part off in an operation (canRemovePart): it becomes a lost part tended with `tendQuality`, and
    // any artificial part on it is gone with it. False, changing nothing, if that can't be done.
    bool removePart(Entity pawn, int part, float tendQuality);
    // Hurts a part by an operation gone wrong, as a cut of `fraction` of its hit points (the nearest part above it that
    // is there, if it is missing). A cut never destroys the part.
    void botch(Entity pawn, int part, float fraction);

    // Hit points of a part right now (0 if it is missing), and with no damage.
    float partHitPoints(Entity pawn, int part) const;
    float partMaxHitPoints(Entity pawn, int part) const;

private:
    Things& things_;
    std::unique_ptr<HealthModel> model_;
    std::vector<Event>* events_ = nullptr;
    const int64_t* clock_ = nullptr;
    uint64_t seed_ = 0;
    std::function<float(Entity)> cleanliness_;
    std::vector<Entity> active_;  // scratch for step
    std::vector<size_t> healable_;  // scratch for step: the injuries that can heal
    std::function<void(Entity, HealthMods&)> mods_;

    // Works out pain, bleeding, capacities and what they mean (the death cause and whether the
    // pawn should be downed) into `health`'s derived fields. Changes nothing else.
    void derive(int body, float healthScale, bool mechanical, const HealthMods& mods, Health& health, DeathCause& cause,
                bool& shouldBeDown) const;
    void recompute(Entity pawn, Health& health, bool announce, bool exact = false);
    void emit(EventType type, Entity pawn, DefId def = DefId{}, int part = -1, float amount = 0.0f,
              const char* message = nullptr);
    void stopWalking(Entity pawn);
    void die(Entity pawn, Health& health, DeathCause cause, bool announce);
    bool hitPart(Entity pawn, DefId damageType, float amount, int target, bool hurtOuter);
    bool damagePart(Entity pawn, Health& health, int part, DefId hediff, float amount);
    void destroyPart(Entity pawn, Health& health, int part, float amount);
    void leaveScar(Entity pawn, Health& health, const Injury& healed, Rng& rng);
    // Rolls whether a fresh wound will turn infected, and when.
    void rollInfection(Entity pawn, Health& health, Injury& injury);
    // Every update interval: infected wounds turn into infections, and each disease moves on.
    void updateDiseases(Entity pawn, Health& health, int64_t now);
    // Ends the disease at `index`: it is gone, and so is the immunity if the def doesn't keep it. Event `PawnRecovered`.
    void cureDisease(Entity pawn, Health& health, size_t index);
};

}  // namespace colony
