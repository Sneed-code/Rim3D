# Transport pods

RimWorld's one-way drop pods, unlocked by the `TransportPod` research project. Sim only: Unreal builds
the two buildings from the Misc tab, reads `Snapshot::podGroups` and `podFlights` and sends `PodCommand`;
the loading window and the world-map targeting are U tasks for later. Code in `TransportPods.h` /
`TransportPods.cpp` (loading, launching, flying), `PodArrivals.cpp` (the sim's side: the command and where
pods come down) and `PodJobs.cpp` (the LoadPod job). Numbers in `data/defs/Rules/TransportPodRules.json`
and the two Things defs in `data/defs/Things/BuildingsTransport.json`. Tests `Tests/PodTests.cpp`.

## The buildings (wiki numbers, checked 2026-10-28)

| | pod launcher | transport pod |
|---|---|---|
| cost | 50 steel, 1 component | 60 steel, 1 component |
| work | 3000 ticks | 1600 ticks |
| HP, flammability | 200, 50% | 250, 50% |
| size | 1 x 2, impassable, cover 75% | 1 x 1, cover 50% |
| holds | 150 chemfuel (`fuel` field: colonists refuel it, ours whenever it isn't full) | 150 kg |

A pod is fuelled by a finished launcher that touches it along an edge (a corner doesn't count). Each
launcher fuels one pod: going through the pods in id order, each takes the first launcher (by id) it
touches that no earlier pod took (`launcherOf`). RimWorld builds the pod on the launcher's marked spot;
ours lets it stand on any side.

## Loading (`PodCommand` "Load")

`pods`, `pawns` and `items` (stacks lying on the map) make a pod group. Refused: no pods, nothing to
load, something that isn't a finished pod, a pod or thing already in a group or named twice, a pawn that
isn't a colonist who could join a caravan (on the map, up, not drafted, not a prisoner, not in a mental
break, not the pawn the player walks), or more mass than the pods hold together (150 kg each; the
wiki: a group's capacity is shared). A colonist counts its own body mass (60 kg for a human); its gear
does not yet.

1. **Loading.** The `LoadPods` task (Hauling, after Refuel) offers each named stack still on the map. A
   hauler walks to it, picks up as much as it can carry, carries it beside the group's nearest pod and
   puts it in (`LoadPod` job: go, pick up, carry, put in; the stack leaves the map and is in the group).
   A big stack takes several trips; the passengers haul too until the loading is done.
2. **Boarding.** When no named stack is left on the map (all in, or gone), the passengers stop what they
   do and walk to a free cell beside the nearest pod and get in (they leave the map). A passenger is left
   behind if it goes down, is drafted, has a mental break, is taken over by the player, or can't get there
   after 5 tries (`PodPassengerDropped`).
3. **Ready** when everyone left is in (`PodsReady`).

Pawns in a pod keep their needs (as RimWorld: they can go hungry if they wait days). `Cancel` gives the
loading up: everything in the pods comes out beside them (`PodsCancelled`). If every pod of a group is
destroyed or taken down, the same happens where the first pod stood.

## Launching (`PodCommand` "Launch")

`group`, `tile` (-1: this map) and `arrival`. It needs something in the pods and a fuelled launcher
beside each pod. The distance is counted in world tiles (a diagonal step is one, as on RimWorld's
hexagons; east and west wrap). **Each launcher burns 2.25 chemfuel a tile, at least 1** (wiki; the
least is recalled from RimWorld's code), so a full launcher reaches 66 tiles; the snapshot gives the
range of the least-fuelled launcher. The pods are used up (destroyed); passengers not in yet and stacks
not loaded stay on the map. `PodsLaunched` (`count` the flight, `amount` the tile).

The flight takes 50 RimWorld ticks a tile, at least 250 (ours: the wiki only says "very quick but not
instant"; 66 tiles take about 1.3 game hours). Then the pods come down, by `arrival`:

| arrival | where | does |
|---|---|---|
| `Land` | the colony's map (tile -1 or the start tile), or another map a caravan has been to ([other-maps.md](other-maps.md)) | the load comes down at cell (`x`, `y`), or the middle of the map, on the nearest walkable cell: pawns on that cell, stacks on free cells around it. On another map the colonists keep a place at home (`AwayPawn`) and come back by forming a caravan there. |
| `Caravan` | any land tile but the colony's | the passengers form a caravan waiting on the tile with the goods ([caravans.md](caravans.md)): it can move on, go home or visit the tile's map |
| `Gift` | a settlement's tile | no passengers: the goods go to the faction, 1 goodwill per 40 silver of market value (the trade gift's rule) |

Refused at launch: landing on a settlement (an attack, not in the game yet), landing on a tile with no
map yet, a caravan with nobody in the pods, a gift with colonists or with nothing, a tile out of range.
If the place no longer allows the arrival when the pods come down, passengers form a caravan there and
goods alone are lost (`PodsArrived` message "lost"), as RimWorld loses unmanned pods sent to a tile
nobody sees.

`PodsArrived`: `count` the flight, `amount` the tile, `message` "landed", "caravan", "gift" or "lost",
`cell` where they landed, `part` the new caravan's id.

## Snapshot

`PodGroupView`: id, state (loading, boarding, ready), pods, passengers still to get in, those aboard,
stacks still to load and loaded, mass and capacity, range in tiles (-1 when a pod has no launcher or
too little fuel). `PodFlightView`: id, from and to tiles, arrival, pods, pawns and stacks, progress 0..1.

## Saves

Section `pods` 1: the next ids, each group (state, pods, stacks wanted, passengers with their walk,
path ticket and cell, loaded stacks, where the first pod stood) and each flight. `PodPassenger` is
rebuilt from it. `PodCommand` is saved by name ("Pod") in the `commands` section.

## Not yet (in BACKLOG.md)

The Unreal side (a loading window, picking a tile on the world map in range, drawing pods taking off
and coming down), loading downed colonists, prisoners and animals, the gear a colonist wears counting
as mass, choosing counts of a stack instead of whole stacks, pods dropping onto a caravan to join it,
pods landing on a settlement as an attack, the launcher's fill level setting, landing under a thick roof
being refused (pods come down on any walkable cell now).
