#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class StateHasher;
class Things;
struct GameTime;
struct HealthMods;

// Resonance: the attuned and their powers (task X2). See docs/design/resonance.md.
//
// Resonance is our name for RimWorld's psycasts (docs/design/naming.md): an attuned pawn has an
// attunement level (RimWorld's psylink level, 1 to 6), strain (neural heat), which every power adds
// to and which falls by itself, and focus (psyfocus), which every power spends. A power is cast with
// CastPowerCommand or by the fighter AI: a short warm-up, then it goes off. The first four are Stun,
// Numb (RimWorld's painblock), Burden and Jump (skip), from the Powers defs.
//
// Unreal does not include this header: it reads Snapshot::attuned and the ThingView fields
// (stunnedFor, numbed, burdened, castPower, castStepsLeft), the PowerCast event and the `Cast`
// PawnAction, and sends AttuneCommand, LearnPowerCommand and CastPowerCommand.

// On an attuned pawn. Every field is saved.
struct Attunement {
    int level = 1;               // 1 to ResonanceRules.maxLevel
    float strain = 0.0f;         // RimWorld's neural heat
    float focus = 0.0f;          // RimWorld's psyfocus, 0 to 1
    std::vector<DefId> powers;   // the Powers defs it knows, in def id order
};

// On a pawn winding up a power: it stands still (jobs wait, fighters hold) until it goes off.
struct Casting {
    DefId power;
    Entity target = kNoEntity;
    CellPos cell;                // where a jump goes
    int stepsLeft = 0;
    bool player = false;
};

// What powers did to a pawn and how long it lasts (a stun is `Stunned`, in Combat.h). Steps left; 0 is off.
struct ResonanceEffects {
    int numbSteps = 0;
    float painFactor = 1.0f;     // while numbed
    int burdenSteps = 0;
    float movingMax = 1.0f;      // while burdened
};

// One Powers def, read once.
struct PowerDef {
    enum class Kind : uint8_t { Stun, PainFactor, MovingMax, Jump };
    DefId def;
    std::string defName;
    Kind kind = Kind::Stun;
    int level = 1;
    float strain = 0.0f;
    float focusCost = 0.0f;
    int warmup = 8;
    float range = 0.0f;
    bool needsSight = true;
    bool self = false;           // the caster may be its own target
    bool hostile = false;        // used on enemies (stun, burden): the AI aims it at them
    float value = 1.0f;          // painFactor or movingMax
    int duration = 0;            // steps
    bool bySensitivity = false;  // duration times the target's sensitivity
    float maxBodySize = 0.0f;    // jump: bigger pawns can't be moved
};

// The numbers from ResonanceRules (data/defs/Rules). The code falls back to the same values.
struct COLONY_API ResonanceRules {
    int maxLevel = 6;
    float strainLimitBase = 2.0f;
    float strainLimitPerLevel = 10.0f;
    float strainRecoveryPerStep = 0.018f;
    float strainRecoveryPerLevel = 0.125f;
    float strainRecoveryPerPain = 3.0f;
    std::vector<float> focusBandBelow{0.25f, 0.5f, 2.0f};
    std::vector<float> focusDecayPerDay{0.035f, 0.055f, 0.075f};
    std::vector<float> minFocusByLevel{0.0f, 0.0f, 0.0f, 0.25f, 0.25f, 0.5f, 0.5f};
    float startFocus = 0.5f;
    int aiEvery = 15;
    float aiNumbPain = 0.35f;
    float aiStunReach = 4.0f;
    float aiBurdenRange = 12.0f;
    float aiJumpThreat = 4.0f;
    float aiJumpMin = 5.0f;

    static ResonanceRules read(const DefDatabase& defs);
};

class COLONY_API ResonanceSystem {
public:
    ResonanceSystem(Things& things, Map& map, HealthSystem& health, JobSystem& jobs, FighterSystem& fighters,
                    const DefDatabase& defs, const GameTime& time, uint64_t seed);

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    const ResonanceRules& rules() const { return rules_; }
    const std::vector<PowerDef>& powers() const { return powers_; }
    const PowerDef* power(DefId def) const;
    const PowerDef* power(const std::string& defName) const;

    // A pawn's resonance sensitivity (RimWorld's psychic sensitivity): its def's
    // `stats.resonanceSensitivity` (1 without), through its genes' ResonanceSensitivity stat.
    float sensitivity(Entity pawn) const;
    // The most strain the pawn may carry: (base + level) x per level x sensitivity; 0 if not attuned.
    float strainLimit(Entity pawn) const;
    const Attunement* attunement(Entity pawn) const;
    bool knows(Entity pawn, DefId power) const;

    // ---- Commands. False, with a reason in plain words, when they can't be done.
    // Sets a pawn's attunement level (0 takes it away). A pawn attuned for the first time starts with
    // ResonanceRules.startFocus. Each new level it reaches teaches it one power of that level it
    // doesn't know, picked at random (RimWorld's rule), if there is one.
    bool attune(Entity pawn, int level, std::string& error);
    // Teaches a power of the pawn's level or lower (RimWorld's psytrainer).
    bool learn(Entity pawn, DefId power, std::string& error);
    // Starts a cast: `target` is the pawn the power is used on (the caster itself for a self power),
    // `cell` where a jump takes it. Checked now: the pawn knows the power and can act, the focus and
    // strain allow it, the target is in range and in sight (and the cell, for a jump).
    bool cast(Entity pawn, DefId power, Entity target, CellPos cell, bool player, std::string& error);
    bool casting(Entity pawn) const;

    // What powers do to health right now: HealthSystem asks for every pawn it works out.
    void healthMods(Entity pawn, HealthMods& mods) const;

    // One step, after the fighters: stuns and effects run out, strain falls and focus fades, casts
    // go off, and attuned fighters choose powers to use.
    void step();

    // Hashes the resonance components of one thing (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

private:
    Things& things_;
    Map& map_;
    HealthSystem& health_;
    JobSystem& jobs_;
    FighterSystem& fighters_;
    const DefDatabase& defs_;
    const GameTime& time_;
    uint64_t seed_ = 0;
    ResonanceRules rules_;
    std::vector<PowerDef> powers_;
    std::vector<Event>* events_ = nullptr;
    std::vector<Entity> scratch_;

    bool canAct(Entity pawn) const;
    bool checkTarget(Entity pawn, const PowerDef& p, Entity target, CellPos cell, std::string& error) const;
    bool sees(Entity from, Entity to) const;
    bool seesCell(Entity from, CellPos cell) const;
    void release(Entity pawn, const Casting& c);
    void stun(Entity pawn, int steps);
    void jump(Entity pawn, CellPos cell);
    void effectsChanged(Entity pawn);
    void think(Entity pawn);
    int64_t now() const;
};

}  // namespace colony
