#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class StateHasher;
class Things;

// Medicine (task H1). See docs/design/medicine.md.
//
// Doctors tend patients with medicine; how well depends on their Medicine skill, the medicine and the bed. This
// holds the numbers (MedicineRules and the medicine items' stats) and the tend quality formula; the jobs are in
// MedicineJobs.cpp and the injuries are in Health.h. Unreal does not include this header.

// How far a patient's medicine may go (RimWorld's medical care setting). Best is the default.
enum class MedicalCare : uint8_t {
    NoCare,      // nobody tends it
    NoMedicine,  // tended by hand only
    Herbal,      // herbal medicine or none
    Normal,      // ordinary medicine, herbal or none
    Best,        // anything, the strongest first
};

COLONY_API const char* medicalCareName(MedicalCare care);
// Parses a name from medicalCareName (case as written); false if it is not one.
COLONY_API bool medicalCareFromName(const std::string& name, MedicalCare& out);

// On a pawn: its medical care setting. A pawn without one has Best.
struct MedicalSettings {
    MedicalCare care = MedicalCare::Best;
};

// One medicine item def (a Things def with `stats.medicalPotency`).
struct MedicineKind {
    DefId def;
    float potency = 1.0f;
    float maxQuality = 1.0f;  // the best tend quality it allows
};

// The numbers from MedicineRules (with RimWorld's values when the def is missing).
struct COLONY_API MedicineRules {
    bool present = false;
    std::vector<std::pair<float, float>> tendQualityBySkill = {{0, 0.2f}, {5, 0.5f}, {10, 0.8f}, {15, 1.0f}, {20, 1.2f}};
    float randomLow = 0.7f;
    float randomHigh = 1.3f;
    float noMedicineFactor = 0.7f;
    float noMedicineMaxQuality = 0.7f;   // HealthRules.medicineMaxTendQuality.None
    int64_t tendWorkSeconds = 864;       // 600 RimWorld ticks, at 100% speed
    float tendXp = 500.0f;
    float selfTendQualityFactor = 0.7f;
    float herbalMaxPotency = 0.6f;
    float normalMaxPotency = 1.0f;
    int defaultSkillLevel = 8;           // for a doctor with no skills (a test pawn)

    static MedicineRules read(const DefDatabase& defs);
};

class COLONY_API MedicineSystem {
public:
    MedicineSystem(Things& things, const DefDatabase& defs);

    const MedicineRules& rules() const { return rules_; }
    // The medicine item kinds, strongest first.
    const std::vector<MedicineKind>& kinds() const { return kinds_; }
    // Null for a def that is not a medicine.
    const MedicineKind* kindOf(DefId def) const;

    // The care setting of a pawn (Best without MedicalSettings).
    MedicalCare careOf(Entity pawn) const;
    // Whether a patient with this setting may be given this medicine.
    bool allows(MedicalCare care, const MedicineKind& kind) const;

    // A doctor's base tend quality for a Medicine skill level (the table, interpolated).
    float skillQuality(int level) const;
    // The quality of one tend: (skill quality x Manipulation x the medicine's potency, or the no-medicine factor,
    // plus the bed's offset) x `random` (0 to 1 mapped onto the random factor range), self-tending x its factor,
    // never above what the medicine allows (or the no-medicine cap).
    float tendQuality(int skillLevel, float manipulation, const MedicineKind* medicine, float bedOffset, bool self,
                      float random01) const;

    // Hash of the settings on pawns, for Sim::stateHash.
    void hashPawn(Entity pawn, StateHasher& h) const;

private:
    Things& things_;
    const DefDatabase& defs_;
    MedicineRules rules_;
    std::vector<MedicineKind> kinds_;
    std::vector<int32_t> kindIndex_;  // by DefId value, -1 for none
};

}  // namespace colony
