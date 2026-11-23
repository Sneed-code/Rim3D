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
    Pole,    // a long shaft in both fists: it runs from the holding hand through the other (spears, sneed 2026-11-18)
};

// Where a weapon is held up (docs/design/weapon-hold.md, "Weapon first, then the hands"): the weapon goes to this place
// on the body, and both hands close on its grip points (never the other way round, which pointed weapons wherever a
// clip's wrist happened to be).
enum class HoldAnchor : uint8_t {
    Chest,  // between the shoulder joints, in the body's facing (x ahead, y right, z up)
    Head,   // the head bone, in the body's facing turned by the aim's pitch
    View,   // the first-person camera
    // The right shoulder joint, in the body's facing turned by the aim (sneed 2026-11-23): a long gun's butt in the
    // shoulder's pocket, pointed at the crosshair from there, so the stock never goes into the face or the head.
    Shoulder,
};

// Which point of the weapon goes to the place.
enum class HoldPlacePoint : uint8_t {
    Main,   // the holding hand's grip
    Sight,  // a gun's rear sight (the eye looks along it)
    Stock,  // a gun's butt (the shoulder's pocket)
};

struct HoldPlace {
    bool set = false;
    HoldAnchor anchor = HoldAnchor::Chest;
    HoldPlacePoint point = HoldPlacePoint::Main;
    HoldPoint at;         // cm from the anchor (x ahead, y right, z up), for a 1.80 m body
    float pitch = 0.0f;   // degrees the weapon points up (a gun's aim adds its own pitch)
    float yaw = 0.0f;     // degrees it points to the right
    float roll = 0.0f;    // degrees it rolls to the right about its own length
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
    // The other hand's fist on `off` (or `support`) turned from the weapon's frame (pitch, yaw, roll degrees): a rifle's
    // fore-end lies across the support hand's palm, a two-handed hilt in both fists alike.
    float offPitch = 0.0f;
    float offYaw = 0.0f;
    float offRoll = 0.0f;
    // A one-handed gun's support hand (both hands on a pistol, drawing only: the sim still counts it one-handed, so a
    // shield on the left arm takes that hand).
    HoldPoint support;
    bool hasSupport = false;
    // Where the weapon is held (the style's `places`, a weapon's own `hold.places` over them): third person ready
    // (drafted, in a fight, the player's own), aimed (guns), blocking (melee); first person the same with `fp`.
    HoldPlace ready, aim, block, fpReady, fpAim, fpBlock;
};

// The hold of a weapon def; `weapon` invalid (or not a weapon) gives the bare-hands style. A def without its own
// `hold` gets one by kind: guns by `twoHanded` (rifle) or not (pistol), melee weapons by `twoHanded` (two-handed blade)
// or not (one-handed blade); missing grip points default to the style's. False only if no style could be found.
COLONY_API bool holdFor(const DefDatabase& defs, DefId weapon, WeaponHold& out);

// The HoldStyles def of a shield (apparel with `shield`), so it is held through the same system.
COLONY_API bool shieldHoldFor(const DefDatabase& defs, DefId shield, WeaponHold& out);

COLONY_API const char* holdGripName(HoldGrip grip);

// The place for a hold's state: `aimed` (guns) or `blocking` (melee) over ready, in first person or third; falls back
// to ready, and from first person to third. Null if the style gives none (the weapon then follows the hand).
COLONY_API const HoldPlace* holdPlaceFor(const WeaponHold& hold, bool firstPerson, bool aimed, bool blocking);

}  // namespace colony
