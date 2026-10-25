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

// The last gene of a lineage whose def has `path` (for example "visual.horns.length"), or null.
// How looks and Unreal find the colours and horns a lineage's genes give.
COLONY_API const Def* lineageGeneWith(const DefDatabase& defs, const Def& lineage, const char* path);

}  // namespace colony
