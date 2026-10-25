# Cover, sight and threat maps

Code: `ColonySim/Cover.h`, `Private/Cover.cpp`, `ColonySim/ThreatMap.h`, `Private/ThreatMap.cpp`, the
obstacle layer in `ColonySim/Map.h`. Tests: `Tests/CoverTests.cpp`. Picture: `simrun --sight FILE`.
Task F3. The fighter AI (F4, F5) and the shooting code (F2) are the users.

## Heights, not percentages

RimWorld gives each thing a flat `fillPercent` and rolls dice against it. Here shots are real
(`game-design.md`), so a thing has a **height** and a pawn has a **stance**:

| Stance | Eye height | Body height |
| --- | --- | --- |
| Standing | 1.6 m | 1.8 m |
| Crouching | 1.0 m | 1.1 m |
| Prone | 0.3 m | 0.4 m |

These and the height scale below are our numbers; RimWorld has no heights. They are constants at the
top of `Cover.h`.

**Where heights come from.** A building's height is its def's `fillPercent` (RimWorld's own number,
imported by D2/D3). 1.0 (walls, doors, boulders) is the tallest value and blocks all sight; anything
lower is that share of 2 m, so sandbags (0.55) are 1.1 m and a crouching pawn hides behind them while
a standing one does not. `Things` turns it into `Footprint::obstacleDm` and gives it to
`Map::setBuilding`, which keeps it in the map's **obstacle layer** (decimetres, one byte per cell;
`Map::obstacleDm`). Rock terrain always reads as the tallest value. A building with no `fillPercent`
(a bed, a stove) has none. Ground height counts too: a ridge hides what is behind it.

A closed door is as tall as its def says (1.0 in RimWorld) and blocks sight. An **open** door stops
blocking: the door system sets its obstacle height to 0 while it is open and back when it closes
(`setDoorSight`, [rooms.md](rooms.md)); the cover map follows.

Plants are not in the layer yet. When C2 adds trees, rocks and bushes it passes their `fillPercent`
the same way (the layer then holds the taller of building and plant).

## Cover: how hidden is a pawn standing here?

`Cover` holds, for every cell and each of the eight compass directions (`Dir8`, +y is north), how
high the thing next door on that side stands above this cell's ground: a building, a rock, or a
rise in the ground. Measured in decimetres, 255 meaning a wall.

```
Cover& cover = map.cover();                     // or sim.cover()
float h = cover.coverHeight(cell, Dir8::N);     // metres; 25.5 is a wall
float f = cover.coverFraction(cell, Dir8::N, Stance::Crouching);   // 0..1 of the body hidden
float f = cover.coverAgainst(cell, Stance::Standing, enemyX, enemyY);   // from a point
```

`coverFraction` is the height divided by the stance's body height, at most 1. `coverAgainst` picks
the direction toward the shooter by 45 degree sectors (no trigonometry, so every platform agrees).
The map edge gives no cover. This is a quick score for choosing where to stand; whether a bullet is
actually stopped is decided by the bullet (F2) hitting real obstacles.

**Rebuilt only where something changes.** The first query computes every cell (15 ms for 512 m, 60 ms
for 1 km). After that `Map::setBuilding` marks the changed cell and the next query recomputes only
that cell and its eight neighbours; twenty walls in a row cost 66 cells. Terrain is fixed for now;
when mining changes terrain it must call `markDirty` on the cell too.

## Line of sight

```
bool seen = cover.lineOfSight(Viewpoint::at(cell, Stance::Standing), Viewpoint{x, y, 0, Stance::Prone});
```

True when `from` can see the **eyes** of `to`. The sim draws the straight line between the two eye
points, walks the cells it crosses and blocks if the line passes below the top of any cell on the
way (ground height plus obstacle). The two end cells never block, so a pawn pressed against a wall can
see away from it. Rules worth knowing:

- Seeing is mutual when both have the same stance. A standing shooter can see a standing pawn behind
  sandbags but not a crouching one; a crouching pawn can't see out over them either, so to fire it
  stands up (F4's "lean out" is a `Standing` viewpoint for the shot).
- A line through a cell corner passes if either of the two cells it touches there is open, and is
  blocked between two diagonal blockers, so a sealed room has no pinholes at its corners.
- Points are in metres, so moving pawns work too. Different levels, or off the map, are not visible
  (levels above the ground come with stairs).
- A test checks it against a brute-force sampled version on 3,000 random pairs over hills and
  walls, and that seeing is mutual.

Cost: about 10 nanoseconds per metre of line in the cloud, so a 40 m ray is about 0.4 microseconds.

## Threat maps

One `ThreatMap` per faction, from `sim.threatMap(factionId)`. It answers "how many known enemies
could shoot at this cell?".

```
ThreatMap& threat = sim.threatMap(faction);
threat.clear();
for (each enemy the faction knows about)
    threat.addSource(Viewpoint::at(enemyCell), weaponRange, weight);   // weight: e.g. its damage rating
float danger = threat.threatAt(cell);
```

`addSource` gives every **walkable** cell within range that the enemy has line of sight to the
source's weight (sources add up). `targetStance` says which stance counts as hit: the default
`Standing` marks everything a pawn could be hit at when upright; `Crouching` marks only cells that a
crouched pawn is exposed on, which shows what sandbag cover would shelter.

It is a cache of what the faction knows, so it is **not saved** and not part of the sim hash. The owner
rebuilds it: clear, then add every known enemy. Measured in the cloud (40 sources of 40 m range among
test walls) a full rebuild takes 25 to 30 ms, about 0.65 ms per source. That is too long for one step,
so F4 must rebuild it a few times a second and spread the sources over steps, as the pathfinder
spreads searches. Cheaper versions (only sources that moved, per-block updates) wait until a
profile asks for them. Weights are floats; the factions themselves (`FactionId` is just a number
for now) come with raids (F6).

## The picture

`simrun --seed 42 --size 512 --days 0 --sight sight.png` scatters test walls (red) and sandbags
(orange) around an open spot, puts a white pawn there and colours what it sees within 60 m: yellow for
cells a standing pawn and a crouching pawn both see, green for cells only a standing pawn sees, dark
for hidden ground. It also prints the cover build time and the 40-source threat timing. CI draws a
128 m one on every run (`simrun_sight`).

## Not done here

- Per-part hitboxes and bullet-versus-wall hits (F2); choosing where to stand (F4).
- Lean, peek and prone transitions as movement; this only prices them.
- Plants and loose items as cover (C2). Open and closed doors are done (C5).
- Upper levels (stairs, floors).
