#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace colony {

class Filth;
class Floors;
class Map;
class StateHasher;
class Things;
struct Event;

// Grime: dirt and blood on bodies, their clothes and their melee weapons (docs/design/grime.md; sneed 2026-11-16:
// Baldur's Gate 3 blood that lingers after a fight until rain or water washes it off, dirt too, each creature's own
// fluid). The ground's stains are the Filth layer's; this is what sticks to pawns. All levels are 0 (clean) to 1.
//
//   - Walking on bare soil, sand or rough stone dusts the legs (up to `soilMax`), mud and marsh cake them;
//     digging, planting, cutting and building dirty the whole body slowly; lying or crawling on bare ground
//     (downed, dead) quickly.
//   - A wound that bleeds wets the body with the body's own fluid (Filth::bloodKindOf); a melee blow that draws
//     it spatters the attacker and bloodies the weapon.
//   - Rain washes whoever stands under the open sky; water washes what is under it (feet in ankle-deep water,
//     everything when swimming); everything wears off by itself over a few days.
//
// Unreal draws the levels as a dirt and a blood layer over skin and clothes, the blood drying from the fluid's wet to
// dry colour by its age (Filth def `look`). Cosmetic for the game's rules: nothing else reads it. The numbers are
// the GrimeRules def's (ours: RimWorld has no grime on bodies).

struct COLONY_API GrimeNumbers {
    float soilFullSeconds = 7200.0f;   // walking on bare soil this long (game seconds) takes legs from clean to soilMax
    float soilMax = 0.5f;
    float mudFullSeconds = 600.0f;     // mud and marsh: clean to 1
    float workFullSeconds = 10800.0f;  // digging, planting, cutting, building: body and legs clean to workMax
    float workMax = 0.7f;
    float groundFullSeconds = 1200.0f; // lying or crawling on bare ground: clean to 1
    float bloodPerDamage = 0.012f;      // a bleeding wound: body blood per point of damage
    float spatterPerDamage = 0.005f;   // the melee attacker who drew it
    float weaponPerDamage = 0.03f;     // its melee weapon
    float rainWashSeconds = 9000.0f;   // full rain (rainRate 1) under the open sky washes everything off in this long
    float ankleWashSeconds = 600.0f;   // feet in water: the legs
    float waistWashSeconds = 900.0f;   // waist or chest deep: the legs at once and the body at this pace
    float swimWashSeconds = 600.0f;    // swimming: everything, the weapon too
    float wearOffSeconds = 172800.0f;  // without water blood wears off in this long (two game days)
    float dirtFadeSeconds = 18000.0f;  // dirt fades from 1 to clean in this long while nothing dirties it (10 real min)

    static GrimeNumbers read(const DefDatabase& defs);
};

struct PawnGrime {
    float dirtLegs = 0.0f;
    float dirtBody = 0.0f;
    float blood = 0.0f;
    float weaponBlood = 0.0f;
    int bloodKind = -1;   // Filth kind index of the blood on the body (the last fresh blood); -1 none
    int weaponKind = -1;  // on the weapon
    int64_t bloodAt = 0;  // game second of the last fresh blood on the body (it dries from then)
    int64_t weaponAt = 0;
    // Where it stood at the last step, to measure how far it walked.
    float lastX = 0.0f;
    float lastY = 0.0f;
    int lastLevel = -1;  // -1: not seen yet

    bool clean() const { return dirtLegs <= 0.0f && dirtBody <= 0.0f && blood <= 0.0f && weaponBlood <= 0.0f; }
};

// Everything saved (docs/design/saves.md, section "grime").
struct GrimeState {
    struct Entry {
        uint32_t pawn = 0;
        float dirtLegs = 0.0f;
        float dirtBody = 0.0f;
        float blood = 0.0f;
        float weaponBlood = 0.0f;
        std::string bloodKind;   // Filth defName, "" none
        std::string weaponKind;
        int64_t bloodAt = 0;
        int64_t weaponAt = 0;
        float lastX = 0.0f;
        float lastY = 0.0f;
        int lastLevel = -1;
    };
    std::vector<Entry> pawns;
};

class COLONY_API Grime {
public:
    // How tall a pawn is, for how deep water is on it; unset: everyone is a man's height.
    using BodyHeight = std::function<float(Entity)>;

    Grime(const Map& map, Things& things, const Floors& floors, const Filth& filth, const DefDatabase& defs);

    const GrimeNumbers& numbers() const { return numbers_; }
    void setBodyHeight(BodyHeight fn) { bodyHeight_ = std::move(fn); }

    // One game second at `now`: the step's events (injuries, melee hits) and the weather's rain rate (0 to about 1.5).
    void step(int64_t now, const std::vector<Event>& events, float rainRate);

    // The pawn's grime, or null if it has none recorded.
    const PawnGrime* of(Entity pawn) const;
    // Sets a pawn's levels outright (dev tools and tests); the blood counts as fresh at `now`.
    void set(Entity pawn, float dirtLegs, float dirtBody, float blood, int bloodKind, int64_t now);
    const std::map<uint32_t, PawnGrime>& all() const { return pawns_; }

    void hashState(StateHasher& h) const;
    GrimeState state() const;
    // Loading a save; false, with `error`, on a Filth kind that does not exist.
    bool restore(const GrimeState& state, std::string& error);

private:
    enum class GroundKind : uint8_t { Clean, Soil, Mud };
    GroundKind groundAt(float x, float y, int level) const;
    void addBlood(PawnGrime& g, float amount, int kind, int64_t now);
    void addWeaponBlood(PawnGrime& g, float amount, int kind, int64_t now);

    const Map& map_;
    Things& things_;
    const Floors& floors_;
    const Filth& filth_;
    const DefDatabase& defs_;
    GrimeNumbers numbers_;
    BodyHeight bodyHeight_;
    std::vector<uint8_t> bleeds_;  // per def id: a Hediffs def whose injuryProps.bleedRate is above 0
    std::map<uint32_t, PawnGrime> pawns_;  // by entity id
};

}  // namespace colony
