#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class Rooms;
class Things;

// Recreation, comfort and the outdoors need (task C18). See docs/design/recreation.md.
//
// Unreal does not include this header: it reads each pawn's joy, comfort and outdoors from the
// snapshot, and the thoughts through `Sim::mood().thoughts(pawn)`.

// How used a pawn is to one kind of recreation (RecreationKinds def, by defName): it makes that
// kind give less joy, and wears off.
struct JoyTolerance {
    std::string kind;
    float amount = 0.0f;
};

// A colonist's recreation, comfort and outdoors needs, each 0 (empty) to 1 (full). Only pawns with
// needs have one, and only when the defs have JoyRules (made when the pawn first steps).
struct Joy {
    float level = 0.6f;     // the Recreation need
    float comfort = 0.5f;   // the Comfort need
    float outdoors = 0.6f;  // the Outdoors need
    std::vector<JoyTolerance> tolerance;  // sorted by kind, only kinds above 0

    float toleranceOf(const std::string& kind) const;
    void addTolerance(const std::string& kind, float amount, float max);
};

// One kind of recreation: what using it does to the pawn's tolerance.
struct RecreationKind {
    std::string defName;
    float gainPerUse = 0.05f;    // tolerance added when a pawn finishes an activity of this kind
    float decayPerDay = 0.1f;    // tolerance lost a day
};

// One thing to do in Joy hours (RecreationActivities def).
struct RecreationActivity {
    DefId def;
    std::string defName;
    int kind = -1;                // index in RecreationRules::kinds
    std::string building;         // the Things def it is done at, or empty
    std::string buildingFlag;     // or any building with this `building.<flag>` (isTable), or empty
    int usersMin = 1;
    int usersMax = 1;
    float joyPerHour = 0.3f;
    double hoursMin = 0.5;
    double hoursMax = 1.0;
    bool outdoors = false;        // done on open ground, no building
    bool daytimeOnly = false;
    bool walks = false;           // the pawn walks about while it does it (WalkNearby)
    bool lies = false;            // the pawn lies on the ground (WatchClouds)
};

// The numbers from JoyRules, RecreationRules, ComfortRules and OutdoorsRules (data/defs/Rules), the
// recreation kinds and activities. `present` is false without JoyRules: pawns then get no joy.
struct COLONY_API RecreationRules {
    bool present = false;

    // JoyRules
    float joyFallPerDay = 0.8f;       // awake
    float toleranceMinGainFactor = 0.1f;
    float toleranceMax = 1.0f;
    int joyFromHour = 19;             // the standard schedule's Joy hours, from through to (inclusive)
    int joyToHour = 21;

    // RecreationRules (ours): when a pawn looks for something to do
    float startJoy = 0.6f;
    float seekBelow = 0.35f;          // in an Anything hour, with no work to do
    float joyHoursUntil = 0.95f;      // in a Joy hour it plays until it is this full
    int dayFromHour = 6;              // "daytimeOnly" activities: from this hour to before `dayToHour`
    int dayToHour = 19;
    float walkRadius = 18.0f;         // how far from itself a pawn walks for a walk, metres
    float spotRadius = 40.0f;         // how far it looks for a place to play, metres

    // ComfortRules
    float comfortStart = 0.5f;
    float comfortFallPerDay = 0.6f;   // when not on furniture
    float comfortRisePerHour = 0.5f;  // moves towards the furniture's comfort at most this fast
    float groundComfort = 0.1f;       // sleeping on the ground
    float defaultBedComfort = 0.6f;   // a bed whose def has no `stats.comfort`

    // OutdoorsRules
    float outdoorsStart = 0.6f;
    float outdoorsFallPerDay = 0.5f;  // indoors
    float outdoorsRisePerDay = 3.0f;  // outdoors

    std::vector<RecreationKind> kinds;
    std::vector<RecreationActivity> activities;

    static RecreationRules read(const DefDatabase& defs);
    int kindIndex(const std::string& defName) const;
    // Joy an hour of this activity gives `joy`'s owner, with tolerance counted (not the building's factor).
    float gainPerHour(const RecreationActivity& activity, const Joy& joy) const;
};

// Keeps every colonist's joy, comfort and outdoors needs once per game second, after the jobs.
// Joy rises inside the recreation job itself (Jobs.h); this system makes the components, lets joy
// fall, tolerance wear off, comfort follow the furniture the pawn sits or lies on and the
// outdoors need follow where the pawn is.
class COLONY_API RecreationSystem {
public:
    RecreationSystem(Things& things, const DefDatabase& defs);

    // Whether a cell is indoors decides the outdoors need; without rooms everywhere is outdoors.
    void setRooms(Rooms* rooms) { rooms_ = rooms; }

    const RecreationRules& rules() const { return rules_; }

    void step();

private:
    // The comfort of what the pawn sits or lies on now, or -1 if it uses no furniture.
    float comfortInUse(Entity pawn) const;

    Things& things_;
    Rooms* rooms_ = nullptr;
    const DefDatabase& defs_;
    RecreationRules rules_;
};

}  // namespace colony
