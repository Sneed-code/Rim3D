# 0001: Stage 0 sim speed test

Date: 2026-10-24. Replaces nothing; answers the "sim too slow at 1 km" risk in `architecture.md`.

## What was tested

`simrun` with placeholder pawns that only haul: pawns start at a stockpile in the middle of the
map, pick the nearest free item, path to it with plain cell A* (8 directions, no corner cutting,
water and rock blocked), carry it back, repeat. Pawns jog at 4 m/s. One game day per run, seed
42, on sneed's PC (Ryzen 7 9800X3D) at low priority while a game was running, so times are
on the high side.

```
simrun --seed 42 --days 1 --size 512  --pawns 20  --items 400
simrun --seed 42 --days 1 --size 1024 --pawns 20  --items 300
simrun --seed 42 --days 1 --size 1024 --pawns 100 --items 1500
simrun --seed 7  --days 1 --size 1024 --pawns 20  --items 300
```

| Map | Pawns | Hauls in a day | Failed jobs | Avg per step | Avg per frame at 3x | Worst step |
| --- | --- | --- | --- | --- | --- | --- |
| 512 m | 20 | 396 of 400 | 0 | 0.016 ms | 0.02 ms | 9 ms |
| 1 km | 20 | 263 of 300 | 0 | 0.020 ms | 0.03 ms | 41 ms |
| 1 km | 100 | 1,363 of 1,500 | 0 | 0.092 ms | 0.14 ms | 40 ms |
| 1 km, seed 7 | 20 | 208 of 300 | 0 | 0.035 ms | 0.05 ms | 98 ms |

The budget is 5 ms per frame at 3x.

## Findings

1. **Average cost is tiny.** Moving and deciding is far under budget even with 100 pawns.
2. **Long path searches are the problem.** A single cell A* search across a 1 km map expands
   on average 20,000 cells (seed 42) to 56,000 cells (seed 7, more rock to go around), and the
   worst ones take 10 to 100 ms. That one search lands in one step and would show as a visible hitch.
3. **1 km is a long walk.** At 4 m/s a round trip to the map edge takes over 4 real minutes at
   1x, so 20 pawns didn't finish hauling 300 scattered items in a day. That's a gameplay point
   (stockpiles near work, pawns prefer close jobs), not a speed one.

## Decision

- **Keep the 1,024 m map.** The sim's per-step cost scales fine.
- **Region-level pathfinding is required, not optional,** before Stage 1 hauling and building:
  search across regions first, then cells inside the chosen regions only (as planned in
  `architecture.md`, AI section, point 1).
- **Cap path work per step.** Path requests go in a queue with a per-step budget (start at
  about 1 ms), so a burst of long searches spreads over a few steps instead of one hitch. Pawns
  wait a step or two for a path, which nobody will notice.
- The scenario test "20 pawns haul everything within one game day, none fail" is in
  `Tests/HaulingTests.cpp` on a 192 m map and runs in CI.
