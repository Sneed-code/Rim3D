#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"

#include <cstdint>

namespace colony {

// What a pawn's wounds look like, for Unreal to draw on the skin (task V7, docs/design/wounds.md). The sim
// derives them from the health of each pawn when it writes a snapshot; they are never saved or hashed.

// How a wound is drawn. Every injury Hediffs def names one in `woundKind`.
enum class WoundKind : uint8_t {
    Cut,        // cuts, stabs, scratches: a split with blood
    Bruise,     // bruises, cracks, crushing: a dark patch
    Burn,       // burns and frostbite: blackened, blistered skin
    Bite,       // bites and shredding: torn flesh
    Gunshot,    // a small hole
    Infection,  // a wound gone septic: swollen and yellow-green
    Scar,       // a healed scar or old wound
    Missing     // a lost part: a stump (`part` is the part that is gone)
};

// The coarse piece of skin a body part is on; the part's BodyParts def says which (`skinRegion`) and its label or
// its parents' say the side. Only outside parts have wounds that show.
enum class BodyRegion : uint8_t { Torso, Head, LeftArm, RightArm, LeftLeg, RightLeg };

// One wound or lost part on a humanlike (or any) pawn, Snapshot::wounds[ThingView::firstWound + n]. Wounds on organs
// and bones inside the body are left out: the outside part over them has its own.
struct WoundView {
    uint16_t part = 0;          // position in the pawn's Bodies def, depth-first, torso 0
    DefId partDef;              // the BodyParts def of that part: Arm, Hand, Eye...
    BodyRegion region = BodyRegion::Torso;
    WoundKind kind = WoundKind::Cut;
    float severity = 0.0f;      // share of the part's hit points the wound took, 0 to 1 (1 for a lost part)
    bool tended = false;        // wrapped in a bandage
    float tendQuality = -1.0f;  // below 0 while untended
    bool bleeding = false;      // untended and bleeding: wet, running blood
};

COLONY_API const char* woundKindName(WoundKind kind);
COLONY_API const char* bodyRegionName(BodyRegion region);

}  // namespace colony
