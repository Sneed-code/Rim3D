#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

// A colonist's character, made from a seed. Defs are named by defName, never DefId, so the
// component saves as it is. See docs/design/colonists.md.

enum class Passion : uint8_t { None, Minor, Major };

struct SkillRecord {
    std::string def;  // Skills def
    int level = 0;    // 0 to SkillRules.maxLevel
    Passion passion = Passion::None;
    float xp = 0.0f;  // experience towards the next level
    bool disabled = false;  // a backstory or trait disables the work this skill is for
    float xpToday = 0.0f;   // experience earned since midnight; the daily soft cap counts it (Skills.h)
};

struct TraitRecord {
    std::string def;  // Traits def
    int degree = 0;   // which of its degreeDatas
};

struct Colonist {
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

    const SkillRecord* skill(const std::string& def) const {
        for (const SkillRecord& s : skills) {
            if (s.def == def) {
                return &s;
            }
        }
        return nullptr;
    }
};

// Makes one adult colonist from `seed` alone: the same seed and defs give the same colonist, and
// nothing else in the sim is touched. Needs the defs of kinds Backstories, Traits, Skills, Names
// and Rules; returns false and fills `error` if one is missing. `category` picks the backstories
// that fit the kind of person ("Colony" for settlers, "Raider" for pirates): a backstory with a
// `categories` list is only used for the categories it names, one without fits all.
COLONY_API bool generateColonist(const DefDatabase& defs, uint64_t seed, bool protagonist, Colonist& out,
                                 std::string& error, const std::string& category = "Colony");

}  // namespace colony
