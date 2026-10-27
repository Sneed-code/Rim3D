# The spaceship ending

Code: `ColonySim/Ship.h`, `Private/Ship.cpp`. Tests: `Tests/ShipTests.cpp`. Data: `data/defs/Things/ShipParts.json`,
`ShipItems.json`, `Research/ShipResearch.json`, `Rules/ShipRules.json`. Sim only; the build menu, the
reactor and launch buttons and the ending screen are Unreal work for later.

## The chain

Research (all Spacer, hi-tech bench; costs are **ours**, the wiki's were not readable, marked `unverified`):
Starflight basics 4000 (needs Fabrication, which stands in for the wiki's Advanced fabrication) opens the
structural beam and the sensor cluster; Ship cryptosleep 2000, Starship reactor 5000 and Ship drive 5000
follow it; Sleeper persuasion 3000 (needs Fabrication) opens the computer core.

Parts (a Things def with `ship.part`; costs, sizes, hit points and work from the RimWorld wiki, work x1.44):

| Part | Size | Cost |
| --- | --- | --- |
| Structural beam | 2x6 | 200 steel, 40 plasteel, 3 component, 1 advanced component |
| Cryptosleep casket | 1x2 | 120 steel, 14 uranium, 3 component, 3 advanced component |
| Reactor | 6x7 | 350 steel, 280 plasteel, 70 uranium, 8 advanced component; makes 1,000 W always |
| Engine | 3x4 | 260 steel, 140 plasteel, 70 uranium, 6 advanced component |
| Computer core | 2x2 | 150 steel, 70 gold, 4 advanced component, 1 Sleeper shard (the persona core) |
| Sensor cluster | 2x2 | ours, unverified (the wiki page would not load) |

New items: plasteel, uranium, gold, advanced component, Sleeper shard (nothing makes or finds them yet).

## The ending

The ship is whatever finished parts stand on the map; they need not touch each other, except that a
**casket must touch a beam** (shares an edge). Commands (`ShipCommand{action, thing, pawn}`):

- `StartReactor` (thing: a finished reactor). It needs no other part. The reactor then takes **15 days**
  (`activationDays`), and while it warms up raids come: the first wave after 6 hours, then one every 36
  hours (**ours**), each of the colony's raid points, the factions in turn (pirates, then the Remnant; one that
  cannot raid is skipped) through `RaidSystem::startRaid`. When the 15 days are up the reactor is **active**
  and the waves stop. Losing the reactor (destroyed or taken down), active or not, drops back to idle.
- `Board` (thing: a casket, pawn: a living colonist, prisoner or tame animal). Puts the pawn in the casket
  (taking it out of any other); no pawn empties the casket. This is an assignment, not a walk: the real
  walk-in job is not done. A rider who dies drops out.
- `Launch`. Needs the reactor active, **3 engines** (`enginesNeeded`), a computer core, a sensor cluster and
  a living rider in a connected casket; otherwise the command fails and says which. The riders leave the map
  (`PawnLeftMap`, `ShipLaunched`), the state becomes **Launched** (`Sim::ship().won()`) and the sim runs on, as
  in RimWorld, so those left behind carry on.

Events: `ShipReactorStarted`, `ShipWave`, `ShipReactorReady`, `ShipReactorLost`, `ShipLaunched`.
`Snapshot::ship` has the phase, time left, waves, part counts, riders, `canLaunch` with the plain-words
`problem`, `won` and `escaped`. Save section `ship` 1.

## Not done

Fuel or power for the parts, "can't be built under a roof" for the reactor and beam, Construction skill
minimums, mechanoid-only waves (the Remnant has no pawn kinds yet, so every wave is pirates), the incident
system's own rules for raids (T1 will own the narrator side; `ShipSystem::sendWave` is the hook), colonists
walking into caskets, and the ending screen.
