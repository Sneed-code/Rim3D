#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class CombatSystem;
class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class RaidSystem;
class Rng;
class StateHasher;
class Things;
struct GameTime;

// Prisoners (task C22). See docs/design/prisoners.md.
//
// A downed enemy is carried to a prison bed by a warden and becomes a prisoner: it keeps its own side (`Faction`) but nobody
// shoots it, it lives in its cell, wardens feed it and talk to it, and what the talk does is the prisoner's mode: wear
// down its resistance, recruit it when the resistance is gone, or set it free. Unreal does not include this header: it
// reads `prisoner`, `prisonerMode`, `resistance` and `prisonBed` from the snapshot and sends SetPrisonerModeCommand and
// SetPrisonBedCommand.

// What a warden's talk with a prisoner does (RimWorld's prisoner interaction mode).
enum class PrisonerMode : uint8_t {
    NoInteraction,     // nobody talks to it
    ReduceResistance,  // each talk lowers its resistance, down to 0
    AttemptRecruit,    // the same, and once the resistance is 0 each talk tries to recruit it
    Release,           // the next talk sets it free (it walks off the map)
};

COLONY_API const char* prisonerModeName(PrisonerMode mode);
// Parses a name from prisonerModeName; false if it is not one.
COLONY_API bool prisonerModeFromName(const std::string& name, PrisonerMode& out);

// On a pawn held in a prison bed. Every field is saved.
struct Prisoner {
    PrisonerMode mode = PrisonerMode::ReduceResistance;
    float resistance = 0.0f;   // 0 when it can be recruited
    int64_t capturedAt = 0;    // game second
    int64_t nextChat = 0;      // game second from which a warden may talk to it again
};

// On a bed building: only prisoners sleep in it, and downed enemies are carried to it.
struct PrisonBed {
    bool marked = true;
};

// The numbers from PrisonerRules (data/defs/Rules). `present` is false when the defs have none; the same values are used.
struct COLONY_API PrisonerRules {
    bool present = false;
    float initialResistanceMin = 5.0f;     // RimWorld wiki, the Warrior pawn kind's example range
    float initialResistanceMax = 9.0f;
    float resistancePerChat = 1.0f;        // RimWorld wiki: 1.0 times the three factors below
    // The prisoner's mood level (0 to 100) against the factor on a talk's effect (RimWorld wiki: 20% at 0, 100% at 50, 150% at 100).
    std::vector<std::pair<float, float>> moodFactor = {{0.0f, 0.2f}, {50.0f, 1.0f}, {100.0f, 1.5f}};
    // Ours: the negotiation ability of a warden is `negotiationBase` plus `negotiationPerLevel` per Social level, times Talking.
    float negotiationBase = 0.5f;
    float negotiationPerLevel = 0.05f;
    int defaultSkillLevel = 8;
    int64_t chatIntervalSeconds = 21600;   // "every several hours" (6 game hours)
    int64_t chatWorkSeconds = 360;
    int64_t feedWorkSeconds = 120;
    float chatXp = 100.0f;
    float recruitChanceBase = 0.5f;        // times the mood factor, between the two limits
    float recruitChanceMin = 0.05f;
    float recruitChanceMax = 0.95f;
    float captureSafeRadius = 25.0f;       // wardens leave a downed enemy alone while an able one is this close
    float feedBelow = 0.3f;                // wardens feed a prisoner whose food is under this
    int edgeSearch = 40;                   // cells along the map edge looked at for a way out

    static PrisonerRules read(const DefDatabase& defs);
};

class COLONY_API PrisonerSystem {
public:
    PrisonerSystem(Things& things, Map& map, CombatSystem& combat, HealthSystem& health, JobSystem& jobs, FighterSystem& fighters,
                   RaidSystem& raids, const DefDatabase& defs, const GameTime& time, Rng& rng);

    const PrisonerRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }

    bool isPrisoner(Entity pawn) const;
    bool isPrisonBed(Entity bed) const;
    // How many prisoners the colony holds.
    int count() const;
    // The prisoners, in id order.
    std::vector<Entity> prisoners() const;

    // Whether wardens may capture this pawn now: a living, downed pawn of another side that is no animal (not wild, not a prisoner
    // already) on the map.
    bool capturable(Entity pawn) const;
    // The prison bed slot a warden (`helper`) would put `pawn` in: free, reachable, the nearest to the pawn. False if there is none.
    bool findPrisonBed(Entity pawn, Entity helper, Entity& bed, CellPos& cell) const;

    // Commands. False, with a reason in plain words, when they can't be done.
    bool setMode(Entity pawn, PrisonerMode mode, std::string& error);
    bool setPrisonBed(Entity bed, bool on, std::string& error);

    // The end of a capture: `pawn` becomes the colony's prisoner in `bed` (the bed's slot is its own, its weapons are dropped, it stops
    // fighting and leaves its raid squad). `warden` is only for the event. Also a dev entry point for tests.
    bool imprison(Entity pawn, Entity bed, Entity warden, std::string& error);

    // Whether a warden may start a talk with this prisoner now: it has a mode that wants one, its time has come, it is not
    // downed, it lies or stands somewhere a warden can walk to, and (Release) it can walk.
    bool chatDue(Entity prisoner) const;
    // One talk: the prisoner's resistance falls, or it is recruited, or it is released, by its mode. Sets the next time.
    void interact(Entity warden, Entity prisoner);
    // How much a talk by `warden` lowers `prisoner`'s resistance now.
    float resistanceReduction(Entity warden, Entity prisoner) const;
    // The chance a recruit attempt on `prisoner` works now (once its resistance is 0).
    float recruitChance(Entity prisoner) const;
    // The prisoner joins the colony (its side becomes the player's, it works and fights for it), or is set free (it walks off the
    // map). False if it is no prisoner (or, for a release, can't walk).
    bool recruit(Entity prisoner, Entity warden);
    bool release(Entity prisoner, Entity warden);
    // The hook for T3's prison break incident: every prisoner who can walk breaks out (it is a prisoner no more and fights
    // its own side's fight, with no weapon, walking off the map; a PrisonBreak event). Returns how many.
    int breakOut(const char* why);

    // Freed prisoners that reach the map edge leave it. Runs once a step, after the fighters.
    void step();

    // After a prisoner or bed is destroyed (or a pawn leaves): nothing to do now, kept for symmetry with the other systems.
    void hashThing(Entity e, StateHasher& h) const;

private:
    int64_t now() const;
    float moodOf(Entity pawn) const;
    CellPos edgeGoal(Entity pawn) const;
    void emit(EventType type, Entity thing, Entity target, float amount, const char* message) const;

    Things& things_;
    Map& map_;
    CombatSystem& combat_;
    HealthSystem& health_;
    JobSystem& jobs_;
    FighterSystem& fighters_;
    RaidSystem& raids_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Rng& rng_;
    PrisonerRules rules_;
    std::vector<Event>* events_ = nullptr;
};

}  // namespace colony
