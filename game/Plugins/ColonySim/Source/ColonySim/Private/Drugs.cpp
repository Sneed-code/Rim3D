// Drugs, tolerance, addiction, withdrawal, overdose and drug policies (Stage 2, docs/design/drugs.md).
#include "ColonySim/Drugs.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Prisoners.h"
#include "ColonySim/Random.h"
#include "ColonySim/Recreation.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr float kDrugsPerSecond = 1.0f / 86400.0f;
constexpr const char* kDrugsThingsKind = "Things";
constexpr const char* kDrugsHediffsKind = "Hediffs";
constexpr const char* kDrugsChemicalsKind = "Chemicals";
constexpr const char* kDrugsPoliciesKind = "DrugPolicies";

float drugsFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

}  // namespace

const char* drugCategoryName(DrugCategory category) {
    switch (category) {
        case DrugCategory::Social: return "Social";
        case DrugCategory::Hard: return "Hard";
        case DrugCategory::Medical: return "Medical";
    }
    return "?";
}

DrugRules DrugRules::read(const DefDatabase& defs) {
    DrugRules r;
    const Def* def = defs.find("Rules", "DrugRules");
    if (def == nullptr) {
        return r;
    }
    r.present = true;
    r.defaultPolicy = def->text("defaultPolicy", r.defaultPolicy);
    r.toleranceDurationFactor = drugsFloat(*def, "toleranceDurationFactor", r.toleranceDurationFactor);
    const size_t points = def->count("addictionChanceByTolerance");
    if (points > 0) {
        r.addictionChanceByTolerance.clear();
        for (size_t i = 0; i < points; ++i) {
            const std::string base = "addictionChanceByTolerance." + std::to_string(i);
            r.addictionChanceByTolerance.emplace_back(drugsFloat(*def, (base + ".0").c_str(), 0.0f),
                                                      drugsFloat(*def, (base + ".1").c_str(), 1.0f));
        }
    }
    r.overdoseSeverityMin = drugsFloat(*def, "overdoseSeverityMin", r.overdoseSeverityMin);
    r.overdoseSeverityMax = drugsFloat(*def, "overdoseSeverityMax", r.overdoseSeverityMax);
    r.overdoseFallPerDay = drugsFloat(*def, "overdoseFallPerDay", r.overdoseFallPerDay);
    r.majorOverdoseChance = drugsFloat(*def, "majorOverdoseChance", r.majorOverdoseChance);
    r.majorOverdoseSeverity = drugsFloat(*def, "majorOverdoseSeverity", r.majorOverdoseSeverity);
    r.newAddictionProgress = drugsFloat(*def, "newAddictionProgress", r.newAddictionProgress);
    r.seekNeedBelow = drugsFloat(*def, "seekNeedBelow", r.seekNeedBelow);
    r.joyBelow = drugsFloat(*def, "joyBelow", r.joyBelow);
    r.redoseBelow = drugsFloat(*def, "redoseBelow", r.redoseBelow);
    r.overdoseHoldOff = drugsFloat(*def, "overdoseHoldOff", r.overdoseHoldOff);
    r.searchSeconds = def->integer("searchSeconds", r.searchSeconds);
    r.hangoverFromSeverity = drugsFloat(*def, "hangoverFromSeverity", r.hangoverFromSeverity);
    r.hangoverSeverity = drugsFloat(*def, "hangoverSeverity", r.hangoverSeverity);
    r.bingeCheckEvery = static_cast<int>(def->integer("bingeCheckEvery", r.bingeCheckEvery));
    r.bingeMeanDaysSocial = drugsFloat(*def, "bingeMeanDaysSocial", r.bingeMeanDaysSocial);
    r.bingeMeanDaysHard = drugsFloat(*def, "bingeMeanDaysHard", r.bingeMeanDaysHard);
    r.bingeMeanDaysWithdrawal = drugsFloat(*def, "bingeMeanDaysWithdrawal", r.bingeMeanDaysWithdrawal);
    r.bingeHoursMin = drugsFloat(*def, "bingeHoursMin", r.bingeHoursMin);
    r.bingeHoursMax = drugsFloat(*def, "bingeHoursMax", r.bingeHoursMax);
    r.bingeRedoseBelow = drugsFloat(*def, "bingeRedoseBelow", r.bingeRedoseBelow);
    r.bingeEndOverdose = drugsFloat(*def, "bingeEndOverdose", r.bingeEndOverdose);
    r.damageCheckEvery = static_cast<int>(def->integer("damageCheckEvery", r.damageCheckEvery));
    r.damageMtbDaysAtThreshold = drugsFloat(*def, "damageMtbDaysAtThreshold", r.damageMtbDaysAtThreshold);
    r.damageMtbDaysAtFull = drugsFloat(*def, "damageMtbDaysAtFull", r.damageMtbDaysAtFull);
    r.damageFraction = drugsFloat(*def, "damageFraction", r.damageFraction);
    r.damageMaxFraction = drugsFloat(*def, "damageMaxFraction", r.damageMaxFraction);
    r.brainDamageMtbDays = drugsFloat(*def, "brainDamageMtbDays", r.brainDamageMtbDays);
    r.brainDamageFraction = drugsFloat(*def, "brainDamageFraction", r.brainDamageFraction);
    r.heartAttackFraction = drugsFloat(*def, "heartAttackFraction", r.heartAttackFraction);
    r.majorOverdose = drugsFloat(*def, "majorOverdoseSeverity", r.majorOverdose);
    r.bingeCheckEvery = std::max(1, r.bingeCheckEvery);
    r.damageCheckEvery = std::max(1, r.damageCheckEvery);
    return r;
}

float DrugRules::addictionMultiplier(float tolerance) const {
    if (addictionChanceByTolerance.empty()) {
        return 1.0f;
    }
    if (tolerance <= addictionChanceByTolerance.front().first) {
        return addictionChanceByTolerance.front().second;
    }
    for (size_t i = 1; i < addictionChanceByTolerance.size(); ++i) {
        const auto& a = addictionChanceByTolerance[i - 1];
        const auto& b = addictionChanceByTolerance[i];
        if (tolerance <= b.first) {
            const float span = b.first - a.first;
            return span > 0.0f ? a.second + (b.second - a.second) * (tolerance - a.first) / span : b.second;
        }
    }
    return addictionChanceByTolerance.back().second;
}

DrugSystem::DrugSystem(Things& things, HealthSystem& health, const DefDatabase& defs)
    : things_(things), health_(health), defs_(defs), rules_(DrugRules::read(defs)) {
    hediffs_.assign(defs.size(), HediffInfo{});
    prevents_.assign(defs.size(), {});
    kindIndex_.assign(defs.size(), -1);

    for (const DefId id : defs.idsOfKind(kDrugsHediffsKind)) {
        const Def& def = defs.get(id);
        if (def.text("hediffClass") != "Environment") {
            continue;
        }
        HediffInfo& info = hediffs_[id.value];
        info.known = true;
        info.fallPerDay = drugsFloat(def, "severityFallPerDay", 0.0f);
        info.restFallFactor = drugsFloat(def, "restFallFactor", 1.0f);
        const size_t stages = def.count("stages");
        for (size_t i = 0; i < stages; ++i) {
            info.stages.push_back(drugsFloat(def, ("stages." + std::to_string(i) + ".minSeverity").c_str(), 0.0f));
        }
        if (info.restFallFactor != 1.0f) {
            restHediffs_.push_back(id);
        }
        for (size_t i = 0, n = def.count("preventsDiseases"); i < n; ++i) {
            prevents_[id.value].push_back(def.textAt("preventsDiseases", i));
        }
        if (def.defName() == "Overdose") {
            overdoseHediff_ = id;
            info.fallPerDay = rules_.overdoseFallPerDay;
        }
    }

    for (const DefId id : defs.idsOfKind(kDrugsChemicalsKind)) {
        const Def& def = defs.get(id);
        DrugChemical c;
        c.def = id;
        c.defName = def.defName();
        c.withdrawalHediff = defs.idOf(kDrugsHediffsKind, def.text("withdrawalHediff"));
        c.hangoverHediff = defs.idOf(kDrugsHediffsKind, def.text("hangoverHediff"));
        c.needFallPerDay = drugsFloat(def, "needFallPerDay", c.needFallPerDay);
        c.needPerDose = drugsFloat(def, "needPerDose", c.needPerDose);
        c.toleranceFallPerDay = drugsFloat(def, "toleranceFallPerDay", 0.0f);
        c.progressPerDay = drugsFloat(def, "progressPerDay", 0.0f);
        c.doseProgressOffset = drugsFloat(def, "doseProgressOffset", 0.0f);
        c.withdrawalRisePerDay = drugsFloat(def, "withdrawalRisePerDay", c.withdrawalRisePerDay);
        c.withdrawalRecoverPerDay = drugsFloat(def, "withdrawalRecoverPerDay", c.withdrawalRecoverPerDay);
        c.permanent = def.flag("permanent");
        c.damagePart = def.text("chemicalDamage.part");
        c.damageMinTolerance = drugsFloat(def, "chemicalDamage.minTolerance", 0.0f);
        c.addictionDamagePart = def.text("addictionDamage.part");
        c.addictionDamageChance = drugsFloat(def, "addictionDamage.chance", 0.0f);
        c.heartAttackMtbDays = drugsFloat(def, "heartAttackMtbDays", 0.0f);
        chemicals_.push_back(std::move(c));
    }
    std::sort(chemicals_.begin(), chemicals_.end(), [](const DrugChemical& a, const DrugChemical& b) { return a.defName < b.defName; });

    for (const DefId id : defs.idsOfKind(kDrugsThingsKind)) {
        const Def& def = defs.get(id);
        if (!def.has("drug.chemical")) {
            continue;
        }
        DrugKind k;
        k.def = id;
        k.defName = def.defName();
        k.chemical = chemicalIndex(def.text("drug.chemical"));
        if (k.chemical < 0) {
            continue;
        }
        const std::string category = def.text("drug.category", "Social");
        k.category = category == "Hard" ? DrugCategory::Hard : category == "Medical" ? DrugCategory::Medical : DrugCategory::Social;
        k.addictiveness = drugsFloat(def, "drug.addictiveness", 0.0f);
        k.minToleranceToAddict = drugsFloat(def, "drug.minToleranceToAddict", 0.0f);
        k.toleranceGain = drugsFloat(def, "drug.toleranceGain", 0.0f);
        k.high = defs.idOf(kDrugsHediffsKind, def.text("drug.high"));
        k.highSeverity = drugsFloat(def, "drug.highSeverity", 0.5f);
        k.joy = drugsFloat(def, "drug.joy", 0.0f);
        k.restOffset = drugsFloat(def, "drug.restOffset", 0.0f);
        k.overdose = def.flag("drug.overdose");
        k.nutrition = drugsFloat(def, "drug.nutrition", 0.0f);
        kindIndex_[id.value] = static_cast<int32_t>(kinds_.size());
        kinds_.push_back(std::move(k));
    }

    for (const DefId id : defs.idsOfKind(kDrugsPoliciesKind)) {
        const Def& def = defs.get(id);
        DrugPolicy p;
        p.defName = def.defName();
        p.label = def.label();
        const size_t entries = def.count("entries");
        for (size_t i = 0; i < entries; ++i) {
            const std::string base = "entries." + std::to_string(i) + ".";
            DrugPolicyEntry e;
            e.drug = def.text((base + "drug").c_str());
            e.forAddiction = def.flag((base + "forAddiction").c_str(), true);
            e.forJoy = def.flag((base + "forJoy").c_str(), false);
            e.scheduled = def.flag((base + "scheduled").c_str(), false);
            e.daysFrequency = drugsFloat(def, (base + "daysFrequency").c_str(), 1.0f);
            e.moodBelow = drugsFloat(def, (base + "moodBelow").c_str(), 1.01f);
            e.joyBelow = drugsFloat(def, (base + "joyBelow").c_str(), 1.01f);
            p.entries.push_back(std::move(e));
        }
        policies_.push_back(std::move(p));
    }
}

void DrugSystem::registerThoughts(MoodSystem& mood) {
    mood_ = &mood;
    std::vector<std::string> done;
    for (const DefId id : defs_.idsOfKind("Thoughts")) {
        const std::string condition = defs_.get(id).text("situation.condition");
        if (condition.rfind("drug:", 0) != 0 || std::find(done.begin(), done.end(), condition) != done.end()) {
            continue;
        }
        const DefId hediff = defs_.idOf(kDrugsHediffsKind, condition.substr(5));
        if (!hediff.valid() || !hediffs_[hediff.value].known) {
            continue;
        }
        done.push_back(condition);
        mood.addSituation(condition, [this, hediff](Entity pawn) {
            const float s = severity(pawn, hediff);
            if (s <= 0.0f) {
                return -1;
            }
            const std::vector<float>& stages = hediffs_[hediff.value].stages;
            int stage = 0;
            for (size_t i = 0; i < stages.size(); ++i) {
                if (s >= stages[i]) {
                    stage = static_cast<int>(i);
                }
            }
            return stage;
        });
    }
}

const DrugKind* DrugSystem::kindOf(DefId thingDef) const {
    if (!thingDef.valid() || thingDef.value >= kindIndex_.size() || kindIndex_[thingDef.value] < 0) {
        return nullptr;
    }
    return &kinds_[static_cast<size_t>(kindIndex_[thingDef.value])];
}

const DrugPolicy* DrugSystem::policyNamed(const std::string& defName) const {
    for (const DrugPolicy& p : policies_) {
        if (p.defName == defName) {
            return &p;
        }
    }
    return nullptr;
}

int DrugSystem::chemicalIndex(const std::string& defName) const {
    for (size_t i = 0; i < chemicals_.size(); ++i) {
        if (chemicals_[i].defName == defName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const DrugState* DrugSystem::state(Entity pawn) const {
    const auto it = states_.find(pawn);
    return it != states_.end() ? &it->second : nullptr;
}

const ChemicalState* DrugSystem::chemicalState(const DrugState& state, const std::string& chemical) const {
    for (const ChemicalState& c : state.chemicals) {
        if (c.chemical == chemical) {
            return &c;
        }
    }
    return nullptr;
}

ChemicalState& DrugSystem::chemicalState(DrugState& state, const std::string& chemical) {
    auto it = std::lower_bound(state.chemicals.begin(), state.chemicals.end(), chemical,
                               [](const ChemicalState& c, const std::string& name) { return c.chemical < name; });
    if (it == state.chemicals.end() || it->chemical != chemical) {
        ChemicalState fresh;
        fresh.chemical = chemical;
        it = state.chemicals.insert(it, std::move(fresh));
    }
    return *it;
}

DrugState& DrugSystem::ensure(Entity pawn) {
    return states_[pawn];
}

float DrugSystem::tolerance(Entity pawn, const std::string& chemical) const {
    const DrugState* s = state(pawn);
    const ChemicalState* c = s != nullptr ? chemicalState(*s, chemical) : nullptr;
    return c != nullptr ? c->tolerance : 0.0f;
}

bool DrugSystem::addicted(Entity pawn, const std::string& chemical) const {
    const DrugState* s = state(pawn);
    const ChemicalState* c = s != nullptr ? chemicalState(*s, chemical) : nullptr;
    return c != nullptr && c->addicted;
}

float DrugSystem::need(Entity pawn, const std::string& chemical) const {
    const DrugState* s = state(pawn);
    const ChemicalState* c = s != nullptr ? chemicalState(*s, chemical) : nullptr;
    return c != nullptr ? c->need : 0.0f;
}

float DrugSystem::progress(Entity pawn, const std::string& chemical) const {
    const DrugState* s = state(pawn);
    const ChemicalState* c = s != nullptr ? chemicalState(*s, chemical) : nullptr;
    return c != nullptr ? c->progress : 0.0f;
}

bool DrugSystem::eligible(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return rules_.present && things_.exists(pawn) && reg.all_of<Colonist, Needs>(pawn) && !reg.all_of<Prisoner>(pawn) &&
           health_.tracks(pawn) && !reg.get<Health>(pawn).dead;
}

double DrugSystem::unit(Entity pawn, int64_t now, uint64_t salt) const {
    const uint64_t h = splitMix64(splitMix64(static_cast<uint64_t>(entt::to_integral(pawn)) * 0x9E3779B97F4A7C15ULL ^
                                             static_cast<uint64_t>(now)) ^
                                  salt);
    return static_cast<double>(h >> 11) / 9007199254740992.0;
}

float DrugSystem::severity(Entity pawn, DefId hediff) const {
    return hediff.valid() ? health_.environmentSeverity(pawn, hediff) : 0.0f;
}

void DrugSystem::setSeverity(Entity pawn, DefId hediff, float value) {
    if (hediff.valid()) {
        health_.setEnvironmentSeverity(pawn, hediff, value < 1.0e-6f ? 0.0f : value);
    }
}

void DrugSystem::emit(EventType type, Entity pawn, DefId def, float amount, const char* message) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.thing = pawn;
    e.def = def;
    e.amount = amount;
    if (message != nullptr) {
        e.message = message;
    }
    const Position* at = things_.registry().try_get<Position>(pawn);
    if (at != nullptr) {
        e.cell = cellOf(*at);
    }
    events_->push_back(std::move(e));
}

// ---- Policies -----------------------------------------------------------------------------------

bool DrugSystem::setPolicy(Entity pawn, const std::string& policy, std::string& error) {
    if (!eligible(pawn)) {
        error = "that pawn can't take drugs";
        return false;
    }
    if (policyNamed(policy) == nullptr) {
        error = "no drug policy '" + policy + "'";
        return false;
    }
    DrugState& s = ensure(pawn);
    s.policy = policy;
    s.edits.clear();
    return true;
}

bool DrugSystem::setEntry(Entity pawn, const DrugPolicyEntry& entry, std::string& error) {
    if (!eligible(pawn)) {
        error = "that pawn can't take drugs";
        return false;
    }
    const DefId def = defs_.idOf(kDrugsThingsKind, entry.drug);
    if (kindOf(def) == nullptr) {
        error = "'" + entry.drug + "' is no drug";
        return false;
    }
    if (!(entry.daysFrequency > 0.0f && entry.daysFrequency <= 1000.0f)) {
        error = "the frequency must be between 0 and 1000 days";
        return false;
    }
    DrugState& s = ensure(pawn);
    for (DrugPolicyEntry& e : s.edits) {
        if (e.drug == entry.drug) {
            e = entry;
            return true;
        }
    }
    s.edits.push_back(entry);
    std::sort(s.edits.begin(), s.edits.end(), [](const DrugPolicyEntry& a, const DrugPolicyEntry& b) { return a.drug < b.drug; });
    return true;
}

DrugPolicyEntry DrugSystem::entryFor(Entity pawn, const std::string& drug) const {
    const DrugState* s = state(pawn);
    if (s != nullptr) {
        for (const DrugPolicyEntry& e : s->edits) {
            if (e.drug == drug) {
                return e;
            }
        }
    }
    const DrugPolicy* policy = policyNamed(s != nullptr && !s->policy.empty() ? s->policy : rules_.defaultPolicy);
    if (policy != nullptr) {
        for (const DrugPolicyEntry& e : policy->entries) {
            if (e.drug == drug) {
                return e;
            }
        }
    }
    DrugPolicyEntry fallback;
    fallback.drug = drug;
    return fallback;
}

// ---- Taking -------------------------------------------------------------------------------------

std::vector<DrugWant> DrugSystem::wants(Entity pawn, int64_t now, ScheduleKind slot) {
    std::vector<DrugWant> out;
    if (!eligible(pawn) || kinds_.empty()) {
        return out;
    }
    entt::registry& reg = things_.registry();
    if (health_.incapacitated(pawn)) {
        return out;
    }
    const DrugState* s = state(pawn);
    if (s != nullptr && now < s->searchAfter) {
        return out;
    }
    const bool sleeping = slot == ScheduleKind::Sleep;
    const bool working = slot == ScheduleKind::Work;
    const Joy* joy = reg.try_get<Joy>(pawn);
    const Mood* mood = reg.try_get<Mood>(pawn);
    const float overdose = severity(pawn, overdoseHediff_);
    const bool binge = s != nullptr && s->bingeUntil > now;
    for (const DrugKind& k : kinds_) {
        const DrugPolicyEntry entry = entryFor(pawn, k.defName);
        const DrugChemical& chem = chemicals_[static_cast<size_t>(k.chemical)];
        const ChemicalState* cs = s != nullptr ? chemicalState(*s, chem.defName) : nullptr;
        // On a binge the policy and the limits do not count: any drug of the chemical, until it is nearly full.
        const bool bingeKind = binge && k.category != DrugCategory::Medical && chem.defName == s->bingeChemical;
        if (severity(pawn, k.high) >= (bingeKind ? rules_.bingeRedoseBelow : rules_.redoseBelow)) {
            continue;  // still high on it
        }
        int priority = 0;
        if (bingeKind && overdose < rules_.overdoseHoldOff) {
            priority = 4;
        } else if (cs != nullptr && cs->addicted && cs->need < rules_.seekNeedBelow && entry.forAddiction) {
            priority = 3;
        } else if (overdose < rules_.overdoseHoldOff && !sleeping) {
            if (entry.scheduled) {
                int64_t last = -1;
                if (s != nullptr) {
                    for (const DrugDose& d : s->last) {
                        if (d.drug == k.defName) {
                            last = d.at;
                        }
                    }
                }
                const bool due = last < 0 || static_cast<double>(now - last) >= static_cast<double>(entry.daysFrequency) * 86400.0;
                const bool moodOk = entry.moodBelow > 1.0f || (mood != nullptr && mood->level / 100.0f < entry.moodBelow);
                const bool joyOk = entry.joyBelow > 1.0f || (joy != nullptr && joy->level < entry.joyBelow);
                if (due && moodOk && joyOk) {
                    priority = 2;
                }
            }
            if (priority == 0 && entry.forJoy && !working && k.category != DrugCategory::Medical && joy != nullptr &&
                joy->level < rules_.joyBelow) {
                priority = 1;
            }
        }
        if (priority > 0) {
            out.push_back(DrugWant{k.def, priority});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const DrugWant& a, const DrugWant& b) { return a.priority > b.priority; });
    return out;
}

void DrugSystem::searchFailed(Entity pawn, int64_t now) {
    if (things_.exists(pawn)) {
        DrugState& s = ensure(pawn);
        s.searchAfter = now + rules_.searchSeconds;
        if (s.bingeUntil > now) {
            endBinge(pawn, s);  // nothing left to binge on
        }
    }
}

bool DrugSystem::takeDose(Entity pawn, DefId drug, int64_t now) {
    const DrugKind* kind = kindOf(drug);
    if (kind == nullptr || !eligible(pawn)) {
        return false;
    }
    entt::registry& reg = things_.registry();
    const DrugChemical& chem = chemicals_[static_cast<size_t>(kind->chemical)];
    DrugState& state = ensure(pawn);
    ChemicalState& cs = chemicalState(state, chem.defName);

    // The high: weaker the more used to the chemical the pawn is.
    const float factor = 1.0f - rules_.toleranceDurationFactor * cs.tolerance;
    const float high = std::min(1.0f, severity(pawn, kind->high) + kind->highSeverity * factor);
    setSeverity(pawn, kind->high, high);

    // Addiction: a pawn who is addicted is set back in its recovery and fills its need; one who is not may become
    // addicted, by the drug's addictiveness times what its tolerance makes of it.
    if (cs.addicted) {
        cs.need = std::min(1.0f, cs.need + chem.needPerDose);
        cs.progress = std::max(0.0f, cs.progress - chem.doseProgressOffset);
    } else if (kind->addictiveness > 0.0f && cs.tolerance >= kind->minToleranceToAddict) {
        const float chance = kind->addictiveness * rules_.addictionMultiplier(cs.tolerance);
        if (unit(pawn, now, 0xADD1C7ULL) < static_cast<double>(chance)) {
            cs.addicted = true;
            cs.need = std::min(1.0f, chem.needPerDose);
            cs.progress = chem.permanent ? 0.0f : rules_.newAddictionProgress;
            emit(EventType::PawnAddicted, pawn, chem.def, cs.need, chem.defName.c_str());
            if (!chem.addictionDamagePart.empty() && unit(pawn, now, 0xDA11A6EULL) < static_cast<double>(chem.addictionDamageChance)) {
                hurtOrgan(pawn, chem.addictionDamagePart, rules_.damageFraction, now, chem.def, 0xDA11A6EULL);
            }
        }
    }
    cs.tolerance = std::min(1.0f, cs.tolerance + kind->toleranceGain);

    // Rest and recreation.
    if (Needs* needs = reg.try_get<Needs>(pawn)) {
        needs->rest = std::clamp(needs->rest + kind->restOffset, 0.0f, 1.0f);
    }
    if (Joy* joy = reg.try_get<Joy>(pawn)) {
        joy->level = std::min(1.0f, joy->level + kind->joy);
    }
    if (kind->nutrition > 0.0f) {
        if (Needs* needs = reg.try_get<Needs>(pawn)) {
            needs->food = std::min(1.0f, needs->food + kind->nutrition);
        }
    }

    // Overdose: only hard drugs.
    if (kind->overdose) {
        const float before = severity(pawn, overdoseHediff_);
        float add = rules_.overdoseSeverityMin +
                    static_cast<float>(unit(pawn, now, 0x0D05EULL)) * (rules_.overdoseSeverityMax - rules_.overdoseSeverityMin);
        if (unit(pawn, now, 0xB16ULL) < static_cast<double>(rules_.majorOverdoseChance)) {
            add = std::max(add, rules_.majorOverdoseSeverity);
        }
        const float after = before + add;
        setSeverity(pawn, overdoseHediff_, after);
        if (after >= 0.5f && before < 0.5f) {
            emit(EventType::DrugOverdose, pawn, kind->def, after, after >= 1.0f ? "fatal" : after >= 0.75f ? "major" : "minor");
        } else if (after >= 0.75f && before < 0.75f) {
            emit(EventType::DrugOverdose, pawn, kind->def, after, after >= 1.0f ? "fatal" : "major");
        }
    }

    // Remember the dose.
    auto it = std::lower_bound(state.last.begin(), state.last.end(), kind->defName,
                               [](const DrugDose& d, const std::string& name) { return d.drug < name; });
    if (it == state.last.end() || it->drug != kind->defName) {
        it = state.last.insert(it, DrugDose{kind->defName, now});
    }
    it->at = now;
    state.searchAfter = 0;
    emit(EventType::PawnTookDrug, pawn, kind->def, high, nullptr);
    return true;
}

// ---- Stepping -----------------------------------------------------------------------------------

void DrugSystem::step(int64_t now) {
    for (auto& [pawn, state] : states_) {
        if (!things_.exists(pawn) || !health_.tracks(pawn) || things_.registry().get<Health>(pawn).dead) {
            continue;
        }
        // Highs fade; a drunk pawn who sobers up has a hangover.
        for (const DrugKind& k : kinds_) {
            const float s = severity(pawn, k.high);
            const DrugChemical& chem = chemicals_[static_cast<size_t>(k.chemical)];
            if (s > 0.0f) {
                const float fall = hediffs_[k.high.value].fallPerDay * kDrugsPerSecond;
                setSeverity(pawn, k.high, s - fall);
                if (chem.hangoverHediff.valid() && s >= rules_.hangoverFromSeverity) {
                    chemicalState(state, chem.defName).armed = true;
                }
            } else if (chem.hangoverHediff.valid()) {
                ChemicalState* armed = nullptr;
                for (ChemicalState& c : state.chemicals) {
                    if (c.chemical == chem.defName && c.armed) {
                        armed = &c;
                    }
                }
                if (armed != nullptr) {
                    armed->armed = false;
                    setSeverity(pawn, chem.hangoverHediff, rules_.hangoverSeverity);
                }
            }
        }
        if (chemicals_.size() > 0) {
            for (const DrugChemical& chem : chemicals_) {
                if (chem.hangoverHediff.valid()) {
                    const float h = severity(pawn, chem.hangoverHediff);
                    if (h > 0.0f) {
                        setSeverity(pawn, chem.hangoverHediff, h - hediffs_[chem.hangoverHediff.value].fallPerDay * kDrugsPerSecond);
                    }
                }
            }
        }
        const float overdose = severity(pawn, overdoseHediff_);
        if (overdose > 0.0f) {
            setSeverity(pawn, overdoseHediff_, overdose - rules_.overdoseFallPerDay * kDrugsPerSecond);
        }

        for (ChemicalState& cs : state.chemicals) {
            const int index = chemicalIndex(cs.chemical);
            if (index < 0) {
                continue;
            }
            const DrugChemical& chem = chemicals_[static_cast<size_t>(index)];
            cs.tolerance = std::max(0.0f, cs.tolerance - chem.toleranceFallPerDay * kDrugsPerSecond);
            float withdrawal = severity(pawn, chem.withdrawalHediff);
            if (cs.addicted) {
                cs.need = std::max(0.0f, cs.need - chem.needFallPerDay * kDrugsPerSecond);
                if (!chem.permanent) {
                    cs.progress += chem.progressPerDay * kDrugsPerSecond;
                }
                if (cs.need <= 0.0f) {
                    // Without it: the withdrawal builds, to 1 (or, for a permanent addiction, until it kills).
                    withdrawal += chem.withdrawalRisePerDay * kDrugsPerSecond;
                    if (!chem.permanent) {
                        withdrawal = std::min(1.0f, withdrawal);
                    }
                } else {
                    withdrawal -= chem.withdrawalRecoverPerDay * kDrugsPerSecond;
                }
                if (!chem.permanent && cs.progress >= 1.0f) {
                    cs.addicted = false;
                    cs.need = 0.0f;
                    cs.progress = 0.0f;
                    emit(EventType::AddictionCured, pawn, chem.def, 0.0f, chem.defName.c_str());
                }
            } else {
                withdrawal -= chem.withdrawalRecoverPerDay * kDrugsPerSecond;
            }
            withdrawal = std::max(0.0f, withdrawal);
            if (withdrawal != severity(pawn, chem.withdrawalHediff)) {
                setSeverity(pawn, chem.withdrawalHediff, withdrawal);
            }
        }
        stepBinge(pawn, state, now);
        stepDamage(pawn, state, now);
    }
}

bool DrugSystem::canBinge(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return eligible(pawn) && !health_.incapacitated(pawn) && !reg.get<Health>(pawn).protagonist && !reg.all_of<PlayerControl>(pawn);
}

bool DrugSystem::bingeing(Entity pawn) const {
    const DrugState* s = state(pawn);
    return s != nullptr && s->bingeUntil > 0;
}

bool DrugSystem::startBinge(Entity pawn, const std::string& chemical, int64_t now) {
    if (!canBinge(pawn) || chemicalIndex(chemical) < 0) {
        return false;
    }
    DrugState& s = ensure(pawn);
    if (s.bingeUntil > 0) {
        return false;
    }
    const double hours = rules_.bingeHoursMin + unit(pawn, now, 0xB146EULL) * (rules_.bingeHoursMax - rules_.bingeHoursMin);
    s.bingeUntil = now + static_cast<int64_t>(hours * 3600.0);
    s.bingeChemical = chemical;
    s.searchAfter = 0;
    emit(EventType::DrugBinge, pawn, chemicals_[static_cast<size_t>(chemicalIndex(chemical))].def, static_cast<float>(hours), "start");
    return true;
}

void DrugSystem::endBinge(Entity pawn, DrugState& state) {
    const int index = chemicalIndex(state.bingeChemical);
    state.bingeUntil = 0;
    state.bingeChemical.clear();
    emit(EventType::DrugBinge, pawn, index >= 0 ? chemicals_[static_cast<size_t>(index)].def : DefId{}, 0.0f, "end");
}

// A binge runs out, or ends at a minor overdose; natural ones start here: a pawn addicted to a social drug in a major
// mood band, or to a hard one in an extreme band, or one in withdrawal at any mood, rolls every few minutes.
void DrugSystem::stepBinge(Entity pawn, DrugState& state, int64_t now) {
    if (state.bingeUntil > 0) {
        if (now >= state.bingeUntil || severity(pawn, overdoseHediff_) >= rules_.bingeEndOverdose || !canBinge(pawn)) {
            endBinge(pawn, state);
        }
        return;
    }
    if (mood_ == nullptr || (now + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.bingeCheckEvery != 0) {
        return;
    }
    bool any = false;
    for (const ChemicalState& cs : state.chemicals) {
        any = any || cs.addicted;
    }
    if (!any || !canBinge(pawn)) {
        return;
    }
    const Mood* mood = things_.registry().try_get<Mood>(pawn);
    const MoodBand band = mood != nullptr ? mood->band : MoodBand::Stable;
    // The chemical to binge on and the mean wait: the worst of what the pawn's state allows.
    const std::string* chemical = nullptr;
    double meanDays = 0.0;
    for (const ChemicalState& cs : state.chemicals) {
        const int index = chemicalIndex(cs.chemical);
        if (!cs.addicted || index < 0) {
            continue;
        }
        bool hard = false;
        bool usable = false;
        for (const DrugKind& k : kinds_) {
            if (k.chemical == index && k.category != DrugCategory::Medical) {
                usable = true;
                hard = hard || k.category == DrugCategory::Hard;
            }
        }
        if (!usable) {
            continue;
        }
        double days = 0.0;
        if (hard && band == MoodBand::Extreme) {
            days = rules_.bingeMeanDaysHard;
        } else if (!hard && (band == MoodBand::Major || band == MoodBand::Extreme)) {
            days = rules_.bingeMeanDaysSocial;
        }
        const DrugChemical& chem = chemicals_[static_cast<size_t>(index)];
        if (chem.withdrawalHediff.valid() && severity(pawn, chem.withdrawalHediff) > 0.0f &&
            (days <= 0.0 || rules_.bingeMeanDaysWithdrawal < days)) {
            days = rules_.bingeMeanDaysWithdrawal;
        }
        if (days > 0.0 && (chemical == nullptr || days < meanDays)) {
            chemical = &cs.chemical;
            meanDays = days;
        }
    }
    if (chemical == nullptr) {
        return;
    }
    const double chance = static_cast<double>(rules_.bingeCheckEvery) / (meanDays * 86400.0);
    if (unit(pawn, now, 0xB146E5ULL) < chance) {
        startBinge(pawn, *chemical, now);
    }
}

bool DrugSystem::hurtOrgan(Entity pawn, const std::string& partDef, float fraction, int64_t now, DefId chemical, uint64_t salt) {
    const entt::registry& reg = things_.registry();
    const DefId damage = defs_.idOf(kDrugsHediffsKind, "ChemicalDamage");
    if (!damage.valid() || !reg.all_of<Thing>(pawn)) {
        return false;
    }
    // Of a paired organ the healthier one is hurt (ties by the dice).
    const std::vector<int> parts = health_.partsOfDef(reg.get<Thing>(pawn).def, partDef);
    int best = -1;
    float bestHp = 0.0f;
    const size_t start = parts.empty() ? 0 : static_cast<size_t>(unit(pawn, now, salt) * static_cast<double>(parts.size())) % parts.size();
    for (size_t i = 0; i < parts.size(); ++i) {
        const int part = parts[(start + i) % parts.size()];
        const float hp = health_.partHitPoints(pawn, part);
        if (hp > bestHp) {
            best = part;
            bestHp = hp;
        }
    }
    if (best < 0) {
        return false;
    }
    const float max = health_.partMaxHitPoints(pawn, best);
    const float amount = std::min(max * fraction, bestHp - max * (1.0f - rules_.damageMaxFraction));
    if (amount <= 0.0f || !health_.addInjury(pawn, best, damage, amount)) {
        return false;
    }
    const std::vector<std::string> labels = health_.partLabels(reg.get<Thing>(pawn).def);
    emit(EventType::ChemicalDamage, pawn, chemical, amount,
         static_cast<size_t>(best) < labels.size() ? labels[static_cast<size_t>(best)].c_str() : partDef.c_str());
    return true;
}

// Heavy use hurts an organ for good (wiki: liver for alcohol from 45% tolerance, lung for smokeleaf from 36%, kidney
// for psychite from 45%; brain after a major overdose; wake-up risks a heart attack). The thresholds are the wiki's,
// the rates are ours.
void DrugSystem::stepDamage(Entity pawn, DrugState& state, int64_t now) {
    if ((now + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.damageCheckEvery != 0) {
        return;
    }
    const double every = static_cast<double>(rules_.damageCheckEvery);
    for (const ChemicalState& cs : state.chemicals) {
        const int index = chemicalIndex(cs.chemical);
        if (index < 0) {
            continue;
        }
        const DrugChemical& chem = chemicals_[static_cast<size_t>(index)];
        if (!chem.damagePart.empty() && cs.tolerance >= chem.damageMinTolerance) {
            const float span = std::max(0.001f, 1.0f - chem.damageMinTolerance);
            const float t = std::clamp((cs.tolerance - chem.damageMinTolerance) / span, 0.0f, 1.0f);
            const double mtb = static_cast<double>(rules_.damageMtbDaysAtThreshold + (rules_.damageMtbDaysAtFull - rules_.damageMtbDaysAtThreshold) * t);
            if (unit(pawn, now, 0xC4E3ULL + static_cast<uint64_t>(index)) < every / (mtb * 86400.0)) {
                hurtOrgan(pawn, chem.damagePart, rules_.damageFraction, now, chem.def, 0xC4E4ULL + static_cast<uint64_t>(index));
            }
        }
        if (chem.heartAttackMtbDays > 0.0f) {
            bool high = false;
            for (const DrugKind& k : kinds_) {
                high = high || (k.chemical == index && severity(pawn, k.high) > 0.0f);
            }
            if (high && unit(pawn, now, 0x4EA27ULL + static_cast<uint64_t>(index)) < every / (static_cast<double>(chem.heartAttackMtbDays) * 86400.0)) {
                hurtOrgan(pawn, "Heart", rules_.heartAttackFraction, now, chem.def, 0x4EA28ULL);
            }
        }
    }
    if (severity(pawn, overdoseHediff_) >= rules_.majorOverdose && unit(pawn, now, 0xB8A1AULL) < every / (static_cast<double>(rules_.brainDamageMtbDays) * 86400.0)) {
        hurtOrgan(pawn, "Brain", rules_.brainDamageFraction, now, DefId{}, 0xB8A1BULL);
    }
}

bool DrugSystem::preventsDisease(Entity pawn, const std::string& disease) const {
    if (states_.find(pawn) == states_.end()) {
        return false;
    }
    for (size_t id = 0; id < prevents_.size(); ++id) {
        if (!prevents_[id].empty() && severity(pawn, DefId{static_cast<uint32_t>(id)}) > 0.0f &&
            std::find(prevents_[id].begin(), prevents_[id].end(), disease) != prevents_[id].end()) {
            return true;
        }
    }
    return false;
}

float DrugSystem::restFallFactor(Entity pawn) const {
    if (restHediffs_.empty() || states_.find(pawn) == states_.end()) {
        return 1.0f;
    }
    float factor = 1.0f;
    for (const DefId hediff : restHediffs_) {
        if (severity(pawn, hediff) > 0.0f) {
            factor *= hediffs_[hediff.value].restFallFactor;
        }
    }
    return factor;
}

void DrugSystem::prune() {
    for (auto it = states_.begin(); it != states_.end();) {
        it = things_.exists(it->first) ? std::next(it) : states_.erase(it);
    }
}

void DrugSystem::hashState(StateHasher& h) const {
    if (states_.empty()) {
        return;  // a game with no drugs hashes as it did before them
    }
    h.add(static_cast<uint64_t>(states_.size()));
    for (const auto& [pawn, s] : states_) {
        h.add(static_cast<uint64_t>(pawn));
        h.add(s.policy.data(), s.policy.size());
        h.add(s.searchAfter);
        if (s.bingeUntil != 0) {  // only while on a binge, so older hashes do not change
            h.add(s.bingeUntil);
            h.add(s.bingeChemical.data(), s.bingeChemical.size());
        }
        h.add(static_cast<uint64_t>(s.edits.size()));
        for (const DrugPolicyEntry& e : s.edits) {
            h.add(e.drug.data(), e.drug.size());
            h.add(e.forAddiction);
            h.add(e.forJoy);
            h.add(e.scheduled);
            h.add(e.daysFrequency);
            h.add(e.moodBelow);
            h.add(e.joyBelow);
        }
        h.add(static_cast<uint64_t>(s.chemicals.size()));
        for (const ChemicalState& c : s.chemicals) {
            h.add(c.chemical.data(), c.chemical.size());
            h.add(c.tolerance);
            h.add(c.addicted);
            h.add(c.need);
            h.add(c.progress);
            h.add(c.armed);
        }
        h.add(static_cast<uint64_t>(s.last.size()));
        for (const DrugDose& d : s.last) {
            h.add(d.drug.data(), d.drug.size());
            h.add(d.at);
        }
    }
}

}  // namespace colony
