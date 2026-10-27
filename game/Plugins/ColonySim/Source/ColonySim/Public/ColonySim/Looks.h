#pragma once

#include "ColonySim/Api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class DefDatabase;
class Rng;

// How a humanlike pawn looks, made from a seed. Only Unreal uses it (docs/design/looks.md): no
// sim rule reads it, so changing the looks never changes the game. The lineage is stored by
// defName, so the component saves as it is.

// RimWorld's body types. Average is RimWorld's Male or Female body, by the pawn's gender.
enum class BodyType : uint8_t { Thin, Average, Fat, Hulk };

struct LooksColor {
    float r = 0.0f;  // 0 to 1, sRGB
    float g = 0.0f;
    float b = 0.0f;
};

// Where on the skin a tattoo or scar sits (V1). Hands count as arms, feet as legs; eyes, ears,
// nose, jaw and skull as the face.
enum class BodyRegion : uint8_t { Face, Neck, Torso, LeftArm, RightArm, LeftLeg, RightLeg };
constexpr int kBodyRegionCount = 7;

// One tattoo or scar. `style` indexes the LooksRules list (`tattoos.styles` or `scars.styles`);
// u and v (0 to 1) place it on the region's skin, size is a factor on its usual size.
struct LooksMark {
    BodyRegion region = BodyRegion::Torso;
    uint8_t style = 0;
    float u = 0.5f;
    float v = 0.5f;
    float size = 1.0f;
};

struct PawnLooks {
    std::string lineage = "Baseline";  // Lineages def
    bool female = false;
    BodyType body = BodyType::Average;
    float weight = 0.0f;   // -1 (thin body) to 1 (heavy body): the body shape blend
    float height = 1.0f;   // times the height of the average man (1.80 m)
    float melanin = 0.0f;  // 0 light to 1 dark: the skin before the lineage tints it
    LooksColor naturalHair;  // the hair colour drawn from the seed
    LooksColor skin;         // what is drawn: melanin and lineage together
    LooksColor hair;         // what is drawn: the natural colour, or the lineage's

    // V1: the face, hair style and skin marks. All from the seed (its own random stream, so the
    // fields above stay what A3 made); a pawn from an older save has an empty `hairStyle` and the
    // defaults. The face numbers are 0 to 1 sliders (0.5 is the average face) that Unreal turns
    // into face morph weights.
    float faceShape = 0.5f;  // narrow and long (0) to wide and round (1)
    float brow = 0.5f;       // light (0) to heavy (1) brow ridge
    float nose = 0.5f;       // small and upturned (0) to large and long (1)
    float jaw = 0.5f;        // soft and narrow (0) to square and wide (1)
    LooksColor eyes;
    std::string hairStyle;     // LooksRules hairStyles name
    std::string naturalBeard;  // LooksRules beardStyles name ("None" for women); what the seed gave
    std::string beard;         // what is drawn: the seed's, or none if a gene says beardless
    float naturalBodyHair = 0.0f;  // 0 none to 1 thick
    float bodyHair = 0.0f;         // what is drawn: the seed's, or the lineage's fur
    std::vector<LooksMark> tattoos;  // from the seed
    std::vector<LooksMark> scars;    // from healed injuries, one per Scar or OldWound the pawn got
};

// Makes the looks of a pawn of this gender and lineage from `seed` alone, with its own random
// stream. Needs the Rules def LooksRules and the lineage's Lineages def; without them it fills in
// plain defaults (an average body, mid skin) and returns false with the reason in `error`.
COLONY_API bool generateLooks(const DefDatabase& defs, uint64_t seed, bool female, const std::string& lineage,
                              PawnLooks& out, std::string& error);

// Gives the looks another lineage: the skin and hair are worked out again from the melanin and
// the new lineage's colours; the body stays. Fails (and changes nothing) for an unknown lineage.
COLONY_API bool setLineage(const DefDatabase& defs, const std::string& lineage, PawnLooks& looks, std::string& error);

// Puts a mark on the pawn's skin for a scar the health system just left on `region`, with the
// style, spot and size rolled from `rng`. `hediff` is the healed injury's defName: a style that
// lists hediffs is only used for those. Does nothing past the LooksRules cap on scars.
COLONY_API void addScarMark(const DefDatabase& defs, PawnLooks& looks, BodyRegion region, const std::string& hediff, Rng& rng);

// The skin region of a body part def (Arm, Toe, Eye...). `left` picks the side for limbs. False for
// parts that are not on the skin (organs, bones).
COLONY_API bool bodyRegionOfPart(const std::string& partDef, bool left, BodyRegion& out);

COLONY_API const char* bodyTypeName(BodyType body);

}  // namespace colony
