#include "ColonySim/Colonist.h"

#include "ColonySim/Random.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace colony {

namespace {

// A piecewise-linear curve from a Rules def: [[x, y], ...].
struct ColonistCurve {
    std::vector<std::pair<double, double>> points;

    double at(double x) const {
        if (points.empty() || x < points.front().first || x > points.back().first) {
            return 0.0;
        }
        for (size_t i = 1; i < points.size(); ++i) {
            if (x <= points[i].first) {
                const double span = points[i].first - points[i - 1].first;
                const double t = span > 0.0 ? (x - points[i - 1].first) / span : 1.0;
                return points[i - 1].second + (points[i].second - points[i - 1].second) * t;
            }
        }
        return 0.0;
    }
};

// Draws from a curve's weights over equal steps of x between lo and hi. Integer weights, so the
// draw is the same on every compiler.
double colonistDraw(const ColonistCurve& curve, double lo, double hi, int steps, Rng& rng) {
    std::vector<uint32_t> cumulative;
    uint32_t total = 0;
    for (int i = 0; i < steps; ++i) {
        const double x = lo + (hi - lo) * (i + 0.5) / steps;
        total += static_cast<uint32_t>(std::llround(curve.at(x) * 1000.0));
        cumulative.push_back(total);
    }
    if (total == 0) {
        return lo;
    }
    const uint32_t pick = rng.below(total);
    const size_t i = static_cast<size_t>(std::upper_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin());
    return lo + (hi - lo) * (static_cast<double>(i) + rng.unit()) / steps;
}

const Def* colonistFind(const DefDatabase& defs, const char* kind, const char* name, std::string& error) {
    const Def* def = defs.find(kind, name);
    if (def == nullptr) {
        error = std::string("colonists need the ") + kind + " def '" + name + "'";
    }
    return def;
}

// Reads the two-number curve points of a Rules field like "ageGenerationCurve".
ColonistCurve colonistCurveField(const Def& rules, const char* field) {
    ColonistCurve curve;
    const size_t n = rules.count(field);
    for (size_t i = 0; i < n; ++i) {
        const std::string x = std::string(field) + "." + std::to_string(i) + ".0";
        const std::string y = std::string(field) + "." + std::to_string(i) + ".1";
        curve.points.emplace_back(rules.number(x.c_str()), rules.number(y.c_str()));
    }
    return curve;
}

bool colonistListHas(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

std::vector<std::string> colonistTextList(const Def& def, const std::string& path) {
    std::vector<std::string> out;
    const size_t n = def.count(path.c_str());
    for (size_t i = 0; i < n; ++i) {
        out.push_back(def.textAt(path.c_str(), i));
    }
    return out;
}

std::string colonistName(const Def& bank, Rng& rng) {
    const size_t n = bank.count("names");
    return n == 0 ? std::string() : bank.textAt("names", rng.below(static_cast<uint32_t>(n)));
}

// A backstory with no `categories` fits every kind of person; otherwise it must list `category`.
const Def* colonistPickBackstory(const DefDatabase& defs, const char* slot, const std::string& category, Rng& rng) {
    std::vector<const Def*> pool;
    for (const DefId id : defs.idsOfKind("Backstories")) {
        const Def& def = defs.get(id);
        if (def.text("slot") == slot && (def.count("categories") == 0 || colonistListHas(colonistTextList(def, "categories"), category))) {
            pool.push_back(&def);
        }
    }
    return pool.empty() ? nullptr : pool[rng.below(static_cast<uint32_t>(pool.size()))];
}

}  // namespace

bool generateColonist(const DefDatabase& defs, uint64_t seed, bool protagonist, Colonist& out, std::string& error,
                      const std::string& category) {
    out = Colonist{};
    out.protagonist = protagonist;
    Rng rng(splitMix64(seed ^ 0x636F6C6F6E697374ULL), 3);

    const Def* genRules = colonistFind(defs, "Rules", "PawnGenRules", error);
    const Def* skillRules = colonistFind(defs, "Rules", "SkillRules", error);
    const Def* males = genRules ? colonistFind(defs, "Names", "FirstNamesMale", error) : nullptr;
    const Def* females = males ? colonistFind(defs, "Names", "FirstNamesFemale", error) : nullptr;
    const Def* lasts = females ? colonistFind(defs, "Names", "LastNames", error) : nullptr;
    const Def* nicks = lasts ? colonistFind(defs, "Names", "Nicknames", error) : nullptr;
    if (genRules == nullptr || skillRules == nullptr || nicks == nullptr) {
        return false;
    }

    // Who: gender, name, age.
    out.female = rng.below(2) == 1;
    out.firstName = colonistName(out.female ? *females : *males, rng);
    out.lastName = colonistName(*lasts, rng);
    out.nickName = colonistName(*nicks, rng);
    const int adultAge = static_cast<int>(genRules->integer("adultAgeMin", 18));
    const ColonistCurve ageCurve = colonistCurveField(*genRules, "ageGenerationCurve");
    const double maxAge = ageCurve.points.empty() ? adultAge + 1.0 : ageCurve.points.back().first;
    out.age = std::max(adultAge, static_cast<int>(colonistDraw(ageCurve, adultAge, std::max<double>(maxAge, adultAge + 1), 64, rng)));

    // Backstories.
    const Def* childhood = colonistPickBackstory(defs, "Childhood", category, rng);
    const Def* adulthood = colonistPickBackstory(defs, "Adulthood", category, rng);
    if (childhood == nullptr || adulthood == nullptr) {
        error = "colonists need Backstories defs for the Childhood and Adulthood slots (category " + category + ")";
        return false;
    }
    out.childhood = childhood->defName();
    out.adulthood = adulthood->defName();
    std::vector<std::string> disabled;
    for (const Def* b : {childhood, adulthood}) {
        for (const std::string& tag : colonistTextList(*b, "workDisables")) {
            if (!colonistListHas(disabled, tag)) {
                disabled.push_back(tag);
            }
        }
    }

    // Traits: a backstory may force one (by chance) or forbid some; the rest are weighted by
    // commonality, one degree each, never two that conflict, never a repeat, and never one that
    // needs work the backstories disabled.
    const int traitMin = static_cast<int>(genRules->numberAt("traitCount", 0, 1));
    const int traitMax = static_cast<int>(genRules->numberAt("traitCount", 1, 3));
    const int wanted = traitMin + static_cast<int>(rng.below(static_cast<uint32_t>(std::max(1, traitMax - traitMin + 1))));
    const std::vector<DefId> traitIds = defs.idsOfKind("Traits");
    std::vector<std::string> forbiddenTraits;
    for (const Def* b : {childhood, adulthood}) {
        for (const std::string& name : colonistTextList(*b, "disallowedTraits")) {
            forbiddenTraits.push_back(name);
        }
    }
    std::vector<const Def*> chosen;
    const auto allowed = [&](const Def& trait) {
        if (colonistListHas(forbiddenTraits, trait.defName())) {
            return false;
        }
        for (const Def* other : chosen) {
            if (other == &trait || colonistListHas(colonistTextList(trait, "conflictingTraits"), other->defName()) ||
                colonistListHas(colonistTextList(*other, "conflictingTraits"), trait.defName())) {
                return false;
            }
        }
        for (const std::string& tag : colonistTextList(trait, "requiredWorkTags")) {
            if (colonistListHas(disabled, tag)) {
                return false;
            }
        }
        return true;
    };
    const auto addTrait = [&](const Def& trait, size_t degreeIndex) {
        const std::string degreePath = "degreeDatas." + std::to_string(degreeIndex);
        out.traits.push_back(TraitRecord{trait.defName(), static_cast<int>(trait.integer((degreePath + ".degree").c_str(), 0))});
        chosen.push_back(&trait);
        for (const std::string& tag : colonistTextList(trait, "disabledWorkTags")) {
            if (!colonistListHas(disabled, tag)) {
                disabled.push_back(tag);
            }
        }
    };
    for (const Def* b : {childhood, adulthood}) {
        for (size_t i = 0; i < b->count("forcedTraits"); ++i) {
            const std::string base = "forcedTraits." + std::to_string(i);
            const bool happens = rng.unit() < static_cast<float>(b->number((base + ".chance").c_str(), 1.0));
            const Def* trait = defs.find("Traits", b->text((base + ".trait").c_str()));
            if (!happens || trait == nullptr || !allowed(*trait)) {
                continue;
            }
            size_t degreeIndex = 0;
            for (size_t d = 0; d < trait->count("degreeDatas"); ++d) {
                if (static_cast<int>(trait->integer(("degreeDatas." + std::to_string(d) + ".degree").c_str(), 0)) ==
                    static_cast<int>(b->integer((base + ".degree").c_str(), 0))) {
                    degreeIndex = d;
                }
            }
            addTrait(*trait, degreeIndex);
        }
    }
    for (int attempt = 0; attempt < 64 && static_cast<int>(chosen.size()) < wanted && !traitIds.empty(); ++attempt) {
        uint32_t total = 0;
        for (const DefId id : traitIds) {
            total += static_cast<uint32_t>(std::llround(defs.get(id).number("commonality", 1.0) * 1000.0));
        }
        uint32_t pick = rng.below(std::max<uint32_t>(total, 1));
        const Def* trait = &defs.get(traitIds.back());
        for (const DefId id : traitIds) {
            const uint32_t w = static_cast<uint32_t>(std::llround(defs.get(id).number("commonality", 1.0) * 1000.0));
            if (pick < w) {
                trait = &defs.get(id);
                break;
            }
            pick -= w;
        }
        if (!allowed(*trait)) {
            continue;
        }
        const size_t degrees = trait->count("degreeDatas");
        addTrait(*trait, degrees == 0 ? 0 : rng.below(static_cast<uint32_t>(degrees)));
    }
    std::sort(disabled.begin(), disabled.end());
    out.disabledWorkTags = disabled;

    // Skills: a random base, plus backstory and trait gains, clamped to 0 to maxLevel.
    const int maxLevel = static_cast<int>(skillRules->integer("maxLevel", 20));
    const ColonistCurve levelCurve = colonistCurveField(*genRules, "skillLevelRandomCurve");
    const double levelMax = levelCurve.points.empty() ? 1.0 : levelCurve.points.back().first;
    const ColonistCurve ageFactorCurve = colonistCurveField(*genRules, "ageSkillMaxFactorCurve");
    const double ageFactor = ageFactorCurve.points.empty() ? 1.0
                                                                   : ageFactorCurve.at(std::clamp<double>(out.age, ageFactorCurve.points.front().first,
                                                                                                          ageFactorCurve.points.back().first));
    const std::vector<std::string> backstoryDefined = colonistTextList(*genRules, "backstoryDefinedSkills");
    const int definedMin = static_cast<int>(genRules->numberAt("backstoryDefinedLevel", 0, 0));
    const int definedMax = std::max(definedMin, static_cast<int>(genRules->numberAt("backstoryDefinedLevel", 1, 4)));
    std::vector<std::pair<int, const Def*>> skillDefs;  // listOrder, def
    for (const DefId id : defs.idsOfKind("Skills")) {
        skillDefs.emplace_back(static_cast<int>(defs.get(id).integer("listOrder", 0)), &defs.get(id));
    }
    std::sort(skillDefs.begin(), skillDefs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [order, skillDef] : skillDefs) {
        (void)order;
        const std::string& name = skillDef->defName();
        // Skills a person's backstory mostly defines start at 0 to 4, the rest from the random curve.
        double level = colonistListHas(backstoryDefined, name)
                           ? static_cast<double>(definedMin + static_cast<int>(rng.below(static_cast<uint32_t>(definedMax - definedMin + 1))))
                           : std::round(colonistDraw(levelCurve, 0.0, levelMax, 120, rng));
        level += childhood->integer(("skillGains." + name).c_str(), 0);
        level += adulthood->integer(("skillGains." + name).c_str(), 0);
        for (const Def* trait : chosen) {
            for (const TraitRecord& record : out.traits) {
                if (record.def != trait->defName()) {
                    continue;
                }
                const size_t degrees = trait->count("degreeDatas");
                for (size_t d = 0; d < degrees; ++d) {
                    const std::string base = "degreeDatas." + std::to_string(d);
                    if (static_cast<int>(trait->integer((base + ".degree").c_str(), 0)) == record.degree) {
                        level += static_cast<int>(trait->integer((base + ".skillGains." + name).c_str(), 0));
                    }
                }
            }
        }
        SkillRecord skill;
        skill.def = name;
        // Age: older people know more. The level is stretched by a random factor between 1 and the
        // age's maximum (below 1 for the young, so a 20-year-old's levels are a little lower).
        if (!ageFactorCurve.points.empty()) {
            level *= 1.0 + (ageFactor - 1.0) * rng.unit();
        }
        skill.level = std::clamp(static_cast<int>(std::lround(level)), 0, maxLevel);
        for (const std::string& tag : colonistTextList(*skillDef, "disablingWorkTags")) {
            skill.disabled = skill.disabled || colonistListHas(disabled, tag);
        }
        const size_t xpIndex = static_cast<size_t>(skill.level);
        const double toNext = skill.level >= maxLevel ? 0.0 : skillRules->numberAt("xpToNextLevel", xpIndex, 0.0);
        skill.xp = static_cast<float>(toNext * rng.range(0.1f, 0.9f));
        out.skills.push_back(std::move(skill));
    }

    // Passions go to the highest skills first (ties broken by the draw order), skipping disabled
    // skills and those a trait forbids.
    std::vector<std::string> forbidden;
    for (const Def* trait : chosen) {
        for (const std::string& s : colonistTextList(*trait, "disallowedPassions")) {
            forbidden.push_back(s);
        }
    }
    std::vector<std::pair<uint32_t, size_t>> ranked;  // sort key, skill index
    for (size_t i = 0; i < out.skills.size(); ++i) {
        const SkillRecord& s = out.skills[i];
        if (!s.disabled && !colonistListHas(forbidden, s.def)) {
            ranked.emplace_back((static_cast<uint32_t>(s.level) << 16) | rng.below(0x10000), i);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const int passionMin = static_cast<int>(genRules->numberAt("passionCount", 0, 0));
    const int passionMax = static_cast<int>(genRules->numberAt("passionCount", 1, 4));
    int passions = passionMin + static_cast<int>(rng.below(static_cast<uint32_t>(std::max(1, passionMax - passionMin + 1))));
    const float majorChance = static_cast<float>(genRules->number("majorPassionChance", 0.33));
    for (size_t i = 0; i < ranked.size() && passions > 0; ++i, --passions) {
        out.skills[ranked[i].second].passion = rng.unit() < majorChance ? Passion::Major : Passion::Minor;
    }
    return true;
}

}  // namespace colony
