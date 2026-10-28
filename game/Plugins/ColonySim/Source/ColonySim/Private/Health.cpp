#include "ColonySim/Health.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Things.h"
#include "ColonySim/Traits.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr const char* kHealthThingsKind = "Things";
constexpr const char* kHealthBodiesKind = "Bodies";
constexpr const char* kHealthBodyPartsKind = "BodyParts";
constexpr const char* kHealthCapacitiesKind = "Capacities";
constexpr const char* kHealthHediffsKind = "Hediffs";
constexpr const char* kHealthDamageTypesKind = "DamageTypes";
constexpr const char* kHealthRulesKind = "Rules";

std::string healthPath(const std::string& base, const char* field) {
    return base.empty() ? std::string(field) : base + "." + field;
}

float healthMean(const std::vector<float>& values) {
    float sum = 0.0f;
    for (const float v : values) {
        sum += v;
    }
    return values.empty() ? 1.0f : sum / static_cast<float>(values.size());
}

}  // namespace

const char* deathCauseName(DeathCause cause) {
    switch (cause) {
        case DeathCause::None: return "";
        case DeathCause::BloodLoss: return "blood loss";
        case DeathCause::VitalPartLost: return "a vital part destroyed";
        case DeathCause::VitalCapacity: return "organ failure";
        case DeathCause::Starvation: return "starvation";
        case DeathCause::Hypothermia: return "hypothermia";
        case DeathCause::Heatstroke: return "heatstroke";
        case DeathCause::Environment: return "exposure";
        case DeathCause::Overdose: return "overdose";
        case DeathCause::Withdrawal: return "withdrawal";
        case DeathCause::Slaughtered: return "slaughtered";
        case DeathCause::Disease: return "illness";
        case DeathCause::Surgery: return "a failed operation";
        case DeathCause::Executed: return "executed";
    }
    return "";
}

// ---- The data, read once from the defs --------------------------------------------------------

class HealthModel {
public:
    struct Part {
        std::string def;
        std::string label;
        std::vector<std::string> groups;  // BodyPartGroups defs, which apparel covers
        int parent = -1;
        int end = 0;  // parts are in depth-first order: this part's children and theirs are (index, end)
        float coverage = 1.0f;     // share of the parent
        float coverageAbs = 1.0f;  // share of the whole body
        float ownOutside = 0.0f;   // its share of hits from outside: its area minus its outside children's
        bool inside = false;
        float hitPoints = 1.0f;
        float bleedRate = 1.0f;
        bool solid = false;
        bool destroyable = true;
        bool lethal = false;
        std::vector<int> children;
    };
    struct Body {
        std::vector<Part> parts;
        std::unordered_map<std::string, std::vector<int>> byDef;
    };
    enum class Combine : uint8_t { Average, Importance, BestWeighted };
    struct Entry {
        std::string part;
        Combine combine = Combine::Average;
        float weight = 1.0f;
        float bestWeight = 0.75f;
    };
    struct Factor {
        int capacity = -1;
        float weight = 1.0f;
        bool hasMax = false;
        float max = 1.0f;
    };
    struct Limbs {
        bool present = false;
        std::string core;
        std::vector<std::string> segments;
        std::string digit;
        float digitWeight = 0.0f;
    };
    struct Capacity {
        std::string name;
        std::vector<Entry> entries;
        std::vector<Factor> factors;
        Limbs limbs;
        bool hasPain = false;
        float painSlope = 0.0f;
        float painIntercept = 1.0f;
        float painMax = 1.0f;
        bool lethalAtZero = false;
        bool hasDownedBelow = false;
        float downedBelow = 0.0f;
        bool hasDownedAtOrBelow = false;
        float downedAtOrBelow = 0.0f;
    };
    struct Stage {
        float minSeverity = 0.0f;
        float painOffset = 0.0f;  // added to the pawn's pain (a disease's stage)
        bool visible = true;      // false while a disease is too early to notice
        std::vector<std::pair<int, float>> offsets;  // capacity, amount
        std::vector<std::pair<int, float>> maxima;
        float painFactor = 1.0f;  // multiplies the pawn's pain (drugs)
        float workSpeed = 1.0f;   // multiplies its global work speed
        float hunger = 1.0f;      // multiplies how fast its food falls
    };
    struct Hediff {
        float painPerSeverity = 0.0f;
        float bleedRate = 0.0f;
        bool canMerge = false;
        bool permanent = false;   // a scar or old wound: never heals
        bool tendable = true;     // a doctor can tend it (a bruise can't be)
        bool neverScars = false;
        float infectionChance = 0.0f;  // chance a fresh wound of this kind turns infected (H2)
    };
    // A disease def (class Disease) by DefId value (H2).
    struct DiseaseDef {
        bool valid = false;
        float lethal = 1.0f;  // 0: it never kills
        float initialSeverity = 0.001f;
        float gainPerDaySick = 0.0f;
        float notImmunePerDay = 0.0f;
        float immunePerDay = 0.0f;
        float tendPerDayAtQuality1 = 0.0f;
        float tendDurationDays = 1.0f;
        float cureAtTotalQuality = 0.0f;  // above 0: tends add up and cure it at this total
        bool tendable = true;
        bool keepsImmunity = true;
        bool wound = false;  // caused by a wound (infection)
        float restFallFactor = 1.0f;
        float hungerRateFactor = 1.0f;
        bool makesSickThought = true;
        std::vector<Stage> stages;  // by minSeverity
        std::vector<uint8_t> lifeThreatening;
    };
    struct DamageType {
        bool valid = false;
        DefId hediff;
        DefId hediffSolid;
        bool reachesInside = false;
    };
    struct Kind {  // a pawn def's body
        int body = -1;
        float healthScale = 1.0f;
        bool mechanical = false;  // `race.mechanical`: no blood, no pain (mechanoids)
    };

    explicit HealthModel(const DefDatabase& defs);

    const DefDatabase& defs;
    std::vector<Body> bodies;
    std::unordered_map<std::string, int> bodyIndex;
    std::vector<Capacity> capacities;
    int movingCapacity = -1;
    int consciousnessCapacity = -1;
    std::vector<Stage> bloodStages;
    float bloodLethal = 1.0f;
    // Environment hediffs (hypothermia...), by DefId value: empty `stages` for any other def.
    struct EnvironmentDef {
        bool valid = false;
        float lethal = 1.0f;
        DeathCause cause = DeathCause::Environment;
        std::vector<Stage> stages;  // by minSeverity
        std::vector<uint8_t> lifeThreatening;
    };
    std::vector<EnvironmentDef> environmentDefs;
    std::vector<DiseaseDef> diseaseDefs;
    std::vector<std::pair<DefId, float>> onsetMtbDays;  // MedicineRules.diseaseOnsetMtbDays
    int bloodFiltrationCapacity = -1;
    DefId missingHediff;
    struct AddedDef {  // a Hediffs def of class AddedPart (H3)
        bool valid = false;
        std::string part;  // the Bodies part def it goes on
        DefId item;
        float efficiency = 1.0f;
    };
    std::vector<AddedDef> addedDefs;      // by DefId value
    std::vector<Hediff> hediffs;          // by DefId value
    std::vector<DamageType> damageTypes;  // by DefId value
    std::vector<Kind> kinds;              // by DefId value of a Things def

    // Rules (Rules/HealthRules).
    float painShock = 0.8f;
    float naturalHealPerDay = 8.0f;
    float restBonus[3] = {4.0f, 8.0f, 14.0f};  // by RestKind
    float tendBonusLow = 4.0f;
    float tendBonusHigh = 12.0f;
    int64_t healInterval = 864;
    float healFactor = 0.01f;
    float lostPartHpFactor = 2.0f;
    float bloodPerDayPerBleed = 1.0f;
    float bloodRecoveryPerDay = 0.333f;
    float bleedMinForBloodLoss = 0.1f;
    float protagonistFactor = 2.0f;
    bool innerHurtsOuter = true;
    // Scars (H1): the chance a healed injury leaves one grows from 0 at scarMinHp hit points lost to scarMax at scarFullHp.
    float scarMax = 0.02f;
    float scarMinHp = 4.0f;
    float scarFullHp = 14.0f;
    float scarSeverity = 1.0f;
    float oldWoundFromHp = 14.0f;
    DefId scarHediff;
    DefId oldWoundHediff;
    // Diseases (H2): the infection of a wound and the rest of MedicineRules that applies.
    DefId infectionHediff;
    int64_t diseaseUpdate = 60;
    int64_t infectDelayMin = 12 * kSecondsPerHour;
    int64_t infectDelayMax = 24 * kSecondsPerHour;
    float tendedInfectionFactor0 = 0.85f;  // a tended wound's chance is this at tend quality 0 ...
    float tendedInfectionFactor1 = 0.05f;  // ... falling to this at quality 1
    float infectionCleanlinessSlope = -0.25f;
    float infectionFactorMin = 0.5f;
    float infectionFactorMax = 2.0f;
    float immunityGainByRest[3] = {1.0f, 1.0f, 1.07f};  // by RestKind
    float immunityGainWhenTended = 1.0f;

    const Kind* kindOf(DefId thingDef) const {
        if (!thingDef.valid() || thingDef.value >= kinds.size() || kinds[thingDef.value].body < 0) {
            return nullptr;
        }
        return &kinds[thingDef.value];
    }
    const EnvironmentDef* environmentOf(DefId hediff) const {
        return hediff.valid() && hediff.value < environmentDefs.size() && environmentDefs[hediff.value].valid
                   ? &environmentDefs[hediff.value]
                   : nullptr;
    }
    const DiseaseDef* diseaseOf(DefId hediff) const {
        return hediff.valid() && hediff.value < diseaseDefs.size() && diseaseDefs[hediff.value].valid ? &diseaseDefs[hediff.value]
                                                                                                        : nullptr;
    }
    static int stageIn(const std::vector<Stage>& stages, float severity) {
        int stage = -1;
        for (size_t i = 0; i < stages.size(); ++i) {
            if (severity >= stages[i].minSeverity) {
                stage = static_cast<int>(i);
            }
        }
        return stage;
    }
    int stageOf(float bloodLoss) const {
        int stage = -1;
        for (size_t i = 0; i < bloodStages.size(); ++i) {
            if (bloodLoss >= bloodStages[i].minSeverity) {
                stage = static_cast<int>(i);
            }
        }
        return stage;
    }

private:
    int buildPart(const Def& bodyDef, const std::string& path, int parent, float absCoverage, Body& out);
    // The `stages` of a hediff def, sorted by minSeverity; `lifeThreatening` (optional) gets each
    // stage's flag in the same order.
    static std::vector<Stage> readStages(const Def& def, const std::unordered_map<std::string, int>& capacityOf,
                                         std::vector<uint8_t>* lifeThreatening);
};

std::vector<HealthModel::Stage> HealthModel::readStages(const Def& def, const std::unordered_map<std::string, int>& capacityOf,
                                                         std::vector<uint8_t>* lifeThreatening) {
    struct Flagged {
        Stage stage;
        uint8_t life = 0;
    };
    std::vector<Flagged> flagged;
    for (size_t i = 0, n = def.count("stages"); i < n; ++i) {
        const std::string at = "stages." + std::to_string(i);
        Flagged f;
        Stage& s = f.stage;
        s.minSeverity = static_cast<float>(def.number((at + ".minSeverity").c_str()));
        s.painOffset = static_cast<float>(def.number((at + ".painOffset").c_str()));
        s.visible = def.flag((at + ".visible").c_str(), true);
        f.life = def.flag((at + ".lifeThreatening").c_str()) ? 1 : 0;
        for (const std::string& key : def.keys((at + ".capacityOffsets").c_str())) {
            const auto found = capacityOf.find(key);
            if (found != capacityOf.end()) {
                s.offsets.emplace_back(found->second, static_cast<float>(def.number((at + ".capacityOffsets." + key).c_str())));
            }
        }
        for (const std::string& key : def.keys((at + ".capacityMax").c_str())) {
            const auto found = capacityOf.find(key);
            if (found != capacityOf.end()) {
                s.maxima.emplace_back(found->second, static_cast<float>(def.number((at + ".capacityMax." + key).c_str())));
            }
        }
        s.painFactor = static_cast<float>(def.number((at + ".painFactor").c_str(), 1.0));
        s.workSpeed = static_cast<float>(def.number((at + ".workSpeed").c_str(), 1.0));
        s.hunger = static_cast<float>(def.number((at + ".hunger").c_str(), 1.0));
        flagged.push_back(std::move(f));
    }
    std::stable_sort(flagged.begin(), flagged.end(),
                     [](const Flagged& a, const Flagged& b) { return a.stage.minSeverity < b.stage.minSeverity; });
    std::vector<Stage> out;
    for (Flagged& f : flagged) {
        out.push_back(std::move(f.stage));
        if (lifeThreatening != nullptr) {
            lifeThreatening->push_back(f.life);
        }
    }
    return out;
}

int HealthModel::buildPart(const Def& bodyDef, const std::string& path, int parent, float absCoverage, Body& out) {
    const int index = static_cast<int>(out.parts.size());
    out.parts.emplace_back();
    {
        Part& p = out.parts.back();
        p.def = bodyDef.text(healthPath(path, "def").c_str());
        p.parent = parent;
        p.coverage = static_cast<float>(bodyDef.number(healthPath(path, "coverage").c_str(), 1.0));
        p.coverageAbs = absCoverage * p.coverage;
        p.inside = bodyDef.text(healthPath(path, "depth").c_str()) == "Inside";
        const Def* partDef = defs.find(kHealthBodyPartsKind, p.def);
        p.label = bodyDef.text(healthPath(path, "label").c_str(), partDef != nullptr ? partDef->label() : p.def);
        if (partDef != nullptr) {
            p.hitPoints = static_cast<float>(partDef->number("hitPoints", 1.0));
            p.bleedRate = static_cast<float>(partDef->number("bleedRate", 1.0));
            p.solid = partDef->flag("solid");
            p.destroyable = partDef->flag("destroyableByDamage", true);
            p.lethal = partDef->flag("lethalIfDestroyed");
        }
        const std::string groupsPath = healthPath(path, "groups");
        for (size_t g = 0; g < bodyDef.count(groupsPath.c_str()); ++g) {
            p.groups.push_back(bodyDef.textAt(groupsPath.c_str(), g));
        }
        out.byDef[p.def].push_back(index);
    }
    const size_t childCount = bodyDef.count(healthPath(path, "parts").c_str());
    for (size_t i = 0; i < childCount; ++i) {
        const float childBase = out.parts[static_cast<size_t>(index)].coverageAbs;
        const int child = buildPart(bodyDef, healthPath(path, "parts") + "." + std::to_string(i), index, childBase, out);
        out.parts[static_cast<size_t>(index)].children.push_back(child);
    }
    Part& p = out.parts[static_cast<size_t>(index)];
    p.end = static_cast<int>(out.parts.size());
    float own = p.coverageAbs;
    for (const int child : p.children) {
        const Part& c = out.parts[static_cast<size_t>(child)];
        if (!c.inside) {
            own -= c.coverageAbs;
        }
    }
    p.ownOutside = std::max(0.0f, own);
    return index;
}

HealthModel::HealthModel(const DefDatabase& database) : defs(database) {
    // Bodies.
    for (const DefId id : defs.idsOfKind(kHealthBodiesKind)) {
        const Def& body = defs.get(id);
        Body built;
        buildPart(body, "corePart", -1, 1.0f, built);
        bodyIndex[body.defName()] = static_cast<int>(bodies.size());
        bodies.push_back(std::move(built));
    }

    // Capacities, in id order.
    const std::vector<DefId> capacityIds = defs.idsOfKind(kHealthCapacitiesKind);
    std::unordered_map<std::string, int> capacityOf;
    for (size_t i = 0; i < capacityIds.size(); ++i) {
        capacityOf[defs.get(capacityIds[i]).defName()] = static_cast<int>(i);
    }
    for (const DefId id : capacityIds) {
        const Def& def = defs.get(id);
        Capacity c;
        c.name = def.defName();
        c.lethalAtZero = def.flag("lethalAtZero");
        c.hasDownedBelow = def.has("downedBelow");
        c.downedBelow = static_cast<float>(def.number("downedBelow"));
        c.hasDownedAtOrBelow = def.has("downedAtOrBelow");
        c.downedAtOrBelow = static_cast<float>(def.number("downedAtOrBelow"));
        for (size_t i = 0, n = def.count("parts"); i < n; ++i) {
            const std::string at = "parts." + std::to_string(i);
            Entry e;
            e.part = def.text((at + ".part").c_str());
            const std::string combine = def.text((at + ".combine").c_str());
            e.combine = combine == "importance"      ? Combine::Importance
                        : combine == "bestWeighted" ? Combine::BestWeighted
                                                     : Combine::Average;
            e.weight = static_cast<float>(def.number((at + ".weight").c_str(), 1.0));
            e.bestWeight = static_cast<float>(def.number((at + ".bestWeight").c_str(), 0.75));
            c.entries.push_back(std::move(e));
        }
        for (size_t i = 0, n = def.count("capacityFactors"); i < n; ++i) {
            const std::string at = "capacityFactors." + std::to_string(i);
            Factor f;
            const auto found = capacityOf.find(def.text((at + ".capacity").c_str()));
            f.capacity = found != capacityOf.end() ? found->second : -1;
            f.weight = static_cast<float>(def.number((at + ".weight").c_str(), 1.0));
            f.hasMax = def.has((at + ".max").c_str());
            f.max = static_cast<float>(def.number((at + ".max").c_str(), 1.0));
            c.factors.push_back(f);
        }
        if (def.has("limbs")) {
            c.limbs.present = true;
            c.limbs.core = def.text("limbs.core");
            for (size_t i = 0, n = def.count("limbs.segments"); i < n; ++i) {
                c.limbs.segments.push_back(def.textAt("limbs.segments", i));
            }
            c.limbs.digit = def.text("limbs.digit");
            c.limbs.digitWeight = static_cast<float>(def.number("limbs.digitWeight"));
        }
        if (def.has("painFactor")) {
            c.hasPain = true;
            c.painSlope = static_cast<float>(def.number("painFactor.slope"));
            c.painIntercept = static_cast<float>(def.number("painFactor.intercept", 1.0));
            c.painMax = static_cast<float>(def.number("painFactor.max", 1.0));
        }
        if (c.name == "Moving") {
            movingCapacity = static_cast<int>(capacities.size());
        } else if (c.name == "Consciousness") {
            consciousnessCapacity = static_cast<int>(capacities.size());
        }
        capacities.push_back(std::move(c));
    }

    // Hediffs and damage types, by def id.
    hediffs.assign(defs.size(), Hediff{});
    damageTypes.assign(defs.size(), DamageType{});
    environmentDefs.assign(defs.size(), EnvironmentDef{});
    diseaseDefs.assign(defs.size(), DiseaseDef{});
    if (const auto found = capacityOf.find("BloodFiltration"); found != capacityOf.end()) {
        bloodFiltrationCapacity = found->second;
    }
    addedDefs.assign(defs.size(), AddedDef{});
    for (const DefId id : defs.idsOfKind(kHealthHediffsKind)) {
        const Def& def = defs.get(id);
        Hediff& h = hediffs[id.value];
        h.painPerSeverity = static_cast<float>(def.number("injuryProps.painPerSeverity"));
        h.bleedRate = static_cast<float>(def.number("injuryProps.bleedRate"));
        h.canMerge = def.flag("injuryProps.canMerge");
        h.permanent = def.flag("permanent");
        h.tendable = def.flag("tendable", true);
        h.neverScars = def.flag("neverScars");
        h.infectionChance = static_cast<float>(def.number("infectionChance"));
        if (def.defName() == "Scar") {
            scarHediff = id;
        } else if (def.defName() == "OldWound") {
            oldWoundHediff = id;
        }
        if (def.text("hediffClass") == "AddedPart") {
            AddedDef& a = addedDefs[id.value];
            a.valid = true;
            a.part = def.text("addedPart.part");
            a.item = defs.idOf(kHealthThingsKind, def.text("addedPart.item"));
            a.efficiency = static_cast<float>(def.number("addedPart.efficiency", 1.0));
        }
        if (def.defName() == "MissingBodyPart") {
            missingHediff = id;
        } else if (def.defName() == "BloodLoss") {
            bloodLethal = static_cast<float>(def.number("lethalSeverity", 1.0));
            bloodStages = readStages(def, capacityOf, nullptr);
        } else if (def.text("hediffClass") == "Environment") {
            EnvironmentDef& e = environmentDefs[id.value];
            e.valid = true;
            e.lethal = static_cast<float>(def.number("lethalSeverity", 1.0));
            e.cause = def.defName() == "Hypothermia"  ? DeathCause::Hypothermia
                      : def.defName() == "Heatstroke" ? DeathCause::Heatstroke
                                                      : def.text("deathCause") == "Overdose"     ? DeathCause::Overdose
                                                      : def.text("deathCause") == "Withdrawal"   ? DeathCause::Withdrawal
                                                                                                 : DeathCause::Environment;
            e.stages = readStages(def, capacityOf, &e.lifeThreatening);
        } else if (def.text("hediffClass") == "Disease") {
            DiseaseDef& d = diseaseDefs[id.value];
            d.valid = true;
            d.lethal = static_cast<float>(def.number("lethalSeverity", 1.0));
            d.initialSeverity = static_cast<float>(def.number("initialSeverity", d.initialSeverity));
            d.gainPerDaySick = static_cast<float>(def.number("immunity.gainPerDaySick"));
            d.notImmunePerDay = static_cast<float>(def.number("immunity.severityPerDayNotImmune"));
            d.immunePerDay = static_cast<float>(def.number("immunity.severityPerDayImmune"));
            d.tendPerDayAtQuality1 = static_cast<float>(def.number("tend.severityPerDayAtQuality1"));
            d.tendDurationDays = std::max(0.01f, static_cast<float>(def.number("tend.durationDays", 1.0)));
            d.cureAtTotalQuality = static_cast<float>(def.number("tend.cureAtTotalQuality"));
            d.tendable = def.flag("tendable", true);
            d.keepsImmunity = def.flag("keepsImmunity", true);
            d.wound = def.text("caused") == "wound";
            d.restFallFactor = static_cast<float>(def.number("restFallFactor", 1.0));
            d.hungerRateFactor = static_cast<float>(def.number("hungerRateFactor", 1.0));
            d.makesSickThought = def.flag("makesSickThought", true);
            d.stages = readStages(def, capacityOf, &d.lifeThreatening);
            if (def.defName() == "Infection") {
                infectionHediff = id;
            }
        }
    }
    for (const DefId id : defs.idsOfKind(kHealthDamageTypesKind)) {
        const Def& def = defs.get(id);
        DamageType& d = damageTypes[id.value];
        d.valid = true;
        d.hediff = defs.idOf(kHealthHediffsKind, def.text("hediff"));
        d.hediffSolid = defs.idOf(kHealthHediffsKind, def.text("hediffSolid"));
        d.reachesInside = def.flag("reachesInside");
    }

    // Which pawn defs have a body.
    kinds.assign(defs.size(), Kind{});
    for (const DefId id : defs.idsOfKind(kHealthThingsKind)) {
        const Def& def = defs.get(id);
        const auto found = bodyIndex.find(def.text("race.body"));
        if (found != bodyIndex.end()) {
            kinds[id.value].body = found->second;
            kinds[id.value].healthScale = std::max(0.01f, static_cast<float>(def.number("race.baseHealthScale", 1.0)));
            kinds[id.value].mechanical = def.flag("race.mechanical");
        }
    }

    // Rules.
    if (const Def* rules = defs.find(kHealthRulesKind, "HealthRules")) {
        painShock = static_cast<float>(rules->number("painShockThreshold", painShock));
        naturalHealPerDay = static_cast<float>(rules->number("naturalHealPerDay", naturalHealPerDay));
        restBonus[0] = static_cast<float>(rules->number("restHealBonusPerDay.Ground", restBonus[0]));
        restBonus[1] = static_cast<float>(rules->number("restHealBonusPerDay.Bed", restBonus[1]));
        restBonus[2] = static_cast<float>(rules->number("restHealBonusPerDay.HospitalBed", restBonus[2]));
        tendBonusLow = static_cast<float>(rules->number("tendedHealBonusPerDay.atQuality0", tendBonusLow));
        tendBonusHigh = static_cast<float>(rules->number("tendedHealBonusPerDay.atQuality1", tendBonusHigh));
        healInterval = std::max<int64_t>(
            1, static_cast<int64_t>(std::llround(rules->number("healIntervalHours", 0.24) * static_cast<double>(kSecondsPerHour))));
        healFactor = static_cast<float>(rules->number("healPerIntervalFactor", healFactor));
        lostPartHpFactor = static_cast<float>(rules->number("lostPartBleedHpFactor", lostPartHpFactor));
        bloodPerDayPerBleed = static_cast<float>(rules->number("bloodLossPerDayPerBleedRate", bloodPerDayPerBleed));
        bloodRecoveryPerDay = static_cast<float>(rules->number("bloodLossRecoveryPerDay", bloodRecoveryPerDay));
        bleedMinForBloodLoss = static_cast<float>(rules->number("bleedRateMinForBloodLoss", bleedMinForBloodLoss));
        protagonistFactor = static_cast<float>(rules->number("protagonistPartHitPointsFactor", protagonistFactor));
        innerHurtsOuter = rules->flag("innerInjuryAlsoHurtsOuter", innerHurtsOuter);
        scarMax = static_cast<float>(rules->number("scarChance.max", scarMax));
        scarMinHp = static_cast<float>(rules->number("scarChance.minHpLost", scarMinHp));
        scarFullHp = static_cast<float>(rules->number("scarChance.fullAtHpLost", scarFullHp));
        scarSeverity = static_cast<float>(rules->number("scarSeverity", scarSeverity));
        oldWoundFromHp = static_cast<float>(rules->number("oldWoundFromHpLost", oldWoundFromHp));
    }
    if (const Def* rules = defs.find(kHealthRulesKind, "MedicineRules")) {
        diseaseUpdate = std::max<int64_t>(1, rules->integer("diseaseUpdateSeconds", diseaseUpdate));
        infectDelayMin = static_cast<int64_t>(std::llround(rules->number("infection.delayHoursMin", 12.0) * static_cast<double>(kSecondsPerHour)));
        infectDelayMax = std::max(infectDelayMin, static_cast<int64_t>(std::llround(rules->number("infection.delayHoursMax", 24.0) * static_cast<double>(kSecondsPerHour))));
        tendedInfectionFactor0 = static_cast<float>(rules->number("infection.tendedChanceFactorAtQuality0", tendedInfectionFactor0));
        tendedInfectionFactor1 = static_cast<float>(rules->number("infection.tendedChanceFactorAtQuality1", tendedInfectionFactor1));
        infectionCleanlinessSlope = static_cast<float>(rules->number("infection.cleanlinessSlope", infectionCleanlinessSlope));
        infectionFactorMin = static_cast<float>(rules->number("infection.chanceFactorMin", infectionFactorMin));
        infectionFactorMax = static_cast<float>(rules->number("infection.chanceFactorMax", infectionFactorMax));
        immunityGainByRest[0] = static_cast<float>(rules->number("immunityGainSpeedByRest.Ground", immunityGainByRest[0]));
        immunityGainByRest[1] = static_cast<float>(rules->number("immunityGainSpeedByRest.Bed", immunityGainByRest[1]));
        immunityGainByRest[2] = static_cast<float>(rules->number("immunityGainSpeedByRest.HospitalBed", immunityGainByRest[2]));
        immunityGainWhenTended = static_cast<float>(rules->number("immunityGainPerDayMultiplierWhenTended", immunityGainWhenTended));
        for (const std::string& key : rules->keys("diseaseOnsetMtbDays")) {
            const DefId disease = defs.idOf(kHealthHediffsKind, key);
            if (disease.valid()) {
                onsetMtbDays.emplace_back(disease, static_cast<float>(rules->number(("diseaseOnsetMtbDays." + key).c_str())));
            }
        }
    }
}

// ---- Working out a pawn's state from its injuries ----------------------------------------------

namespace {

// Per-recompute scratch: which parts are gone, how well each works, and the capacities.
struct HealthEval {
    const HealthModel& model;
    const HealthModel::Body& body;
    const Health& health;
    float hpFactor;
    std::vector<float> damage;   // hit points lost per part
    std::vector<uint8_t> gone;   // missing, or under a missing part, or under a fitted artificial part
    std::vector<uint8_t> covered;  // under a fitted artificial part: it stands for them, so they work at full
    std::vector<float> added;    // efficiency of the artificial part fitted on each part, 0 for none
    std::vector<float> value;    // capacity values, as they are worked out
    std::vector<uint8_t> state;  // 0 not yet, 1 in progress, 2 done
    float pain = 0.0f;
    float bleedPerDay = 0.0f;

    HealthEval(const HealthModel& m, const HealthModel::Body& b, const Health& h, float factor)
        : model(m), body(b), health(h), hpFactor(factor), damage(b.parts.size(), 0.0f), gone(b.parts.size(), 0),
          covered(b.parts.size(), 0), added(b.parts.size(), 0.0f), value(m.capacities.size(), 1.0f), state(m.capacities.size(), 0) {
        for (const MissingPart& missing : h.missing) {
            if (missing.part < body.parts.size()) {
                const int end = body.parts[missing.part].end;
                for (int i = missing.part; i < end; ++i) {
                    gone[static_cast<size_t>(i)] = 1;
                }
            }
        }
        // A fitted artificial part works whether or not the natural one is there, and stands for everything under it.
        for (const AddedPart& a : h.added) {
            if (a.part >= body.parts.size() || !a.hediff.valid() || a.hediff.value >= model.addedDefs.size()) {
                continue;
            }
            added[a.part] = std::max(0.0f, model.addedDefs[a.hediff.value].efficiency);
            const int end = body.parts[a.part].end;
            for (int i = a.part + 1; i < end; ++i) {
                gone[static_cast<size_t>(i)] = 1;
                covered[static_cast<size_t>(i)] = 1;
            }
        }
        for (const AddedPart& a : h.added) {
            if (a.part < body.parts.size() && added[a.part] > 0.0f) {
                gone[a.part] = 0;
            }
        }
        for (const Injury& injury : h.injuries) {
            if (injury.part < damage.size()) {
                damage[injury.part] += injury.severity;
            }
        }
    }

    float maxHp(int part) const { return body.parts[static_cast<size_t>(part)].hitPoints * hpFactor; }
    // The share of its hit points a part has left, 0 if it is gone.
    float hpFraction(int part) const {
        if (gone[static_cast<size_t>(part)] != 0) {
            return 0.0f;
        }
        const float max = maxHp(part);
        return std::clamp((max - damage[static_cast<size_t>(part)]) / max, 0.0f, 1.0f);
    }
    // How well a part works. An artificial part has its own number, which a bionic one puts above 1.
    float efficiency(int part) const {
        const size_t i = static_cast<size_t>(part);
        if (covered[i] != 0) {
            return 1.0f;
        }
        const float fraction = hpFraction(part);
        return added[i] > 0.0f ? added[i] * fraction : fraction;
    }

    // Efficiencies of every part of a def in the body, or of those under `core` (exclusive).
    std::vector<float> efficiencies(const std::string& def, int core = -1) const {
        std::vector<float> out;
        const auto found = body.byDef.find(def);
        if (found == body.byDef.end()) {
            return out;
        }
        for (const int part : found->second) {
            if (core < 0 || (part > core && part < body.parts[static_cast<size_t>(core)].end)) {
                out.push_back(efficiency(part));
            }
        }
        return out;
    }

    void workOutPainAndBleeding() {
        for (const Injury& injury : health.injuries) {
            if (injury.part >= body.parts.size() || !injury.hediff.valid() || injury.hediff.value >= model.hediffs.size()) {
                continue;
            }
            const HealthModel::Hediff& h = model.hediffs[injury.hediff.value];
            pain += injury.severity * h.painPerSeverity;
            if (!injury.tended()) {
                bleedPerDay += injury.severity * h.bleedRate * body.parts[injury.part].bleedRate;
            }
        }
        if (model.missingHediff.valid()) {
            const HealthModel::Hediff& h = model.hediffs[model.missingHediff.value];
            for (const MissingPart& missing : health.missing) {
                if (missing.part >= body.parts.size() || missing.tended()) {
                    continue;
                }
                const float lost = maxHp(missing.part);
                pain += lost * h.painPerSeverity;
                bleedPerDay += lost * model.lostPartHpFactor * h.bleedRate * body.parts[missing.part].bleedRate;
            }
        }
    }

    float capacity(int index) {
        const size_t i = static_cast<size_t>(index);
        if (state[i] == 2) {
            return value[i];
        }
        if (state[i] == 1) {
            return 1.0f;  // a cycle in the defs: ignore the edge
        }
        state[i] = 1;
        const HealthModel::Capacity& c = model.capacities[i];
        float v = 1.0f;
        if (c.limbs.present) {
            v *= limbs(c.limbs);
        }
        for (const HealthModel::Entry& entry : c.entries) {
            const std::vector<float> eff = efficiencies(entry.part);
            if (eff.empty()) {
                continue;
            }
            switch (entry.combine) {
                case HealthModel::Combine::Average:
                    v *= healthMean(eff);
                    break;
                case HealthModel::Combine::Importance:
                    v *= 1.0f + (healthMean(eff) - 1.0f) * entry.weight;
                    break;
                case HealthModel::Combine::BestWeighted: {
                    if (eff.size() == 1) {
                        v *= eff[0];
                        break;
                    }
                    const size_t best = static_cast<size_t>(std::max_element(eff.begin(), eff.end()) - eff.begin());
                    float rest = 0.0f;
                    for (size_t k = 0; k < eff.size(); ++k) {
                        rest += k == best ? 0.0f : eff[k];
                    }
                    rest /= static_cast<float>(eff.size() - 1);
                    v *= eff[best] * entry.bestWeight + rest * (1.0f - entry.bestWeight);
                    break;
                }
            }
        }
        for (const HealthModel::Factor& f : c.factors) {
            if (f.capacity < 0) {
                continue;
            }
            float other = capacity(f.capacity);
            if (f.hasMax) {
                other = std::min(other, f.max);
            }
            v *= 1.0f + (other - 1.0f) * f.weight;
        }
        if (c.hasPain) {
            v *= std::min(c.painMax, c.painIntercept + c.painSlope * pain);
        }
        // RimWorld's order (PawnCapacityUtility): the offsets of every stage that touches the capacity (blood loss, cold,
        // illness) are added up first, and the lowest of their caps comes last. Capping stage by stage let one stage's
        // offset push a capped value under the cap: an extreme infection (at most 0.1) and food poisoning (-0.1) made
        // consciousness 0 and killed the pawn (year run, seed 1).
        float offsets = 0.0f;
        float cap = std::numeric_limits<float>::max();
        const auto add = [&](const HealthModel::Stage& s) {
            for (const auto& [which, offset] : s.offsets) {
                offsets += which == index ? offset : 0.0f;
            }
            for (const auto& [which, maximum] : s.maxima) {
                cap = which == index ? std::min(cap, maximum) : cap;
            }
        };
        const int stage = model.stageOf(health.bloodLoss);
        if (stage >= 0) {
            add(model.bloodStages[static_cast<size_t>(stage)]);
        }
        for (const EnvironmentHediff& env : health.environment) {
            const HealthModel::EnvironmentDef* def = model.environmentOf(env.def);
            const int envStage = def != nullptr ? HealthModel::stageIn(def->stages, env.severity) : -1;
            if (envStage >= 0) {
                add(def->stages[static_cast<size_t>(envStage)]);
            }
        }
        for (const Disease& disease : health.diseases) {
            const HealthModel::DiseaseDef* def = model.diseaseOf(disease.def);
            const int diseaseStage = def != nullptr ? HealthModel::stageIn(def->stages, disease.severity) : -1;
            if (diseaseStage >= 0) {
                add(def->stages[static_cast<size_t>(diseaseStage)]);
            }
        }
        v = std::min(v + offsets, cap);
        value[i] = std::max(0.0f, v);
        state[i] = 2;
        return value[i];
    }

    // Each limb (a leg, an arm): its core part's efficiency times each segment's, times a share
    // for its digits (toes, fingers); the capacity is the mean over the limbs.
    float limbs(const HealthModel::Limbs& limb) const {
        const auto cores = body.byDef.find(limb.core);
        if (cores == body.byDef.end() || cores->second.empty()) {
            return 1.0f;
        }
        float sum = 0.0f;
        for (const int core : cores->second) {
            float e = efficiency(core);
            for (const std::string& segment : limb.segments) {
                const std::vector<float> eff = efficiencies(segment, core);
                if (!eff.empty()) {
                    e *= healthMean(eff);
                }
            }
            const std::vector<float> digits = efficiencies(limb.digit, core);
            if (!digits.empty()) {
                e *= (1.0f - limb.digitWeight) + limb.digitWeight * healthMean(digits);
            }
            sum += e;
        }
        return sum / static_cast<float>(cores->second.size());
    }
};

}  // namespace

// ---- HealthSystem ---------------------------------------------------------------------------------

HealthSystem::HealthSystem(Things& things) : things_(things), model_(std::make_unique<HealthModel>(things.defs())) {}

HealthSystem::~HealthSystem() = default;

namespace {

const HealthModel::Kind* healthKindOf(const HealthModel& model, const entt::registry& reg, Entity pawn) {
    if (!reg.valid(pawn) || !reg.all_of<Thing, Health>(pawn)) {
        return nullptr;
    }
    return model.kindOf(reg.get<Thing>(pawn).def);
}

float healthHpFactor(const HealthModel& model, const Health& health) {
    return (health.protagonist ? model.protagonistFactor : 1.0f) * health.stageHp;
}

// Whether a disease has reached a stage the pawn would notice.
bool healthDiseaseVisible(const HealthModel& model, const Disease& disease) {
    const HealthModel::DiseaseDef* def = model.diseaseOf(disease.def);
    const int stage = def != nullptr ? HealthModel::stageIn(def->stages, disease.severity) : -1;
    return stage >= 0 && def->stages[static_cast<size_t>(stage)].visible;
}

// A doctor can tend it now: it can be tended, shows, and no tend is still working.
bool healthDiseaseNeedsTending(const HealthModel& model, const Disease& disease, int64_t now) {
    const HealthModel::DiseaseDef* def = model.diseaseOf(disease.def);
    return def != nullptr && def->tendable && healthDiseaseVisible(model, disease) && disease.tendedUntil <= now;
}

}  // namespace

bool HealthSystem::tracks(Entity pawn) const { return healthKindOf(*model_, things_.registry(), pawn) != nullptr; }

int HealthSystem::partCount(DefId pawnDef) const {
    const HealthModel::Kind* kind = model_->kindOf(pawnDef);
    return kind != nullptr ? static_cast<int>(model_->bodies[static_cast<size_t>(kind->body)].parts.size()) : 0;
}

std::vector<std::string> HealthSystem::partLabels(DefId pawnDef) const {
    std::vector<std::string> labels;
    if (const HealthModel::Kind* kind = model_->kindOf(pawnDef)) {
        for (const HealthModel::Part& p : model_->bodies[static_cast<size_t>(kind->body)].parts) {
            labels.push_back(p.label);
        }
    }
    return labels;
}

DefId HealthSystem::damageTypeId(const std::string& name) const {
    return things_.defs().idOf(kHealthDamageTypesKind, name);
}

bool HealthSystem::incapacitated(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<Health>(pawn) && reg.get<Health>(pawn).incapacitated();
}

bool HealthSystem::kill(Entity pawn, DeathCause cause) {
    entt::registry& reg = things_.registry();
    if (!tracks(pawn) || !reg.all_of<Health>(pawn) || reg.get<Health>(pawn).dead) {
        return false;
    }
    die(pawn, reg.get<Health>(pawn), cause, true);
    return true;
}

void HealthSystem::emit(EventType type, Entity pawn, DefId def, int part, float amount, const char* message) {
    if (events_ == nullptr) {
        return;
    }
    const entt::registry& reg = things_.registry();
    Event event;
    event.type = type;
    event.thing = pawn;
    event.def = def;
    event.part = part;
    event.amount = amount;
    if (const Position* pos = reg.try_get<Position>(pawn)) {
        event.cell = cellOf(*pos);
    }
    if (message != nullptr) {
        event.message = message;
    }
    events_->push_back(std::move(event));
}

void HealthSystem::stopWalking(Entity pawn) {
    if (Mover* mover = things_.registry().try_get<Mover>(pawn)) {
        mover->path.clear();
        mover->next = 0;
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
}

void HealthSystem::die(Entity pawn, Health& health, DeathCause cause, bool announce) {
    if (health.dead) {
        return;
    }
    health.dead = true;
    health.cause = cause;
    health.downed = false;
    health.movingFactor = 0.0f;
    stopWalking(pawn);
    if (announce) {
        emit(EventType::PawnDied, pawn, DefId{}, -1, 0.0f, deathCauseName(cause));
    }
}

void HealthSystem::derive(int bodyIndex, float healthScale, bool mechanical, const HealthMods& mods, Health& health, DeathCause& cause,
                          bool& shouldBeDown, float painShockOffset) const {
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(bodyIndex)];
    HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    eval.workOutPainAndBleeding();
    // The stages of the pawn's environment hediffs (a drug high) scale pain, work speed and hunger.
    float painFactor = 1.0f;
    health.workSpeedFactor = 1.0f;
    health.hungerFactor = 1.0f;
    for (const EnvironmentHediff& env : health.environment) {
        const HealthModel::EnvironmentDef* def = model_->environmentOf(env.def);
        const int stage = def != nullptr ? HealthModel::stageIn(def->stages, env.severity) : -1;
        if (stage >= 0) {
            const HealthModel::Stage& st = def->stages[static_cast<size_t>(stage)];
            painFactor *= st.painFactor;
            health.workSpeedFactor *= st.workSpeed;
            health.hungerFactor *= st.hunger;
        }
    }
    health.pain = mechanical ? 0.0f : eval.pain / healthScale * painFactor * mods.painFactor;
    for (const Disease& disease : health.diseases) {
        const HealthModel::DiseaseDef* def = model_->diseaseOf(disease.def);
        const int stage = def != nullptr ? HealthModel::stageIn(def->stages, disease.severity) : -1;
        if (stage >= 0) {
            health.pain += def->stages[static_cast<size_t>(stage)].painOffset;
        }
    }
    health.bleedPerDay = mechanical ? 0.0f : eval.bleedPerDay;
    health.bloodStage = model_->stageOf(health.bloodLoss);
    // Pain feeds the capacities, so they get the divided value.
    eval.pain = health.pain;
    health.capacities.resize(model_->capacities.size());
    for (size_t i = 0; i < model_->capacities.size(); ++i) {
        health.capacities[i] = eval.capacity(static_cast<int>(i));
    }
    if (model_->movingCapacity >= 0 && mods.movingMax < 1.0f) {
        float& moving = health.capacities[static_cast<size_t>(model_->movingCapacity)];
        moving = std::min(moving, mods.movingMax);  // a cap, not a factor: a pawn already slower is unaffected
    }

    cause = DeathCause::None;
    for (size_t p = 0; p < body.parts.size() && cause == DeathCause::None; ++p) {
        if (body.parts[p].lethal && eval.gone[p] != 0) {
            cause = DeathCause::VitalPartLost;
        }
    }
    for (size_t c = 0; c < model_->capacities.size() && cause == DeathCause::None; ++c) {
        if (model_->capacities[c].lethalAtZero && health.capacities[c] <= 0.0f) {
            cause = DeathCause::VitalCapacity;
        }
    }
    if (cause == DeathCause::None && health.bloodLoss >= model_->bloodLethal) {
        cause = DeathCause::BloodLoss;
    }
    for (const EnvironmentHediff& env : health.environment) {
        const HealthModel::EnvironmentDef* def = model_->environmentOf(env.def);
        if (cause == DeathCause::None && def != nullptr && env.severity >= def->lethal) {
            cause = def->cause;
        }
    }
    for (const Disease& disease : health.diseases) {
        const HealthModel::DiseaseDef* def = model_->diseaseOf(disease.def);
        if (cause == DeathCause::None && def != nullptr && def->lethal > 0.0f && disease.severity >= def->lethal) {
            cause = DeathCause::Disease;
        }
    }
    shouldBeDown = health.pain > model_->painShock + painShockOffset;
    for (size_t c = 0; c < model_->capacities.size(); ++c) {
        const HealthModel::Capacity& cap = model_->capacities[c];
        shouldBeDown = shouldBeDown || (cap.hasDownedBelow && health.capacities[c] < cap.downedBelow) ||
                       (cap.hasDownedAtOrBelow && health.capacities[c] <= cap.downedAtOrBelow);
    }
}

void HealthSystem::recompute(Entity pawn, Health& health, bool announce) {
    health.dirty = false;
    const HealthModel::Kind* kind = healthKindOf(*model_, things_.registry(), pawn);
    if (kind == nullptr) {
        return;
    }
    DeathCause cause = DeathCause::None;
    bool shouldBeDown = false;
    HealthMods mods;
    if (mods_) {
        mods_(pawn, mods);
    }
    derive(kind->body, kind->healthScale, kind->mechanical, mods, health, cause, shouldBeDown,
           traits_ != nullptr ? traits_->of(pawn).painShockOffset : 0.0f);

    if (!health.dead && cause != DeathCause::None) {
        die(pawn, health, cause, announce);
    }
    if (!health.dead) {
        const bool wasDowned = health.downed;
        health.downed = shouldBeDown;
        if (shouldBeDown && !wasDowned && !restoring_) {
            stopWalking(pawn);
        }
        if (announce && shouldBeDown != wasDowned) {
            emit(shouldBeDown ? EventType::PawnDowned : EventType::PawnGotUp, pawn);
        }
    }
    health.movingFactor = 0.0f;
    if (!health.incapacitated()) {
        health.movingFactor = model_->movingCapacity >= 0
                                  ? std::clamp(health.capacities[static_cast<size_t>(model_->movingCapacity)], 0.0f, 1.0f)
                                  : 1.0f;
    }
}

void HealthSystem::refresh(Entity pawn) {
    entt::registry& reg = things_.registry();
    if (reg.valid(pawn) && reg.all_of<Health>(pawn)) {
        recompute(pawn, reg.get<Health>(pawn), false);
    }
}

void HealthSystem::refreshAll() {
    entt::registry& reg = things_.registry();
    // Saves before health section 6 do not hold `downed`, so every downed pawn looks newly downed here. It was
    // downed before the save, and whatever path it holds now it held then. A wound still waiting to be worked
    // out (`dirty`, saved) keeps its saved downed flag and stays pending, so the next step downs and announces it.
    restoring_ = true;
    for (const auto [entity, health] : reg.view<Health>().each()) {
        const bool pending = health.dirty;
        const bool downed = health.downed;
        recompute(entity, health, false);
        if (pending) {
            health.downed = downed;
            health.dirty = true;
        }
    }
    restoring_ = false;
}

// ---- Damage ---------------------------------------------------------------------------------------

namespace {

// Picks the part a hit from outside lands on: by how much of the body each outside part
// covers, parts that are gone left out (their share goes to the rest).
int healthPickOutside(const HealthModel::Body& body, const std::vector<uint8_t>& gone, Rng& rng) {
    float total = 0.0f;
    for (size_t i = 0; i < body.parts.size(); ++i) {
        total += (!body.parts[i].inside && gone[i] == 0) ? body.parts[i].ownOutside : 0.0f;
    }
    if (total <= 0.0f) {
        return 0;
    }
    const float roll = rng.unit() * total;
    float acc = 0.0f;
    int last = 0;
    for (size_t i = 0; i < body.parts.size(); ++i) {
        if (body.parts[i].inside || gone[i] != 0 || body.parts[i].ownOutside <= 0.0f) {
            continue;
        }
        acc += body.parts[i].ownOutside;
        last = static_cast<int>(i);
        if (roll < acc) {
            return last;
        }
    }
    return last;
}

// From a part hit, goes in to an organ or bone under it: each step in has the chance of the
// inside children's coverage added up.
int healthGoInside(const HealthModel::Body& body, const std::vector<uint8_t>& gone, int part, Rng& rng) {
    for (;;) {
        float sum = 0.0f;
        for (const int child : body.parts[static_cast<size_t>(part)].children) {
            const HealthModel::Part& c = body.parts[static_cast<size_t>(child)];
            sum += (c.inside && gone[static_cast<size_t>(child)] == 0) ? c.coverage : 0.0f;
        }
        if (sum <= 0.0f) {
            return part;
        }
        const float roll = rng.unit();
        if (roll >= sum) {
            return part;
        }
        float acc = 0.0f;
        int next = part;
        for (const int child : body.parts[static_cast<size_t>(part)].children) {
            const HealthModel::Part& c = body.parts[static_cast<size_t>(child)];
            if (!c.inside || gone[static_cast<size_t>(child)] != 0) {
                continue;
            }
            acc += c.coverage;
            next = child;
            if (roll < acc) {
                break;
            }
        }
        part = next;
    }
}

}  // namespace

bool HealthSystem::applyDamage(Entity pawn, DefId damageType, float amount, int part, Rng& rng) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || reg.get<Health>(pawn).dead || !(amount > 0.0f) || !damageType.valid() ||
        damageType.value >= model_->damageTypes.size() || !model_->damageTypes[damageType.value].valid) {
        return false;
    }
    const HealthModel::DamageType& type = model_->damageTypes[damageType.value];
    Health& health = reg.get<Health>(pawn);
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));

    int target = part;
    if (part >= 0) {
        if (static_cast<size_t>(part) >= body.parts.size() || eval.gone[static_cast<size_t>(part)] != 0) {
            return false;
        }
    } else {
        target = healthPickOutside(body, eval.gone, rng);
        if (type.reachesInside) {
            target = healthGoInside(body, eval.gone, target, rng);
        }
    }

    return hitPart(pawn, damageType, amount, target, part < 0);
}

bool HealthSystem::applyHit(Entity pawn, DefId damageType, float amount, int part) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || reg.get<Health>(pawn).dead || !(amount > 0.0f) || !damageType.valid() ||
        damageType.value >= model_->damageTypes.size() || !model_->damageTypes[damageType.value].valid || part < 0) {
        return false;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const Health& health = reg.get<Health>(pawn);
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    if (static_cast<size_t>(part) >= body.parts.size() || eval.gone[static_cast<size_t>(part)] != 0) {
        return false;
    }
    return hitPart(pawn, damageType, amount, part, true);
}

int HealthSystem::goInside(Entity pawn, DefId damageType, int part, Rng& rng) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || part < 0 || !damageType.valid() || damageType.value >= model_->damageTypes.size() ||
        !model_->damageTypes[damageType.value].reachesInside) {
        return part;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const Health& health = reg.get<Health>(pawn);
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    if (static_cast<size_t>(part) >= body.parts.size() || eval.gone[static_cast<size_t>(part)] != 0) {
        return part;
    }
    return healthGoInside(body, eval.gone, part, rng);
}

std::vector<BodyPartInfo> HealthSystem::bodyParts(DefId pawnDef) const {
    std::vector<BodyPartInfo> out;
    if (const HealthModel::Kind* kind = model_->kindOf(pawnDef)) {
        for (const HealthModel::Part& p : model_->bodies[static_cast<size_t>(kind->body)].parts) {
            out.push_back(BodyPartInfo{p.label, p.parent, p.inside, p.ownOutside, p.groups, p.def});
        }
    }
    return out;
}

bool HealthSystem::hitPart(Entity pawn, DefId damageType, float amount, int target, bool hurtOuter) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    const HealthModel::DamageType& type = model_->damageTypes[damageType.value];
    Health& health = reg.get<Health>(pawn);
    if (const PawnGenes* genes = reg.try_get<PawnGenes>(pawn)) {
        amount *= genes->effects.damageTaken;  // docs/design/genes.md
    }
    if (traits_ != nullptr) {
        amount *= traits_->of(pawn).damageFactor;  // Tough
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const auto hediffFor = [&](int p) {
        const DefId solid = type.hediffSolid;
        return body.parts[static_cast<size_t>(p)].solid && solid.valid() ? solid : type.hediff;
    };
    bool hurt = damagePart(pawn, health, target, hediffFor(target), amount);
    if (hurt && hurtOuter && model_->innerHurtsOuter && body.parts[static_cast<size_t>(target)].inside) {
        int outer = body.parts[static_cast<size_t>(target)].parent;
        while (outer > 0 && body.parts[static_cast<size_t>(outer)].inside) {
            outer = body.parts[static_cast<size_t>(outer)].parent;
        }
        // The covering part may have just been destroyed along with the part hit.
        const HealthEval after(*model_, body, health, healthHpFactor(*model_, health));
        if (outer >= 0 && after.gone[static_cast<size_t>(outer)] == 0) {
            damagePart(pawn, health, outer, hediffFor(outer), amount);
        }
    }
    recompute(pawn, health, true);
    return hurt;
}

bool HealthSystem::damagePart(Entity pawn, Health& health, int part, DefId hediff, float amount) {
    const HealthModel::Kind* kind = healthKindOf(*model_, things_.registry(), pawn);
    if (kind == nullptr || !hediff.valid()) {
        return false;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    const HealthModel::Part& p = body.parts[static_cast<size_t>(part)];
    const float remaining = eval.maxHp(part) - eval.damage[static_cast<size_t>(part)];
    if (remaining <= 0.0f) {
        return false;
    }
    if (amount >= remaining) {
        if (p.destroyable) {
            destroyPart(pawn, health, part, amount);
            return true;
        }
        // A part that can't be destroyed (skull, ribcage...) is left with one hit point.
        amount = remaining - 1.0f;
        if (amount <= 0.0f) {
            return false;
        }
    }
    bool merged = false;
    if (hediff.value < model_->hediffs.size() && model_->hediffs[hediff.value].canMerge) {
        for (Injury& injury : health.injuries) {
            if (injury.part == part && injury.hediff == hediff && !injury.tended()) {
                injury.severity += amount;
                merged = true;
                break;
            }
        }
    }
    if (!merged) {
        Injury injury;
        injury.part = static_cast<uint16_t>(part);
        injury.hediff = hediff;
        injury.severity = amount;
        health.injuries.push_back(injury);
        rollInfection(pawn, health, health.injuries.back());
    }
    health.dirty = true;
    emit(EventType::PawnInjured, pawn, hediff, part, amount);
    return true;
}

void HealthSystem::destroyPart(Entity pawn, Health& health, int part, float amount) {
    const HealthModel::Kind* kind = healthKindOf(*model_, things_.registry(), pawn);
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const int end = body.parts[static_cast<size_t>(part)].end;
    const auto inside = [&](int p) { return p >= part && p < end; };
    health.injuries.erase(std::remove_if(health.injuries.begin(), health.injuries.end(),
                                         [&](const Injury& i) { return inside(i.part); }),
                          health.injuries.end());
    health.missing.erase(std::remove_if(health.missing.begin(), health.missing.end(),
                                        [&](const MissingPart& m) { return inside(m.part); }),
                         health.missing.end());
    health.added.erase(std::remove_if(health.added.begin(), health.added.end(),
                                      [&](const AddedPart& a) { return inside(a.part); }),
                       health.added.end());
    health.diseases.erase(std::remove_if(health.diseases.begin(), health.diseases.end(),
                                         [&](const Disease& d) { return d.part >= 0 && inside(d.part); }),
                          health.diseases.end());
    MissingPart lost;
    lost.part = static_cast<uint16_t>(part);
    health.missing.push_back(lost);
    health.dirty = true;
    emit(EventType::PawnPartLost, pawn, model_->missingHediff, part, amount);
}

int HealthSystem::tendAll(Entity pawn, float quality) {
    entt::registry& reg = things_.registry();
    if (healthKindOf(*model_, reg, pawn) == nullptr) {
        return 0;
    }
    Health& health = reg.get<Health>(pawn);
    if (health.dead) {
        return 0;
    }
    quality = std::max(0.0f, quality);
    int tended = 0;
    const int64_t now = clock_ != nullptr ? *clock_ : 0;
    for (size_t i = 0; i < health.injuries.size(); ++i) {
        Injury& injury = health.injuries[i];
        if (!injury.tended() && !model_->hediffs[injury.hediff.value].permanent) {
            injury.tendQuality = quality;
            ++tended;
            // A wound that was going to turn infected may be cleaned in time: the chance falls by the tend quality.
            if (injury.infectAt >= 0) {
                Rng rng(splitMix64(seed_ ^ (static_cast<uint64_t>(entt::to_integral(pawn)) * 0x9E3779B97F4A7C15ULL) ^
                                   (static_cast<uint64_t>(i) << 40) ^ static_cast<uint64_t>(now) ^ 0x7E4D0A11ULL));
                const float q = std::min(1.0f, quality);
                const float stillInfected = model_->tendedInfectionFactor0 + (model_->tendedInfectionFactor1 - model_->tendedInfectionFactor0) * q;
                if (rng.unit() >= stillInfected) {
                    injury.infectAt = -1;
                }
            }
        }
    }
    for (size_t i = 0; i < health.diseases.size();) {
        Disease& disease = health.diseases[i];
        if (!healthDiseaseNeedsTending(*model_, disease, now)) {
            ++i;
            continue;
        }
        const HealthModel::DiseaseDef* def = model_->diseaseOf(disease.def);
        disease.tendQuality = quality;
        disease.tendedUntil = now + static_cast<int64_t>(std::llround(static_cast<double>(def->tendDurationDays) * static_cast<double>(kSecondsPerDay)));
        disease.tendTotal += quality;
        ++tended;
        if (def->cureAtTotalQuality > 0.0f && disease.tendTotal >= def->cureAtTotalQuality) {
            cureDisease(pawn, health, i);  // the next one is at i now
            continue;
        }
        ++i;
    }
    for (MissingPart& missing : health.missing) {
        if (!missing.tended()) {
            missing.tendQuality = quality;
            ++tended;
        }
    }
    if (tended > 0) {
        recompute(pawn, health, true);
    }
    return tended;
}

bool HealthSystem::needsTending(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    if (health == nullptr || health->dead || healthKindOf(*model_, reg, pawn) == nullptr) {
        return false;
    }
    for (const Injury& injury : health->injuries) {
        const HealthModel::Hediff& h = model_->hediffs[injury.hediff.value];
        if (!injury.tended() && h.tendable && !h.permanent) {
            return true;
        }
    }
    if (std::any_of(health->missing.begin(), health->missing.end(), [](const MissingPart& m) { return !m.tended(); })) {
        return true;
    }
    const int64_t now = clock_ != nullptr ? *clock_ : 0;
    return std::any_of(health->diseases.begin(), health->diseases.end(),
                       [&](const Disease& d) { return healthDiseaseNeedsTending(*model_, d, now); });
}

bool HealthSystem::needsRest(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    if (health == nullptr || health->dead || healthKindOf(*model_, reg, pawn) == nullptr) {
        return false;
    }
    if (health->bleedPerDay > 0.0f) {
        return true;
    }
    if (std::any_of(health->diseases.begin(), health->diseases.end(), [&](const Disease& d) { return healthDiseaseVisible(*model_, d); })) {
        return true;
    }
    return std::any_of(health->injuries.begin(), health->injuries.end(), [&](const Injury& injury) {
        return injury.tended() && !model_->hediffs[injury.hediff.value].permanent;
    });
}

bool HealthSystem::urgentlyNeedsTending(Entity pawn) const {
    const Health* health = things_.registry().valid(pawn) ? things_.registry().try_get<Health>(pawn) : nullptr;
    if (health == nullptr || !needsTending(pawn)) {
        return false;
    }
    if (health->bleedPerDay > 0.0f) {
        return true;
    }
    // A disease at a life-threatening stage that nobody is tending counts as urgent too.
    const int64_t now = clock_ != nullptr ? *clock_ : 0;
    return std::any_of(health->diseases.begin(), health->diseases.end(), [&](const Disease& d) {
        const HealthModel::DiseaseDef* def = model_->diseaseOf(d.def);
        const int stage = def != nullptr ? HealthModel::stageIn(def->stages, d.severity) : -1;
        return stage >= 0 && static_cast<size_t>(stage) < def->lifeThreatening.size() &&
               def->lifeThreatening[static_cast<size_t>(stage)] != 0 && healthDiseaseNeedsTending(*model_, d, now);
    });
}

// ---- Environment hediffs and injuries without a damage type ------------------------------------------

float HealthSystem::environmentSeverity(Entity pawn, DefId hediff) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Health>(pawn)) {
        return 0.0f;
    }
    for (const EnvironmentHediff& env : reg.get<Health>(pawn).environment) {
        if (env.def == hediff) {
            return env.severity;
        }
    }
    return 0.0f;
}

float HealthSystem::environmentLethalSeverity(DefId hediff) const {
    const HealthModel::EnvironmentDef* def = model_->environmentOf(hediff);
    return def != nullptr ? def->lethal : 0.0f;
}

bool HealthSystem::setEnvironmentSeverity(Entity pawn, DefId hediff, float severity) {
    entt::registry& reg = things_.registry();
    const HealthModel::EnvironmentDef* def = model_->environmentOf(hediff);
    if (def == nullptr || !tracks(pawn)) {
        return false;
    }
    Health& health = reg.get<Health>(pawn);
    if (health.dead) {
        return false;
    }
    severity = std::max(0.0f, severity);
    auto it = std::find_if(health.environment.begin(), health.environment.end(),
                           [&](const EnvironmentHediff& e) { return e.def == hediff; });
    const bool had = it != health.environment.end();
    const float before = had ? it->severity : 0.0f;
    if (severity == before) {
        return true;
    }
    // A hediff that is not there has no stage, whatever the first stage's minimum.
    const int stageBefore = had ? HealthModel::stageIn(def->stages, before) : -1;
    if (severity <= 0.0f) {
        health.environment.erase(it);
    } else if (it != health.environment.end()) {
        it->severity = severity;
    } else {
        // Kept in def id order, so the state is the same however it came about.
        const auto at = std::find_if(health.environment.begin(), health.environment.end(),
                                     [&](const EnvironmentHediff& e) { return e.def.value > hediff.value; });
        health.environment.insert(at, EnvironmentHediff{hediff, severity});
    }
    // The capacities change only when the stage does, or the hediff becomes deadly.
    const int stageAfter = severity > 0.0f ? HealthModel::stageIn(def->stages, severity) : -1;
    if (stageAfter != stageBefore || (severity >= def->lethal) != (before >= def->lethal)) {
        health.dirty = true;
    }
    return true;
}

bool HealthSystem::addInjury(Entity pawn, int part, DefId hediff, float severity) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || !hediff.valid() || hediff.value >= model_->hediffs.size() || severity <= 0.0f) {
        return false;
    }
    Health& health = reg.get<Health>(pawn);
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    if (health.dead || part < 0 || static_cast<size_t>(part) >= body.parts.size()) {
        return false;
    }
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    if (eval.gone[static_cast<size_t>(part)] != 0) {
        return false;
    }
    health.injuries.push_back(Injury{static_cast<uint16_t>(part), hediff, severity, -1.0f});
    recompute(pawn, health, true);
    return true;
}

std::vector<int> HealthSystem::partsOfDef(DefId pawnDef, const std::string& partDef) const {
    std::vector<int> out;
    if (const HealthModel::Kind* kind = model_->kindOf(pawnDef)) {
        const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
        for (size_t i = 0; i < body.parts.size(); ++i) {
            if (body.parts[i].def == partDef) {
                out.push_back(static_cast<int>(i));
            }
        }
    }
    return out;
}

// A healed injury may leave a scar (or, if it was a big one, an old wound that aches): the chance grows with the
// hit points it cost at its worst (HealthRules.scarChance). Bruises and the like never scar.
void HealthSystem::leaveScar(Entity pawn, Health& health, const Injury& healed, Rng& rng) {
    const HealthModel::Hediff& h = model_->hediffs[healed.hediff.value];
    if (h.neverScars || h.permanent || !model_->scarHediff.valid()) {
        return;
    }
    const float span = std::max(0.001f, model_->scarFullHp - model_->scarMinHp);
    const float chance = model_->scarMax * std::clamp((healed.peak - model_->scarMinHp) / span, 0.0f, 1.0f);
    if (chance <= 0.0f || rng.unit() >= chance) {
        return;
    }
    const DefId def = healed.peak >= model_->oldWoundFromHp && model_->oldWoundHediff.valid() ? model_->oldWoundHediff
                                                                                              : model_->scarHediff;
    Injury scar;
    scar.part = healed.part;
    scar.hediff = def;
    scar.severity = model_->scarSeverity;
    scar.peak = model_->scarSeverity;
    health.injuries.push_back(scar);
    emit(EventType::PawnScarred, pawn, def, healed.part, scar.severity);

    // V1: a scar on the skin shows. The part's own def and the side come from its chain of parents.
    if (PawnLooks* looks = things_.registry().try_get<PawnLooks>(pawn)) {
        const HealthModel::Kind* kind = healthKindOf(*model_, things_.registry(), pawn);
        if (kind != nullptr) {
            const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
            BodyRegion region = BodyRegion::Torso;
            bool left = false;
            bool found = false;
            for (int p = healed.part; p >= 0 && !found; p = body.parts[static_cast<size_t>(p)].parent) {
                const HealthModel::Part& part = body.parts[static_cast<size_t>(p)];
                left = left || part.label.rfind("left ", 0) == 0;
                found = bodyRegionOfPart(part.def, left, region) && !part.inside;
                if (!found && part.inside) {
                    break;  // an organ or bone: nothing shows on the skin
                }
            }
            if (found) {
                addScarMark(things_.defs(), *looks, region, things_.defs().get(healed.hediff).defName(), rng);
            }
        }
    }
}

// ---- Infections and diseases (H2) --------------------------------------------------------------------

namespace {

// The pawn's immunity record for a disease def, made (at 0) if it has none. Kept sorted by def id.
Immunity& healthImmunityOf(Health& health, DefId disease) {
    const auto at = std::lower_bound(health.immunities.begin(), health.immunities.end(), disease,
                                     [](const Immunity& i, DefId d) { return i.def.value < d.value; });
    if (at != health.immunities.end() && at->def == disease) {
        return *at;
    }
    return *health.immunities.insert(at, Immunity{disease, 0.0f});
}

const Immunity* healthImmunityFind(const Health& health, DefId disease) {
    const auto at = std::lower_bound(health.immunities.begin(), health.immunities.end(), disease,
                                     [](const Immunity& i, DefId d) { return i.def.value < d.value; });
    return at != health.immunities.end() && at->def == disease ? &*at : nullptr;
}

}  // namespace

void HealthSystem::rollInfection(Entity pawn, Health& health, Injury& injury) {
    if (clock_ == nullptr || !model_->infectionHediff.valid()) {
        return;
    }
    const HealthModel::Hediff& h = model_->hediffs[injury.hediff.value];
    if (h.infectionChance <= 0.0f || h.permanent) {
        return;
    }
    float factor = 1.0f;
    if (cleanliness_) {
        factor = std::clamp(1.0f + model_->infectionCleanlinessSlope * cleanliness_(pawn), model_->infectionFactorMin,
                            model_->infectionFactorMax);
    }
    // The roll comes from the seed, the pawn, the second and the wound, not from the shared stream.
    Rng rng(splitMix64(seed_ ^ (static_cast<uint64_t>(entt::to_integral(pawn)) * 0x9E3779B97F4A7C15ULL) ^
                       static_cast<uint64_t>(*clock_) ^ (static_cast<uint64_t>(injury.part) << 48) ^
                       (static_cast<uint64_t>(health.injuries.size()) << 32) ^ 0x1FEC7104ULL));
    if (rng.unit() >= h.infectionChance * factor) {
        return;
    }
    const uint32_t span = static_cast<uint32_t>(model_->infectDelayMax - model_->infectDelayMin);
    injury.infectAt = *clock_ + model_->infectDelayMin + static_cast<int64_t>(span > 0 ? rng.below(span + 1) : 0);
}

void HealthSystem::updateDiseases(Entity pawn, Health& health, int64_t now) {
    // Wounds that were going to turn infected, and haven't been cleaned, do now.
    for (size_t i = 0; i < health.injuries.size(); ++i) {
        if (health.injuries[i].infectAt >= 0 && now >= health.injuries[i].infectAt) {
            const int part = health.injuries[i].part;
            health.injuries[i].infectAt = -1;
            contract(pawn, model_->infectionHediff, -1.0f, part);
            if (health.dead) {
                return;
            }
        }
    }
    if (health.diseases.empty()) {
        return;
    }

    const float dt = static_cast<float>(model_->diseaseUpdate) / static_cast<float>(kSecondsPerDay);
    float gainSpeed = model_->immunityGainByRest[static_cast<size_t>(health.rest)];
    if (model_->bloodFiltrationCapacity >= 0 && static_cast<size_t>(model_->bloodFiltrationCapacity) < health.capacities.size()) {
        gainSpeed *= health.capacities[static_cast<size_t>(model_->bloodFiltrationCapacity)];
    }
    bool changed = false;
    std::vector<DefId> gained;  // the defs whose immunity has risen this update: it rises once, however many wounds
    for (size_t i = 0; i < health.diseases.size();) {
        Disease& disease = health.diseases[i];
        const HealthModel::DiseaseDef* def = model_->diseaseOf(disease.def);
        if (def == nullptr) {
            health.diseases.erase(health.diseases.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            continue;
        }
        const bool tended = disease.tendedUntil > now && disease.tendQuality >= 0.0f;
        if (!tended) {
            disease.tendQuality = -1.0f;
        }
        Immunity& record = healthImmunityOf(health, disease.def);
        if (std::find(gained.begin(), gained.end(), disease.def) == gained.end()) {
            gained.push_back(disease.def);
            record.level = std::min(1.0f, record.level + def->gainPerDaySick * gainSpeed * dt * (tended ? model_->immunityGainWhenTended : 1.0f));
        }
        float rate = record.level >= 1.0f ? def->immunePerDay : def->notImmunePerDay;
        if (tended) {
            rate += def->tendPerDayAtQuality1 * std::min(disease.tendQuality, 1.3f);
        }
        const int before = HealthModel::stageIn(def->stages, disease.severity);
        disease.severity = std::min(1.0f, disease.severity + rate * dt);
        if (disease.severity <= 0.0f) {
            cureDisease(pawn, health, i);
            changed = true;
            continue;
        }
        changed = changed || before != HealthModel::stageIn(def->stages, disease.severity) ||
                  (def->lethal > 0.0f && disease.severity >= def->lethal);
        ++i;
    }
    if (changed) {
        recompute(pawn, health, true);
    }
}

void HealthSystem::cureDisease(Entity pawn, Health& health, size_t index) {
    const DefId cured = health.diseases[index].def;
    const int part = health.diseases[index].part;
    const HealthModel::DiseaseDef* def = model_->diseaseOf(cured);
    health.diseases.erase(health.diseases.begin() + static_cast<std::ptrdiff_t>(index));
    const bool another = std::any_of(health.diseases.begin(), health.diseases.end(), [&](const Disease& d) { return d.def == cured; });
    if (!another) {
        const auto at = std::find_if(health.immunities.begin(), health.immunities.end(), [&](const Immunity& m) { return m.def == cured; });
        if (at != health.immunities.end() && (def == nullptr || !def->keepsImmunity || at->level <= 0.0f)) {
            health.immunities.erase(at);
        }
    }
    emit(EventType::PawnRecovered, pawn, cured, part);
    health.dirty = true;
}

bool HealthSystem::contract(Entity pawn, DefId disease, float severity, int part) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    const HealthModel::DiseaseDef* def = model_->diseaseOf(disease);
    if (kind == nullptr || def == nullptr) {
        return false;
    }
    Health& health = reg.get<Health>(pawn);
    if (health.dead) {
        return false;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    if (def->wound) {
        if (part < 0 || static_cast<size_t>(part) >= body.parts.size()) {
            return false;
        }
        for (const MissingPart& missing : health.missing) {
            if (part >= missing.part && part < body.parts[missing.part].end) {
                return false;  // nothing there to infect
            }
        }
    } else {
        part = -1;
    }
    if (const Immunity* record = healthImmunityFind(health, disease); record != nullptr && record->level >= 1.0f) {
        return false;
    }
    if (std::any_of(health.diseases.begin(), health.diseases.end(), [&](const Disease& d) { return d.def == disease && d.part == part; })) {
        return false;
    }
    const float start = severity < 0.0f ? def->initialSeverity : std::min(severity, 1.0f);
    if (start <= 0.0f) {
        return false;
    }
    Disease sickness;
    sickness.def = disease;
    sickness.severity = start;
    sickness.part = part;
    const auto at = std::find_if(health.diseases.begin(), health.diseases.end(), [&](const Disease& d) {
        return d.def.value > disease.value || (d.def == disease && d.part > part);
    });
    health.diseases.insert(at, sickness);
    healthImmunityOf(health, disease);
    emit(EventType::PawnSick, pawn, disease, part, start);
    health.dirty = true;
    recompute(pawn, health, true);
    return true;
}

std::vector<Entity> HealthSystem::outbreak(DefId disease, std::vector<Entity> candidates, int victims, Rng& rng) {
    std::vector<Entity> caught;
    const entt::registry& reg = things_.registry();
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](Entity pawn) {
                                        const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
                                        return health == nullptr || health->dead || !tracks(pawn) || immunity(pawn, disease) >= 1.0f ||
                                               diseaseSeverity(pawn, disease) > 0.0f;
                                    }),
                     candidates.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (size_t i = 0; i < candidates.size() && static_cast<int>(caught.size()) < victims; ++i) {
        std::swap(candidates[i], candidates[i + rng.below(static_cast<uint32_t>(candidates.size() - i))]);
        if (contract(candidates[i], disease)) {
            caught.push_back(candidates[i]);
        }
    }
    return caught;
}

DefId HealthSystem::diseaseId(const std::string& name) const {
    const DefId id = things_.defs().idOf(kHealthHediffsKind, name);
    return model_->diseaseOf(id) != nullptr ? id : DefId{};
}

bool HealthSystem::isDisease(DefId hediff) const { return model_->diseaseOf(hediff) != nullptr; }

float HealthSystem::diseaseSeverity(Entity pawn, DefId disease) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    float worst = 0.0f;
    if (health != nullptr) {
        for (const Disease& d : health->diseases) {
            if (d.def == disease) {
                worst = std::max(worst, d.severity);
            }
        }
    }
    return worst;
}

float HealthSystem::immunity(Entity pawn, DefId disease) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    const Immunity* record = health != nullptr ? healthImmunityFind(*health, disease) : nullptr;
    return record != nullptr ? record->level : 0.0f;
}

bool HealthSystem::setImmunity(Entity pawn, DefId disease, float level) {
    entt::registry& reg = things_.registry();
    if (!tracks(pawn) || model_->diseaseOf(disease) == nullptr) {
        return false;
    }
    Health& health = reg.get<Health>(pawn);
    level = std::clamp(level, 0.0f, 1.0f);
    if (level <= 0.0f && !std::any_of(health.diseases.begin(), health.diseases.end(), [&](const Disease& d) { return d.def == disease; })) {
        health.immunities.erase(std::remove_if(health.immunities.begin(), health.immunities.end(),
                                               [&](const Immunity& m) { return m.def == disease; }),
                                health.immunities.end());
        return true;
    }
    healthImmunityOf(health, disease).level = level;
    return true;
}

bool HealthSystem::sick(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    return health != nullptr && !health->dead &&
           std::any_of(health->diseases.begin(), health->diseases.end(), [&](const Disease& d) { return healthDiseaseVisible(*model_, d); });
}

float HealthSystem::foodRateFactor(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    float factor = 1.0f;
    if (health != nullptr) {
        for (const Disease& d : health->diseases) {
            const HealthModel::DiseaseDef* def = model_->diseaseOf(d.def);
            factor *= def != nullptr && healthDiseaseVisible(*model_, d) ? def->hungerRateFactor : 1.0f;
        }
    }
    return factor;
}

float HealthSystem::restFallFactor(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    float factor = 1.0f;
    if (health != nullptr) {
        for (const Disease& d : health->diseases) {
            const HealthModel::DiseaseDef* def = model_->diseaseOf(d.def);
            factor *= def != nullptr && healthDiseaseVisible(*model_, d) ? def->restFallFactor : 1.0f;
        }
    }
    return factor;
}

float HealthSystem::onsetMtbDays(DefId disease) const {
    for (const auto& [def, days] : model_->onsetMtbDays) {
        if (def == disease) {
            return days;
        }
    }
    return 0.0f;
}

void HealthSystem::registerSituations(MoodSystem& mood) const {
    mood.addSituation("illness", [this](Entity pawn) {
        const entt::registry& reg = things_.registry();
        const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
        if (health == nullptr || health->dead) {
            return -1;
        }
        for (const Disease& d : health->diseases) {
            const HealthModel::DiseaseDef* def = model_->diseaseOf(d.def);
            if (def != nullptr && def->makesSickThought && healthDiseaseVisible(*model_, d)) {
                return 0;
            }
        }
        return -1;
    });
}

// ---- Each game second -------------------------------------------------------------------------

void HealthSystem::step(int64_t seconds, uint64_t seed) {
    entt::registry& reg = things_.registry();
    active_.clear();
    for (const auto [entity, health] : reg.view<Health>().each()) {
        if (!health.dead && (health.dirty || !health.injuries.empty() || !health.missing.empty() || health.bloodLoss > 0.0f ||
                             !health.diseases.empty())) {
            active_.push_back(entity);
        }
    }
    // Events come out in id order, whatever order the registry holds things in.
    std::sort(active_.begin(), active_.end());

    constexpr float kSecondsPerDayF = static_cast<float>(kSecondsPerDay);
    for (const Entity pawn : active_) {
        Health& health = reg.get<Health>(pawn);
        if (health.dirty) {
            recompute(pawn, health, true);
        }
        if (health.dead) {
            continue;
        }

        // Bleeding: blood loss rises while the total bleed rate is at least the minimum, and
        // recovers slowly when it is not.
        if (health.bleedPerDay >= model_->bleedMinForBloodLoss) {
            health.bloodLoss += health.bleedPerDay * model_->bloodPerDayPerBleed / kSecondsPerDayF;
        } else if (health.bloodLoss > 0.0f) {
            health.bloodLoss = std::max(0.0f, health.bloodLoss - model_->bloodRecoveryPerDay / kSecondsPerDayF);
        }
        if (model_->stageOf(health.bloodLoss) != health.bloodStage || health.bloodLoss >= model_->bloodLethal) {
            recompute(pawn, health, true);
            if (health.dead) {
                continue;
            }
        }

        // Wounds turning infected and diseases moving on, once an update interval (each pawn at its own offset).
        if ((!health.diseases.empty() || !health.injuries.empty()) &&
            (seconds + static_cast<int64_t>(entt::to_integral(pawn))) % model_->diseaseUpdate == 0) {
            updateDiseases(pawn, health, seconds);
            if (health.dead) {
                continue;
            }
        }

        // Natural healing: every healInterval seconds (offset by the pawn's id so they don't all
        // heal in the same step), one random injury heals by the day's rate times a small factor.
        const int64_t interval = model_->healInterval;
        // Scars and old wounds never heal.
        healable_.clear();
        for (size_t i = 0; i < health.injuries.size(); ++i) {
            if (!model_->hediffs[health.injuries[i].hediff.value].permanent) {
                healable_.push_back(i);
            }
        }
        if (!healable_.empty() && (seconds + static_cast<int64_t>(entt::to_integral(pawn) % static_cast<uint32_t>(interval))) % interval == 0) {
            Rng rng(splitMix64(seed ^ (static_cast<uint64_t>(entt::to_integral(pawn)) * 0x9E3779B97F4A7C15ULL) ^
                               static_cast<uint64_t>(seconds)));
            const size_t pick = healable_[rng.below(static_cast<uint32_t>(healable_.size()))];
            Injury& injury = health.injuries[pick];
            injury.peak = std::max(injury.peak, injury.severity);
            float rate = model_->naturalHealPerDay + model_->restBonus[static_cast<size_t>(health.rest)];
            if (injury.tended()) {
                const float q = std::clamp(injury.tendQuality, 0.0f, 1.0f);
                rate += model_->tendBonusLow + (model_->tendBonusHigh - model_->tendBonusLow) * q;
            }
            if (const PawnGenes* genes = reg.try_get<PawnGenes>(pawn)) {
                rate *= genes->effects.woundHealing;
            }
            injury.severity -= rate * model_->healFactor;
            if (injury.severity <= 0.0f) {
                const Injury healed = injury;
                health.injuries.erase(health.injuries.begin() + static_cast<std::ptrdiff_t>(pick));
                leaveScar(pawn, health, healed, rng);
            }
            recompute(pawn, health, true);
        }
    }
}

// ---- Reading ---------------------------------------------------------------------------------------

float HealthSystem::partMaxHitPoints(Entity pawn, int part) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr) {
        return 0.0f;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    if (part < 0 || static_cast<size_t>(part) >= body.parts.size()) {
        return 0.0f;
    }
    return body.parts[static_cast<size_t>(part)].hitPoints * healthHpFactor(*model_, reg.get<Health>(pawn));
}

// ---- Surgery (task H3) ------------------------------------------------------------------------------

PartCondition HealthSystem::partCondition(Entity pawn, int part, bool* ok) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (ok != nullptr) {
        *ok = false;
    }
    if (kind == nullptr) {
        return PartCondition::Whole;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    if (part < 0 || static_cast<size_t>(part) >= body.parts.size()) {
        return PartCondition::Whole;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    const Health& health = reg.get<Health>(pawn);
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    const size_t i = static_cast<size_t>(part);
    if (eval.covered[i] != 0) {
        return PartCondition::Covered;
    }
    if (eval.added[i] > 0.0f) {
        return PartCondition::Fitted;
    }
    if (eval.gone[i] == 0) {
        return PartCondition::Whole;
    }
    const bool exact = std::any_of(health.missing.begin(), health.missing.end(),
                                   [part](const MissingPart& m) { return m.part == part; });
    return exact ? PartCondition::Missing : PartCondition::Under;
}

std::string HealthSystem::partDef(Entity pawn, int part) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || part < 0) {
        return std::string();
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    return static_cast<size_t>(part) < body.parts.size() ? body.parts[static_cast<size_t>(part)].def : std::string();
}

bool HealthSystem::partIsLimb(Entity pawn, int part) const {
    const std::string def = partDef(pawn, part);
    if (def.empty()) {
        return false;
    }
    for (const HealthModel::Capacity& c : model_->capacities) {
        if (!c.limbs.present) {
            continue;
        }
        if (def == c.limbs.core || def == c.limbs.digit ||
            std::find(c.limbs.segments.begin(), c.limbs.segments.end(), def) != c.limbs.segments.end()) {
            return true;
        }
    }
    return false;
}

bool HealthSystem::canRemovePart(Entity pawn, int part) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || reg.get<Health>(pawn).dead || partCondition(pawn, part) != PartCondition::Whole) {
        return false;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const HealthModel::Part& p = body.parts[static_cast<size_t>(part)];
    if (!p.destroyable) {
        return false;
    }
    for (int i = part; i < p.end; ++i) {
        if (body.parts[static_cast<size_t>(i)].lethal) {
            return false;
        }
    }
    return true;
}

DefId HealthSystem::addedAt(Entity pawn, int part) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Health>(pawn)) {
        return DefId{};
    }
    for (const AddedPart& a : reg.get<Health>(pawn).added) {
        if (a.part == part) {
            return a.hediff;
        }
    }
    return DefId{};
}

bool HealthSystem::addedInfo(DefId hediff, std::string& partDef, DefId& item, float& efficiency) const {
    if (!hediff.valid() || hediff.value >= model_->addedDefs.size() || !model_->addedDefs[hediff.value].valid) {
        return false;
    }
    const HealthModel::AddedDef& a = model_->addedDefs[hediff.value];
    partDef = a.part;
    item = a.item;
    efficiency = a.efficiency;
    return true;
}

bool HealthSystem::installPart(Entity pawn, int part, DefId hediff, float tendQuality) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || !hediff.valid() || hediff.value >= model_->addedDefs.size() || !model_->addedDefs[hediff.value].valid) {
        return false;
    }
    Health& health = reg.get<Health>(pawn);
    const PartCondition condition = partCondition(pawn, part);
    if (health.dead || (condition != PartCondition::Whole && condition != PartCondition::Missing) ||
        model_->bodies[static_cast<size_t>(kind->body)].parts[static_cast<size_t>(part)].def != model_->addedDefs[hediff.value].part) {
        return false;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    const int end = body.parts[static_cast<size_t>(part)].end;
    // The natural part and what is under it are replaced: their wounds go.
    health.injuries.erase(std::remove_if(health.injuries.begin(), health.injuries.end(),
                                         [&](const Injury& i) { return i.part >= part && i.part < end; }),
                          health.injuries.end());
    for (MissingPart& m : health.missing) {
        if (m.part == part && !m.tended()) {
            m.tendQuality = std::max(0.0f, tendQuality);
        }
    }
    // Lost parts under the one that was lost are covered by the new part.
    health.missing.erase(std::remove_if(health.missing.begin(), health.missing.end(),
                                        [&](const MissingPart& m) { return m.part > part && m.part < end; }),
                         health.missing.end());
    health.added.push_back(AddedPart{static_cast<uint16_t>(part), hediff});
    recompute(pawn, health, true);
    return true;
}

DefId HealthSystem::uninstallPart(Entity pawn, int part, float tendQuality) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || part < 0) {
        return DefId{};
    }
    Health& health = reg.get<Health>(pawn);
    const auto found = std::find_if(health.added.begin(), health.added.end(), [part](const AddedPart& a) { return a.part == part; });
    if (health.dead || found == health.added.end()) {
        return DefId{};
    }
    const DefId hediff = found->hediff;
    health.added.erase(found);
    const bool lost = std::any_of(health.missing.begin(), health.missing.end(),
                                  [part](const MissingPart& m) { return m.part == part; });
    if (!lost) {
        destroyPart(pawn, health, part, 0.0f);
    }
    for (MissingPart& m : health.missing) {
        if (m.part == part) {
            m.tendQuality = std::max(0.0f, tendQuality);
        }
    }
    recompute(pawn, health, true);
    return hediff;
}

bool HealthSystem::removePart(Entity pawn, int part, float tendQuality) {
    if (!canRemovePart(pawn, part)) {
        return false;
    }
    Health& health = things_.registry().get<Health>(pawn);
    destroyPart(pawn, health, part, 0.0f);
    for (MissingPart& m : health.missing) {
        if (m.part == part) {
            m.tendQuality = std::max(0.0f, tendQuality);
        }
    }
    recompute(pawn, health, true);
    return true;
}

void HealthSystem::botch(Entity pawn, int part, float fraction) {
    entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr || part < 0 || !(fraction > 0.0f)) {
        return;
    }
    Health& health = reg.get<Health>(pawn);
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    if (health.dead || static_cast<size_t>(part) >= body.parts.size()) {
        return;
    }
    int target = part;
    while (target > 0 && partCondition(pawn, target) != PartCondition::Whole && partCondition(pawn, target) != PartCondition::Fitted) {
        target = body.parts[static_cast<size_t>(target)].parent;
    }
    const DefId cut = damageTypeId("Cut");
    if (!cut.valid() || target < 0) {
        return;
    }
    // A cut never destroys the part: it leaves one hit point at least.
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    const float room = eval.maxHp(target) - eval.damage[static_cast<size_t>(target)] - 1.0f;
    if (room <= 0.0f) {
        return;
    }
    const DefId hediff = model_->damageTypes[cut.value].hediff;
    damagePart(pawn, health, target, hediff, std::min(fraction * eval.maxHp(target), room));
    recompute(pawn, health, true);
}

float HealthSystem::partHitPoints(Entity pawn, int part) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    if (kind == nullptr) {
        return 0.0f;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    if (part < 0 || static_cast<size_t>(part) >= body.parts.size()) {
        return 0.0f;
    }
    const Health& health = reg.get<Health>(pawn);
    const HealthEval eval(*model_, body, health, healthHpFactor(*model_, health));
    return eval.hpFraction(part) * eval.maxHp(part);
}

float HealthSystem::workSpeedFactor(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    return health != nullptr ? health->workSpeedFactor : 1.0f;
}

float HealthSystem::hungerFactor(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    return health != nullptr ? health->hungerFactor : 1.0f;
}

float HealthSystem::capacityValue(Entity pawn, const std::string& capacity) const {
    const entt::registry& reg = things_.registry();
    const Health* health = reg.valid(pawn) ? reg.try_get<Health>(pawn) : nullptr;
    if (health == nullptr || health->dead) {
        return health == nullptr ? 1.0f : 0.0f;
    }
    for (size_t i = 0; i < model_->capacities.size() && i < health->capacities.size(); ++i) {
        if (model_->capacities[i].name == capacity) {
            return std::max(0.0f, health->capacities[i]);
        }
    }
    return 1.0f;
}

bool HealthSystem::report(Entity pawn, HealthReport& out) const {
    const entt::registry& reg = things_.registry();
    const HealthModel::Kind* kind = healthKindOf(*model_, reg, pawn);
    out = HealthReport();
    if (kind == nullptr) {
        return false;
    }
    // A pawn that has not been through a step yet has no derived values: work them out on a copy.
    Health health = reg.get<Health>(pawn);
    if (health.dirty) {
        DeathCause cause = DeathCause::None;
        bool shouldBeDown = false;
        HealthMods mods;
        if (mods_) {
            mods_(pawn, mods);
        }
        derive(kind->body, kind->healthScale, kind->mechanical, mods, health, cause, shouldBeDown,
               traits_ != nullptr ? traits_->of(pawn).painShockOffset : 0.0f);
        if (!health.dead && cause != DeathCause::None) {
            health.dead = true;
            health.cause = cause;
        }
        health.downed = !health.dead && shouldBeDown;
    }
    const HealthModel::Body& body = model_->bodies[static_cast<size_t>(kind->body)];
    out.dead = health.dead;
    out.downed = health.downed;
    out.cause = health.cause;
    out.protagonist = health.protagonist;
    out.pain = health.pain;
    out.bleedPerDay = health.bleedPerDay;
    out.bloodLoss = health.bloodLoss;
    for (const Injury& injury : health.injuries) {
        HealthReport::InjuryLine line;
        line.part = body.parts[injury.part].label;
        line.injury = injury.hediff.valid() ? things_.defs().get(injury.hediff).label() : std::string();
        line.severity = injury.severity;
        line.tended = injury.tended();
        line.tendQuality = injury.tendQuality;
        line.permanent = injury.hediff.valid() && model_->hediffs[injury.hediff.value].permanent;
        out.injuries.push_back(std::move(line));
    }
    for (const MissingPart& missing : health.missing) {
        const bool fitted = std::any_of(health.added.begin(), health.added.end(),
                                        [&](const AddedPart& a) { return a.part == missing.part; });
        if (!fitted) {
            out.missingParts.push_back(body.parts[missing.part].label);
        }
    }
    for (const AddedPart& a : health.added) {
        if (a.part < body.parts.size() && a.hediff.valid() && a.hediff.value < model_->addedDefs.size()) {
            out.addedParts.push_back(HealthReport::AddedLine{body.parts[a.part].label, things_.defs().get(a.hediff).label(),
                                                             model_->addedDefs[a.hediff.value].efficiency});
        }
    }
    for (const EnvironmentHediff& env : health.environment) {
        const HealthModel::EnvironmentDef* def = model_->environmentOf(env.def);
        if (def == nullptr) {
            continue;
        }
        HealthReport::EnvironmentLine line;
        line.name = things_.defs().get(env.def).label();
        line.severity = env.severity;
        const int stage = HealthModel::stageIn(def->stages, env.severity);
        if (stage >= 0) {
            line.stage = things_.defs().get(env.def).text(("stages." + std::to_string(stage) + ".label").c_str());
            line.lifeThreatening = stage < static_cast<int>(def->lifeThreatening.size()) && def->lifeThreatening[static_cast<size_t>(stage)] != 0;
        }
        out.environment.push_back(std::move(line));
    }
    const int64_t reportNow = clock_ != nullptr ? *clock_ : 0;
    for (const Disease& disease : health.diseases) {
        const HealthModel::DiseaseDef* def = model_->diseaseOf(disease.def);
        if (def == nullptr) {
            continue;
        }
        HealthReport::DiseaseLine line;
        line.name = things_.defs().get(disease.def).label();
        if (disease.part >= 0 && static_cast<size_t>(disease.part) < body.parts.size()) {
            line.part = body.parts[static_cast<size_t>(disease.part)].label;
        }
        line.severity = disease.severity;
        line.immunity = immunity(pawn, disease.def);
        line.tended = disease.tendedUntil > reportNow && disease.tendQuality >= 0.0f;
        const int stage = HealthModel::stageIn(def->stages, disease.severity);
        if (stage >= 0) {
            line.stage = things_.defs().get(disease.def).text(("stages." + std::to_string(stage) + ".label").c_str());
            line.lifeThreatening = static_cast<size_t>(stage) < def->lifeThreatening.size() && def->lifeThreatening[static_cast<size_t>(stage)] != 0;
            line.visible = def->stages[static_cast<size_t>(stage)].visible;
        } else {
            line.visible = false;
        }
        out.diseases.push_back(std::move(line));
    }
    for (size_t i = 0; i < model_->capacities.size() && i < health.capacities.size(); ++i) {
        out.capacities.push_back(HealthReport::CapacityLine{model_->capacities[i].name, health.capacities[i]});
    }
    return true;
}

}  // namespace colony
