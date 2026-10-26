# Medicine and disease data (D7)

Data only: no sim code reads most of this yet. The tasks that use it are H1 (doctors, tending, scars),
E2 (temperature sicknesses), T-tasks (disease incidents) and later surgery and bench work. Test:
`DefsTests.cpp` "medicine and disease data (D7)". Numbers are RimWorld's from memory; every def lists
what to check in `unverified` (the wiki pages could not be fetched this run), and field names that are
ours are in `ours`.

## Items (`Things/Items.json`)

| Item | Potency | Best tend quality it allows | Notes |
| --- | --- | --- | --- |
| `MedicineHerbal` herbal medicine | 0.6 | 0.7 | healroot's yield (D4's `Plant_Healroot`); rots fast |
| `MedicineIndustrial` medicine | 1.0 | 1.0 | as before, now in the `Medicine` category |
| `MedicineLumen` lumen medicine | 1.6 | 1.3 | |

`HealthRules.medicineMaxTendQuality` (F1) holds the same caps by medicine kind.

Artificial parts are items in the `BodyPartsArtificial` category: `PegLeg`, `WoodenFoot`, `WoodenHand`,
`ProstheticArm`, `ProstheticLeg`, `BionicArm`, `BionicLeg`, `BionicEye`, `BionicEar`. Wooden ones can be
made at the crafting spot (`MakePegLeg` and so on); the prosthetic and bionic ones have no maker until a
bench task adds one.

## Hediffs (`Hediffs/Hediffs.json`)

- **Diseases** (`hediffClass: Disease`, parent `DiseaseBase`): `Infection` (what a wound can turn into,
  `infectionChance` is already on the injuries), `Flu`, `Plague`, `Malaria`, `SleepingSickness`, `GutWorms`,
  `MuscleParasites`, `FoodPoisoning`. Fields: `immunity {gainPerDaySick, severityPerDayNotImmune,
  severityPerDayImmune}` (severity rises until immunity is built, then falls), `tend {durationDays,
  severityPerDayAtQuality1}` (a good tend slows it; the real effect scales with quality), `lethalSeverity`
  (0 means it never kills: gut worms, muscle parasites, food poisoning), and `stages` like blood loss
  (`capacityOffsets`, `painOffset`, `lifeThreatening`). Sleeping sickness and gut worms also carry
  `restFallFactor` and `hungerRateFactor`.
- **Environment** (`hediffClass: Environment`): `Hypothermia`, `Heatstroke`, `ToxicBuildup`, with
  `severityPerDay` inputs for E2 and stages down to near death; `Frostbite` is an injury of fingers and toes
  (`extremities`, `appliesBelowC`).
- **Scars**: `Scar` (no pain) and `OldWound` (aches) are what a healed injury may leave behind; the chance is
  `HealthRules.scarChance` (F1). The hediffs' `neverScars` flag (bruise, frostbite) stays.
- **Artificial parts** (`hediffClass: AddedPart`): `addedPart {part, item, efficiency, betterThanNatural}`.
  Efficiency: wooden 0.6, prosthetic 0.8, bionic 1.25.

## Recipes (`Recipes/Recipes.json`)

`SurgeryBase` (skill Medicine, no bench, `surgery.needsBed`). `Install<Part>` for each of the nine
parts: `surgery {part, addsHediff, ingredient, successChance, requiresMissing}` plus `minSkill` (wooden
0, prosthetic 4, bionic 8) and `workAmount` in game seconds. `RemoveBodyPart` and `AmputateLimb` have
`anyPart` and `removes`. Surgery recipes have no `recipeUsers`, so the bill system never offers them at a
bench; H1 and later read them for the doctor job and the patient's health tab. Craft recipes
(`MakePegLeg`, `MakeWoodenFoot`, `MakeWoodenHand`) use the new `CraftingSpot`
(`Things/Buildings.json`, 1 wood).

## Hospital bed

`HospitalBed`: a bed (`building.bed`) with `building.medical`, comfort 0.7, `medicalTendQualityOffset`
0.15, `surgerySuccessChanceFactor` and `immunityGainSpeedFactor` 1.07; heals at the faster
`HealthRules.restHealBonusPerDay.HospitalBed` rate that F1 already has. Cost 25 steel and 2 components.

## Rules

`Rules/MedicineRules`: tend quality by Medicine skill (with a random factor and a penalty with no medicine),
surgery success by skill, disease onset mean-time-between in days, food poisoning chances and the body
temperatures at which cold and heat start (all unverified or ours; H1 and E2 tune them).

The system that reads the items, rules and the hospital bed is [medicine.md](medicine.md) (H1).

## Checker

`tools/check_defs.py` now also checks recipe `ingredients[].items[]`, `surgery.addsHediff`,
`surgery.ingredient`, `surgery.part` and hediff `addedPart.part` and `addedPart.item`.
