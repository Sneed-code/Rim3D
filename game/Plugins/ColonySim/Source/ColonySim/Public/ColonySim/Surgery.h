#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class HealthSystem;
class ResearchSystem;
class StateHasher;
class Things;
struct MedicineKind;

// Surgery and prosthetics (task H3). See docs/design/surgery.md.
//
// Operations are lines on a patient (RimWorld's operation bills): a surgery Recipes def (amputate, remove a part, install a
// peg leg or a bionic arm) on one body part. A doctor with the Doctor work type fetches the part to fit (and a medicine),
// walks to the patient in bed, and operates; whether it works is rolled from the doctor, the bed, the room and the medicine.
// Unreal does not include this header: it reads the lines through `SurgerySystem::lines` and the results through events.

// A Recipes def with a `surgery` block, read once.
struct SurgeryRecipe {
    DefId def;
    std::string defName;
    std::string label;
    int minSkill = 0;
    float work = 0.0f;           // game seconds of work at speed 1
    float successChance = 1.0f;  // the recipe's own factor
    float xp = 0.0f;             // Medicine experience for the doctor
    bool needsBed = true;
    // An install: fits `adds` (a Hediffs def of class AddedPart) on a part of `partDef`, using up one `ingredient` item.
    bool install = false;
    DefId adds;
    DefId ingredient;
    std::string partDef;
    bool requiresMissing = false;  // the natural part must be lost already
    // A removal: takes any part off (or the artificial part on it, which comes back as an item).
    bool removes = false;
    bool limbsOnly = false;
};

// One line on a patient. Ids count up per patient and are never reused.
struct Operation {
    uint32_t id = 0;
    DefId recipe;
    uint16_t part = 0;
};

// On a patient that has had an operation planned.
struct Operations {
    std::vector<Operation> list;
    uint32_t nextId = 1;
};

// The numbers (MedicineRules, with these values when the def is missing).
struct SurgeryNumbers {
    std::vector<std::pair<float, float>> successBySkill = {{0.0f, 0.5f}, {5.0f, 0.75f}, {10.0f, 1.0f}, {15.0f, 1.2f}, {20.0f, 1.4f}};
    std::vector<std::pair<float, float>> cleanlinessFactor = {{-5.0f, 0.85f}, {0.0f, 1.0f}, {2.0f, 1.07f}};
    float noMedicineFactor = 0.7f;
    float medicinePotencyWeight = 0.3f;  // factor = noMedicineFactor + weight x potency, but at least noMedicineFactor
    float maxSuccessChance = 0.98f;
    float failDeathChanceFactor = 0.1f;  // a failed operation kills with (1 - success chance) x this
    float botchHpFraction = 0.3f;        // a failed operation cuts the part for this share of its hit points
    float xp = 1200.0f;                  // Medicine experience for a recipe that names none
    int maxOperations = 12;              // lines on one patient

    static SurgeryNumbers read(const DefDatabase& defs);
};

// What decides the chance of one operation.
struct SurgeryConditions {
    int skill = 8;                        // the doctor's Medicine level
    float manipulation = 1.0f;            // the doctor's Manipulation and Sight
    float sight = 1.0f;
    float bedFactor = 1.0f;               // the bed's `stats.surgerySuccessChanceFactor`
    float cleanliness = 0.0f;             // of the room the patient lies in
    const MedicineKind* medicine = nullptr;  // the medicine used, if any
};

struct SurgeryResult {
    bool done = false;     // the operation was still possible and went ahead (it is off the patient's list either way)
    bool success = false;
    bool died = false;     // the patient died of it
    DefId returned;        // an artificial part taken off, as the item to put on the map
};

// A line as a panel shows it.
struct OperationLine {
    uint32_t id = 0;
    std::string recipe;    // the Recipes defName
    std::string label;     // "install peg leg"
    std::string part;      // "left leg"
    int minSkill = 0;
};

class COLONY_API SurgerySystem {
public:
    SurgerySystem(Things& things, const DefDatabase& defs, HealthSystem& health, const ResearchSystem* research);

    const SurgeryNumbers& numbers() const { return numbers_; }
    const std::vector<SurgeryRecipe>& recipes() const { return recipes_; }
    // Null for a def that is not a surgery recipe, or a name that is not one.
    const SurgeryRecipe* recipe(DefId def) const;
    const SurgeryRecipe* recipe(const std::string& defName) const;

    // ---- What can be done --------------------------------------------------------------------
    // Whether `recipe` can be done on this part of this patient now; `why` says what is wrong in plain words.
    bool eligible(Entity patient, const SurgeryRecipe& recipe, int part, std::string& why) const;
    // Every part of the patient the recipe can be done on now, in body order.
    std::vector<int> targets(Entity patient, const SurgeryRecipe& recipe) const;

    // ---- The patient's list (commands) -------------------------------------------------------
    // Plans an operation. Returns its id, or 0 with `error` in plain words.
    uint32_t add(Entity patient, const std::string& recipe, int part, std::string& error);
    bool remove(Entity patient, uint32_t operation, std::string& error);
    const Operations* of(Entity patient) const;
    const Operation* find(Entity patient, uint32_t operation) const;
    // True if the patient has an operation planned.
    bool pending(Entity patient) const;
    std::vector<OperationLine> lines(Entity patient) const;

    // ---- Doing it ----------------------------------------------------------------------------
    float skillFactor(int level) const;
    float cleanlinessFactor(float cleanliness) const;
    // The chance an operation works: the recipe's, times the doctor's skill, Manipulation and Sight, the bed, the room and
    // the medicine; never above `maxSuccessChance`.
    float successChance(const SurgeryRecipe& recipe, const SurgeryConditions& conditions) const;
    // Does the operation: `success` (rolled by the caller), the quality the wound is tended with, and a roll from 0 to 1 for
    // whether a failure kills. The operation leaves the list; operations that can no longer be done go with it.
    SurgeryResult perform(Entity patient, uint32_t operation, float chance, bool success, float tendQuality, float deathRoll01);
    // Takes off the lines that can no longer be done.
    void prune(Entity patient);

    void hashPawn(Entity pawn, StateHasher& h) const;

private:
    Things& things_;
    const DefDatabase& defs_;
    HealthSystem& health_;
    const ResearchSystem* research_;
    SurgeryNumbers numbers_;
    std::vector<SurgeryRecipe> recipes_;
    std::vector<int32_t> index_;  // by DefId value, -1 for none
};

}  // namespace colony
