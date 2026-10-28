# Drugs, addiction and drug policies

Code: `ColonySim/Drugs.h`, `Private/Drugs.cpp` (the system), `Private/DrugJobs.cpp` (choosing a dose, the player's
order), the dose itself in `JobSystem::eat` (`Private/NeedJobs.cpp`). Tests: `Tests/DrugTests.cpp`. Data:
`Things/Drugs.json` (the drugs, leaves, plants, drug lab and barrel), `Chemicals/`, `Hediffs/Drugs.json`,
`Recipes/Drugs.json`, `DrugPolicies/`, `Thoughts/Drugs.json`, `Rules/DrugRules.json`, new research in
`Research/Research.json`. Numbers are from the RimWorld wiki; what the wiki does not give is in each def's
`unverified` or `ours` list.

## The drugs

| Drug | Chemical | Kind | Made from, where | Research |
| --- | --- | --- | --- | --- |
| beer | alcohol | social | 25 hops make 5 beer, fermenting barrel | none |
| smokeleaf joint | smokeleaf | social | 4 leaves, crafting spot or drug lab | none |
| psychite tea | psychite | social | 4 psychoid leaves, a stove, Cooking 2 | psychoid brewing |
| flake | psychite | hard | 4 psychoid leaves, drug lab | psychite refining |
| yayo | psychite | hard | 8 psychoid leaves, drug lab | psychite refining |
| go-juice | go-juice | hard | 2 neutroamine and 1 yayo, drug lab | go-juice production |
| wake-up | wake-up | hard | 2 neutroamine, drug lab | wake-up production |
| luciferium | luciferium | medical | not made: found or traded | |

Bills also come in fours (`...Bulk`) for the lab. The drug lab needs `DrugProduction` research. Neutroamine is an item
traders and starting crates bring; nothing grows or makes it. New crops: smokeleaf, psychoid and hops plants, grown in
growing zones like any crop. The plants' names stay RimWorld's (`naming.md` proposes new ones, not applied).

## Taking a drug

A drug is a `Things` def with a `drug` block (chemical, category, addictiveness, minimum tolerance to addict,
tolerance gain, the high hediff and its severity, joy, rest, overdose). A pawn takes it through the Ingest job (walk
to it, pick it up, drink it where it lies); `eat` hands a drug to `DrugSystem::takeDose` instead of the food rules.
A dose:

- **High.** Raises the drug's high hediff by `highSeverity x (1 - 0.75 x tolerance)`, capped at 1. The high is an
  `Environment` hediff with stages, so its capacity offsets (go-juice +50% Moving, smokeleaf -30% Consciousness...)
  reach work and walking with no other code. It falls by `severityFallPerDay` (alcohol 0.75 a day; the numbers are
  picked so one dose lasts the wiki's hours). Its mood is a thought whose stage follows the hediff's stage.
- **Tolerance.** Goes up by the drug's gain and wears off per day (alcohol 1.6%, smokeleaf and psychite 1.5%).
  Psychite tea, flake and yayo share one. Go-juice, wake-up and luciferium have none.
- **Addiction roll.** A pawn that is not addicted becomes so with chance `addictiveness x multiplier(tolerance)` once its
  tolerance is at least the drug's minimum (social drugs: 25% beer, 15% joint, 10% tea; hard drugs from the first
  dose). The multiplier is the wiki's table: x1 at 0%, x5 at 50%, x15 at 80%. The roll uses a hash of pawn and game
  second, not the shared random stream. Luciferium always addicts.
- **Rest and joy.** Rest and recreation go up by the drug's amounts at once (wake-up +100% rest, smokeleaf -10%).
- **Overdose** (hard drugs only). +0.18 to 0.35 severity of the `Overdose` hediff a dose (and 0.5% of doses +0.75 at
  once). It falls 1 a day; 0.5 is minor, 0.75 major (nearly unconscious), 1 kills (`DeathCause::Overdose`).

## Addiction

Per pawn and chemical (`ChemicalState`): tolerance, addicted, need, progress.

- A dose fills the **need** by 0.9; it falls each day (alcohol, smokeleaf, psychite 0.5; go-juice, wake-up 0.333;
  luciferium 0.15, so a dose every 6 days).
- **Progress** is the way out: it starts at 0.5, rises each day (3.33%, go-juice and wake-up 4.5%) and a dose sets it
  back (beer 20%, joint 6%, tea 15%). At 1 the addiction is over (`AddictionCured`). Luciferium never heals.
- With the need at 0 the **withdrawal** hediff rises (full in half a day) with the wiki's penalties (alcohol -35 mood,
  -20% Consciousness, -30% Moving, -50% Manipulation...), and falls again when the need is filled or the addiction is
  gone. Luciferium's withdrawal keeps rising 0.1 a day and **kills at 1**, about ten days after the need ran out.
- Alcohol: a pawn that got drunk (high over 40%) has a **hangover** when the high is gone, 1 falling to 0 in a day.

## Drug policies

`DrugPolicies` defs: `NoDrugs`, `Social` (the default: beer and joints for joy), `OneDrinkPerDay`, `Unrestricted`,
`LuciferiumUser`. Each drug has an entry: for addiction (default on), for recreation, scheduled (every `daysFrequency`
days, only if mood or recreation is under a limit). `SetDrugPolicyCommand{pawn, policy}` gives one;
`SetDrugPolicyEntryCommand` changes a drug in the pawn's own copy; `TakeDrugCommand{pawn, thing}` orders a dose.

`thinkDrugs` sits after the urgent needs and before the schedule. Priorities: an addict whose need is under 0.3 and
whose policy allows the drug for addiction (3), a scheduled dose that is due (2), recreation when joy is under 0.5 in
an Anything or Joy hour, never for medical drugs (1). It does not drink while still high (over 0.25) or while the
overdose is over 0.4, and it does not take joy drugs during Work hours. No reachable item: the pawn waits a minute.

## Side effects

A hediff stage may carry `painFactor`, `workSpeed` and `hunger` (all multiply, default 1); `HealthSystem` works them out
with the capacities (`Health::workSpeedFactor`, `hungerFactor`; pain is scaled before it reaches the capacities and
the shock limit). Pain: smokeleaf x0.8, psychite tea x0.9, flake and yayo x0.5, go-juice x0.1, luciferium x0.8; beer
only "less the drunker" on the wiki, so x0.9, x0.8, x0.7 by stage is ours. Work speed: wake-up x1.5, go-juice x1.2 (they
multiply); `JobSystem::workFactor` (formerly `lightWorkFactor`) and research use it. Hunger: smokeleaf x1.3.

**Binges** (`DrugSystem::stepBinge`). A colonist addicted to a social drug in a major (or worse) mood band, or to a hard
drug in an extreme one, or one in withdrawal at any mood, rolls every 216 seconds against a mean wait of 1.5, 0.5 or 2 days
(`DrugRules`, ours). Never the protagonist (`Health::protagonist`) or a pawn under `PlayerControl`. A binge lasts 4 to 8
hours; `wants` then returns every non-medical drug of the chemical at priority 4, ignoring the policy and the
joy limits, until the high is over 0.9. It ends at the time, at a minor overdose, when the pawn is downed, or when
no drug can be found (`searchFailed`). Events `DrugBinge` ("start" with the hours, "end"). `startBinge` is for tests and dev.

**Chemical damage** (`stepDamage`, every 600 seconds): a `ChemicalDamage` injury (permanent, no pain, a share of the
organ's hit points, never past 80%). Thresholds are the wiki's: alcohol above 45% tolerance hurts the liver, smokeleaf
above 36% the lung, psychite above 45% a kidney (and 15% of new psychite addictions at once). The mean wait is 60 days
at the threshold falling to 20 at full tolerance, 20% of the organ a hit (ours). A major overdose (0.75) hurts the brain
every 2.5 days on average (wiki). Wake-up: a heart attack, mean 120 days while high (wiki), 50% of the heart (ours).
Event `ChemicalDamage` (`message` the part). Cirrhosis, carcinoma and asthma as illnesses are not built; the organ damage
stands in.

**Ambrosia** (social, found not made): 0.5 severity high over 16 hours, +5 mood, +50% recreation, feeds 0.2, tolerance
+3.2% a dose, addiction from 15% tolerance at 1%, the mildest withdrawal (-10 mood). **Penoxycyline** (medical, 2
neutroamine at a drug lab, research `PenoxycyclineProduction`): a 5.5 day protection high whose hediff names
`preventsDiseases` (malaria, plague, sleeping sickness); never addictive. `preventsDisease(pawn, disease)` is what H2's
illness rolls ask. Policy `Prophylaxis` (ours) schedules it every 5.5 days.

## Save, hash, events

Save section `drugs` 2: per pawn the policy, its own entries, the chemical states, the last dose of each drug, the
search wait and the binge (end second, chemical). The high, withdrawal, hangover and overdose live in the pawn's environment hediffs (health section 2).
`stateHash` now includes environment hediffs (only when a pawn has some, so older hashes do not change) and the drug
records (only when there are some). Events: `PawnTookDrug`, `PawnAddicted`, `AddictionCured`, `DrugOverdose`, `DrugBinge`, `ChemicalDamage`.

## Not built

Wake-up's meditation rate, vomiting and social fight changes from alcohol, luciferium's scar healing and berserk,
cirrhosis, carcinoma and asthma as illnesses (the organ damage stands in), the ambrosia sprout event, chemical interest and
fascination traits, the drug dependency genes, drug inventory counts, the colonists carrying drugs, the item's
"nutrition" of beer, and the barrel's six days of fermenting (the bill's work stands in for it). Unreal draws nothing yet:
the hediffs show in the health report and the thoughts in the mood list; a U task can add the drinking animation and the
drug icons.
