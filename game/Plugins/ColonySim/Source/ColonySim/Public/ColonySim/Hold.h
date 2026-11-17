#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"

#include <string>

namespace colony {

// How a weapon is held (docs/design/weapon-hold.md): the one system every weapon and shield goes through.
//
// A weapon def names a hold style and its grip points (`hold` in Things defs); a style (HoldStyles defs) says how many
// hands, which hand, how the hand closes on it (a blade's hilt, a gun's pistol grip, a bow's riser, a shield's straps),
// the melee attack style the sim plays swings from (MeleeRules.styleAttacks), and the clips Unreal plays for it: a
// third-person upper-body stance, aim, block and reload, and first-person ones. A new weapon only adds a `hold` block;
// a new kind of weapon adds one style. The sim reads only `attackStyle`; the rest is for drawing.

// A point in the weapon's own frame, cm: +x along the weapon (blade or barrel), +y right, +z up (a blade's edge, a gun's
// sights), as the weapon's `visual.parts` are drawn.
struct HoldPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Which hand holds the weapon.
enum class HoldHand : uint8_t { Right, Left };

// How the holding hand closes on it.
enum class HoldGrip : uint8_t {
    None,    // fists
    Blade,   // a hilt or haft across the fist (swords, clubs, maces, spears, axes)
    Gun,     // a pistol grip: the barrel along the hand, raked back
    Bow,     // a bow's riser in the fist, upright
    Shield,  // straps on the forearm
};

struct WeaponHold {
    DefId style;              // the HoldStyles def; invalid only without HoldStyles defs
    std::string styleName;    // its defName
    std::string attackStyle;  // MeleeRules.styleAttacks key (empty: the style swings nothing of its own)
    int hands = 1;            // 0 fists, 1 one hand, 2 two hands
    HoldHand hand = HoldHand::Right;
    HoldGrip grip = HoldGrip::Blade;
    HoldPoint main;           // where the holding hand's palm goes
    HoldPoint off;            // where the other hand's palm goes (two hands)
    bool hasOff = false;
    float rake = 0.0f;        // degrees a gun's grip leans back from upright
};

// The hold of a weapon def; `weapon` invalid (or not a weapon) gives the bare-hands style. A def without its own
// `hold` gets one by kind: guns by `twoHanded` (rifle) or not (pistol), melee weapons by `twoHanded` (two-handed blade)
// or not (one-handed blade); missing grip points default to the style's. False only if no style could be found.
COLONY_API bool holdFor(const DefDatabase& defs, DefId weapon, WeaponHold& out);

// The HoldStyles def of a shield (apparel with `shield`), so it is held through the same system.
COLONY_API bool shieldHoldFor(const DefDatabase& defs, DefId shield, WeaponHold& out);

COLONY_API const char* holdGripName(HoldGrip grip);

}  // namespace colony
