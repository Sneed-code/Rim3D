# Medicine, doctors and rescue

Surgery and prosthetics (H3) build on this: [surgery.md](surgery.md).

Code: `ColonySim/Medicine.h`, `Private/Medicine.cpp` (the numbers and the tend quality), `Private/MedicineJobs.cpp`
(the three jobs and the bed and medicine searches), the four scanners in `Private/WorkGivers.cpp`, scars in
`Private/Health.cpp`. Tests: `Tests/MedicineTests.cpp`. Task H1. Data: D7 ([medicine-data.md](medicine-data.md)); the
numbers are in `Rules/MedicineRules` and `Rules/HealthRules`.

A colonist who is hurt gets looked after the way RimWorld's do: a doctor tends the wounds with medicine, a downed
colonist is carried to a bed, and the patient lies in bed until the wounds have healed. A healed wound may leave a scar.

## Tending

`TendPatients` (the Doctor work type) offers every colonist (a pawn of the player's side that thinks, so with a
`Worker`) with an injury or lost part nobody has tended (`HealthSystem::needsTending`: the injury's Hediffs def is
`tendable`, which a bruise is not, and it is not a scar). If anyone is bleeding, only the bleeding ones are offered.
A doctor may tend itself (quality x 0.7, `selfTendQualityFactor`). Nearest first.

`JobType::TendPatient`: walk to the medicine, pick one up, walk to the patient, work, tend. The medicine is the one the
patient's **care setting** allows (below), the strongest first, then the nearest, that nobody else holds. With none the
doctor tends by hand. The work takes `tendWorkSeconds` (864 = RimWorld's 600 ticks) at 100% speed; speed is Manipulation
times the light (`lightWorkFactor`). A patient who walks off is followed (a few times, `maxRepaths`). At the end
`HealthSystem::tendAll(patient, quality)` tends every untended injury and lost part at once (so they stop bleeding and
heal faster), the medicine is used up if anything was tended, the doctor gets `tendXp` (500) of Medicine experience, and a
`PawnTended` event comes out (`thing` the doctor, `target` the patient, `def` the medicine, `amount` the quality).

```
quality = ((skill quality(Medicine level) x Manipulation) x (medicine potency, or 0.7 with none) + bed offset)
          x roll(0.7 to 1.3) [x 0.7 if tending itself], never above what the medicine allows (herbal 0.7, medicine 1.0,
          lumen 1.3, none 0.7)
```

`skill quality` is the table `tendQualityBySkill` read in between its points (0.2 at 0, 0.5 at 5, 0.8 at 10, 1.0 at 15,
1.2 at 20); the bed offset is the bed's `stats.medicalTendQualityOffset` (0.15 for the hospital bed) if the patient lies
in a bed. The roll is a hash of the game second and the two pawns' ids, not the shared random stream, so who is asked
first can't change it. A colonist with no skills (a test pawn) counts as level 8. `MedicineSystem::tendQuality`.

**Care setting** (`MedicalCare`, on the pawn as `MedicalSettings`; none means Best): NoCare (nobody tends it),
NoMedicine, Herbal (herbal or none), Normal (potency up to 1.0) and Best. `SetMedicalCareCommand{pawn, "Normal"}`
sets it. The Herbal and Normal limits are potencies in `MedicineRules` (`careHerbalMaxPotency` 0.6,
`careNormalMaxPotency` 1.0; **ours**, RimWorld has them as fixed categories).

## Rescue

`Rescue` (Doctor): a downed colonist that no bed holds (`JobSystem::needsRescue`). It is offered after tending, so a
bleeding patient is tended where it lies first. `JobType::Rescue`: walk to the patient, lift it (`PawnAction::LiftPawn`),
carry it (the patient's position follows the rescuer's every second), lay it in the bed (`LayPawn`). The patient's
`Health::rest` becomes `HospitalBed` or `Bed`, which decides how fast it heals. There is no carried-pawn component: the
rescuer's job holds the patient (`thing`), the bed (`chair`) and the bed's cell (`cell`), and all three are saved with
the job. A rescue fails, and the patient stays where it is, if the patient gets up, the bed is lost or someone else lies
in it.

**Which bed** (`JobSystem::findPatientBed`): a medical bed (`building.medical`) first, then the patient's own bed, then a
free bed nobody owns; the nearest of its kind; one the helper can reach and nobody has reserved or lies in. A borrowed
bed is not claimed. **Medical beds are not slept in**: `findBed` (ordinary sleeping) skips them, as RimWorld does.

A downed pawn's `Health::rest` follows the bed under it every step (`restUnderPawn`); a pawn that is up and not lying in
a bed goes back to the ground rate.

## Bed rest

`GoToMedicalBed` (the Patient work type) and `RestToHeal` (PatientBedRest) offer the patient itself a bed when it needs
rest (`HealthSystem::needsRest`: an injury that is tended and still healing, or any bleeding) and isn't down: the first
only a medical bed, the second any bed (medical, own, free). `JobType::BedRest`: walk to the bed, lie in it, and stay
until `needsRest` is false (then the job is done). It lets go of the bed to eat (a hungry patient), not to sleep. The
doctor tends a resting patient in bed. Scars and old wounds never count.

## Scars

A healed injury (it reaches 0 hit points) may leave a permanent one (`HealthSystem::leaveScar`): the chance rises from
0 at `minHpLost` (4) to `max` (2%) at `fullAtHpLost` (14) hit points lost *at the injury's worst* (`Injury::peak`, kept
as it heals; saved from health section 3), as `HealthRules.scarChance`. Bruises (`neverScars`) and scars never scar. It
is a `Scar` of `scarSeverity` (1 hit point, no pain), or an `OldWound` (same size, it aches) if the injury had cost
`oldWoundFromHpLost` (14) or more; both **ours** (the first guess at RimWorld's rule). A scar is an ordinary `Injury`
whose Hediffs def is `permanent`: it takes its hit point off the part for good, is never picked to heal, is never
tended, and shows in the health report (`InjuryLine::permanent`). Event `PawnScarred`.

## Urgent work

Work types with a natural priority of 1100 or more (Firefighter, Patient, Doctor, PatientBedRest, BasicWorker) are asked
before the others **at the same priority level**, whatever the distance (RimWorld asks work types in order; the rest of
ours go nearest first; this is **ours**). And every 10 game seconds a pawn on a job it may drop (idling, hauling,
cleaning, research, mining, cutting, playing) asks those work types only (`interruptForEmergency`): a doctor drops a
wait or a haul within ten seconds of someone bleeding.

## Save

Section `medicine` (version 1): `medical`, the care setting of every pawn that has one. `Health` section is version 3:
each injury also stores `peak`. A rescue or tend in progress saves in the job (`thing`, `site`, `chair`, `cell`, `step`),
and the carried medicine is the pawn's `Carrying`. Also fixed: the job-type range check in the save now allows every job
type (it stopped at `Research`, so a save made while someone was cleaning filth could not be loaded).

## Not here

Infections, diseases and immunity are built ([diseases.md](diseases.md)), and so are surgery and prosthetics ([surgery.md](surgery.md)). Not here: feeding patients, medicine stock policy, the health tab's care setting and bed buttons (U8 follow-up), a
carried pawn shown in a rescuer's arms (Unreal draws it where the sim puts it), self-tend toggle, tending animals.
