#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

// A colonist's character, made from a seed. Defs are named by defName, never DefId, so the
// component saves as it is. See docs/design/colonists.md.

// The highest a skill can ever be: maxLevel 20 plus skill traits' bonus levels, with room to spare.
constexpr int kSkillLevelLimit = 30;

enum class Passion : uint8_t { None, Minor, Major };

struct SkillRecord {
    std::string def;  // Skills def
    int level = 0;    // 0 to SkillRules.maxLevel
    Passion passion = Passion::None;
    float xp = 0.0f;  // experience towards the next level
    bool disabled = false;  // a backstory or trait disables the work this skill is for
    float xpToday = 0.0f;   // experience earned since midnight; the daily soft cap counts it (Skills.h)
    int bonus = 0;  // levels the traits added (trait points): a positive bonus lets the skill pass maxLevel by that much
};

struct TraitRecord {
    std::string def;  // Traits def
    int degree = 0;   // which of its degreeDatas
};

struct Colonist {
    static constexpr std::size_t kMaxWorkGroupLength = 24;
    std::string firstName;
    std::string nickName;
    std::string lastName;
    bool female = false;
    int age = 18;  // years
    std::string childhood;  // Backstories defs
    std::string adulthood;
    std::vector<TraitRecord> traits;
    std::vector<SkillRecord> skills;           // in Skills `listOrder`
    std::vector<std::string> disabledWorkTags;  // sorted, no repeats
    bool protagonist = false;                   // gets 2x part hit points (F1) and the talent tree later
    std::string workGroup;                      // the player's work group for the Work tab (SetWorkGroupCommand), or empty

    const SkillRecord* skill(const std::string& def) const {
        for (const SkillRecord& s : skills) {
            if (s.def == def) {
                return &s;
            }
        }
        return nullptr;
    }
};

// How a colonist is made. traitPoints -1 is RimWorld's way (two backstories, 1 to 3 random traits);
// 0 or more is ours (sneed 2026-11-21, docs/design/new-game.md): no backstories, traits bought from a
// pool of that many points (a trait's `points`; negative traits give points), all spent, and skill
// traits that can lift a skill past maxLevel. `age` 0 draws one; `noTraits` (the protagonist) has none.
struct ColonistGenOptions {
    std::string category = "Colony";
    int traitPoints = -1;
    int age = 0;
    bool noTraits = false;
};

// Makes one adult colonist from `seed` alone: the same seed and defs give the same colonist, and
// nothing else in the sim is touched. Needs the defs of kinds Backstories, Traits, Skills, Names
// and Rules; returns false and fills `error` if one is missing. `category` picks the backstories
// that fit the kind of person ("Colony" for settlers, "Raider" for pirates): a backstory with a
// `categories` list is only used for the categories it names, one without fits all.
COLONY_API bool generateColonist(const DefDatabase& defs, uint64_t seed, bool protagonist, Colonist& out,
                                 std::string& error, const std::string& category = "Colony");
COLONY_API bool generateColonist(const DefDatabase& defs, uint64_t seed, bool protagonist, Colonist& out,
                                 std::string& error, const ColonistGenOptions& options);

// Trait points (ours): what one trait costs (its degree's `points`; negative gives points), and
// what a set costs in all.
COLONY_API int traitPointCost(const DefDatabase& defs, const TraitRecord& trait);
COLONY_API int traitPointsSpent(const DefDatabase& defs, const std::vector<TraitRecord>& traits);

// The most negative traits one person may take to earn points, and the most traits in all.
constexpr int kMaxNegativeTraits = 3;
constexpr int kMaxTraits = 8;

// Checks a set of traits for a pool of `pool` points: known traits and degrees, no repeats or
// conflicts, at most kMaxNegativeTraits negative ones and kMaxTraits in all, and no more points
// spent than the pool. Fills `error` and returns false if not.
COLONY_API bool traitSetFits(const DefDatabase& defs, const std::vector<TraitRecord>& traits, int pool, std::string& error);

// Gives `colonist` these traits in place of theirs: skills keep their base level and take the new
// traits' gains (SkillRecord::bonus), disabled work comes from the traits, and passions on skills that
// are now disabled or forbidden go. For point-mode colonists (no backstories).
COLONY_API void setColonistTraits(const DefDatabase& defs, Colonist& colonist, const std::vector<TraitRecord>& traits);

// Random traits bought from `pool` points, all spent, as generateColonist buys them, from `seed`.
COLONY_API std::vector<TraitRecord> randomTraitSet(const DefDatabase& defs, uint64_t seed, int pool);

// Redraws `colonist`'s nickname and first name (from the Names banks, by `seed`) until neither
// matches anyone in `taken`, so a colony never has two Chips. Names that don't clash are kept, so
// games without a clash are unchanged. Gives up after a fixed number of draws (more people than
// names). Needs the same Names defs as generateColonist.
COLONY_API void makeColonistNameUnique(const DefDatabase& defs, uint64_t seed, Colonist& colonist,
                                       const std::vector<const Colonist*>& taken);

// A first name from the Names banks by `seed` (empty without them): for pawns that have no Colonist record.
COLONY_API std::string randomFirstName(const DefDatabase& defs, uint64_t seed, bool female);

}  // namespace colony
