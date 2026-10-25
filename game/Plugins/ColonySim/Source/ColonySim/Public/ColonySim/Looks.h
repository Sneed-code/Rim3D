#pragma once

#include "ColonySim/Api.h"

#include <cstdint>
#include <string>

namespace colony {

class DefDatabase;

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
};

// Makes the looks of a pawn of this gender and lineage from `seed` alone, with its own random
// stream. Needs the Rules def LooksRules and the lineage's Lineages def; without them it fills in
// plain defaults (an average body, mid skin) and returns false with the reason in `error`.
COLONY_API bool generateLooks(const DefDatabase& defs, uint64_t seed, bool female, const std::string& lineage,
                              PawnLooks& out, std::string& error);

// Gives the looks another lineage: the skin and hair are worked out again from the melanin and
// the new lineage's colours; the body stays. Fails (and changes nothing) for an unknown lineage.
COLONY_API bool setLineage(const DefDatabase& defs, const std::string& lineage, PawnLooks& looks, std::string& error);

COLONY_API const char* bodyTypeName(BodyType body);

}  // namespace colony
