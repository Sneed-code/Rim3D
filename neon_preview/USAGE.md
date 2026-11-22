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

# Round 2, 2026-11-22: five animated props with Neon High, plus an Opus 5.5 test

## Setup
- Runner: `tools/neon_assets/run_neon_assets.sh` on branch `claude/project-thread-f6sbmg`, launched hidden from Git Bash. Every bash, agy and Blender process ran at BelowNormal priority, and Blender always ran with `-b`. No windows opened.
- Gemini: `MODEL=gemini-4-neon-high JOBS=3`. Opus: `MODEL=claude-opus-5-5-high JOBS=1`. `agy models` lists the Opus ids as `claude-opus-5-5-low`, `-medium` and `-high`.
- All six jobs started at 10:10 AEST (00:10 UTC).

## Quota (`agy -p "/usage"`)
| When | Gemini weekly | Gemini 5-hour | Claude/GPT weekly | Claude/GPT 5-hour |
|---|---|---|---|---|
| Before (10:10) | 94% | 79% | 100% | 100% |
| After Opus stopped (10:28) | 89% | 62% | **46%** | **0%** |
| After all Gemini jobs (11:09) | 79% | 25% | 46% | 0% |

- **Gemini, 5 animated assets:** 54% of the 5-hour window and 15% of the weekly. That's about 11% of the 5-hour window and 3% of the week per asset, a bit more than round 1 because these props are animated and bigger.
- **Claude group (separate from Gemini, as sneed heard):** one part-finished Opus job used **the whole 5-hour window and 54% of the week**. The 5-hour window resets 15:10 AEST; the weekly resets 2026-11-29 10:10 AEST.

## Jobs
| Job | Model | Wall time | Status | Input tok | Output tok | (thinking) | Cache-read tok | Total |
|---|---|---|---|---|---|---|---|---|
| Gun_LMG | Neon High | 28 min 01 s | SUCCESS, first try | 197,742 | 128,373 | 24,191 | 2,441,227 | 326,115 |
| Gun_Minigun | Neon High | 33 min 18 s | SUCCESS, first try | 249,099 | 136,693 | 35,183 | 3,810,035 | 385,792 |
| ShipComputerCore | Neon High | 41 min 10 s | SUCCESS, first try | 292,032 | 170,281 | 35,089 | 5,778,654 | 462,313 |
| PowerSwitch | Neon High | 23 min 49 s | SUCCESS, first try | 185,219 | 123,155 | 25,580 | 2,260,789 | 308,374 |
| Hatchery | Neon High | 24 min 40 s | SUCCESS, first try | 198,141 | 129,677 | 25,620 | 2,472,255 | 327,818 |
| Gun_Revolver | **Opus 5.5 High** | 17 min 16 s | **ERROR: quota reached** | 136,569 | 99,551 | (not reported) | 186,046 | 236,120 |

- The Gemini jobs needed no reruns.
- The Opus revolver stopped with `RESOURCE_EXHAUSTED (429) Individual quota reached`. It had written 771 lines of build script (`build_revolver.py`, `rv_common.py`), but no GLB, textures or pictures yet.
- I have not done its one "continue" rerun: that can't start before 15:10, and it would likely use most of the 46% weekly that's left. It's waiting for sneed's decision.

## Checks (each GLB re-imported in Blender -b)
| Asset | Tris (budget) | GLB | Size x/y/z m | Named parts and pivots | Animations (24 fps) |
|---|---|---|---|---|---|
| Gun_LMG | 16,082 (20k) | 4.5 MB | 1.10 x 0.15 x 0.32 | Body, FeedCover (hinge), Bipod (hinge), AmmoBox (mount), Trigger (pin), empty Muzzle: all there | TriggerPull 0.17 s, Reload 2.5 s, BipodDeploy 0.5 s |
| Gun_Minigun | 21,360 (24k) | 6.7 MB | 1.05 x 0.47 x 0.44 | Body, Barrels (spin axis), Trigger (pin), empty Muzzle: all there | SpinUp 1.0 s, Firing 1.0 s loop, SpinDown 1.5 s, TriggerPull 0.25 s |
| ShipComputerCore | 13,348 (14k) | 10.8 MB | 1.98 x 1.91 x 2.23 | Body, CoreCrystal, FanL, FanR (hubs), StatusLights, empty Core: all there | Boot 1.5 s, Idle 2.0 s loop |
| PowerSwitch | 2,860 (3k) | 3.5 MB | 0.41 x 0.48 x 1.16 | Body, Lever (hinge), LampGreen, LampRed, plus the optional Spark | SwitchOff 0.38 s, SwitchOn 0.38 s, Spark 0.17 s |
| Hatchery | 7,238 (8k) | 11.6 MB | 0.94 x 0.93 x 0.80 | Body, Lid (back hinge), HeatLamp, Egg1 (bottom), empty Eggs: all there | LidOpen 1.29 s, LidClose 1.29 s (0.8 s swing plus hold), Incubate 3.0 s loop |

- Every asset is under its triangle budget and sits on z=0; the guns sit on the grip origin, as the hand-weapon convention says.
- Textures are 2048, or 1024 for the PowerSwitch as briefed. The core, switch and hatchery use their briefed extra emissive materials, which gives them 3 materials.
- Pictures are in `pictures\`: `<Name>_sheet.jpg` at 1600 px and `<Name>_anim.gif` at 0.5-3.7 MB.

## Opinions
- **Gun_LMG:** complete, with every part and action and a nice belt-feed close-up. The saturated olive paint and purple heat coil make it read a bit toy-like.
- **Gun_Minigun:** the best gun: a chunky, believable shape with hoses, a battery box and real wear. The heat tint runs the whole barrel length as a rainbow instead of just the tips.
- **ShipComputerCore:** the cabinet, fans, light strip and crystal are all separate and animated. The crystal looks like crumpled blue paper rather than a lattice, and the wear is a uniform grey smudge.
- **PowerSwitch:** a very good small prop, with readable copper knife contacts, chipped grey paint and honest scale. The red handle is a bit oversized.
- **Hatchery:** charming. The glass lid, straw, eggs and glowing lamp all read well. The cabinet itself is plain, and the straw stuck in the seam looks like matchsticks.
- **Opus 5.5 in Antigravity:** I can't judge its quality because it ran out before making anything. On this plan its quota covers about one prop per week, not a practical asset worker.

## Verdict
Neon High went 5 for 5 on animated props, first try, in 24-41 min each and about 0.3-0.46M tokens. Running 3 at once, the five took 58 min and about half a 5-hour window.
