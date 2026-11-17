#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Cover.h"

#include <cstdint>

namespace colony {

class DefDatabase;

// Stealth: what a fighter has to see or hear before it acts. See docs/design/stealth.md.
//
// A fighter knows only the enemies it saw (in its sight cone, in range for the light and the enemy's stance, with a
// clear line), heard (a shot, a blow, a blast, footsteps close behind it) or was called to by a friend within
// `calloutRadius`. Its `Alert` state says how watchful it is; the code that looks and listens is in
// FighterSystem (Fighters.cpp `see`, Stealth.cpp for the rest).

// How watchful a fighter is. Saved as a number: don't reorder.
enum class Alert : uint8_t {
    Unaware,     // at ease: it sees only what is in front of it
    Suspicious,  // it noticed something (a shape far off, footsteps): it turns to look, and a second look confirms
    Alerted,     // it knows there is a threat: it looks all round and fights, flees or hides as its orders say
};

// The numbers (StealthRules in data/defs/Rules/StealthRules.json, all ours: RimWorld has no stealth). sneed's own
// numbers are the 20 m call and the 8 m melee noise.
struct StealthRules {
    float coneDegrees = 120.0f;         // the whole sight cone of a fighter that isn't alerted, centred on its facing
    float alertedConeDegrees = 360.0f;  // and of one that is: it keeps looking round
    float closeSense = 1.5f;            // metres: anyone this near is noticed whichever way the fighter faces
    float instantRange = 8.0f;          // an enemy seen this near alerts at once
    int noticeSteps = 30;               // seen further off: suspicious first, alerted if still in view this many steps later
    float darkSight = 0.3f;             // not alerted: share of the sight range at which a target in pitch dark is seen...
    float fullLight = 0.5f;             // ...rising to the whole range for a target in this much light or more
    float crouchSight = 0.6f;           // share of the range at which a crouched target is seen by a fighter not alerted
    float proneSight = 0.4f;            // and one lying down
    float calloutRadius = 20.0f;        // friends this near the one who saw or heard an enemy learn where it is (sneed)
    bool calledCanCall = false;         // a fighter alerted by a call doesn't call others in turn (sneed)
    int searchSeconds = 900;            // alerted with no enemy known for this long: back to suspicious (30 real seconds)
    int suspiciousSeconds = 600;        // suspicious with nothing new for this long: back to unaware (20 real seconds)
    int surpriseSteps = 30;             // a fighter caught unaware waits this much longer before its first shot or blow
    float walkNoise = 3.0f;             // metres at which the footsteps of an upright walking pawn are heard
    float runNoise = 7.0f;              // and of one running (`runSpeed` metres a real second or more)
    float runSpeed = 6.0f;
    float crouchNoise = 0.0f;           // a crouched pawn's steps
    float gunNoise = 90.0f;             // metres a gunshot is heard at, unless the gun's def has `noiseRadius`
    float meleeNoise = 8.0f;            // a melee blow (sneed), unless the weapon's def has `noiseRadius`
    float hitNoise = 8.0f;              // a bullet striking a pawn: friends this near know something is up (not where from)
    float explosionNoise = 150.0f;      // a blast
};

COLONY_API StealthRules stealthReadRules(const DefDatabase& defs);

// Whether a point `dx, dy` metres away is inside a cone of `degrees` (the whole angle) round the unit vector
// `faceX, faceY`. A cone of 360 or more holds everything; a point at the fighter itself is always inside.
COLONY_API bool stealthInCone(float faceX, float faceY, float dx, float dy, float degrees);

// The share of the sight range a target is seen at by a watcher that isn't alerted: by the light on it (0 to 1) and
// its stance. 1 for an alerted watcher.
COLONY_API float stealthSightShare(const StealthRules& rules, float light, Stance targetStance, bool watcherAlerted);

// How far a pawn's footsteps carry, metres: `speed` in metres a step, by its stance. 0 when it stands still or lies.
COLONY_API float stealthStepNoise(const StealthRules& rules, float speed, Stance stance);

}  // namespace colony
