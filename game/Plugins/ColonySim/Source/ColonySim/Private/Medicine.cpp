#include "ColonySim/Medicine.h"

#include "ColonySim/Hash.h"
#include "ColonySim/Things.h"

#include <algorithm>

namespace colony {

const char* medicalCareName(MedicalCare care) {
    switch (care) {
        case MedicalCare::NoCare: return "NoCare";
        case MedicalCare::NoMedicine: return "NoMedicine";
        case MedicalCare::Herbal: return "Herbal";
        case MedicalCare::Normal: return "Normal";
        case MedicalCare::Best: return "Best";
    }
    return "?";
}

bool medicalCareFromName(const std::string& name, MedicalCare& out) {
    for (const MedicalCare care : {MedicalCare::NoCare, MedicalCare::NoMedicine, MedicalCare::Herbal, MedicalCare::Normal,
                                   MedicalCare::Best}) {
        if (name == medicalCareName(care)) {
            out = care;
            return true;
        }
    }
    return false;
}

MedicineRules MedicineRules::read(const DefDatabase& defs) {
    MedicineRules r;
    if (const Def* health = defs.find("Rules", "HealthRules")) {
        r.noMedicineMaxQuality = static_cast<float>(health->number("medicineMaxTendQuality.None", r.noMedicineMaxQuality));
    }
    const Def* rules = defs.find("Rules", "MedicineRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    if (const size_t n = rules->count("tendQualityBySkill")) {
        r.tendQualityBySkill.clear();
        for (size_t i = 0; i < n; ++i) {
            const std::string at = "tendQualityBySkill." + std::to_string(i);
            r.tendQualityBySkill.emplace_back(static_cast<float>(rules->number((at + ".0").c_str())),
                                              static_cast<float>(rules->number((at + ".1").c_str())));
        }
        std::sort(r.tendQualityBySkill.begin(), r.tendQualityBySkill.end());
    }
    r.randomLow = static_cast<float>(rules->number("tendQualityRandomFactor.0", r.randomLow));
    r.randomHigh = static_cast<float>(rules->number("tendQualityRandomFactor.1", r.randomHigh));
    r.noMedicineFactor = static_cast<float>(rules->number("tendQualityNoMedicineFactor", r.noMedicineFactor));
    r.tendWorkSeconds = std::max<int64_t>(1, rules->integer("tendWorkSeconds", r.tendWorkSeconds));
    r.tendXp = static_cast<float>(rules->number("tendXp", r.tendXp));
    r.selfTendQualityFactor = static_cast<float>(rules->number("selfTendQualityFactor", r.selfTendQualityFactor));
    r.herbalMaxPotency = static_cast<float>(rules->number("careHerbalMaxPotency", r.herbalMaxPotency));
    r.normalMaxPotency = static_cast<float>(rules->number("careNormalMaxPotency", r.normalMaxPotency));
    return r;
}

MedicineSystem::MedicineSystem(Things& things, const DefDatabase& defs) : things_(things), defs_(defs) {
    rules_ = MedicineRules::read(defs_);
    kindIndex_.assign(defs_.size(), -1);
    for (const DefId id : defs_.idsOfKind("Things")) {
        const Def& def = defs_.get(id);
        if (def.has("stats.medicalPotency")) {
            MedicineKind kind;
            kind.def = id;
            kind.potency = static_cast<float>(def.number("stats.medicalPotency", 1.0));
            kind.maxQuality = static_cast<float>(def.number("stats.medicalQualityMax", kind.potency));
            kinds_.push_back(kind);
        }
    }
    std::stable_sort(kinds_.begin(), kinds_.end(), [](const MedicineKind& a, const MedicineKind& b) {
        return a.potency != b.potency ? a.potency > b.potency : a.def.value < b.def.value;
    });
    for (size_t i = 0; i < kinds_.size(); ++i) {
        kindIndex_[kinds_[i].def.value] = static_cast<int32_t>(i);
    }
}

const MedicineKind* MedicineSystem::kindOf(DefId def) const {
    if (!def.valid() || def.value >= kindIndex_.size() || kindIndex_[def.value] < 0) {
        return nullptr;
    }
    return &kinds_[static_cast<size_t>(kindIndex_[def.value])];
}

MedicalCare MedicineSystem::careOf(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const MedicalSettings* settings = reg.valid(pawn) ? reg.try_get<MedicalSettings>(pawn) : nullptr;
    return settings != nullptr ? settings->care : MedicalCare::Best;
}

bool MedicineSystem::allows(MedicalCare care, const MedicineKind& kind) const {
    switch (care) {
        case MedicalCare::NoCare:
        case MedicalCare::NoMedicine: return false;
        case MedicalCare::Herbal: return kind.potency <= rules_.herbalMaxPotency + 1.0e-4f;
        case MedicalCare::Normal: return kind.potency <= rules_.normalMaxPotency + 1.0e-4f;
        case MedicalCare::Best: return true;
    }
    return false;
}

float MedicineSystem::skillQuality(int level) const {
    const auto& table = rules_.tendQualityBySkill;
    if (table.empty()) {
        return 1.0f;
    }
    const float x = static_cast<float>(level);
    if (x <= table.front().first) {
        return table.front().second;
    }
    for (size_t i = 1; i < table.size(); ++i) {
        if (x <= table[i].first) {
            const float span = std::max(1.0e-6f, table[i].first - table[i - 1].first);
            return table[i - 1].second + (table[i].second - table[i - 1].second) * (x - table[i - 1].first) / span;
        }
    }
    return table.back().second;
}

float MedicineSystem::tendQuality(int skillLevel, float manipulation, const MedicineKind* medicine, float bedOffset, bool self,
                                  float random01) const {
    const float factor = medicine != nullptr ? medicine->potency : rules_.noMedicineFactor;
    float quality = skillQuality(skillLevel) * std::max(0.0f, manipulation) * factor + bedOffset;
    quality *= rules_.randomLow + (rules_.randomHigh - rules_.randomLow) * random01;
    if (self) {
        quality *= rules_.selfTendQualityFactor;
    }
    const float cap = medicine != nullptr ? medicine->maxQuality : rules_.noMedicineMaxQuality;
    return std::clamp(quality, 0.0f, cap);
}

void MedicineSystem::hashPawn(Entity pawn, StateHasher& h) const {
    if (const MedicalSettings* settings = things_.registry().try_get<MedicalSettings>(pawn)) {
        h.add(static_cast<int>(settings->care));
    }
}

}  // namespace colony
