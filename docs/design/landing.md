# Shuttles and the landing zone

sneed (2026-11-17): shuttles (and caravans, traders and visitors) have a zone the player places. With no zone they
come down at a clear random spot, flattening small plants such as grass and bushes; only hard things stop them.

Code: `ColonySim/Landing.h`, `Private/Landing.cpp` (`LandingSystem`). Tests: `Tests/LandingTests.cpp`. Data:
`Rules/LandingRules`, Things `Shuttle_VTOL`. Save section `landing` 1. All numbers are ours (RimWorld's shuttle is a
different, smaller thing).

## The shuttle

`Shuttle_VTOL`: a VTOL crew shuttle about **12 m long and 6 m wide** (footprint 6 x 12 cells facing North, nose to +y,
side door on the -x side), shaped after RimWorld's shuttle but not a copy: two pilot seats, a storage locker behind
them, eight passenger seats, eight belly lift thrusters for vertical landing and take-off, and two large rear thrusters
that swivel from straight down (hover) to straight back (forward flight). It is not built by the player. While it stands
it is an ordinary impassable building with 1000 hit points (ours): pawns walk round it and raiders can shoot it; if it is
destroyed it is gone, with anyone still aboard. The 3D model is a Codex asset (`Rim3D_Codex/VTOLShuttle` on sneed's PC).

## The landing zone

`SetLandingZoneCommand{area, add}` paints ground cells on or off (one zone, any shape; fails if nothing changed). The
snapshot has `landingZone` (row order) and `landingZoneChanges`.

## Where a shuttle sets down

`LandingSystem::findSite`:

1. **In the zone**, if any footprint (any of the four facings) fits wholly inside it and is clear; of those, the one whose
   middle is nearest the zone's middle. Pawns standing there are moved aside to the nearest open cell (as a finished
   wall does).
2. **Otherwise at random**: up to 300 tries (`searchTries`) at 14 to 60 m (`searchRadiusM`) from the colony's middle in a
   random direction and facing, then 300 anywhere on the map. Here a pawn in the footprint also rules the spot out.

A footprint is clear when every cell is on the map, walkable ground (not deep water or rock; marsh, mud and shallow
water are fine), has no building (walls, furniture, blueprints), no roof or upper floor over it, no items and no tree,
the ground under it rises no more than 1.5 m (`maxSlopeM`) from lowest to highest, and the cell outside the door can be
walked. Grass, bushes, crops and other small plants don't count: they are flattened (removed) when it touches down.

## Phases

On the step clock (real time, 30 a second at 1x), so a long day doesn't slow it:

| Phase | Lasts | What happens |
| --- | --- | --- |
| `Descending` | 360 steps, 12 s (`descendSteps`) | it claims the footprint at once (the building is placed, pawns step aside); passengers stay aboard |
| `Landed` | until sent up | small plants under it are flattened; its passengers step out round the door |
| `Ascending` | 300 steps, 10 s (`ascendSteps`) | then the building is removed |

`Snapshot::shuttles` (`ShuttleView`): id, building, the middle (x, y) and ground height, facing, phase, `progress` 0 to 1
through the phase, its visit and the door cell. Unreal draws the descent and climb, the gear, the swivelling rear
thrusters, the flames and the dust from these; the sim does not model the flight itself.

## Visitors and traders by shuttle

`TradeSystem::startVisit` (docs/design/trade.md) takes edge `kShuttleEdge` (-2) to bring visitors or a trade caravan by
shuttle. With edge -1 (any, which is what the narrator uses) half of them (`visitShuttleChance`) come by shuttle and the
rest walk in; when no spot can be found they walk in. Orbital traders stay in orbit. A shuttle visit:

- is made at once with no members; its pawn kinds ride in the shuttle (`Shuttle::passengers`, saved) and step out round
  the door when it lands. The visit waits while the shuttle comes down, and its stay is counted from the landing.
- gathers about 4 m out from the door;
- when its time is up, walks back to the door and climbs aboard within 2 cells of it (one that can't reach the door walks
  off the map instead); when all are aboard or gone the shuttle lifts off.

With a landing zone, **walking visits gather at the zone's middle** instead of the colony's.

## Not done (BACKLOG)

- The player's own shuttle (fly colonists to another tile like transport pods), and shuttle quests.
- The Unreal side: the landing zone paint tool and overlay, the shuttle model with its thruster animations, flame and
  dust effects. Comes once the Codex model is reviewed.
- Pushing items aside in the zone instead of refusing the spot.
