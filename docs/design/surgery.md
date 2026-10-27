# Surgery and prosthetics

Code: `ColonySim/Surgery.h`, `Private/Surgery.cpp` (the operation list and the success rule), `Private/SurgeryJobs.cpp` (the
`Operate` job), the artificial-part functions at the end of `Private/Health.cpp`, the `OperateGiver` in `Private/WorkGivers.cpp`.
Tests: `Tests/SurgeryTests.cpp`. Task H3. Data: D7 ([medicine-data.md](medicine-data.md)); the new numbers are in
`Rules/MedicineRules`.

A colonist who lost a leg can walk again on a peg leg, a prosthetic or a bionic one. A doctor operates on a patient lying in
bed: it takes a limb off, takes an organ out, or fits a part. Whether it works is a roll.

## Operations on a patient

An **operation** is a line on a patient (`Operations` component: `list` of `Operation{id, recipe, part}`, `nextId`): a Recipes
def with a `surgery` block (`AmputateLimb`, `RemoveBodyPart`, `InstallPegLeg`, `InstallWoodenHand`, `InstallProstheticArm`,
`InstallBionicLeg`...) on one body part (its index in the patient's Bodies def). It is RimWorld's operation bill.
`AddOperationCommand{pawn, recipe, part}` plans one and `RemoveOperationCommand{pawn, operation}` takes it off; ids count up per
patient and are never reused. `SurgerySystem::lines(pawn)` gives a panel the labels. At most 12 lines per patient.

`SurgerySystem::eligible(patient, recipe, part, why)` is the one rule for what can be done, used by the command, the doctor
and the job:

- **Install** (`surgery.addsHediff`): the part's def must be the recipe's `surgery.part` ("Leg" for both legs), with no
  artificial part on it already, and not under a lost part. `requiresMissing` recipes (peg leg, wooden foot and hand, prosthetic
  arm and leg) need the natural part to be gone; the bionic ones (arm, leg, eye, ear) also go on a healthy or wounded part and
  replace it.
- **Remove** (`surgery.removes`): a natural part that is there, can be destroyed and has nothing in it that keeps the patient
  alive (no torso, neck, head, brain, heart, liver...); `AmputateLimb` is for limbs only (the parts the limb capacities are made of:
  arm, hand, fingers, leg, foot, toes). On a part with an artificial one on it, the recipe takes the artificial part off.
- The recipe's `researchPrerequisite` must be done (`InstallBionicLeg` needs `BionicReplacements`).
- A line whose part has changed meanwhile (the leg was taken off by another operation) is dropped (`prune`).

## Artificial parts in the body

`Health::added` lists `AddedPart{part, hediff}`: a Hediffs def of class `AddedPart` on a body part. It stands for the whole part and
everything under it:

- its **efficiency** (`addedPart.efficiency`: wooden 0.6, prosthetic 0.8, bionic 1.25) replaces the natural part's, times the
  share of its hit points it still has (damage on it still counts), so a bionic leg works at more than 100%;
- the parts under it (foot, toes, hand, fingers) work at full and can't be hit (`PartCondition::Covered`);
- fitting one over a **lost** part tends the lost part (it stops bleeding and hurting); fitting over a **wounded** part clears the
  wounds on it and what is under it;
- if the artificial part's hit points run out it is destroyed like a natural part: the part is lost, the artificial part is gone;
- `HealthSystem::partCondition(pawn, part)` says Whole, Missing, Under, Covered or Fitted; `installPart`, `uninstallPart`,
  `removePart`, `botch` do the changes; the report lists `addedParts` and leaves a fitted part out of `missingParts`.

The capacities follow with no special code: one peg leg and one natural leg make Moving (1.0 + 0.6) / 2 = 0.8, a prosthetic
(1.0 + 0.8) / 2 = 0.9, a bionic leg on a healthy one (1.0 + 1.25) / 2.

## The job

`Operate` (the Doctor work type, task `Operate`, after Tend and Rescue; `OperateGiver`): a patient who has an operation planned
**goes to bed** (`JobSystem::needsBedRest`: needs rest, or has an operation; the Patient and bed-rest work types and prisoners
use it; the patient stays until the line is done or removed, as RimWorld's does), and a doctor does the first line it can:
skilled enough (`minSkill` against its Medicine level), the part to fit and a medicine within reach (`findPartItem` for the part,
`findMedicine` for the medicine the patient's care setting allows; no medicine is fine). The patient must lie in a bed
(`lyingInBed`: downed on it, resting or sleeping in it). Any bed will do; the hospital bed is better.

The job is the DoBill steps with the patient for the bench: go to the part, pick it up, go to a cell beside the bed, hand it
over (then the medicine), work (`recipe.work` at the Medicine speed times Manipulation times the light, like any bill), finish.
Both items are used up when the operation is done, **whether or not it works**. A job that ends early puts what was handed over back
on the ground. The job fails if the patient leaves the bed or the line is taken off.

## Does it work

```
chance = recipe.successChance x skillFactor(Medicine level) x Manipulation x Sight
         x bed.surgerySuccessChanceFactor (1.07 for the hospital bed, 1 otherwise)
         x cleanlinessFactor(the room's cleanliness; the cell's, outdoors)
         x medicineFactor (0.7 with none, else 0.7 + 0.3 x potency but at least 0.7)
         never above 0.98
```

`skillFactor` is `surgerySuccessBySkill` read between its points (the wiki's: 0.1 at level 0, 0.6 at 5, 0.9 at 10, 1.0 at 15, 1.1 at 20);
`cleanlinessFactor` is `surgeryCleanlinessFactor` (0.85 at -5, 1.0 at 0, 1.07 at 2). The roll comes from the game second, the
doctor, the patient and the line, not the shared stream, so who is asked first can't change it.

- **Success:** the part is taken off, or fitted; the wound is tended with a normal tend quality (skill, medicine, bed) so it does not
  bleed. Taking an artificial part off gives its item back (dropped beside the doctor).
- **Failure:** the part is cut for `surgeryBotchHpFraction` (30%) of its hit points (the nearest part above it if it is lost), never
  destroyed; the part to fit is lost; and the patient dies with chance `(1 - chance) x surgeryFailDeathChanceFactor` (10%): death cause
  "a failed operation" (`DeathCause::Surgery`).
- The doctor gets `workSkillXp` (or `surgeryXp`, 1,200) of Medicine experience either way.

**Harvest organ** (`HarvestOrgan`, `surgery.harvests`): a removal of a whole, natural part that has an item named "Organ" plus the
part def (OrganKidney, OrganLung, OrganStomach, OrganEye, OrganEar). It drops beside the doctor. On success it calls
`PrisonerSystem::noteOrganHarvested`, so the moods in `prisoners.md` fire. Parts the patient could not live without (heart, liver)
are refused, like any removal. The 600 s work is a guess.

Event `OperationDone` (`thing` the doctor, `target` the patient, `def` the recipe, `part`, `amount` the chance, `message`
"success", "failed" or "the patient died"); `PawnAction::Operate` when the work starts.

## Numbers that are ours

Everything but the skill table and the hospital bed factor is a first guess (`unverified` or `ours` in `MedicineRules`): the cleanliness
curve, the medicine factor, the 98% cap, the botch size, the death rule, the XP and the 12-line limit. RimWorld has them as
separate stats; tune them here.

## Save

Health section 4: `added` (part, Hediffs defName). Medicine section 2: `operations` (nextId and the lines, recipe by name). An
`Operate` job in progress saves in the job (`site` the patient, `bill` the line, `cell` where the doctor stands, `picks`) like a bill,
and `afterLoad` checks the carried item matches the step. Both are in the state hash.

## Not here

Organ transplants (harvested organs exist as items but nothing fits them yet), implants (cochlear, joywire...) and other non-limb added parts, surgery on
animals, the operation tab on the health panel and a part-picking UI (U18), treating a prosthetic's wear, a doctor operating on itself,
anaesthetic.
