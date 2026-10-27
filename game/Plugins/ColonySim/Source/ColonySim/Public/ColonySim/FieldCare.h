#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <vector>

namespace colony {

class DefDatabase;
class FighterSystem;
class HealthSystem;
class JobSystem;
class Map;
class RaidSystem;
class Things;
struct GameTime;
struct JobTarget;

// Medics, firefighters and rescue under fire (task F10). See docs/design/field-care.md.
//
// While the colony alarm is on, a colonist with the Medic defense role does only Doctor work and a Firefighter only
// Firefighter work, and only on targets the enemies can't see (a downed friend behind cover, a fire out of their
// sight); with nothing to do they wait behind the line, on the colony's side of home. An enemy that comes close
// makes them defend themselves. And the alarm's fighters drag a downed friend lying in the open to the nearest cell
// out of the enemies' sight, when the way there is reasonably safe, so the medics can reach it.
//
// It keeps no state of its own: everything it decides follows from the alarm, the defense roles, the fighters and
// the jobs, which are saved. Unreal does not include this header: it sees the Medic and Firefighter roles in
// `defenseRole`, a duty's jobs as jobs, a fighter's carry as `rescuing` and the LiftPawn and LayPawn actions.

// The numbers (FieldCareRules in data/defs/Rules/FieldCareRules.json, all ours: RimWorld has no alarm roles).
struct FieldCareRules {
    int thinkEvery = 15;              // game seconds between looks at the duties and the downed (the alarm's pace)
    float exposureRange = 45.0f;      // an enemy this close with a line of sight to a cell can shoot there
    float keepAway = 10.0f;           // and a cell this close to an enemy is never safe, seen or not (melee, a corner)
    float rallyBehind = 6.0f;         // medics and firefighters wait this far from home, away from the threat
    int rallySearch = 8;              // cells round that point searched for one out of sight
    float selfDefenseRadius = 8.0f;   // an enemy this close in sight makes a medic or firefighter fight
    float dragRadius = 15.0f;         // a fighter this close to a downed friend in the open may drag it to cover
    float dragEnemyRadius = 8.0f;     // not with an enemy this close to the friend
    int dragMaxExposure = 2;          // nor if more enemies than this see the friend or any point of the way
    int dragDistance = 12;            // the cover is at most this many cells from the friend
    float routeStep = 1.5f;           // metres between the points of a way that are checked
};

class COLONY_API FieldCare {
public:
    FieldCare(Things& things, Map& map, HealthSystem& health, JobSystem& jobs, FighterSystem& fighters, RaidSystem& raids,
              const DefDatabase& defs, const GameTime& time);
    FieldCare(const FieldCare&) = delete;
    FieldCare& operator=(const FieldCare&) = delete;

    const FieldCareRules& rules() const { return rules_; }

    // The work type (index in JobSystem::workTypes()) a colonist is on duty for now: the alarm is on, its role is
    // Medic (Doctor) or Firefighter, it isn't fighting, controlled or down. -1 if it isn't on duty.
    int dutyWorkType(Entity pawn) const;
    // Whether an on-duty colonist may take work on this target, whose cell is `cell`: the enemies can't see the cell,
    // none is within keepAway, no fighter is carrying the patient, and the patient isn't a fighter still on its feet.
    bool dutyAllows(Entity pawn, const JobTarget& target, CellPos cell);
    // Where an on-duty colonist waits: rallyBehind metres from home away from the threat, the nearest cell there out
    // of the enemies' sight that it can reach; home with no threat known; where it stands with no home.
    CellPos rallyPoint(Entity pawn);

    // How many able enemies of the colony see a pawn standing at (x, y) from within exposureRange.
    int exposure(float x, float y, int level);
    // exposure 0 and no enemy within keepAway.
    bool safe(CellPos cell);

    // Forgets what it worked out about the enemies this second. Sim calls it at the start of every step, after the
    // commands, so a look between steps (a test, Unreal) can't change what the step decides.
    void invalidate();

    // Runs after the raid system and before the fighters: on-duty colonists who are about to walk into sight drop
    // the job, those an enemy comes close to fight, and fighters near a downed friend in the open drag it to cover.
    void step();

    // Counters for tests and simrun: drags started, duty jobs given up because the target came under fire.
    uint64_t drags() const { return drags_; }
    uint64_t givenUp() const { return givenUp_; }

private:
    struct Hostile {
        Entity pawn;
        float x, y;
        int level;
    };
    void refreshHostiles();
    void checkDuty(Entity pawn);
    void considerDrags();
    bool dropCell(Entity patient, CellPos& out);
    bool routeSafeEnough(float ax, float ay, float bx, float by, int level);
    int64_t now() const;

    Things& things_;
    Map& map_;
    HealthSystem& health_;
    JobSystem& jobs_;
    FighterSystem& fighters_;
    RaidSystem& raids_;
    const GameTime& time_;
    FieldCareRules rules_;
    int doctorWork_ = -1;
    int firefighterWork_ = -1;
    std::vector<Hostile> hostiles_;  // cache: the colony's able enemies on the map, by id
    int64_t hostilesAt_ = -1;        // the game second it was made in
    uint64_t drags_ = 0;
    uint64_t givenUp_ = 0;
};

}  // namespace colony
