#include "ColonySim/Looks.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Genes.h"
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

// A weighted pick from a list of objects in a def: index by the `weightKey` number of each entry
// (whole hundredths, so every compiler draws the same), or -1 for an empty list or no weight.
int looksPickWeighted(const Def& def, const std::string& path, const char* weightKey, Rng& rng) {
    const size_t n = def.count(path.c_str());
    std::vector<uint32_t> cumulative;
    uint32_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        const std::string key = path + "." + std::to_string(i) + "." + weightKey;
        total += static_cast<uint32_t>(std::llround(std::max(0.0, def.number(key.c_str(), 1.0)) * 100.0));
        cumulative.push_back(total);
    }
    if (total == 0) {
        return -1;
    }
    const uint32_t pick = rng.below(total);
    return static_cast<int>(std::upper_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin());
}

// The average of two draws: faces cluster round the average with a few at the ends.
float looksBell(Rng& rng) {
    return (rng.unit() + rng.unit()) * 0.5f;
}

constexpr const char* kLooksRegionNames[kBodyRegionCount] = {"Face", "Neck", "Torso", "LeftArm", "RightArm", "LeftLeg", "RightLeg"};

LooksMark looksNewMark(BodyRegion region, int style, const Def& rules, const char* sizePath, Rng& rng) {
    LooksMark mark;
    mark.region = region;
    mark.style = static_cast<uint8_t>(std::clamp(style, 0, 255));
    mark.u = rng.unit();
    mark.v = rng.unit();
    float lo = 0.7f;
    float hi = 1.3f;
    looksRange(rules, sizePath, lo, hi);
    mark.size = rng.range(lo, hi);
    return mark;
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

// Skin and hair as drawn: the colour of the lineage's last skin gene over the melanin colour, a
// little darker for darker skin so the shading between pawns of one lineage stays.
void looksApplyLineage(const DefDatabase& defs, const Def* rules, const Def* lineage, PawnLooks& out) {
    const LooksColor base = rules ? looksMelaninColor(*rules, out.melanin) : LooksColor{0.75f, 0.58f, 0.45f};
    out.skin = base;
    out.hair = out.naturalHair;
    out.beard = out.naturalBeard;
    out.bodyHair = out.naturalBodyHair;
    if (lineage == nullptr) {
        return;
    }
    if (const Def* skinGene = lineageGeneWith(defs, *lineage, "visual.skinColor")) {
        const float amount = std::clamp(static_cast<float>(skinGene->number("visual.skinColorAmount", 0.0)), 0.0f, 1.0f);
        if (amount > 0.0f && skinGene->count("visual.skinColor") >= 3) {
            const LooksColor tint = looksColorField(*skinGene, "visual.skinColor", base);
            const float shade = 1.1f - 0.35f * out.melanin;
            out.skin = looksLerp(base, LooksColor{tint.r * shade, tint.g * shade, tint.b * shade}, amount);
            out.skin.r = std::clamp(out.skin.r, 0.0f, 1.0f);
            out.skin.g = std::clamp(out.skin.g, 0.0f, 1.0f);
            out.skin.b = std::clamp(out.skin.b, 0.0f, 1.0f);
        }
    }
    if (const Def* beardGene = lineageGeneWith(defs, *lineage, "visual.beard")) {
        if (!beardGene->flag("visual.beard", true) && !out.beard.empty()) {
            out.beard = "None";
        }
    }
    if (const Def* furGene = lineageGeneWith(defs, *lineage, "visual.bodyHair")) {
        out.bodyHair = std::clamp(static_cast<float>(furGene->number("visual.bodyHair", 0.0)), 0.0f, 1.0f);
    }
    if (const Def* hairGene = lineageGeneWith(defs, *lineage, "visual.hairColor")) {
        if (hairGene->count("visual.hairColor") >= 3) {
            out.hair = looksColorField(*hairGene, "visual.hairColor", out.naturalHair);
        }
    }
}

// V1: the face sliders, eyes, hair style, beard, body hair and tattoos, from their own random
// stream so the A3 fields of the same seed do not move.
void looksFace(const Def& rules, uint64_t seed, bool female, PawnLooks& out) {
    Rng rng(splitMix64(seed ^ 0x66616365736565ULL), 11);
    out.faceShape = looksBell(rng);
    out.brow = looksBell(rng);
    out.nose = looksBell(rng);
    out.jaw = looksBell(rng);
    // Men have heavier brows and squarer jaws on the whole; women softer ones.
    const float shift = static_cast<float>(rules.number(female ? "faceGenderShift.female" : "faceGenderShift.male", 0.0));
    out.brow = std::clamp(out.brow + shift, 0.0f, 1.0f);
    out.jaw = std::clamp(out.jaw + shift, 0.0f, 1.0f);

    out.eyes = LooksColor{0.25f, 0.16f, 0.1f};
    const int eye = looksPickWeighted(rules, "eyeColors", "weight", rng);
    if (eye >= 0) {
        out.eyes = looksColorField(rules, "eyeColors." + std::to_string(eye) + ".color", out.eyes);
    }

    out.hairStyle = "Short";
    const int hair = looksPickWeighted(rules, "hairStyles", female ? "female" : "male", rng);
    if (hair >= 0) {
        out.hairStyle = rules.text(("hairStyles." + std::to_string(hair) + ".name").c_str(), out.hairStyle);
    }
    out.naturalBeard = "None";
    if (!female) {
        const int beard = looksPickWeighted(rules, "beardStyles", "weight", rng);
        if (beard >= 0) {
            out.naturalBeard = rules.text(("beardStyles." + std::to_string(beard) + ".name").c_str(), "None");
        }
    }

    float lo = 0.0f;
    float hi = 0.5f;
    looksRange(rules, female ? "bodyHair.female" : "bodyHair.male", lo, hi);
    out.naturalBodyHair = std::clamp(rng.range(lo, hi), 0.0f, 1.0f);

    // Tattoos: most pawns have none; the rest one to `max`, on regions picked by weight.
    const uint32_t most = static_cast<uint32_t>(std::max(0.0, rules.number("tattoos.max", 0.0)));
    const size_t styles = rules.count("tattoos.styles");
    if (most > 0 && styles > 0 && rng.unit() < static_cast<float>(rules.number("tattoos.chance", 0.0))) {
        const uint32_t count = 1 + rng.below(most);
        for (uint32_t i = 0; i < count; ++i) {
            const int region = looksPickWeighted(rules, "tattoos.regions", "weight", rng);
            BodyRegion where = BodyRegion::Torso;
            if (region >= 0) {
                const std::string name = rules.text(("tattoos.regions." + std::to_string(region) + ".region").c_str());
                for (int r = 0; r < kBodyRegionCount; ++r) {
                    if (name == kLooksRegionNames[r]) {
                        where = static_cast<BodyRegion>(r);
                    }
                }
            }
            const int style = static_cast<int>(rng.below(static_cast<uint32_t>(styles)));
            out.tattoos.push_back(looksNewMark(where, style, rules, "tattoos.sizeRange", rng));
        }
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
        looksApplyLineage(defs, nullptr, lineageDef, out);
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

    looksFace(*rules, seed, female, out);
    looksApplyLineage(defs, rules, lineageDef, out);
    return ok;
}

void addScarMark(const DefDatabase& defs, PawnLooks& looks, BodyRegion region, const std::string& hediff, Rng& rng) {
    const Def* rules = defs.find("Rules", "LooksRules");
    if (rules == nullptr) {
        return;
    }
    const size_t cap = static_cast<size_t>(std::max(0.0, rules->number("scars.max", 24.0)));
    if (looks.scars.size() >= cap) {
        return;
    }
    // Styles that name this hediff, else the ones that name none.
    const size_t n = rules->count("scars.styles");
    std::vector<int> matching;
    std::vector<int> general;
    for (size_t i = 0; i < n; ++i) {
        const std::string path = "scars.styles." + std::to_string(i) + ".hediffs";
        const size_t listed = rules->count(path.c_str());
        if (listed == 0) {
            general.push_back(static_cast<int>(i));
            continue;
        }
        for (size_t j = 0; j < listed; ++j) {
            if (rules->textAt(path.c_str(), j) == hediff) {
                matching.push_back(static_cast<int>(i));
                break;
            }
        }
    }
    const std::vector<int>& pool = matching.empty() ? general : matching;
    const int style = pool.empty() ? 0 : pool[rng.below(static_cast<uint32_t>(pool.size()))];
    looks.scars.push_back(looksNewMark(region, style, *rules, "scars.sizeRange", rng));
}

bool bodyRegionOfPart(const std::string& partDef, bool left, BodyRegion& out) {
    static const char* const kFace[] = {"Head", "Skull", "Brain", "Eye", "Ear", "Nose", "Jaw", "Tongue"};
    static const char* const kArm[] = {"Shoulder", "Clavicle", "Arm", "Humerus", "Radius", "Hand", "Finger"};
    static const char* const kLeg[] = {"Leg", "Femur", "Tibia", "Foot", "Toe"};
    for (const char* name : kFace) {
        if (partDef == name) {
            out = BodyRegion::Face;
            return partDef != "Skull" && partDef != "Brain" && partDef != "Tongue";
        }
    }
    for (const char* name : kArm) {
        if (partDef == name) {
            out = left ? BodyRegion::LeftArm : BodyRegion::RightArm;
            return partDef != "Clavicle" && partDef != "Humerus" && partDef != "Radius";
        }
    }
    for (const char* name : kLeg) {
        if (partDef == name) {
            out = left ? BodyRegion::LeftLeg : BodyRegion::RightLeg;
            return partDef != "Femur" && partDef != "Tibia";
        }
    }
    if (partDef == "Neck") {
        out = BodyRegion::Neck;
        return true;
    }
    if (partDef == "Torso" || partDef == "Waist") {
        out = BodyRegion::Torso;
        return true;
    }
    return false;
}

bool setLineage(const DefDatabase& defs, const std::string& lineage, PawnLooks& looks, std::string& error) {
    const Def* lineageDef = defs.find("Lineages", lineage);
    if (lineageDef == nullptr) {
        error = "no Lineages def '" + lineage + "'";
        return false;
    }
    looks.lineage = lineage;
    looksApplyLineage(defs, defs.find("Rules", "LooksRules"), lineageDef, looks);
    return true;
}

}  // namespace colony
