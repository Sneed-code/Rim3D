# Neon assets (Google Gemini 4 Neon) and AI Studio sounds

Tested 2026-11-21/22 for sneed, who has Google AI Pro. Neon is a second headless asset maker next to Codex
(`codex-models.md`); same briefs, same deliverables (GLB, PBR maps, previews, contact sheet, README).

## Making assets with Neon

- Neon is in Google's **Antigravity CLI** (`agy`, 1.5.14 at `%LOCALAPPDATA%\agy\bin\agy.exe`, not on PATH), not
  in the Gemini CLI. It reuses the Antigravity app's Google sign-in: no browser login.
- Model ids `gemini-4-neon-low|medium|high|xhigh` (`agy models`). We use **high**.
- Runner: `tools/neon_assets/run_neon_assets.sh` (Git Bash on sneed's PC; `MODEL`, `OUT`, `JOBS`). Each
  `tools/neon_assets/<Name>.md` is a full brief: `tools/codex_assets/_common.md` + the asset brief + an animation
  part (named glTF actions on the moving parts and a `preview_anim.gif`).
- Headless command per job, run in the asset folder:
  `agy.exe --model gemini-4-neon-high --sandbox --dangerously-skip-permissions --output-format json -p "<prompt>" < NUL`.
  `--sandbox` limits the terminal only, not file writes: the prompt keeps the job in its folder. Blender only with
  `-b`. `agy -p "/usage"` shows the quota.
- Output: `Documents\Rimworld3D\neon_test\<Name>\`, copied to `Rim3D_Codex\<Name>\` so `tools/codex_prep.py` and
  GunArt import them like the Codex models.

## Results

| Asset | Animations | Time | Tokens | Verdict |
|---|---|---|---|---|
| ShipSensorCluster | DishScan, BeaconBlink | 28 min | 383k | close to brief, a bit clean |
| CowboyHat | none | 22 min | 289k | strong |
| Gun_Minigun | SpinUp, Firing, SpinDown, TriggerPull | 33 min | 386k | best gun; heat tint too rainbow |
| Gun_LMG | TriggerPull, Reload, BipodDeploy | 28 min | 326k | complete, colours a bit toy-like |
| ShipComputerCore | Boot, Idle (fans, crystal, lights) | 41 min | 462k | good cabinet, crystal looks like paper |
| PowerSwitch | SwitchOn, SwitchOff, Spark | 24 min | 308k | very good |
| Hatchery | LidOpen, LidClose, Incubate | 25 min | 328k | charming, plain box |

All seven first try, under their triangle budgets, named parts at their pivots. Quota: a static prop about 3% of
the weekly Gemini allowance and 10% of a 5-hour window, an animated one about 3% and 11%; three at once is fine.
**Claude Opus 5.5 in Antigravity** has its own, much smaller quota: one revolver job used the whole 5-hour window and
54% of the week and stopped unfinished. Not usable as an asset worker on this plan.

## AI Studio sounds (sneed's Chrome)

- Works through the Claude in Chrome extension on aistudio.google.com while sneed is signed in (AI Studio use is
  separate from the Gemini app quota). Playback must be silenced first: a page script mutes media elements and routes
  Web Audio through a zero gain before generating. Chrome saves downloads to sneed's Desktop.
- **Lyria 3.5** (`/new_music?model=lyria-3.5`): a 1-minute track in about 20 s. **Lyria 3 Clip**: 30 s clips; it
  will make non-musical sound design (robot servos and hydraulics, insect chittering) when told "sound effects only".
- **Gemini 3.8 Flash TTS** (`/generate-speech`): stock voices (Algenib gravelly male, Kore female), a free-text style
  and tags such as `<grunt> <groan> <scream> <whimper> <sob> <pant> <moan> <hiss> <growl>`. Several sounds per take
  with `<short pause>`, then cut apart by silence (`ffmpeg silencedetect`, -38 dB, 0.25 s). Voice Design and voice
  cloning need a paid API project (not set up). Non-human TTS still sounds like a person imitating a machine.
- Headless alternative: an API key sneed makes in AI Studio, stored as an environment variable on the PC.
- Samples so far live on the PC in `Documents\Rimworld3D\ai_studio_sounds\` (not in Git); nothing is wired into the
  game yet.
