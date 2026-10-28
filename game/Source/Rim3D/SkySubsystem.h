#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"

#include "SkySubsystem.generated.h"

class AActor;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPostProcessComponent;
struct FPostProcessSettings;
class USceneComponent;
class USimSubsystem;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UStaticMesh;
class UVolumetricCloudComponent;
namespace colony { class Sim; }

/** One season's colour grade (data/look.json "seasons"). */
struct FLookSeason
{
	/** White balance in kelvin: 6500 neutral, lower looks colder, higher warmer. */
	float White = 6500.0f;
	float Saturation = 1.0f;
	float Contrast = 1.0f;
	FLinearColor Gain = FLinearColor::White;
};

/** One step of the Look.Quality setting (data/look.json "quality"). */
struct FLookQuality
{
	FString Name;
	/** The engine's quality level for every scalability group (Lumen needs 2 or more). */
	int32 Scalability = 2;
	bool bVolumetricFog = true;
	bool bContactShadows = true;
	bool bClouds = true;
	/** r.VolumetricFog.GridPixelSize: smaller is sharper and dearer. */
	int32 FogGridPixels = 12;
};

/** How the world is lit and graded (data/look.json, docs/design/look.md). The numbers here are the defaults the file overrides. */
struct FLookConfig
{
	// Light in real-world units.
	float SunClearNoonLux = 110000.0f;
	FLinearColor SunColor = FLinearColor(1.0f, 0.975f, 0.94f);
	float SunFadeBelowDeg = -2.0f;
	float SunFadeAboveDeg = 3.0f;
	float ContactShadowLength = 0.04f;
	float ShadowDistanceCm = 30000.0f;
	float SunVolumetricScattering = 1.0f;
	float MoonFullLux = 0.5f;
	float MoonNewShare = 0.25f;
	FLinearColor MoonColor = FLinearColor(0.62f, 0.74f, 1.0f);
	float SkyLightDay = 1.0f;
	float SkyLightNight = 1.0f;
	float SkyLightOvercast = 0.8f;
	float LightningLux = 30000.0f;

	// The camera's exposure, EV100.
	float NoonEV = 15.0f;
	float Follow = 0.80f;
	float MinEV = -1.5f;
	float MaxEV = 16.0f;
	float BrighterEV = 0.75f;
	float DarkerEV = 0.25f;
	float SpeedUp = 2.0f;
	float SpeedDown = 1.5f;
	float SkyShareOfSun = 0.16f;

	// Fog.
	float FogClearDensity = 0.006f;
	float FogSightDensity = 0.3f;
	float FogRainDensity = 0.02f;
	float FogMaxDensity = 0.6f;
	float FogHeightFalloff = 0.08f;
	float FogStartDistanceCm = 500.0f;
	FLinearColor FogOvercastColor = FLinearColor(0.62f, 0.64f, 0.68f);
	FLinearColor FogSnowColor = FLinearColor(0.78f, 0.80f, 0.84f);
	float FogVeilAlbedo = 0.35f;
	FLinearColor VolumetricAlbedo = FLinearColor(0.9f, 0.9f, 0.92f);
	float VolumetricExtinction = 1.0f;
	float VolumetricScatteringDistribution = 0.55f;
	float VolumetricDistanceCm = 9000.0f;
	float LowSunMist = 0.03f;
	float LowSunMistFromDeg = -3.0f;
	float LowSunMistToDeg = 14.0f;

	// Film and lens.
	float FilmSlope = 0.88f;
	float FilmToe = 0.55f;
	float FilmShoulder = 0.26f;
	float FilmBlackClip = 0.0f;
	float FilmWhiteClip = 0.04f;
	float Bloom = 0.35f;
	float Vignette = 0.2f;
	float AmbientOcclusion = 0.55f;
	float AmbientOcclusionRadiusCm = 120.0f;
	float LocalExposureHighlights = 0.85f;
	float LocalExposureShadows = 0.85f;

	/** Spring, summer, autumn, winter. */
	FLookSeason Seasons[4];
	/** Low, Medium, High, Epic. */
	TArray<FLookQuality> Quality;

	FLookConfig();
	/** Reads a look file over these numbers. False, with the reason, if it is missing or broken (the defaults stay). */
	bool Load(const FString& Path, FString& OutError);
	/** The colour grade for a day of the year (0 to 60), blended between the seasons' middle days. */
	FLookSeason GradeFor(float DayOfYear) const;
};

/** What the sky is doing now, as worked out from the sim's clock and weather (smoke test `sky`, docs/design/sky.md). */
struct FSkyState
{
	/** Hour of the day, 0 to 24 (the sim's clock, or `Sky.Hour`). */
	float Hour = 12.0f;
	/** Day of the year, 0 to 60. */
	float DayOfYear = 0.0f;
	/** Degrees above the horizon (negative: below it) and compass bearing (0 north, 90 east). */
	float SunElevation = 0.0f;
	float SunAzimuth = 0.0f;
	float MoonElevation = 0.0f;
	float MoonAzimuth = 0.0f;
	/** 0 new moon to 1 full. */
	float MoonFullness = 1.0f;
	/** Unit vectors from the ground toward the sun and the moon (Unreal axes: x north, y east, z up). */
	FVector ToSun = FVector::UpVector;
	FVector ToMoon = FVector::DownVector;
	/** How much of the day the sky shows: 0 night to 1 sun well up. */
	float Daylight = 1.0f;
	/** 0 clear to 1 heavy cloud (the weather's sky glow turned round). */
	float Overcast = 0.0f;
	float SunLux = 110000.0f;
	float MoonLux = 0.0f;
	float SkyLightIntensity = 1.0f;
	/** How much light falls on open ground, lux (sun, sky and moon), and the exposure the camera is held near for it (EV100). */
	float GroundLux = 0.0f;
	float ExposureEV = 15.0f;
	float FogDensity = 0.0f;
	/** The grey or snow-white veil mixed into the fog's colour as the sky clouds over (the rest of its colour is the sky atmosphere's). */
	FLinearColor FogVeil = FLinearColor::Black;
	/** The cloud material's Cloud_GlobalCoverage: -0.4 a few wisps, 0 mostly cloud, 0.2 and more dark overcast. */
	float CloudCoverage = -0.4f;
	/** 0 to 1: how hard it rains or snows now, and how wet the ground is. */
	float Rain = 0.0f;
	float Snow = 0.0f;
	float Wetness = 0.0f;
	bool bLightning = false;
};

/**
 * The sky, and the weather in the air (docs/design/sky.md): what the sim's clock, season and weather (E1) look like.
 * All of it is cosmetic; the sim decides what the weather does.
 *
 * - The sun and the moon are directional lights that cross the sky with the sim's clock (sunrise 06:00, sunset 18:00 to
 *   match the sim's daylight curve); the sun climbs higher in summer than in winter, the moon waxes and wanes over 30 days.
 * - A sky atmosphere makes the blue sky, the sunset reds and the moon's glow; a real-time sky light lights the ground from it.
 * - Volumetric clouds that cover more as the weather clouds over, exponential fog that thickens with the weather's sight factor.
 * - Rain and snow: streaks and flakes in a block of air around where the camera looks, only over cells without a roof;
 *   rain splashes on the ground. Thunderstorms flash.
 * - The ground gets wet in the rain (the terrain's `Wetness`) and dries after; snow on the ground is USnowSubsystem's.
 * - The level's own sun, sky light and fog are put out while this one is in use, and put back when it is not.
 * - The look (L1, docs/design/look.md): light in real-world units from data/look.json, an exposure that follows it, the film
 *   curve, one colour grade per season, volumetric fog, and the Look.Quality setting (0 Low to 3 Epic).
 *
 * Console: Sky.Enable (0 puts the level's own sky back), Sky.Hour (a time of day to show instead of the sim's; negative:
 * the sim's), Sky.DayOfYear (the same for the day of the year), Sky.Precipitation (0 draws no rain or snow),
 * Sky.CloudCoverage, Sky.CloudDensity and Sky.FogLuminance (for tuning the look), Look.Quality, Look.Exposure (an EV100 to hold
 * the camera at instead; negative follows the light), Look.Reload (reads data/look.json again).
 */
UCLASS()
class RIM3D_API USkySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The date as the HUD shows it: "Spring 6, Year 1". */
	static FString DescribeDate(const colony::Sim& Sim);
	/** The weather as the HUD shows it: "12°C, rain". */
	static FString DescribeClimate(const colony::Sim& Sim);

	/** The state the sky was last set to. */
	const FSkyState& GetState() const { return State; }
	/** Works out the state for a time of day and day of the year with the weather the sim has now (no scene needed). */
	FSkyState ComputeState(const colony::Sim& Sim, float Hour, float DayOfYear) const;

	/** Whether the sky has its own lights and atmosphere in the world. */
	bool IsSceneBuilt() const { return Holder != nullptr; }
	UDirectionalLightComponent* GetSun() const { return Sun; }
	UDirectionalLightComponent* GetMoon() const { return Moon; }
	UExponentialHeightFogComponent* GetFog() const { return Fog; }
	UVolumetricCloudComponent* GetClouds() const { return Clouds; }
	USkyAtmosphereComponent* GetAtmosphere() const { return Atmosphere; }
	/** The level's own lights and fog that were put out. */
	int32 GetLevelLightsHidden() const { return LevelParts.Num(); }

	/** Rain streaks, snowflakes and splashes drawn now. */
	int32 GetRainDrawn() const { return RainDrawn; }
	int32 GetSnowDrawn() const { return SnowDrawn; }
	int32 GetSplashesDrawn() const { return SplashesDrawn; }
	/** Particles hidden in the last frame because a roof is over them. */
	int32 GetRoofedHidden() const { return RoofedHidden; }
	/** Lightning flashes since the sim started or loaded. */
	int32 GetFlashes() const { return Flashes; }
	/** The lightning light's intensity right now (0 between flashes). */
	float GetFlashIntensity() const { return FlashIntensity; }
	/** The centre of the block of air the precipitation fills, world units. */
	FVector GetPrecipitationCentre() const { return PrecipCentre; }
	float GetPrecipitationHalfSizeCm() const;
	/** Frames the sky was applied. */
	int32 GetFramesApplied() const { return FramesApplied; }
	/** The exposure the camera is held near now, EV100. */
	float GetExposure() const { return State.ExposureEV; }
	/** The look file's numbers (data/look.json), and whether the file was read. */
	const FLookConfig& GetLook() const { return Look; }
	bool IsLookFileLoaded() const { return bLookLoaded; }
	/** The Look.Quality step in use (0 Low to 3 Epic) and its name. */
	int32 GetQualityLevel() const { return AppliedQuality; }
	FString GetQualityName() const;
	/** The post-process settings the sky drives (exposure, film, grade); null before the scene is built. */
	const FPostProcessSettings* GetPostProcess() const;
	/** Times the camera's exposure was made to jump to its target (a time jump, a new game). */
	int32 GetExposureCuts() const { return ExposureCuts; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FDrop
	{
		FVector Position = FVector::ZeroVector;
		/** Cm per second downward, and a number 0 to 1 that makes this one different from the next. */
		float Speed = 0.0f;
		float Seed = 0.0f;
		/** False while this drop is over a roofed cell. */
		bool bShown = true;
	};

	struct FSplash
	{
		FVector Position = FVector::ZeroVector;
		float Age = 1.0f;
	};

	void BuildScene();
	void DestroyScene();
	void HideLevelParts();
	void ShowLevelParts();
	void ApplyState(const FSkyState& NewState);
	void LoadLook();
	void ApplyQuality(int32 Level);
	void ApplyWetness(const colony::Sim& Sim, float GameSeconds, float RealDelta);
	void UpdateLightning(const colony::Sim& Sim, float RealDelta);
	void UpdatePrecipitation(const colony::Sim& Sim, USimSubsystem& Source, float Delta);
	UInstancedStaticMeshComponent* MakeInstanced(UStaticMesh* Mesh);
	UStaticMesh* MakeBox(float X, float Y, float Z, const FLinearColor& Color);
	void RespawnDrop(FDrop& Drop, const colony::Sim& Sim, const USimSubsystem& Source, bool bAnywhere);
	void ResetPrecipitation();
	double GroundZ(const colony::Sim& Sim, const USimSubsystem& Source, double X, double Y) const;

	UPROPERTY()
	TObjectPtr<AActor> Holder;
	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> Sun;
	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> Moon;
	UPROPERTY()
	TObjectPtr<UDirectionalLightComponent> Lightning;
	UPROPERTY()
	TObjectPtr<USkyAtmosphereComponent> Atmosphere;
	UPROPERTY()
	TObjectPtr<USkyLightComponent> SkyLight;
	UPROPERTY()
	TObjectPtr<UExponentialHeightFogComponent> Fog;
	UPROPERTY()
	TObjectPtr<UVolumetricCloudComponent> Clouds;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;
	UPROPERTY()
	TObjectPtr<UPostProcessComponent> Exposure;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> RainInstances;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> SnowInstances;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> SplashInstances;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;
	/** The level's own sun, sky light, fog, atmosphere and clouds, put out while ours are in use. */
	TArray<TWeakObjectPtr<USceneComponent>> LevelParts;
	FLookConfig Look;
	bool bLookLoaded = false;
	int32 AppliedQuality = -1;
	int32 ExposureCuts = 0;
	float LastExposureEV = -100.0f;

	FSkyState State;
	TArray<FDrop> Rain;
	TArray<FDrop> Snow;
	TArray<FSplash> Splashes;
	FRandomStream Random = FRandomStream(0x5C1);
	FVector PrecipCentre = FVector::ZeroVector;
	bool bPrecipCentreKnown = false;
	int32 RainDrawn = 0;
	int32 SnowDrawn = 0;
	int32 SplashesDrawn = 0;
	int32 RoofedHidden = 0;
	int32 Flashes = 0;
	float FlashIntensity = 0.0f;
	float FlashAge = 99.0f;
	float NextFlashIn = 6.0f;
	float GroundWet = 0.0f;
	float LastWetnessSeconds = -1.0f;
	int32 FramesApplied = 0;
	int32 BuiltGeneration = -1;
	bool bSceneWanted = false;
	TWeakObjectPtr<USimSubsystem> Bridge;
};
