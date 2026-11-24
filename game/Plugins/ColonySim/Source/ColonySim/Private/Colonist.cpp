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

// The degree data index of `degree` in `trait`, or -1.
int colonistDegreeIndex(const Def& trait, int degree) {
    const size_t n = trait.count("degreeDatas");
    for (size_t d = 0; d < n; ++d) {
        if (static_cast<int>(trait.integer(("degreeDatas." + std::to_string(d) + ".degree").c_str(), 0)) == degree) {
            return static_cast<int>(d);
        }
    }
    return n == 0 && degree == 0 ? 0 : -1;
}

bool colonistTraitsClash(const Def& a, const Def& b) {
    return &a == &b || colonistListHas(colonistTextList(a, "conflictingTraits"), b.defName()) ||
           colonistListHas(colonistTextList(b, "conflictingTraits"), a.defName());
}

// Buys traits from `pool` points until all are spent (ours, docs/design/new-game.md): each round picks
// one trait degree by commonality from those that fit (no repeats or conflicts, none needing work
// the traits so far disabled), a negative one now and then (it gives points), else one that costs
// no more than what is left.
std::vector<TraitRecord> colonistBuyTraits(const DefDatabase& defs, int pool, Rng& rng) {
    std::vector<TraitRecord> out;
    std::vector<const Def*> chosen;
    std::vector<std::string> disabled;
    int left = pool;
    int negatives = 0;
    const std::vector<DefId> ids = defs.idsOfKind("Traits");
    for (int round = 0; round < 64 && left > 0 && static_cast<int>(out.size()) < kMaxTraits; ++round) {
        const int slotsLeft = kMaxTraits - static_cast<int>(out.size());
        const bool negative = negatives < 2 && slotsLeft > 3 && rng.unit() < 0.2f;
        // With few slots left, only traits big enough to spend the rest in time.
        const int minCost = (left + slotsLeft - 1) / slotsLeft;
        struct Option {
            const Def* trait;
            int degree;
            int cost;
            uint32_t weight;
        };
        std::vector<Option> options;
        uint32_t total = 0;
        for (const DefId id : ids) {
            const Def& trait = defs.get(id);
            if (trait.flag("abstract")) {
                continue;
            }
            bool fits = true;
            for (const Def* other : chosen) {
                fits = fits && !colonistTraitsClash(trait, *other);
            }
            for (const std::string& tag : colonistTextList(trait, "requiredWorkTags")) {
                fits = fits && !colonistListHas(disabled, tag);
            }
            if (!fits) {
                continue;
            }
            for (size_t d = 0; d < trait.count("degreeDatas"); ++d) {
                const std::string base = "degreeDatas." + std::to_string(d);
                if (!trait.has((base + ".points").c_str())) {
                    continue;
                }
                const int cost = static_cast<int>(trait.integer((base + ".points").c_str(), 0));
                if (negative ? cost >= 0 : (cost <= 0 || cost > left || cost < minCost)) {
                    continue;
                }
                const uint32_t w = static_cast<uint32_t>(std::llround(trait.number("commonality", 1.0) * 1000.0));
                if (w == 0) {
                    continue;
                }
                options.push_back(Option{&trait, static_cast<int>(trait.integer((base + ".degree").c_str(), 0)), cost, w});
                total += w;
            }
        }
        if (options.empty()) {
            continue;
        }
        uint32_t pick = rng.below(total);
        const Option* got = &options.back();
        for (const Option& o : options) {
            if (pick < o.weight) {
                got = &o;
                break;
            }
            pick -= o.weight;
        }
        out.push_back(TraitRecord{got->trait->defName(), got->degree});
        chosen.push_back(got->trait);
        for (const std::string& tag : colonistTextList(*got->trait, "disabledWorkTags")) {
            disabled.push_back(tag);
        }
        left -= got->cost;
        negatives += got->cost < 0 ? 1 : 0;
    }
    return out;
}

// The skill gains of a set of traits for one skill.
int colonistTraitGain(const DefDatabase& defs, const std::vector<TraitRecord>& traits, const std::string& skill) {
    int gain = 0;
    for (const TraitRecord& record : traits) {
        const Def* trait = defs.find("Traits", record.def);
        const int d = trait != nullptr ? colonistDegreeIndex(*trait, record.degree) : -1;
        if (d >= 0) {
            gain += static_cast<int>(trait->integer(("degreeDatas." + std::to_string(d) + ".skillGains." + skill).c_str(), 0));
        }
    }
    return gain;
}

// The rest of a point-mode colonist (ours, docs/design/new-game.md), after name and age: no backstories; traits
// bought from the pool (none for the protagonist); then skills from a pool (sneed 2026-11-24): `skillPointPool`
// levels (30) handed out at random among the skills the person can use, a disabled skill's share going to the others,
// some skills drawn as favourites so people differ; the traits' gains on top, which may pass maxLevel. One skill gets
// a burning (major) passion and another a minor one (`pointPassions`), the person's best skills first.
bool generatePointColonist(const DefDatabase& defs, Rng& rng, const Def& genRules, const Def& skillRules,
                           const ColonistGenOptions& options, Colonist& out) {
    const std::vector<TraitRecord> traits = options.noTraits ? std::vector<TraitRecord>{} : colonistBuyTraits(defs, options.traitPoints, rng);

    const int maxLevel = static_cast<int>(skillRules.integer("maxLevel", 20));
    std::vector<std::pair<int, const Def*>> skillDefs;  // listOrder, def
    for (const DefId id : defs.idsOfKind("Skills")) {
        skillDefs.emplace_back(static_cast<int>(defs.get(id).integer("listOrder", 0)), &defs.get(id));
    }
    std::sort(skillDefs.begin(), skillDefs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [order, skillDef] : skillDefs) {
        (void)order;
        SkillRecord skill;
        skill.def = skillDef->defName();
        out.skills.push_back(std::move(skill));
    }
    // The traits first: they say which skills are disabled (those get no points) and add their gains.
    setColonistTraits(defs, out, traits);
    std::vector<double> weight(out.skills.size(), 0.0);
    double weightSum = 0.0;
    for (size_t i = 0; i < out.skills.size(); ++i) {
        if (!out.skills[i].disabled) {
            const double u = rng.unit();
            weight[i] = 0.15 + u * u * u;  // a few favourites, the rest a little each
            weightSum += weight[i];
        }
    }
    const int pool = std::max(0, static_cast<int>(genRules.integer("skillPointPool", 30)));
    for (int point = 0; point < pool && weightSum > 0.0; ++point) {
        double pick = rng.unit() * weightSum;
        size_t at = out.skills.size();
        for (size_t i = 0; i < out.skills.size(); ++i) {
            if (weight[i] <= 0.0) {
                continue;
            }
            at = i;
            pick -= weight[i];
            if (pick < 0.0) {
                break;
            }
        }
        if (at >= out.skills.size()) {
            break;
        }
        SkillRecord& skill = out.skills[at];
        ++skill.level;
        if (skill.level - skill.bonus >= maxLevel) {
            weightSum -= weight[at];  // full: no more points to it
            weight[at] = 0.0;
        }
    }
    for (SkillRecord& skill : out.skills) {
        const int index = std::clamp(skill.level, 0, maxLevel);
        const double toNext = skill.level >= maxLevel + std::max(0, skill.bonus) ? 0.0 : skillRules.numberAt("xpToNextLevel", static_cast<size_t>(index), 0.0);
        skill.xp = static_cast<float>(toNext * rng.range(0.1f, 0.9f));
    }

    std::vector<std::string> forbidden;
    for (const TraitRecord& record : traits) {
        if (const Def* trait = defs.find("Traits", record.def)) {
            for (const std::string& name : colonistTextList(*trait, "disallowedPassions")) {
                forbidden.push_back(name);
            }
        }
    }
    std::vector<std::pair<uint32_t, size_t>> ranked;  // sort key, skill index
    for (size_t i = 0; i < out.skills.size(); ++i) {
        const SkillRecord& s = out.skills[i];
        if (!s.disabled && !colonistListHas(forbidden, s.def)) {
            ranked.emplace_back((static_cast<uint32_t>(std::max(0, s.level - s.bonus)) << 16) | rng.below(0x10000), i);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const int majors = static_cast<int>(genRules.numberAt("pointPassions", 0, 1));
    const int minors = static_cast<int>(genRules.numberAt("pointPassions", 1, 1));
    for (size_t i = 0; i < ranked.size() && static_cast<int>(i) < majors + minors; ++i) {
        out.skills[ranked[i].second].passion = static_cast<int>(i) < majors ? Passion::Major : Passion::Minor;
    }
    return true;
}

}  // namespace

int traitPointCost(const DefDatabase& defs, const TraitRecord& record) {
    const Def* trait = defs.find("Traits", record.def);
    const int d = trait != nullptr ? colonistDegreeIndex(*trait, record.degree) : -1;
    return d < 0 ? 0 : static_cast<int>(trait->integer(("degreeDatas." + std::to_string(d) + ".points").c_str(), 0));
}

int traitPointsSpent(const DefDatabase& defs, const std::vector<TraitRecord>& traits) {
    int spent = 0;
    for (const TraitRecord& t : traits) {
        spent += traitPointCost(defs, t);
    }
    return spent;
}

bool traitSetFits(const DefDatabase& defs, const std::vector<TraitRecord>& traits, int pool, std::string& error) {
    if (static_cast<int>(traits.size()) > kMaxTraits) {
        error = "at most " + std::to_string(kMaxTraits) + " traits";
        return false;
    }
    int negatives = 0;
    std::vector<const Def*> seen;
    for (const TraitRecord& t : traits) {
        const Def* trait = defs.find("Traits", t.def);
        const int d = trait != nullptr && !trait->flag("abstract") ? colonistDegreeIndex(*trait, t.degree) : -1;
        if (d < 0 || !trait->has(("degreeDatas." + std::to_string(d) + ".points").c_str())) {
            error = "no trait '" + t.def + "' of degree " + std::to_string(t.degree) + " to choose";
            return false;
        }
        for (const Def* other : seen) {
            if (colonistTraitsClash(*trait, *other)) {
                error = "'" + t.def + "' can't go with '" + other->defName() + "'";
                return false;
            }
        }
        seen.push_back(trait);
        negatives += traitPointCost(defs, t) < 0 ? 1 : 0;
    }
    if (negatives > kMaxNegativeTraits) {
        error = "at most " + std::to_string(kMaxNegativeTraits) + " negative traits";
        return false;
    }
    if (traitPointsSpent(defs, traits) > pool) {
        error = "those traits cost " + std::to_string(traitPointsSpent(defs, traits)) + " points of " + std::to_string(pool);
        return false;
    }
    return true;
}

void setColonistTraits(const DefDatabase& defs, Colonist& colonist, const std::vector<TraitRecord>& traits) {
    colonist.traits = traits;
    std::vector<std::string> disabled;
    std::vector<std::string> forbidden;
    for (const TraitRecord& record : traits) {
        if (const Def* trait = defs.find("Traits", record.def)) {
            for (const std::string& tag : colonistTextList(*trait, "disabledWorkTags")) {
                if (!colonistListHas(disabled, tag)) {
                    disabled.push_back(tag);
                }
            }
            for (const std::string& name : colonistTextList(*trait, "disallowedPassions")) {
                forbidden.push_back(name);
            }
        }
    }
    std::sort(disabled.begin(), disabled.end());
    colonist.disabledWorkTags = disabled;
    for (SkillRecord& skill : colonist.skills) {
        const int base = skill.level - skill.bonus;
        const int gain = colonistTraitGain(defs, traits, skill.def);
        skill.level = std::max(0, base + gain);
        skill.bonus = skill.level - base;
        skill.disabled = false;
        if (const Def* skillDef = defs.find("Skills", skill.def)) {
            for (const std::string& tag : colonistTextList(*skillDef, "disablingWorkTags")) {
                skill.disabled = skill.disabled || colonistListHas(disabled, tag);
            }
        }
    }
    // A disabled skill's levels and passion go to the skills the person can use (sneed 2026-11-24): the points are
    // handed round one at a time, best skills first; the passion goes to the best skill without one.
    const Def* skillRules = defs.find("Rules", "SkillRules");
    const int maxLevel = skillRules != nullptr ? static_cast<int>(skillRules->integer("maxLevel", 20)) : 20;
    std::vector<size_t> open;
    for (size_t i = 0; i < colonist.skills.size(); ++i) {
        if (!colonist.skills[i].disabled) {
            open.push_back(i);
        }
    }
    const auto baseOf = [&colonist](size_t i) { return colonist.skills[i].level - colonist.skills[i].bonus; };
    std::stable_sort(open.begin(), open.end(), [&](size_t a, size_t b) { return baseOf(a) > baseOf(b); });
    int spare = 0;
    std::vector<Passion> loose;
    for (SkillRecord& skill : colonist.skills) {
        const int base = skill.level - skill.bonus;
        if (skill.disabled && base > 0) {
            spare += base;
            skill.level -= base;
        }
        if ((skill.disabled || colonistListHas(forbidden, skill.def)) && skill.passion != Passion::None) {
            loose.push_back(skill.passion);
            skill.passion = Passion::None;
        }
    }
    while (spare > 0 && !open.empty()) {
        bool any = false;
        for (const size_t i : open) {
            if (spare > 0 && baseOf(i) < maxLevel) {
                ++colonist.skills[i].level;
                --spare;
                any = true;
            }
        }
        if (!any) {
            break;  // every open skill is full
        }
    }
    std::sort(loose.begin(), loose.end(), [](Passion a, Passion b) { return a > b; });
    for (const Passion passion : loose) {
        for (const size_t i : open) {
            SkillRecord& skill = colonist.skills[i];
            if (skill.passion == Passion::None && !colonistListHas(forbidden, skill.def)) {
                skill.passion = passion;
                break;
            }
        }
    }
}

std::vector<TraitRecord> randomTraitSet(const DefDatabase& defs, uint64_t seed, int pool) {
    Rng rng(splitMix64(seed ^ 0x7472616974736574ULL), 9);
    return colonistBuyTraits(defs, pool, rng);
}

void makeColonistNameUnique(const DefDatabase& defs, uint64_t seed, Colonist& colonist, const std::vector<const Colonist*>& taken) {
    const auto nickTaken = [&](const std::string& name) {
        return std::any_of(taken.begin(), taken.end(), [&](const Colonist* other) { return other != nullptr && other->nickName == name; });
    };
    const auto firstTaken = [&](const std::string& name) {
        return std::any_of(taken.begin(), taken.end(), [&](const Colonist* other) { return other != nullptr && other->firstName == name; });
    };
    if (!nickTaken(colonist.nickName) && !firstTaken(colonist.firstName)) {
        return;
    }
    const Def* nicks = defs.find("Names", "Nicknames");
    const Def* firsts = defs.find("Names", colonist.female ? "FirstNamesFemale" : "FirstNamesMale");
    Rng rng(splitMix64(seed ^ 0x756E697175656E6DULL), 5);
    constexpr int kTries = 64;
    for (int i = 0; nicks != nullptr && i < kTries && nickTaken(colonist.nickName); ++i) {
        colonist.nickName = colonistName(*nicks, rng);
    }
    for (int i = 0; firsts != nullptr && i < kTries && firstTaken(colonist.firstName); ++i) {
        colonist.firstName = colonistName(*firsts, rng);
    }
}

std::string randomFirstName(const DefDatabase& defs, uint64_t seed, bool female) {
    const Def* bank = defs.find("Names", female ? "FirstNamesFemale" : "FirstNamesMale");
    if (bank == nullptr) {
        return {};
    }
    Rng rng(splitMix64(seed ^ 0x6E616D65666F7273ULL), 7);
    return colonistName(*bank, rng);
}

bool generateColonist(const DefDatabase& defs, uint64_t seed, bool protagonist, Colonist& out, std::string& error,
                      const std::string& category) {
    ColonistGenOptions options;
    options.category = category;
    return generateColonist(defs, seed, protagonist, out, error, options);
}

bool generateColonist(const DefDatabase& defs, uint64_t seed, bool protagonist, Colonist& out, std::string& error,
                      const ColonistGenOptions& options) {
    const std::string& category = options.category;
    const bool pointMode = options.traitPoints >= 0;
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
    if (options.age > 0) {
        out.age = options.age;
    }
    if (pointMode) {
        return generatePointColonist(defs, rng, *genRules, *skillRules, options, out);
    }

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
    std::vector<DefId> traitIds = defs.idsOfKind("Traits");
    // Traits made for trait points only (the skill traits that replace backstories) are not drawn here.
    traitIds.erase(std::remove_if(traitIds.begin(), traitIds.end(), [&](DefId id) { return defs.get(id).flag("pointsOnly"); }), traitIds.end());
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
