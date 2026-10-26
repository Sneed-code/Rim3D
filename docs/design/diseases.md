# Infections and diseases

Code: `Health.h` (`Disease`, `Immunity`, `HealthSystem::contract` and friends), `Private/Health.cpp` (section "Infections and
diseases"). Tests: `Tests/DiseaseTests.cpp`. Data: `Hediffs` of class `Disease` and `Rules/MedicineRules`
([medicine-data.md](medicine-data.md)). Task H2. The numbers come from the RimWorld wiki (read 2026-10-26); what is ours
is said below.

A pawn can be sick with any number of diseases. Each is a `Disease` on its `Health` (def, severity 0 to 1, the last
tend, an infection's wound) and each disease def the pawn has met has an `Immunity` (0 to 1).

## The race

Every `diseaseUpdateSeconds` (60) a pawn with a disease moves it on (each pawn at its own offset). Per day:

- **Immunity** rises by `immunity.gainPerDaySick` x the gain speed. Gain speed is the rest factor (ground 1, bed 1.05 **ours**,
  hospital bed 1.07, `MedicineRules.immunityGainSpeedByRest`) x the pawn's BloodFiltration. Several infections in
  different wounds raise one immunity, once.
- **Severity** changes by `severityPerDayNotImmune` while immunity is under 1 and by `severityPerDayImmune` (negative) after.
  While a tend lasts it also changes by `tend.severityPerDayAtQuality1` x the tend quality.
- At severity 0 the disease is gone (`PawnRecovered`); the immunity stays, so the pawn can't catch it again, except an
  infection (`keepsImmunity: false`, **ours**: the next wound can still go bad).
- At `lethalSeverity` (1) the pawn dies (`DeathCause::Disease`, "illness"). Gut worms, muscle parasites and food poisoning
  never kill (`lethalSeverity` 0).

Flu from the wiki: immunity 0.2388 a day against severity 0.2488, so a healthy pawn on the ground dies on day 4.0 just before
immunity arrives on day 4.2; in a bed (1.05) or a hospital bed (1.07) it wins, and tending slows the climb by up to 0.0773.
Untended infection kills in 1.2 days, immunity only comes on day 1.55: it must be tended.

**Stages** work like blood loss's: the stage at the current severity adds `capacityOffsets`, `capacityMax` caps (an infection
or sleeping sickness in the last stage leaves 10% Consciousness) and `painOffset` to the pain. A stage with `visible: false`
isn't noticed yet (nothing tends or rests for it; no stage of the current data is hidden). Vomiting is not modelled.

## Tending

`HealthSystem::tendAll` (the doctor's job and the dev command) tends diseases too. A disease that can be tended
(`tendable`, not food poisoning), shows, and has no tend running gets the quality, kept for `tend.durationDays` (flu and
infection 0.5, plague 0.625, sleeping sickness 1.33, worms and parasites 2). When it runs out `needsTending` is true again and a
doctor comes back. A sick colonist `needsRest` and goes to bed (H1's bed rest) until the disease is gone.
`urgentlyNeedsTending` is also true for a disease in a life-threatening stage.

**Gut worms and muscle parasites** don't progress and don't go by themselves: each tend adds its quality to a total and at
3.0 (`tend.cureAtTotalQuality`, every 2 days) they are cured. Worms double hunger (`hungerRateFactor`, in `NeedsSystem`);
parasites double the fall of rest (`restFallFactor`) and cost Moving and Manipulation 30%.

## Infection

When a wound is dealt (`damagePart`, so also bullets and bites), `rollInfection` rolls the Hediffs def's `infectionChance`
(cut, stab, gunshot, scratch, crush 15%, shredded 20%, bite and burn 30%; bruises, cracks and scars 0) times a factor for the
cleanliness of the cell the pawn stands in (`1 - 0.25 x cleanliness`, kept between 0.5 and 2; **ours**, RoomStats). On
success the injury gets `infectAt`, 6 to 18 hours on (the wiki's 15,000 to 45,000 ticks). Tending the wound first can still stop
it: the chance it goes on is 0.85 at tend quality 0 falling to 0.05 at quality 1 (`MedicineRules.infection`). At `infectAt`
the wound becomes an `Infection` disease on that part (`contract` with the part); a body part that is lost takes its infections
with it. A small wound that heals before `infectAt` never turns infected. The roll comes from the game seed, the pawn, the second and the wound, not the shared random stream.

## Food poisoning

`SpoilageSystem`'s poison handler (H2 installs it, [spoilage.md](spoilage.md)) gives `FoodPoisoning` (starts at severity 1,
falls 0.8 a day, Digestion and Eating down). Rotten food doesn't poison yet.

## For other systems

- `HealthSystem::contract(pawn, disease, severity, part)` gives a disease; `ContractDiseaseCommand` is the dev command.
- **T3's outbreaks:** `outbreak(disease, candidates, victims, rng)` gives it to up to `victims` of the candidates that can
  catch it (alive, not immune, not already sick); `onsetMtbDays(disease)` is `MedicineRules.diseaseOnsetMtbDays`.
  Nothing makes anyone ill by itself yet except wounds and bad food.
- `sick`, `immunity`, `diseaseSeverity`, `setImmunity`; the health report has a `diseases` list; events `PawnSick` and
  `PawnRecovered`; the mood situation `illness` (the Sick thought) for any disease with `makesSickThought`.

## Save

Health section version 4: each injury's `infectAt`, then `diseases` (def, severity, tend quality, tended until, tend total,
part) and `immunities`. All are in `stateHash`. Older saves load without them.

## Not here

Vomiting, the animal factor on infection (RimWorld: a fifth of a colonist's chance), the cleanliness of a tended wound, the
immunity-gain genes and traits, diseases for animals, a sick pawn's ban from work beyond what the capacities do, surgery to
cure worms (H3), the health tab's disease list (U8 follow-up).
