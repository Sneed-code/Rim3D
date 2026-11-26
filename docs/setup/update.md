# One game folder, one click to update

sneed's rule (2026-11-21): **there is one game, `C:\Dev\Rim3D`, and every merge reaches everything in it** (the
colony, the title screen, the gun range, the character creator, the test scenes). No separate copies to play from.

## Play (sneed)

Double-click **`C:\Dev\Rim3D\tools\update-and-play.bat`** (close the game first). It:

1. pulls the newest version from GitHub (`main`),
2. builds it at low priority (6 compiler jobs, below-normal priority),
3. redoes only the one-off art steps whose inputs changed since they last ran on this PC (hair, clothes, blood,
   wetness, explosions, sounds, guns ...),
4. starts the game on its title screen. The gun range, the character creator and the other test scenes are under
   **Debug** there (`docs/design/new-game.md` "Debug scenes"); **F10** in a scene comes back to the title.

`tools\play.bat` starts the title screen without updating.

## How the art steps work (Claude)

`tools/update.py` runs the steps in `tools/art_steps.json` in order. Each step has a fingerprint: the Git blob ids of
its `inputs` (scripts, asset lists, defs), of its commandlet's own source files and the project headers they include,
the names, sizes and times of the files in its `raw` folders (downloads and Blender output, not in Git), and its
command line. The fingerprints of the last successful runs live in `game/Saved/ArtSteps/stamps.json` on each PC. A step
runs when its fingerprint changed, when a step it comes `after` ran in this update, or when something it `makes` is
missing. A step `coveredBy` another is done by that one when both would run (the full `MakeHumanArt` run also makes the
garments). A step that failed is not tried again on the same inputs (`--force <step>` does). Logs go to
`game/Saved/ArtSteps/logs/<step>.log`.

```
python tools/update.py --list        what would run now, and why
python tools/update.py --adopt       mark every step done for the files as they are (after running steps by hand)
python tools/update.py --force blood-art wet-look
python tools/update.py --no-pull --no-build --offline
```

**When you add or change a one-off step** (a new commandlet, a new Blender or download script, a new input file), add it
to `tools/art_steps.json` in the same PR, so the next update runs it on sneed's PC by itself. Never tell sneed to run a
commandlet by hand. Steps that write files in Git (`MotionClips`, `tools/clothes_art.py` setting defs) stay out: their
output is committed. Needs Blender 5.2 with MPFB for the MakeHuman steps (`BLENDER` overrides the path); a step whose
need is missing waits for the next update. `{codex}` in a step is the Codex models folder outside the repo
(`RIM3D_CODEX` overrides `~\OneDrive\Documents\ChatGPT\Models\Rim3D_Codex`).

## Stopping an update part way

Closing the window or pressing Ctrl+C during an art step leaves that step half done (the MakeHuman import, for
one, clears its folder first). `update.py` then stamps nothing for it, so the next `update-and-play.bat` runs it
again. `makehuman-bodies` also lists the creator bodies in its `makes`, so a missing body is redone even when
nothing changed (2026-11-26: the colony creator showed Unreal's grey dummy after a stopped import).

Each step is stamped with its fingerprint taken when it finishes, not when the update started: steps whose raw
folders earlier steps fill (`makehuman-bodies` reads the Blender bodies, `hair-grooms` the grown hair) used to look
changed on every update and ran again for nothing.

## Test copies (Claude only)

PR test builds on the PC still use separate clones (`pc-standing-ok`), but nobody plays from them, and
`update.py` never pulls a copy that is not on `main`.
