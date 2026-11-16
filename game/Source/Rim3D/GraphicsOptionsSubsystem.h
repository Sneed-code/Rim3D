#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"

#include "GraphicsOptionsSubsystem.generated.h"

/** Which temporal upscaler draws the frame (Gfx.Upscaler). */
enum class EGfxUpscaler : int32
{
	/** Unreal's own Temporal Super Resolution. Works on any card. */
	Tsr = 0,
	/** NVIDIA DLSS (needs the DLSS plugin and an RTX card; falls back to TSR without them). */
	Dlss = 1,
};

/**
 * The player's graphics options (docs/design/graphics-options.md): quality step, hardware ray tracing, upscaler and render
 * resolution, DLSS frame generation and ray reconstruction. Each is a console variable (Gfx.*, and Look.Quality from the sky),
 * so a smoke test or the command line can set them too; the Settings window changes them and they are kept in
 * GameUserSettings.ini under [Rim3D.Graphics]. Lives for the whole process, so the options hold across maps.
 */
UCLASS()
class UGraphicsOptionsSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Whether the engine started with ray tracing (project setting r.RayTracing, a DX12 card that can do it). */
	static bool IsRayTracingAvailable();
	/** Whether the DLSS plugin is loaded and DLSS works on this card. */
	static bool IsDlssAvailable();
	/** Whether DLSS frame generation is offered (the plugin's Streamline part on a card that supports it). */
	static bool IsFrameGenerationAvailable();

	/** Pushes the Gfx.* values to the engine (called whenever one changes). */
	void Apply();
	/** Writes the options to GameUserSettings.ini. */
	void Save() const;

	/** The upscaler actually in use (DLSS asked for but missing gives TSR). */
	EGfxUpscaler GetActiveUpscaler() const { return ActiveUpscaler; }
	/** The render resolution in percent of the screen actually in use. */
	float GetActiveScreenPercent() const { return ActiveScreenPercent; }

	/** Draws the Graphics part of the Settings window (ImGui, inside an open window). */
	void DrawOptions(float Scale);

private:
	void Load();

	EGfxUpscaler ActiveUpscaler = EGfxUpscaler::Tsr;
	float ActiveScreenPercent = 100.0f;
	bool bApplying = false;
	FDelegateHandle PostEngineInitHandle;
};
