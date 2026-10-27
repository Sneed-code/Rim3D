# Disasters and bad events

Code: `ColonySim/Disasters.h`, `Private/Disasters.cpp` (`DisasterSystem`, `Sim::disasters()`); hooks in
`Power` (`setBlackout`), `Weather` (`setTemperatureOffset`, `setGlowScale`) and `AnimalSystem`
(`wildKinds`, `spawnManhunters`, `goMad`, in `AnimalFights.cpp`). Data: `Rules/DisasterRules.json`,
`Incidents/Incidents.json`, `Thoughts/Thoughts4.json`, `Hediffs` `ToxicBuildup`. Tests:
`Tests/DisasterTests.cpp`. Task T3 (the part that is not diseases, raids or prison breaks).

Each is an **incident worker** the narrator calls ([narrator.md](narrator.md)); `FireIncidentCommand
{"SolarFlare"}` makes one happen now. A worker that can't (no power, no crops, one already on) fails in
plain words and the narrator draws another. Each sends a `Letter` event with its text, and the ones that
last send `DisasterStarted` and `DisasterEnded` (`message` the name, `count` the `DisasterKind`, `amount`
the length in hours). The protagonist's mood rule ([mental-breaks.md](mental-breaks.md)) still applies:
the mood moves, a break never takes the player's character.

## Conditions that last

Kept in `DisasterSystem` (saved as section `disasters`, in the state hash), one of each kind at a time.

| Incident | Lasts | What it does |
|---|---|---|
| `SolarFlare` | 0.15 to 0.5 days | Every power net is dark: no generator makes power and no battery gives any (`PowerGrid::setBlackout`). Needs a net. |
| `ToxicFallout` | 2.5 to 10.5 days | Every hour each plant under the open sky dies with chance 0.6; pawns outdoors gain `ToxicBuildup` at 0.4 a day (animals half, from the hediff's `outdoorsInFallout`); everyone else loses 0.1 a day. Colonists outside get the thought `ToxicFallout` (-5). Earliest day 60, cooldown 90. |
| `VolcanicWinter` | 7.5 to 40 days | Outdoor temperature -7 degrees (also in the forecast colonists plan sowing by) and the sun's light down 30%, reached over 20 hours. Earliest day 60, cooldown 140. |
| `Flashstorm` | 6 to 14 hours | A 21 m circle round a colonist. About every four minutes a bolt hits a random cell in it: a small Burn blast (30 damage, 2 m) and a fire. Event `LightningStrike`. |
| `PsychicDrone` | 0.75 to 1.75 days | Colonists of one sex (the one that has colonists) get a thought of level low, medium or high: -12, -22 or -32. Cooldown 15 days. |
| `PsychicSoothe` | 1.5 to 3 days | The same, +16. Never at the same time as a drone. |

## Happens at once

- **`ShortCircuit`** (RimWorld's "zzzt"): a random member of a random power net shorts out
  (`PowerGrid::shortCircuit`: the net's batteries empty, the conduit or building burns out), then a Burn blast of
  radius 1 + 0.1 times the square root of the energy released in Wd (up to 5 m) and fires round it.
- **`Blight`**: a random sown crop plant in a growing zone is the centre; plants of that crop in growing zones
  within a 10 to 20 m circle die with chance 0.7 (the centre always). Wild plants and the same crop outside
  zones are left alone.
- **`ManhunterPack`**: the incident's points (the colony's threat points times 1.4, the wiki's 40% more
  than a raid) buy wild animals of the biome: a kind is picked by commonality among those whose power
  (100 times body size, at least 20, ours) is within the points, and `points / power` of them (1 to 30)
  come in at a random edge as manhunters for 24 to 54 hours, the first set on a random colonist.
- **`MadAnimal`**: one adult animal, wild or tame, within 60 m of a colonist turns manhunter for 2 to 4 hours.

## Numbers

All in `DisasterRules`, with the `unverified` and `ours` lists naming the guesses. From the wiki: the
durations of the flare, fallout, winter and psychic waves, the winter's -7 degrees and 30% light, the
fallout's 40% buildup a day, the soothe's +16 and the pack's extra 40% points and 24 to 54 hours. Recalled
or ours: the flashstorm's size and rate, the drone's three levels, the blight's radius and kill chance, the
animal power, the short circuit's blast, the mad animal's range and length, the incidents' `baseChance`.

## Not yet

- Volcanic winter's halved wildlife; toxic fallout's permanent damage (dementia, cancer) at 40% buildup, the
  rotting of animals that die of it and the sun lamp that protects plants under a roof; blight that spreads
  and withers over days (here it kills at once); a solar flare that also stops turrets' targeting beyond the
  power cut (they are powered buildings, so they do stop); lightning on tall buildings; the extreme drone
  (a ship part's). Zzzt is the same event as `ShortCircuit`.
- Unreal draws none of it yet (U20's letters; a dark sky for the flare and winter, bolts, a fallout haze).
- Disease outbreaks, infestation and the prison break are other tasks (H2, F12, C22 hooks).
