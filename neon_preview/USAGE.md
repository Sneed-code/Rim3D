# Gemini 4 Neon (High): headless asset test, 2026-11-22

## Setup
- The old **Gemini CLI is not installed**, and Neon isn't in it anyway. Neon ships in Google's **Antigravity CLI** (`agy`).
- Installed `agy` 1.5.14 to `%LOCALAPPDATA%\agy\bin\agy.exe`. It came from Google's official manifest and I checked its SHA512. I did not run `agy install`, so PATH and shell aliases are unchanged.
- **Login:** none needed. `agy` reuses the Antigravity desktop app's Google sign-in (Pro plan) from Windows Credential Manager. No browser window opened.
- **Model id:** `gemini-4-neon-high`. "High" is built into the model id; `--effort high` also exists. The other levels are `-low`, `-medium` and `-xhigh`. `agy models` lists them all.
- **Headless command**, run from inside the asset folder as the working directory:
  ```
  "%LOCALAPPDATA%\agy\bin\agy.exe" --model gemini-4-neon-high --sandbox --dangerously-skip-permissions --output-format json -p "<prompt>" < NUL > neon.log 2> neon.err.log
  ```
  - `--dangerously-skip-permissions` is the YOLO equivalent. Without it, headless runs auto-deny shell tools.
  - `--sandbox` restricts the terminal only. Blender still ran fine under it. **It does not confine file writes to the folder:** in my test a write to the parent folder succeeded. Folder discipline comes from the prompt; both jobs stayed in their folders.
  - From Git Bash, set `MSYS_NO_PATHCONV=1`, or slash commands like `/usage` get mangled into paths.
- Both jobs ran with `cmd /c run_neon.cmd`, hidden and at BelowNormal priority. No windows appeared, and every Blender run used `-b`. Each job's runner, prompt, log and timestamps are in its folder (`run_neon.cmd`, `neon_prompt.txt`, `neon.log`, `neon_times.txt`).

## Smoke test
`-p "Reply with exactly: NEON OK..."`: 7.2 s, 1 turn. Tokens: 8,683 in, 421 out (412 thinking), 9,104 total. Reply: "NEON OK, Gemini 4 Neon".

## Jobs (both ran at once, start 2026-11-21 23:25:59 UTC = 9:26am AEST)
| Job | Wall time | Status | Input tok | Output tok | (of which thinking) | Cache-read tok | Total (in+out) | Turns |
|---|---|---|---|---|---|---|---|---|
| ShipSensorCluster | 28 min 29 s (ended 9:54:28) | SUCCESS, first try | 227,155 | 156,237 | 39,585 | 3,469,732 | 383,392 | 1 |
| CowboyHat | 21 min 49 s (ended 9:47:48) | SUCCESS, first try | 178,402 | 110,734 | 28,250 | 2,076,911 | 289,136 | 1 |

- "Turns" counts prompts (1 each). The CLI doesn't report the number of internal model requests or tool calls.
- Neither job needed a resume or rerun.
- **Quota after both jobs** (`agy -p "/usage"`):
  - Gemini models weekly limit: **94% remaining** (resets 2026-11-28 23:24 UTC).
  - Gemini 5-hour limit: **79% remaining** (resets 2026-11-22 04:24 UTC).
  - Claude/GPT group: 100% on both limits.
  - The smoke tests used a tiny amount, so the two asset jobs cost about **6% of the weekly and 21% of the 5-hour Gemini allowance**. That's roughly 4-5 such assets per 5-hour window.

## Results: checked by re-importing each GLB in Blender -b

### ShipSensorCluster
- **GLB:** `ShipSensorCluster.glb`, 12.0 MB, **12,053 tris** (budget 14k). Size 1.72 x 1.72 x 2.15 m, bottom at z=0.
- **Objects:**
  - `Base`: 5,637 tris, pivot at origin.
  - `Dish`: 6,096 tris, pivot (0, 0, 0.72) on the yaw axis.
  - `BeaconLens`: 320 tris, child of Base, own emissive red material `M_BeaconLens`.
  - Empty `Beacon` at (0, 0, 2.025).
  - All named parts are present.
- **Actions in GLB:**
  - `DishScan`: frames 0-288 at 24 fps = 12.0 s.
  - `BeaconBlink`: frames 0-36 at 24 fps = 1.5 s.
  - Both are present.
- **Textures:** 2048 BaseColor, Normal and ORM, plus 512 lens maps including the emissive. Two materials.
- **Files:** all deliverables are there, including the 48-frame 800x600 `preview_anim.gif`. The original is 10.6 MB; the copy in pictures\ is shrunk to 640x480 at 3.4 MB.
- **Opinion:** honestly close to the brief. It has a clean, readable silhouette, all the requested parts, correct animations, good wear on the base and a believable weld patch. It looks a bit "clean CG" and boxy rather than photoreal AAA, but it would be usable in game as is.

### CowboyHat
- **GLB:** `CowboyHat.glb`, 10.3 MB, **5,892 tris** (budget 6k). Size 32.8 x 38.2 x 13.9 cm, against a brief of 33 x 38 x 14.
- **Objects:** a single `Hat` mesh with one material. No animation was required.
- **Textures:** 2048 BaseColor, Normal and ORM.
- **Files:** all deliverables are there. The contact sheet adds an underside view of the sweatband.
- **Opinion:** strong. The proportions, felt texture, stitched band, buckle, modelled sweatband and fade all read well. The crown pinches are a little lumpy, and the brim has less side curl than a real cattleman, but it's the better of the two.

## Verdict
Neon High delivered two complete, on-brief assets fully autonomously, with no follow-up needed, in 22-28 min each. That is comparable to our Codex runs. Each job used about 300-400k tokens, plus heavy cache reads.
