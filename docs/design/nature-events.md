# Events: nature and good news (T2)

Code: `NatureEvents.{h,cpp}` (the workers and the four spells), `Scenario.cpp` (`Sim::joinColonist`),
`AnimalSystem::arrive`, `Husbandry::joinColony`, `RaidSystem::findEntry`, `Weather::setConditions`,
`Lights::setSkyFloor`. Tests: `NatureEventTests`. Data: the Incidents defs (chance, earliest day and
cooldown are D10's; the fields below are new and marked `ours`).

The narrator ([narrator.md](narrator.md)) picks an incident; its **worker** does the thing and writes
the letter's text. `FireIncidentCommand` makes any of them happen at once.

| Incident | What happens |
| --- | --- |
| `WandererJoin` | A new colonist, made like the Crashlanded ones (same clothes, a seed of its own), walks in from a map edge that can reach the colony and joins at once. Population-scaled: fewer when the colony is big. |
| `RefugeePodCrash` | A colonist lands in a pod on open ground 10 to 40 m from the colony, hurt by the landing until downed (blunt blows). Colonists rescue and tend it like any downed colonist (H1). |
| `ResourcePodCrash` | One of `contents` (steel, silver, wood, components, herbal medicine, cloth, by weight) falls in stacks round an open spot 10 to 40 m out. |
| `HerdMigration` | 8 to 16 adults of one plant-eating, non-biting wild animal of the biome come in from an edge and stay 24 to 72 hours before they may leave again. **They graze and wander about; a march across the whole map is not done.** |
| `AnimalSelfTame` | The wild animal nearest the colony that is calm, adult, not a predator and not too wild (`maxWildness` 0.8) becomes tame at once (an `AnimalTamed` event, home where it stands). |
| `ColdSnap`, `HeatWave` | The outdoor temperature changes by `temperatureOffset` (-20, +17) for `durationDays` (2 to 5). Rooms follow through E2 as usual. |
| `Eclipse` | The sun's glow is multiplied by `skyFactor` (0.15) for `durationHours` (6 to 12). Solar power, light and plants see it. |
| `Aurora` | Open ground never gets darker than `nightGlow` (0.4) for `durationHours` (6 to 12). Only the light on open ground: solar power is not helped. |
| `BerryBloom`, `HealrootBloom` | 8 to 14 plants (`Plant_Berry`, `Plant_Healroot`) sprout at half growth within 5 m of a spot 15 to 45 m from the colony. |

The four spells (cold snap, heat wave, eclipse, aurora) are kept by `NatureEvents` as
`NatureCondition`s (incident, start, end); one of a kind at a time. `Weather::setConditions` takes the
temperature offset and the sky factor, `Lights::setSkyFloor` the aurora's floor, all wired in `Sim`.
The snapshot's `narrator.conditions` lists the ones in force (incident, start, end) for U20's letters
and the sky. Save section `nature` (rng stream and the spells); in `Sim::stateHash`.

## Numbers

Chances, earliest days and cooldowns are D10's recalled numbers. The cold snap and heat wave offsets
are the wiki's (-20 and +17); durations come from D10. Everything marked `ours` in the defs (eclipse
and aurora strength, pod contents, herd size and stay, bloom size, bloom growth) is ours and
meant to be tuned.

## Not done

- No mood effects for eclipse and aurora yet; Unreal draws no darker sky or aurora (U20/U-art).
- A herd that really walks across the map, and ones that migrate with young.
- Pod art and a landing animation; the pod's contents just appear.
- The refugee has no faction of its own and is a colonist from the start (RimWorld lets you choose
  to rescue or capture it).
