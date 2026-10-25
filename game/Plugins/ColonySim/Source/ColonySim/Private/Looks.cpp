#include "ColonySim/Looks.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Random.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace colony {

namespace {

constexpr const char* kLooksBodyNames[] = {"Thin", "Average", "Fat", "Hulk"};

LooksColor looksLerp(const LooksColor& a, const LooksColor& b, float t) {
    return LooksColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

LooksColor looksColorField(const Def& def, const std::string& path, LooksColor fallback) {
    if (def.count(path.c_str()) < 3) {
        return fallback;
    }
    return LooksColor{static_cast<float>(def.numberAt(path.c_str(), 0, fallback.r)),
                      static_cast<float>(def.numberAt(path.c_str(), 1, fallback.g)),
                      static_cast<float>(def.numberAt(path.c_str(), 2, fallback.b))};
}

// A [lo, hi] pair from a def, or the fallback.
void looksRange(const Def& def, const std::string& path, float& lo, float& hi) {
    if (def.count(path.c_str()) >= 2) {
        lo = static_cast<float>(def.numberAt(path.c_str(), 0, lo));
        hi = static_cast<float>(def.numberAt(path.c_str(), 1, hi));
    }
}

// The skin colour for this melanin, from the LooksRules gradient [[melanin, r, g, b], ...].
LooksColor looksMelaninColor(const Def& rules, float melanin) {
    const size_t n = rules.count("skinColors");
    LooksColor before{0.75f, 0.58f, 0.45f};
    float beforeAt = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const std::string stop = "skinColors." + std::to_string(i);
        const float at = static_cast<float>(rules.numberAt(stop.c_str(), 0, 0.0));
        const LooksColor color{static_cast<float>(rules.numberAt(stop.c_str(), 1, 0.0)),
                               static_cast<float>(rules.numberAt(stop.c_str(), 2, 0.0)),
                               static_cast<float>(rules.numberAt(stop.c_str(), 3, 0.0))};
        if (melanin <= at) {
            const float span = at - beforeAt;
            return i == 0 || span <= 0.0f ? color : looksLerp(before, color, (melanin - beforeAt) / span);
        }
        before = color;
        beforeAt = at;
    }
    return before;
}

// Skin and hair as drawn: the lineage's colour over the melanin colour, a little darker for
// darker skin so the shading between pawns of one lineage stays.
void looksApplyLineage(const Def* rules, const Def* lineage, PawnLooks& out) {
    const LooksColor base = rules ? looksMelaninColor(*rules, out.melanin) : LooksColor{0.75f, 0.58f, 0.45f};
    out.skin = base;
    out.hair = out.naturalHair;
    if (lineage == nullptr) {
        return;
    }
    const float amount = std::clamp(static_cast<float>(lineage->number("visual.skinColorAmount", 0.0)), 0.0f, 1.0f);
    if (amount > 0.0f && lineage->count("visual.skinColor") >= 3) {
        const LooksColor tint = looksColorField(*lineage, "visual.skinColor", base);
        const float shade = 1.1f - 0.35f * out.melanin;
        out.skin = looksLerp(base, LooksColor{tint.r * shade, tint.g * shade, tint.b * shade}, amount);
        out.skin.r = std::clamp(out.skin.r, 0.0f, 1.0f);
        out.skin.g = std::clamp(out.skin.g, 0.0f, 1.0f);
        out.skin.b = std::clamp(out.skin.b, 0.0f, 1.0f);
    }
    if (lineage->count("visual.hairColor") >= 3) {
        out.hair = looksColorField(*lineage, "visual.hairColor", out.naturalHair);
    }
}

}  // namespace

const char* bodyTypeName(BodyType body) {
    return kLooksBodyNames[std::min<size_t>(static_cast<size_t>(body), 3)];
}

bool generateLooks(const DefDatabase& defs, uint64_t seed, bool female, const std::string& lineage, PawnLooks& out,
                   std::string& error) {
    out = PawnLooks{};
    out.female = female;
    out.lineage = lineage;
    out.naturalHair = LooksColor{0.2f, 0.14f, 0.1f};
    Rng rng(splitMix64(seed ^ 0x6C6F6F6B73ULL), 9);

    const Def* rules = defs.find("Rules", "LooksRules");
    const Def* lineageDef = defs.find("Lineages", lineage);
    bool ok = true;
    if (rules == nullptr) {
        error = "looks need the Rules def 'LooksRules'";
        ok = false;
    } else if (lineageDef == nullptr) {
        error = "no Lineages def '" + lineage + "'";
        ok = false;
    }
    if (rules == nullptr) {
        looksApplyLineage(nullptr, lineageDef, out);
        return ok;
    }

    // Body type by weight (whole numbers, the same draw on every compiler), then where in its
    // range the body sits.
    uint32_t weights[4] = {};
    uint32_t total = 0;
    for (size_t i = 0; i < 4; ++i) {
        const std::string path = std::string("bodyTypeWeights.") + kLooksBodyNames[i];
        weights[i] = static_cast<uint32_t>(std::llround(std::max(0.0, rules->number(path.c_str(), i == 1 ? 1.0 : 0.0)) * 100.0));
        total += weights[i];
    }
    uint32_t pick = rng.below(std::max<uint32_t>(total, 1));
    for (size_t i = 0; i < 4; ++i) {
        if (pick < weights[i]) {
            out.body = static_cast<BodyType>(i);
            break;
        }
        pick -= weights[i];
    }
    float lo = -0.3f;
    float hi = 0.3f;
    looksRange(*rules, std::string("weightRange.") + bodyTypeName(out.body), lo, hi);
    out.weight = std::clamp(rng.range(lo, hi), -1.0f, 1.0f);

    lo = female ? 0.9f : 0.96f;
    hi = female ? 0.98f : 1.04f;
    looksRange(*rules, female ? "heightFemale" : "heightMale", lo, hi);
    out.height = rng.range(lo, hi);
    if (out.body == BodyType::Hulk) {
        out.height += static_cast<float>(rules->number("hulkHeightBonus", 0.0));
    }

    lo = 0.0f;
    hi = 1.0f;
    looksRange(*rules, "melaninRange", lo, hi);
    out.melanin = std::clamp(rng.range(lo, hi), 0.0f, 1.0f);

    // Natural hair: a colour from the list, by weight.
    const size_t hairs = rules->count("hairColors");
    std::vector<uint32_t> cumulative;
    total = 0;
    for (size_t i = 0; i < hairs; ++i) {
        const std::string path = "hairColors." + std::to_string(i) + ".weight";
        total += static_cast<uint32_t>(std::llround(std::max(0.0, rules->number(path.c_str(), 1.0)) * 100.0));
        cumulative.push_back(total);
    }
    if (total > 0) {
        const uint32_t hairPick = rng.below(total);
        const size_t i = static_cast<size_t>(std::upper_bound(cumulative.begin(), cumulative.end(), hairPick) - cumulative.begin());
        out.naturalHair = looksColorField(*rules, "hairColors." + std::to_string(i) + ".color", out.naturalHair);
    }

    looksApplyLineage(rules, lineageDef, out);
    return ok;
}

bool setLineage(const DefDatabase& defs, const std::string& lineage, PawnLooks& looks, std::string& error) {
    const Def* lineageDef = defs.find("Lineages", lineage);
    if (lineageDef == nullptr) {
        error = "no Lineages def '" + lineage + "'";
        return false;
    }
    looks.lineage = lineage;
    looksApplyLineage(defs.find("Rules", "LooksRules"), lineageDef, looks);
    return true;
}

}  // namespace colony
