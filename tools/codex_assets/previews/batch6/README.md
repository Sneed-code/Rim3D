# Batch 6 import test (temporary, 2026-11-25)

PC run in `C:\Dev\Rim3D-climb` on `claude/project-thread-guazth` (f55158a2).

- codex_prep: 23 of 23 (`codexprep_lines.txt`). All "turned none", sizes match the defs. MealSurvivalPack's folder is
  locked to the Codex sandbox account; Codex copied it to `Rim3D_Codex\MealSurvivalPack_copy` and it was prepped from there.
  A full `codex_prep.py` run with no names dies at the batch 5 `ElectricStove` folder (same lock).
- GunArt (codex_art.json, guns.json): every `SM_<Name>` (17 + geyser/beam parts) and the 6 new guns imported.
- `buildings` smoke: FAIL only on `SolarGenerator, WindTurbine` meshes (placeholder art missing in this clone, not batch 6).
  10 moving defs, 33 part meshes, all found; 16 moving parts drawn, 14 moved by a clip.
- `buildings/`: the 17 new defs put down near the colonists (local-only `-BuildingsGallery` phase, not committed), two views each.
  Contact sheets: `buildings1/2.jpg`, `items1/2.jpg`.
- `gunrange -GunRangeScript`: PASS. `guns/`: the hold tour for the 9 new guns (ready, aimed, side, front).
  Sheets: `hold_bows.jpg`, `hold_pila_launchers.jpg`, `hold_incendiary_smoke.jpg`.

Findings: the three bows are held turned 90 degrees (arrow straight up, limbs lying along the aim line). Hold_Bow uses
the blade hand frame (+X through the fist, +Z along the fingers), but the bow models have limbs on Z and the arrow on +X.
`meshRotation` [90,0,0] does nothing: WeaponDrawSubsystem::MakeDisplayed reads `MeshFix` and drops it (`(void)MeshFix`),
and codex_prep's `turn` only turns about X. Launchers (EMP, doomsday, incendiary, smoke) sit on the shoulder correctly.
The pila is held one-handed at about 45 degrees up (blade frame lean).
