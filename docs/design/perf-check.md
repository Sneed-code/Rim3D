# Performance check (G3)

Task G3: what the game costs in a packaged build at Stage 2 size, and what to do about it. The decision is
`docs/decisions/0007-sim-stays-on-game-thread.md`; this file is how to measure and what was measured.

## Measuring

```
powershell -ExecutionPolicy Bypass -File tools\package.ps1
powershell -ExecutionPolicy Bypass -File tools\smoke.ps1 -Test perf -Packaged -Force
```

`package.ps1` makes a Development Win64 build in `Rim3D-packaged` next to the repo, below normal priority on
6 cores (the first packaging took about 9 minutes after a warm editor cache; a C++-only change can use `-SkipCook`). The packaged game
reads the defs from the repo (`-SimData=`), so data changes need no repackage. The `perf` smoke test
([smoke-tests.md](smoke-tests.md)) draws offscreen at 2560 x 1440 and writes
`Rim3D-packaged\Windows\Rim3D\Saved\Smoke\perf.json` (per phase: fps, p95, p99 and worst frame, median game
thread, render thread and GPU, the sim's ms a step and a frame, the frames that ran the hourly plant pass) and a
frame profile in `...\Saved\Profiling\CSV` (Unreal's CSV profiler: every frame, every game thread category).

The scene: the `simrun --perf` colony ([sim-cost.md](sim-cost.md)) on a 1 km map, 30 colonists, 300 tame and
279 wild animals, run ahead to 07:00, then at 3x (the sim's dearest speed) from the overseer camera: the usual
view, close up, zoomed right out over most of the map, a grass fire, rain, 38 to 41 pirates walking in, and
the fight against the 30 colonists with rifles (about 1,000 shots in 30 s).

## Results (2026-10-27, sneed's PC: Ryzen 7 9800X3D, RTX 5080, 2560 x 1440)

Packaged Development build, medians unless said. A light game may have been open on the PC.

| phase | fps | p99 frame | worst frame | game thread | render thread | GPU | sim per frame | sim per step |
|---|---|---|---|---|---|---|---|---|
| colony | 166 | 9.2 | 13.5 | 6.0 | 3.8 | 4.0 | 1.00 | 1.22 |
| close up | 165 | 10.4 | 34.7 | 5.6 | 3.5 | 3.7 | 0.95 | 1.14 |
| overview | 133 | 8.9 | 11.0 | 5.8 | 7.4 | 6.6 | 1.15 | 1.08 |
| fire | 219 | 8.8 | 20.7 | 4.0 | 3.9 | 3.4 | 0.72 | 1.08 |
| rain | 190 | 8.8 | 14.0 | 5.0 | 4.3 | 3.6 | 0.89 | 1.21 |
| raid walking in | 170 | 9.7 | 34.1 | 5.9 | 4.7 | 3.8 | 1.13 | 1.39 |
| raid fight | 176 | 9.0 | 11.6 | 5.6 | 4.6 | 4.0 | 0.97 | 1.22 |

(milliseconds.) The same test in the editor (`-game`, not packaged) ran at 57 to 72 fps with a 13 to 17 ms game
thread: the editor's own overhead, so performance is judged on packaged builds only.

- **Everything is well over 60 fps.** The game thread is the busiest (4 to 6 ms), the sim about a sixth of it.
  The overview is the dearest view on the GPU (6.6 ms: most of the map's 510,000 plants in sight).
- **The sim costs about 1 ms a frame at 3x**, 1.1 to 1.4 ms a step (more than `simrun`'s 0.58 because these are
  busy daytime hours: hauling, building, the raid). The 4 ms budget of decision 0006 has room four times over.
- **The hourly plant pass** was a 14 ms step once a game hour; in the editor those frames took 22 to 27 ms. G3
  spread it over a game minute (sim-cost.md); its frames are now 3 to 5 ms, like any other.
- **Two frames of about 35 ms** in 2.5 measured minutes: one when the raid arrives (38 pawns and their actors at
  once) and one 27 ms sim step close up whose cause the per-system profile didn't catch. Rare enough to leave;
  in BACKLOG.md.
- Screenshots: a 1440p screenshot frame takes about 400 ms, so the test takes them before measuring.

## Not measured

- A window on screen (`-Visible`): offscreen skips presenting, so a visible run may cost a little more GPU.
- Saves, the UI panels open, a second level of buildings, a full day and night. V25 (art performance) repeats
  this check with faces, hair, clothes and wounds.
