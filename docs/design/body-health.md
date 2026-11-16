# Body health: zones, downing, blood and poise

sneed, 2026-11-16 (thread "Make combat real-time with smart AI"). Part of the real-time combat rework
([real-time-combat.md](real-time-combat.md)). It sits on top of [health.md](health.md): the body of parts, organs,
injuries, scars, infections and diseases stay as they are; what decides **downed, unconscious, bleeding and death**
for people changes. Animals keep RimWorld's rules for now.

## sneed's rules

| Rule | Number |
| --- | --- |
| Zone hit points | head 50, torso 100, each arm 50, each leg 75 |
| Downed | torso, head or a leg under 25%, or 3 or more zones under 50% |
| Unconscious | head under 25%, or blood at 1000 or less (wakes above 1000) |
| Torso-downed | crawls slowly away (fleeing) while conscious |
| Bleeding (each zone, until patched) | 75% or less: 1 a second; 50% or less: 2; 25% or less: 3 |
| Blood | 3000 (hidden). Dead at 0 |
| Patching a zone | 1 / 2 / 3 seconds (light / moderate / heavy) |
| After patching | blood comes back at 10 a second, starting 30 seconds after |
| AI and patching | patches only when safe; under 1500 blood it takes cover or runs to patch |
| Protagonist | never downed; about 100 hit points over the whole body; dies at 0 (not a pawn sneed pilots) |
| Downed and dead | can still be shot |
| Poise | base 50. An attack's force over the target's poise knocks it down; then 30 s before it can be knocked down again |
| Dismemberment | overkill on a zone of at least 50% of its base HP. Arm: 50% below the elbow, 50% the whole arm; leg: below the knee or the whole leg; torso or head: death and a gib. Force over poise + 25 on a killing blow: the whole corpse gibs. Protagonist: never |

"No active bleeding for now": no dripping effect on screen yet; the blood loss itself counts.

## How it maps onto the body

- **Zones** are subtrees of the body: the **head** zone is the neck and everything under it, each **arm** a shoulder
  and everything under it, each **leg** a leg and everything under it, the **torso** everything else (the torso and its
  organs). Listed in `HealthRules` `zones` by body part def, so another body can get zones by data.
- **Hit points**: every part in a zone has its hit points scaled so the zone's root has the zone's hit points (head
  25 to 50: x2; torso 40 to 100: x2.5; shoulder 30 to 50: x1.67; leg 30 to 75: x2.5). Organs keep their share, so a
  shot can still destroy a heart or a brain (death), as now.
- **A zone's health** is 1 minus the damage on its outside parts (inside hits hurt the outside part over them, so
  each blow counts once) over the zone's hit points. A lost part counts its full hit points.
- **Blood** is the existing blood loss (0 to 1) shown as 3000 x (1 - loss); unconscious at a loss of 2/3, dead at 1.
- Time: "a second" is a real second at 1x speed (30 sim steps).

## Phases

| Part | What | Where |
| --- | --- | --- |
| A | Zones, scaled part HP, downed / unconscious by zone, zone bleeding, blood store, regrowth after patching, protagonist 100 HP, shooting the downed and dead | sim |
| B | Self-patching (1/2/3 s a zone), AI patches when safe, under 1500 blood takes cover or runs to patch | sim |
| C | Poise and attack force: knockdown with 30 s immunity | sim |
| D | Torso-downed crawl | sim + an Unreal crawl clip |
| E | Damage numbers, layered upper-body staggers by zone and a limp after a leg hit, blood on clothes | Unreal (PC) |
| F | Dismemberment and gibs (pre-cut limbs with caps, gib chunk sets, skull and brain interior) | sim rule + Unreal art (PC) |

## Part A as built

Code: `HealthModel::buildZones`, `HealthEval::workOutZones`, the zone branch of `HealthSystem::derive` and the blood
lines of `HealthSystem::step` (`Private/Health.cpp`). Data: `HealthRules` `zones` (`Rules/Rules.json`): the bodies
that have zones (`bodies`, now only `Human`), the zone list (name, root part def, hit points) and every number in the
table above. Tests: `Tests/BodyZoneTests.cpp`. `Tests/HealthTests.cpp` takes the Human zones off and keeps checking
RimWorld's rules, which animals still use.

- **Zones**: a rule's root part def starts a zone at every part of that def (two shoulders, two legs); the rule whose
  root is the body's core part (Torso) takes what is left. Every part's hit points are scaled by its zone (torso 40 to
  100: a heart has 37.5).
- **Zone health** (`Health::zones`, `HealthSystem::zoneHealth`, by `zoneNames` order): 1 minus the damage on the
  zone's outside parts (each part at most its hit points), a lost part's full hit points, over the zone's hit points.
- **Downed** by sneed's rules only: pain shock and RimWorld's capacity thresholds no longer down a person (they still
  slow it). A vital organ destroyed or a vital capacity at 0 still kills, as before. So the Wimp trait (a lower pain shock
  threshold) and numb (resonance, takes pain away) no longer change whether a person is down; they still work for
  bodies without zones. Whether pain should come back as a rule of its own is sneed's call.
- **Unconscious** (`Health::unconscious`): head zone under 25%, or blood 1000 or less. It is a kind of downed.
- **Bleeding**: a zone at 75% or less with an untended wound that bleeds (a cut, stab or gunshot, not a bruise or
  burn) or an untended lost part in it bleeds 1, 2 or 3 blood a
  real second (`bleedPerDay` holds it as blood loss a day, so the HUD and AI keep working). Tending is "patching" until
  part B adds patching yourself in 1 to 3 seconds. RimWorld's blood loss stages (consciousness offsets) are off for a
  zoned body: blood only matters at 1000 and 0.
- **Blood comes back**: once nothing bleeds, `Health::bleedStoppedAt` (saved, health section 7) holds the step; from
  30 seconds (900 steps) on, 10 blood a second.
- **Protagonist**: never downed or unconscious; dies (`DeathCause::Wounds`, "wounds") when its zones have lost 100 hit
  points in all. Its parts are no longer doubled (that stays for an unzoned body). It still bleeds, and a destroyed
  brain or heart still kills.
- Not yet: shooting corpses (part A2: the dead have no hitboxes yet; the downed can already be shot), patching (B),
  poise (C), crawling (D).

## Later (sneed, 2026-11-16)

- **Ragdolls**: unconscious pawns, corpses and severed limbs are physics ragdolls in Unreal (cosmetic: the sim keeps
  where the body is; a ragdoll that drifts is pulled back to the sim's spot).
- **Head trophies**: a severed head becomes an item that can be carried and placed as a prop (on a pike, a shelf).
  Dismemberment (part F) makes severed parts real items in the sim (def, the pawn they came from, its looks), so a
  trophy is that item plus a prop to put it on.
