#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Pathfinder.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class Fire;
class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class MeleeSystem;
class MoodSystem;
class Rng;
class Rooms;
class StateHasher;
class Things;
struct GameTime;

// Mental breaks (task C19). See docs/design/mental-breaks.md.
//
// A colonist whose mood is under a break threshold may snap, at RimWorld's rates: the lower the
// mood band, the harder the break (minor: wander, binge, hide; major: tantrum, long wander, insulting
// spree; extreme: berserk, give up and leave, fire-starting spree). While it lasts the colonist is
// out of the job system's hands and this system walks it. It ends when its time is up and the mood
// has come back above the threshold (and the pawn gets Catharsis).
//
// Unreal does not include this header: it reads a pawn's break from the snapshot (`mentalBreak`)
// and the events PawnMentalBreak, PawnMentalBreakEnded, PawnInsulted and PawnLeftColony.

// What a break makes the pawn do: the `state` of a MentalBreaks def.
enum class BreakState : uint8_t { Wander, Binge, HideInRoom, Tantrum, InsultSpree, Berserk, LeaveColony, FireSpree };

COLONY_API const char* breakStateName(BreakState state);
COLONY_API bool breakStateFromName(const std::string& name, BreakState& out);

// On a pawn that has had a mental break, or is in one. Every field is saved.
struct MentalBreaker {
    bool active = false;
    std::string def;  // the MentalBreaks def, by defName
    BreakState state = BreakState::Wander;
    int64_t startedAt = 0;   // game second
    int64_t endsAfter = 0;   // the break may end from this second on, once the mood has recovered
    int64_t hardEnd = 0;     // and ends at this second whatever the mood
    float recoverLevel = 0.0f;  // the mood (0 to 100) it has to reach
    int64_t lastEnded = -1;  // game second the last break ended, -1 for never
    // Walking to `goal` (docs/design/mental-breaks.md): 0 standing, 1 waiting for a path, 2 walking.
    uint8_t walk = 0;
    uint8_t repaths = 0;
    PathTicket path;  // while `walk` is 1
    bool hasGoal = false;
    CellPos goal;
    Entity target = kNoEntity;  // what it smashes, hits, insults or eats
    int32_t done = 0;           // things smashed, meals eaten, colonists insulted so far
    int64_t nextAct = 0;        // game second it may look for a new goal or target
    int64_t nextHit = 0;        // game second of its next blow, or when the meal in its hands is finished
    std::vector<Entity> visited;  // targets it is finished with (insulted, or not worth trying again)
};

// The numbers: MentalBreakRules (when) and MentalBreakBehaviour (what) in data/defs/Rules.
struct COLONY_API MentalBreakRules {
    bool present = false;
    double meanDays[3] = {4.0, 1.5, 0.5};  // minor, major, extreme
    // (mood / threshold, multiplier of the mean days), x ascending.
    std::vector<std::pair<float, float>> curve = {{0.0f, 0.1f}, {0.5f, 0.5f}, {1.0f, 1.0f}};
    int64_t minSecondsBetween = 24 * 3600;
    float recoveryMargin = 0.1f;  // of the 0 to 100 scale, above the threshold
    bool restOnlyWhenAwake = true;
    double pyromaniacMeanDays = 50.0;  // a pyromaniac's extra fire-starting break at any mood; 0 for never
    // Behaviour (all ours but the interval).
    int checkEvery = 216;  // game seconds between a pawn's chances; RimWorld's 150 ticks
    int wanderRadius = 15;
    int wanderWaitMin = 4;
    int wanderWaitMax = 15;
    int hideRadius = 40;
    int smashRadius = 15;
    int berserkRadius = 25;
    float reach = 1.5f;
    float talkReach = 2.5f;
    int fireRadius = 20;
    int fireEvery = 5;
    int punchEvery = 2;
    float punchDamage = 6.0f;
    std::string punchDamageType = "Blunt";
    int smashDamage = 8;
    int tantrumMaxSmashes = 3;
    int bingeMeals = 4;
    int eatSeconds = 8;
    int repathEvery = 5;
    int searchEvery = 10;
    double hardEndFactor = 2.0;
    double hardEndMinHours = 12.0;

    static MentalBreakRules read(const DefDatabase& defs);
};

class COLONY_API MentalBreakSystem {
public:
    MentalBreakSystem(Things& things, Map& map, HealthSystem& health, FighterSystem& fighters, JobSystem& jobs,
                      MoodSystem& mood, Rooms& rooms, Pathfinder& pathfinder, const DefDatabase& defs,
                      const GameTime& time);
    MentalBreakSystem(const MentalBreakSystem&) = delete;
    MentalBreakSystem& operator=(const MentalBreakSystem&) = delete;

    const MentalBreakRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Whether colonists snap by themselves (default yes). Off for tests of something else that run for days.
    void setNatural(bool natural) { natural_ = natural; }
    // Tests make the pyromaniac's extra fire-starting break (`MentalBreakRules::pyromaniacMeanDays`) common.
    void setPyromaniacMeanDays(double days) { rules_.pyromaniacMeanDays = days; }

    // Berserk fights with F8's melee (the pawn's weapon or fists, real swings, dodges and blocks); without
    // it, or against a thing that isn't a pawn, it punches as before.
    void setMelee(MeleeSystem* melee) { melee_ = melee; }
    // Fire-starting (E4): gives FireSpree the fire to light, and provides the "fire" feature.
    void setFire(Fire* fire);

    // A feature a break can need (a MentalBreaks def's `requires`): "fire" once E4 is in. A break
    // that needs one nothing provides never starts.
    void provide(const std::string& feature);

    // True while the pawn is in a break.
    bool active(Entity pawn) const;
    // The MentalBreaks def of the pawn's break, or an invalid id.
    DefId breakOf(Entity pawn) const;

    // Dev commands (StartMentalBreakCommand, EndMentalBreakCommand). False, with a reason in plain
    // words, when they can't be done. A forced break ignores the mood and the wait between breaks.
    bool start(Entity pawn, const std::string& breakDef, Rng& rng, std::string& error);
    bool end(Entity pawn, std::string& error);

    // One game second: chances for pawns in a break band, then everyone in a break acts. Call before
    // the mood system, so its events (breaks ended, insults) give memories the same step.
    void step(int64_t now, Rng& rng);

    // Call before a thing is destroyed: a pawn after it forgets it, and a pawn waiting for a path
    // gives the request up.
    void forget(Entity thing);
    // After loading: checks what the save says. False, with `error`, when it can't be.
    bool afterLoad(std::string& error);
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct Kind {
        DefId def;
        std::string name;
        int band = 0;  // 0 minor, 1 major, 2 extreme
        double commonality = 1.0;
        double hoursMin = 0.0;
        double hoursMax = 0.0;
        BreakState state = BreakState::Wander;
        int count = 0;
        std::vector<std::string> needs;  // the def's `requires`
    };
    enum class Walk : uint8_t { None, Going, Arrived, Failed };

    const Kind* kindNamed(const std::string& name) const;
    bool provided(const Kind& kind) const;
    bool eligible(Entity pawn) const;
    bool feasible(Entity pawn, const Kind& kind, Rng& rng);
    void natural(Entity pawn, int64_t now, Rng& rng);
    void begin(Entity pawn, const Kind& kind, int band, int64_t now, Rng& rng);
    void finish(Entity pawn, MentalBreaker& b, int64_t now, const char* reason);
    void tick(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);

    // Walking.
    void beginWalk(Entity pawn, MentalBreaker& b, CellPos goal);
    Walk walkStep(Entity pawn, MentalBreaker& b);
    void stopWalk(Entity pawn, MentalBreaker& b);

    // The states.
    void tickWander(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);
    void tickHide(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);
    void tickBinge(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);
    void tickAttack(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng, bool pawnsFirst);
    void tickFireSpree(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);
    void tickInsult(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);
    void tickLeave(Entity pawn, MentalBreaker& b, int64_t now, Rng& rng);
    void leaveColony(Entity pawn, MentalBreaker& b);

    // Finding places and targets.
    bool wanderGoal(Entity pawn, Rng& rng, CellPos& out) const;
    bool hideSpot(Entity pawn, CellPos& out);
    bool edgeGoal(Entity pawn, CellPos& out) const;
    Entity findFood(Entity pawn, const MentalBreaker& b) const;
    Entity findSmashTarget(Entity pawn, const MentalBreaker& b) const;
    Entity findPawnTarget(Entity pawn, const MentalBreaker& b, float radius, bool colonistsOnly) const;
    // Walks (re-planning as a pawn target moves) until within `reach` of `target`; true once there.
    // Berserk on a pawn: walks (re-planning as it moves) until the target is within the pawn's melee reach; true once there.
    bool chase(Entity pawn, MentalBreaker& b, Entity target, int64_t now);
    bool fireGoal(Entity pawn, Rng& rng, CellPos& cell, CellPos& stand) const;
    bool approach(Entity pawn, MentalBreaker& b, Entity target, float reach, int64_t now);
    float distanceTo(Entity pawn, Entity target) const;
    void action(Entity pawn, Entity target, PawnAction what);
    void emit(EventType type, Entity pawn, const std::string& message, float amount);

    Things& things_;
    Map& map_;
    HealthSystem& health_;
    FighterSystem& fighters_;
    JobSystem& jobs_;
    MoodSystem& mood_;
    Rooms& rooms_;
    Pathfinder& pathfinder_;
    MeleeSystem* melee_ = nullptr;
    Fire* fire_ = nullptr;
    const DefDatabase& defs_;
    const GameTime& time_;
    MentalBreakRules rules_;
    std::vector<Kind> kinds_;
    std::vector<std::string> features_;
    std::vector<float> nutrition_;  // by Things def: what one of it feeds, 0 for anything that isn't food
    std::vector<Event>* events_ = nullptr;
    bool natural_ = true;
};

}  // namespace colony
