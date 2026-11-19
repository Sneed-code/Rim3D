#pragma once

#include "ColonySim/Api.h"

#include <string>
#include <utility>
#include <vector>

namespace colony {

class Def;
class DefDatabase;

// A humanlike pawn's genes (docs/design/genes.md): the genes of its lineage, with their stat
// effects added up. Derived from the lineage (PawnLooks::lineage), never saved: it is made again
// when a pawn is made, when its lineage changes and when a save is loaded.
//
// A gene's stat is an offset or a factor in its Genes def (`statOffsets`, `statFactors`); a stat
// is (base + sum of offsets) times the product of factors. `aptitudes` add to skill levels.

// The stats the sim reads each step, worked out once when the genes are made.
struct GeneEffects {
    float moveSpeed = 0.0f;           // metres per game second added to walking speed
    float comfyMinC = 0.0f;           // added to the comfortable range, degrees C
    float comfyMaxC = 0.0f;
    float foodRate = 1.0f;            // times hunger: the lineage's metabolism and the genes' own factor
    float moodOffset = 0.0f;          // added to mood
    float damageTaken = 1.0f;         // times the damage of every hit
    float woundHealing = 1.0f;        // times natural healing
    float miningSpeed = 1.0f;
    float plantWorkSpeed = 1.0f;

    // sneed's races (docs/design/races.md), all numbers in the Genes defs.
    float moveSpeedFactor = 1.0f;     // times walking speed (Impid 1.05)
    float learning = 1.0f;            // times the experience learned (baseline 1.2, Orcoid 0.9)
    float workSpeed = 1.0f;           // times every work speed (Orcoid 0.9)
    float meleeDamage = 1.0f;         // times its melee blows' damage (Orcoid 1.1)
    float rangedDamage = 1.0f;        // times its shots' damage (drow 1.1)
    float shootingAccuracy = 1.0f;    // times its aim (more is steadier)
    float healthScale = 1.0f;         // times its hit points and its blood (Orcoid 1.1)
    float thermalDamage = 1.0f;       // times heat and cold injury build-up and burn damage (Impid 0.1)
    float environmentDamage = 1.0f;   // times fall and collapse damage (cat-strain 0.8)
    float debuffFactor = 1.0f;        // times every penalty: pain, lost capacities, limb slows, bad moods, tiredness (cat-strain 1.33)
    float jumpHeight = 1.0f;          // times the jump's height, for Unreal (cat-strain 2)
    float climbSpeed = 1.0f;          // times wall-climbing speed (cat-strain 2)
    bool darkVision = false;          // no slower working or walking in the dark (darknessSlowdown 0)
    float darkWorkSpeed = 1.0f;       // times work where the light is under darkBelow (drow 1.1)
    float sunWorkSpeed = 1.0f;        // times work under open sky while the sun is up (drow 0.9)
    float sunShootingAccuracy = 1.0f; // times aim under open sky while the sun is up (drow 0.9)
    bool noOutdoorsNeed = false;      // never feels cooped up indoors, like an undergrounder (drow)
    bool nocturnal = false;           // joins with its schedule turned round: up at night, asleep by day (drow)
    float darkBelow = 0.3f;           // GeneRules: light under this is dark
    float sunAbove = 0.3f;            // GeneRules: the sun is up when its glow is over this
};

struct PawnGenes {
    std::vector<std::string> genes;                       // Genes defNames, in the lineage's order
    float metabolicEfficiency = 0.0f;                      // the lineage's, for the hunger curve
    GeneEffects effects;
    std::vector<std::pair<std::string, int>> aptitudes;    // Skills defName and levels added, summed
    std::vector<std::pair<std::string, float>> offsets;    // every statOffsets entry, summed by stat
    std::vector<std::pair<std::string, float>> factors;    // every statFactors entry, multiplied by stat
};

// Makes the genes of a lineage. Fails (and fills `out` as far as it got) with the reason in
// `error` for an unknown lineage or gene; a game without the Genes or Rules defs has no effects.
COLONY_API bool makeGenes(const DefDatabase& defs, const std::string& lineage, PawnGenes& out, std::string& error);

// Any stat a gene def names: (base + offsets) * factors. A stat no gene names gives `base`.
COLONY_API float geneStat(const PawnGenes& genes, const char* stat, float base);

// Skill levels the genes add (negative for poor and awful).
COLONY_API int geneAptitude(const PawnGenes& genes, const std::string& skill);

// The work speed factor of a pawn's genes where the light is `glow` (0 to 1), `sunlit` under open sky
// with the sun up: workSpeed, the dark bonus and the sunlight penalty. The light's own slowdown is
// the caller's (geneLightWork).
COLONY_API float geneWorkFactor(const GeneEffects& fx, float glow, bool sunlit);

// The light's own work and walk factor for a pawn with these genes: 1 with dark vision, else `factor`.
inline float geneLightFactor(const GeneEffects* fx, float factor) { return fx != nullptr && fx->darkVision ? 1.0f : factor; }

// A penalty made worse by the genes' debuff factor: a factor under 1 moves away from 1 by `debuff`
// (0.75 with 1.33 is 0.6675); factors of 1 or more are left alone.
inline float geneWorsen(float factor, float debuff) {
    return factor < 1.0f ? (1.0f - (1.0f - factor) * debuff > 0.0f ? 1.0f - (1.0f - factor) * debuff : 0.0f) : factor;
}

// The last gene of a lineage whose def has `path` (for example "visual.horns.length"), or null.
// How looks and Unreal find the colours and horns a lineage's genes give.
COLONY_API const Def* lineageGeneWith(const DefDatabase& defs, const Def& lineage, const char* path);

}  // namespace colony
