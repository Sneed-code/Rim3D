#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Random.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace colony {

class HealthModel;
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
    VitalCapacity   // a capacity with `lethalAtZero` fell to zero (breathing, blood pumping...)
};

COLONY_API const char* deathCauseName(DeathCause cause);

// One injury: hit points of damage on a body part. Part index is the position in the Bodies def,
// depth-first, torso 0.
struct Injury {
    uint16_t part = 0;
    DefId hediff;             // Hediffs def: Cut, Gunshot, Bruise...
    float severity = 0.0f;    // hit points lost on the part
    float tendQuality = -1.0f;  // below 0: untended. A tended injury stops bleeding and heals faster
    bool tended() const { return tendQuality >= 0.0f; }
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
    float bloodLoss = 0.0f;          // severity of the BloodLoss hediff, 0 to 1 (1 is death)
    bool dead = false;
    DeathCause cause = DeathCause::None;

    // ---- Derived. Never saved or hashed.
    bool downed = false;
    float pain = 0.0f;
    float bleedPerDay = 0.0f;    // total bleed rate, blood loss per day
    float movingFactor = 1.0f;   // the Moving capacity, 0 while downed or dead; scales walking speed
    std::vector<float> capacities;  // by position in the Capacities defs (by id)
    bool dirty = true;           // injuries changed: work the derived values out again
    int bloodStage = -1;         // current BloodLoss stage, -1 for none

    bool incapacitated() const { return downed || dead; }
};

// Everything a health panel shows, with names instead of ids. Filled by HealthSystem::report.
struct HealthReport {
    struct InjuryLine {
        std::string part;    // "left arm"
        std::string injury;  // "gunshot"
        float severity = 0.0f;
        bool tended = false;
    };
    struct CapacityLine {
        std::string name;  // def name, "Consciousness"
        float value = 1.0f;
    };
    bool dead = false;
    bool downed = false;
    DeathCause cause = DeathCause::None;
    bool protagonist = false;
    float pain = 0.0f;
    float bleedPerDay = 0.0f;
    float bloodLoss = 0.0f;
    std::vector<InjuryLine> injuries;
    std::vector<std::string> missingParts;  // "left hand"
    std::vector<CapacityLine> capacities;
};

// What combat needs to know about one body part (HealthSystem::bodyParts).
struct BodyPartInfo {
    std::string label;  // "left arm"
    int parent = -1;
    bool inside = false;     // an organ or bone under another part
    float ownOutside = 0.0f;  // its share of hits from outside (its area minus its outside children's)
    std::vector<std::string> groups;  // BodyPartGroups defs, which apparel covers
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

    // One game second: bleeding, blood loss, natural healing, death and downing. `seed` is the
    // game's seed, which healing's choices are made from (not the shared random stream, so the
    // order pawns are visited in can't matter).
    void step(int64_t seconds, uint64_t seed);

    // Works the derived values out for one pawn or all of them, without events. Called after
    // loading a save.
    void refresh(Entity pawn);
    void refreshAll();

    bool incapacitated(Entity pawn) const;
    bool report(Entity pawn, HealthReport& out) const;

    // Hit points of a part right now (0 if it is missing), and with no damage.
    float partHitPoints(Entity pawn, int part) const;
    float partMaxHitPoints(Entity pawn, int part) const;

private:
    Things& things_;
    std::unique_ptr<HealthModel> model_;
    std::vector<Event>* events_ = nullptr;
    std::vector<Entity> active_;  // scratch for step

    // Works out pain, bleeding, capacities and what they mean (the death cause and whether the
    // pawn should be downed) into `health`'s derived fields. Changes nothing else.
    void derive(int body, float healthScale, Health& health, DeathCause& cause, bool& shouldBeDown) const;
    void recompute(Entity pawn, Health& health, bool announce);
    void emit(EventType type, Entity pawn, DefId def = DefId{}, int part = -1, float amount = 0.0f,
              const char* message = nullptr);
    void stopWalking(Entity pawn);
    void die(Entity pawn, Health& health, DeathCause cause, bool announce);
    bool hitPart(Entity pawn, DefId damageType, float amount, int target, bool hurtOuter);
    bool damagePart(Entity pawn, Health& health, int part, DefId hediff, float amount);
    void destroyPart(Entity pawn, Health& health, int part, float amount);
};

}  // namespace colony
