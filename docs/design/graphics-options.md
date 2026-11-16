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
| Upscaler | `Gfx.Upscaler` | TSR, DLSS | DLSS (TSR when the plugin or card can't) |
| Render resolution | `Gfx.Resolution` | Native, Quality 67%, Balanced 58%, Performance 50%, Ultra performance 33% | Quality with DLSS, Native with TSR |
| Ray reconstruction | `Gfx.RayReconstruction` | on, off (DLSS with ray tracing only) | off |
| Frame generation | `Gfx.FrameGeneration` | on, off (DLSS only) | off |

Ray tracing switches at run time; it is offered only when the engine started with it (`IsRayTracingEnabled()`: DX12, an RTX or
other ray tracing card, a recent driver). The render resolution goes through the scalability's resolution quality, so changing
`Look.Quality` keeps it. DLSS options show only when the plugin's console variables exist (`r.NGX.DLSS.Enable`,
`r.Streamline.DLSSG.Enable`), so a PC or build without the plugin draws with TSR and the window says the plugin is missing.

## Fallbacks

- Ray tracing too slow or broken: untick "Ray-traced light" (software Lumen, as before this change).
- DLSS missing or misbehaving: pick TSR. Known issue reported with the first 5.8 plugin (v8.7): ghosting and missing
  anti-aliasing unless ray reconstruction is on; we install v8.9.0 (2026-10-16), and ray reconstruction is one tick away.
- The whole change: set `r.RayTracing=False` in `DefaultEngine.ini` (a restart and shader rebuild).

## Next

Toward the SFM look, separate tasks: skin shading (subsurface profile tuning, specular and sheen on MetaHuman and MPFB skin),
a cinematic camera (depth of field, a soft key and rim light for close-ups in the creator and the protagonist view), and a
"photo mode" that holds the game and renders at native resolution with ray reconstruction.
