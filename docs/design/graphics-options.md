# Graphics options: ray tracing and DLSS

sneed (2026-11-16) wants cinematic skin and lighting, toward SFM renders. This is the lighting foundation for that: hardware
ray tracing for Lumen and NVIDIA DLSS, with a fallback for each. Skin shading, rim lights and depth of field are a later
pass (see "Next").

## What is on

- **Hardware ray tracing** (project settings in `game/Config/DefaultEngine.ini`): `r.RayTracing=True`,
  `r.Lumen.HardwareRayTracing=True`. Lumen's global illumination and reflections trace against the RTX cores' scene instead of
  its software distance fields: no light leaking through thin walls and roofs, reflections that show pawns and things off
  screen, truer bounce light on skin. Shadows stay virtual shadow maps (`r.RayTracing.Shadows=0`): they are sharper than ray
  traced shadows and already paid for.
- **GPU skin cache for every skinned mesh** (`r.SkinCache.DefaultBehavior=1`, scene limit 1024 MB): a pawn or animal only shows
  in the ray-traced scene, and so in reflections and bounce light, through the skin cache.
- **DLSS** (NVIDIA's plugin, not in Git, `assets/manifest.json`): upscaling from a lower render resolution, DLAA at native,
  ray reconstruction and frame generation.

## The Settings window

The **Settings** button in the top menu now has a Graphics part under the day length. Each option is a console variable, so
smoke tests and the command line (`-ini:Engine:[ConsoleVariables]:Gfx.RayTracing=0`) can set it; the window keeps the player's
choice in `GameUserSettings.ini` under `[Rim3D.Graphics]`. Code: `UGraphicsOptionsSubsystem` (an engine subsystem, so the
options hold across maps).

| Option | Variable | Values | Default |
| --- | --- | --- | --- |
| Quality | `Look.Quality` | Low, Medium, High, Epic (`docs/design/look.md`) | High |
| Ray-traced light | `Gfx.RayTracing` | on, off (software Lumen) | on |
| Upscaler | `Gfx.Upscaler` | TSR, DLSS (TSR when the plugin or card can't) | TSR (see "Cost") |
| Render resolution | `Gfx.Resolution` | Native, Quality 67%, Balanced 58%, Performance 50%, Ultra performance 33% | Quality with DLSS, Native with TSR |
| Ray reconstruction | `Gfx.RayReconstruction` | on, off (DLSS with ray tracing only) | off |
| Frame generation | `Gfx.FrameGeneration` | on, off (DLSS only) | off |

Ray tracing switches at run time; it is offered only when the engine started with it (`IsRayTracingEnabled()`: DX12, an RTX or
other ray tracing card, a recent driver). The render resolution goes through the scalability's resolution quality, so changing
`Look.Quality` keeps it. DLSS options show only when the plugin's console variables exist (`r.NGX.DLSS.Enable`,
`r.Streamline.DLSSG.Enable`), so a PC or build without the plugin draws with TSR and the window says the plugin is missing.

## Cost (2026-11-16, sneed's PC, editor `perf` test at 1440p)

fps / p99 frame ms / GPU ms per phase; before, ray tracing + DLSS Quality, ray tracing + TSR native:

| Phase | Before | RT + DLSS 67% | RT + TSR native |
| --- | --- | --- | --- |
| colony | 67.3 / 18.0 / 4.1 | 56.3 / 22.7 / 5.0 | 64.2 / 18.9 / 6.9 |
| colony_close | 59.0 / 47.9 / 4.0 | 47.6 / 59.0 / 5.8 | 58.5 / 39.0 / 7.4 |
| overview | 67.8 / 17.9 / 5.8 | 64.3 / 19.0 / 6.5 | 68.6 / 16.9 / 7.9 |
| fire | 63.7 / 44.1 / 4.2 | 55.2 / 55.2 / 5.6 | 62.6 / 37.9 / 7.3 |
| rain | 65.7 / 19.7 / 4.4 | 56.2 / 22.8 / 5.3 | 65.1 / 18.8 / 6.9 |
| raid_approach | 59.5 / 46.2 / 4.1 | 48.8 / 55.8 / 5.0 | 58.9 / 39.0 / 6.6 |
| raid_fight | 50.3 / 51.5 / 4.3 | 50.6 / 61.1 / 5.3 | 44.6 / 61.2 / 7.4 |

The game is held back by the processor (game thread about 15 ms, render thread 11 to 13 ms); the graphics card is busy only 4
to 8 ms. Ray tracing adds about 3 ms of GPU time at native resolution, which the card has spare, so with TSR it costs almost no
frames. DLSS buys nothing while the processor is the limit and its own render-thread work made it about 10 fps slower, so the
default is TSR native; DLSS stays in the Settings window for when the processor side gets faster. With ray tracing on in the
project, switching it off in Settings still leaves about 3 ms of GPU work (harmless while the processor limits).

Look bench (`2026-11-16_1614_after_rt_dlss` against `_1554_before_rt`): close up, shadows are truer (less leaking, no blue moon
tint indoors) and torchlight bounces warm orange onto the bed and walls; shaded areas are darker, so a shaded colonist and the
jungle-floor plants nearly vanish in the close-up (tune exposure or sky light next); rocks a touch grainy; skin unchanged; no
ghosting, no black pawns. The first build after switching recompiled shaders in about 4 minutes.

The DLSS plugin loads after the engine (in the editor even after `OnPostEngineInit`), so the options apply again when its
modules load (`OnModulesChanged`).

## Fallbacks

- Ray tracing too slow or broken: untick "Ray-traced light" (software Lumen, as before this change).
- DLSS missing or misbehaving: pick TSR. Known issue reported with the first 5.8 plugin (v8.7): ghosting and missing
  anti-aliasing unless ray reconstruction is on; we install v8.9.0 (2026-10-16), and ray reconstruction is one tick away.
- The whole change: set `r.RayTracing=False` in `DefaultEngine.ini` (a restart and shader rebuild).

## Shade and skin (2026-11-16, second step)

- **Shade.** Hardware ray tracing leaks less sky light into shaded places than software Lumen, so shade went too dark. `data/look.json`
  `film.skylightLeaking` (Lumen skylight leaking, post process, 0.06) lets a little sky light reach every shaded spot, and
  `film.localExposureShadows` 0.85 to 0.75 lifts dark areas against bright ones.
- **Skin shine.** The generated skin material was matte (roughness 0.55, specular 0.35). `data/look.json` `skin` (roughness 0.42,
  specular 0.5) goes onto every pawn's face, hand and bare skin through one shared dynamic instance per skin material
  (`USimPawnSubsystem::GetShinySkin`, `ApplySkinShine`); console `Look.SkinRoughness` / `Look.SkinSpecular` override it for
  new pawns while tuning.
- **Cinematic depth of field** in the protagonist's third person view: a full-frame lens at f/4 focused on the colonist
  (`Look.DepthOfField`, 0 off), so the far background softens like a film camera. It fades out while aiming and is off in first
  person, so a distant target stays sharp.

## Next

Toward the SFM look, separate tasks: skin shading (subsurface profile tuning, specular and sheen on MetaHuman and MPFB skin),
a cinematic camera (depth of field, a soft key and rim light for close-ups in the creator and the protagonist view), and a
"photo mode" that holds the game and renders at native resolution with ray reconstruction.
