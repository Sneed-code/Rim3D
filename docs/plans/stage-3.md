# Stage 3: the wider world (short plan, first section only)

Stage 3 is not planned in full yet; sneed's playtest of Stage 2 (G4) will shape it. The world map is the first piece because everything else (caravans, other maps, quests, the start screen) stands on it.

## World map (sim side)

| Task | What | Where | Model |
|---|---|---|---|
| W1 | A seeded planet of tiles with biome, temperature, rainfall, elevation, hills, rivers, roads, faction settlements and a chosen start tile (`docs/design/world-map.md`). **Done.** | cloud | Opus, medium |
| W2 | The sim owns a `World` (save section, start tile in the scenario, `siteFor` feeds the S8 map generator). **Done**: the start tile sets the biome, climate and map seed; S8 reads `Sim::site()` for the rest. | cloud | Sonnet, medium |
| W3 | World screen in Unreal: draw the tiles, pick a start tile, show the tile's facts. **Done** (`UWorldScreenSubsystem`, smoke test `worldmap`). | PC Unreal | Sonnet, medium |
| W4 | Caravans: form, travel along tiles and roads by RimWorld's travel speeds and terrain costs, arrive at settlements | cloud | Opus, high |
| W5 | Other maps: made when visited, using `siteFor`, and kept when left | cloud | Opus, high |
