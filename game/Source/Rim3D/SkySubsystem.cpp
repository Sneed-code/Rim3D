#include "SkySubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "TerrainSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Weather.h"

DEFINE_LOG_CATEGORY_STATIC(LogSky, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GSkyEnable(
		TEXT("Sky.Enable"), 1, TEXT("1 draws the sim's sky (sun, moon, atmosphere, clouds, fog, rain and snow), 0 leaves the level's own sky."), ECVF_Default);
	TAutoConsoleVariable<float> GSkyHour(
		TEXT("Sky.Hour"), -1.0f, TEXT("A time of day (0 to 24) to show instead of the sim's clock; negative follows the sim."), ECVF_Default);
	TAutoConsoleVariable<float> GSkyDayOfYear(
		TEXT("Sky.DayOfYear"), -1.0f, TEXT("A day of the year (0 to 60) to take the sun's height and the moon's phase from instead of the sim's; negative follows the sim."), ECVF_Default);
	TAutoConsoleVariable<float> GSkyCloudCoverage(
		TEXT("Sky.CloudCoverage"), -99.0f, TEXT("The cloud material value Cloud_GlobalCoverage to draw instead of the weather s (below -50 follows the weather)."), ECVF_Default);
	TAutoConsoleVariable<float> GSkyFogLuminance(
		TEXT("Sky.FogLuminance"), 1.0f, TEXT("Scales the brightness of the fog's own colour (0 leaves the fog to the atmosphere)."), ECVF_Default);
	TAutoConsoleVariable<float> GSkyCloudDensity(
		TEXT("Sky.CloudDensity"), -1.0f, TEXT("The clouds' density (Cloud_GlobalDensity); negative leaves the material's own."), ECVF_Default);
	TAutoConsoleVariable<int32> GSkyPrecipitation(
		TEXT("Sky.Precipitation"), 1, TEXT("1 draws rain and snow in the air, 0 draws none."), ECVF_Default);

	const TCHAR* const SkySolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const SkyCloudMaterialPath = TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst");

	// The sim's daylight curve (LightRules skyGlowByHour) rises from 05:00 to 07:00 and falls from 17:00 to 19:00,
	// so the sun is on the horizon at 06:00 and 18:00.
	constexpr float SkySunriseHour = 6.0f;
	constexpr float SkySunsetHour = 18.0f;
	// The sun's noon height in degrees at midsummer and midwinter (the year's mid-points are days 22.5 and 52.5).
	constexpr float SkyNoonElevationMean = 48.0f;
	constexpr float SkyNoonElevationSwing = 20.0f;
	constexpr float SkyMidsummerDay = 22.5f;
	constexpr float SkyDaysPerYear = 60.0f;
	constexpr float SkyDaysPerMoonCycle = 30.0f;
	// What a clear noon's sun is in lux when the level has no sun of its own to copy.
	constexpr float SkyDefaultSunLux = 10.0f;
	// A game's moonlight, not the real thing (a hundred-thousandth of the sun): about an eighth of a clear noon at full moon.
	constexpr float SkyMoonLux = 1.2f;
	constexpr float SkyLightningLux = 40.0f;

	// Rain and snow: a block of air round where the camera looks, and how much goes in it.
	constexpr float SkyPrecipHalfSizeCm = 3500.0f;
	constexpr float SkyPrecipHeightCm = 3200.0f;
	constexpr int32 SkyMaxRain = 7000;
	constexpr int32 SkyMaxSnow = 6000;
	constexpr int32 SkyMaxSplashes = 260;
	constexpr float SkyRainSpeedCm = 1300.0f;
	constexpr float SkySnowSpeedCm = 160.0f;
	constexpr float SkySplashSeconds = 0.28f;

	float SkySmooth(float From, float To, float Value)
	{
		const float T = FMath::Clamp((Value - From) / (To - From), 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	FVector SkyDirection(float ElevationDeg, float AzimuthDeg)
	{
		const float El = FMath::DegreesToRadians(ElevationDeg);
		const float Az = FMath::DegreesToRadians(AzimuthDeg);
		return FVector(FMath::Cos(El) * FMath::Cos(Az), FMath::Cos(El) * FMath::Sin(Az), FMath::Sin(El));
	}
}

bool USkySubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USkySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USkySubsystem, STATGROUP_Tickables);
}

float USkySubsystem::GetPrecipitationHalfSizeCm() const
{
	return SkyPrecipHalfSizeCm;
}

void USkySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
	Collection.InitializeDependency<UTerrainSubsystem>();
}

void USkySubsystem::Deinitialize()
{
	DestroyScene();
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------
// What the sim says about the date and the weather, for the HUD.

FString USkySubsystem::DescribeDate(const colony::Sim& Sim)
{
	const colony::GameTime& Time = Sim.time();
	FString Season(UTF8_TO_TCHAR(colony::seasonName(Time.season())));
	if (!Season.IsEmpty())
	{
		Season[0] = FChar::ToUpper(Season[0]);
	}
	const int32 DayInSeason = static_cast<int32>(Time.dayOfYear() % colony::kDaysPerSeason) + 1;
	return FString::Printf(TEXT("%s %d, Year %d"), *Season, DayInSeason, static_cast<int32>(Time.year()) + 1);
}

FString USkySubsystem::DescribeClimate(const colony::Sim& Sim)
{
	const colony::Weather& Weather = Sim.weather();
	FString Label = TEXT("clear");
	const colony::DefId Current = Weather.current();
	if (Current.valid() && Current.value < Sim.defs().size())
	{
		Label = UTF8_TO_TCHAR(Sim.defs().get(Current).label().c_str());
	}
	return FString::Printf(TEXT("%d°C, %s"), FMath::RoundToInt(static_cast<float>(Weather.outdoorTemperature())), *Label);
}

// ---------------------------------------------------------------------------
// The state of the sky.

FSkyState USkySubsystem::ComputeState(const colony::Sim& Sim, float Hour, float DayOfYear) const
{
	FSkyState S;
	S.Hour = Hour;
	S.DayOfYear = DayOfYear;

	const float Noon = SkyNoonElevationMean + SkyNoonElevationSwing * FMath::Cos(2.0f * PI * (DayOfYear - SkyMidsummerDay) / SkyDaysPerYear);
	float Elevation;
	float Azimuth;
	if (Hour >= SkySunriseHour && Hour <= SkySunsetHour)
	{
		// East at sunrise, south at noon, west at sunset.
		const float T = (Hour - SkySunriseHour) / (SkySunsetHour - SkySunriseHour);
		Elevation = Noon * FMath::Sin(PI * T);
		Azimuth = 90.0f + 180.0f * T;
	}
	else
	{
		// Under the horizon: west at sunset, north at midnight, east at sunrise.
		const float Night = (Hour > SkySunsetHour ? Hour - SkySunsetHour : Hour + 24.0f - SkySunsetHour) / (24.0f - (SkySunsetHour - SkySunriseHour));
		Elevation = -Noon * 0.7f * FMath::Sin(PI * Night);
		Azimuth = FMath::Fmod(270.0f + 180.0f * Night, 360.0f);
	}
	S.SunElevation = Elevation;
	S.SunAzimuth = Azimuth;
	S.ToSun = SkyDirection(Elevation, Azimuth);

	// The moon is always across the sky from the sun, so it is up at night.
	S.MoonElevation = -Elevation;
	S.MoonAzimuth = FMath::Fmod(Azimuth + 180.0f, 360.0f);
	S.ToMoon = SkyDirection(S.MoonElevation, S.MoonAzimuth);
	S.MoonFullness = 0.5f + 0.5f * FMath::Cos(2.0f * PI * (static_cast<float>(Sim.time().day()) + Hour / 24.0f - 5.0f) / SkyDaysPerMoonCycle);

	const colony::WeatherEffects Effects = Sim.weather().effects();
	S.Daylight = SkySmooth(-4.0f, 10.0f, Elevation);
	S.Overcast = FMath::Clamp((1.0f - Effects.skyGlow) / 0.55f, 0.0f, 1.0f);
	S.Rain = FMath::Clamp(Effects.rainRate * 1.4f, 0.0f, 1.0f);
	S.Snow = FMath::Clamp(Effects.snowRate / 1.2f, 0.0f, 1.0f);
	S.Wetness = Effects.wetness;
	S.bLightning = Effects.lightning;

	const float BaseSun = LevelSunLux > 0.5f ? LevelSunLux : SkyDefaultSunLux;
	S.SunLux = BaseSun * S.Daylight * FMath::Square(FMath::Clamp(Effects.skyGlow, 0.0f, 1.0f));
	S.MoonLux = SkyMoonLux * (0.25f + 0.75f * S.MoonFullness) * SkySmooth(0.0f, 12.0f, S.MoonElevation) * (1.0f - S.Daylight) * FMath::Lerp(1.0f, 0.3f, S.Overcast);
	S.SkyLightIntensity = 0.1f + 0.9f * S.Daylight * FMath::Lerp(1.0f, 0.7f, S.Overcast);

	// How far a pawn sees through the air under the open sky is how far you see: the fog is the weather's sight factor turned
	// into a density (0.5 is thick fog), with a little more for rain and a faint haze when it is clear. (The engine divides
	// the density by 1000, per centimetre: 0.3 makes a mist you cannot see 50 m through.)
	const float Sight = FMath::Clamp(Effects.sightFactor, 0.2f, 1.0f);
	S.FogDensity = FMath::Min(0.006f + 0.3f * (1.0f / Sight - 1.0f) + 0.02f * Effects.rainRate, 0.6f);
	// The cloud material's own number, found by eye: -0.4 is a clear sky with a few wisps, -0.3 scattered cumulus, -0.15 broken
	// cloud, 0 mostly cloud, 0.2 and up a dark overcast.
	S.CloudCoverage = -0.4f + 0.75f * FMath::Pow(S.Overcast, 0.7f);
	return S;
}

// ---------------------------------------------------------------------------
// The scene: lights, atmosphere, clouds, fog.

UStaticMesh* USkySubsystem::MakeBox(float X, float Y, float Z, const FLinearColor& Color)
{
	VisualMesh::FPart Part;
	Part.Shape = VisualMesh::EShape::Box;
	Part.Size = FVector3f(X, Y, Z);
	Part.Color = Color;
	TArray<VisualMesh::FPart> Parts;
	Parts.Add(Part);
	UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, nullptr, false);
	if (Mesh)
	{
		KeepMeshes.Add(Mesh);
	}
	return Mesh;
}

UInstancedStaticMeshComponent* USkySubsystem::MakeInstanced(UStaticMesh* Mesh)
{
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Holder);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bReceivesDecals = false;
	Component->bUseAsOccluder = false;
	Component->SetupAttachment(Holder->GetRootComponent());
	Component->RegisterComponent();
	return Component;
}

void USkySubsystem::HideLevelParts()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == Holder)
		{
			continue;
		}
		TInlineComponentArray<USceneComponent*> Components;
		Actor->GetComponents(Components);
		for (USceneComponent* Component : Components)
		{
			if (!Component->IsVisible())
			{
				continue;
			}
			if (const UDirectionalLightComponent* Light = Cast<UDirectionalLightComponent>(Component))
			{
				// The level's own sun sets how bright a clear noon is.
				LevelSunLux = FMath::Max(LevelSunLux, Light->Intensity);
			}
			if (Component->IsA<UDirectionalLightComponent>() || Component->IsA<USkyLightComponent>() || Component->IsA<UExponentialHeightFogComponent>() ||
				Component->IsA<USkyAtmosphereComponent>() || Component->IsA<UVolumetricCloudComponent>())
			{
				LevelParts.Add(Component);
				Component->SetVisibility(false);
			}
		}
	}
}

void USkySubsystem::ShowLevelParts()
{
	for (const TWeakObjectPtr<USceneComponent>& Part : LevelParts)
	{
		if (USceneComponent* Component = Part.Get())
		{
			Component->SetVisibility(true);
		}
	}
	LevelParts.Reset();
}

void USkySubsystem::BuildScene()
{
	UWorld* World = GetWorld();
	if (!World || Holder)
	{
		return;
	}
	HideLevelParts();

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("SkyRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("Sky"));
#endif

	const auto MakeLight = [this, Root](float Lux, const FLinearColor& Color, int32 AtmosphereIndex, bool bShadows)
	{
		UDirectionalLightComponent* Light = NewObject<UDirectionalLightComponent>(Holder);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensity(Lux);
		Light->SetLightColor(Color);
		Light->SetCastShadows(bShadows);
		Light->SetupAttachment(Root);
		if (AtmosphereIndex >= 0)
		{
			Light->SetAtmosphereSunLight(true);
			Light->SetAtmosphereSunLightIndex(AtmosphereIndex);
		}
		Light->RegisterComponent();
		return Light;
	};
	Sun = MakeLight(SkyDefaultSunLux, FLinearColor(1.0f, 0.97f, 0.92f), 0, true);
	Sun->SetDynamicShadowDistanceMovableLight(50000.0f);
	Moon = MakeLight(0.0f, FLinearColor(0.62f, 0.74f, 1.0f), 1, false);
	Lightning = MakeLight(0.0f, FLinearColor(0.82f, 0.88f, 1.0f), -1, false);
	Lightning->SetVisibility(false);

	Atmosphere = NewObject<USkyAtmosphereComponent>(Holder);
	Atmosphere->SetupAttachment(Root);
	Atmosphere->RegisterComponent();

	SkyLight = NewObject<USkyLightComponent>(Holder);
	SkyLight->SetMobility(EComponentMobility::Movable);
	SkyLight->SourceType = SLS_CapturedScene;
	SkyLight->SetRealTimeCapture(true);
	SkyLight->SetIntensity(1.0f);
	SkyLight->SetupAttachment(Root);
	SkyLight->RegisterComponent();

	Fog = NewObject<UExponentialHeightFogComponent>(Holder);
	Fog->SetFogDensity(0.001f);
	Fog->SetFogHeightFalloff(0.08f);
	Fog->SetStartDistance(500.0f);
	Fog->SetFogMaxOpacity(0.9f);
	Fog->SetupAttachment(Root);
	Fog->RegisterComponent();

	// Clouds from the engine's simple volumetric cloud material (its coverage is driven by the weather).
	if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, SkyCloudMaterialPath))
	{
		CloudMaterial = UMaterialInstanceDynamic::Create(Base, this);
		Clouds = NewObject<UVolumetricCloudComponent>(Holder);
		Clouds->SetMaterial(CloudMaterial);
		Clouds->SetupAttachment(Root);
		Clouds->RegisterComponent();
	}
	else
	{
		UE_LOG(LogSky, Warning, TEXT("Could not load %s; no clouds"), SkyCloudMaterialPath);
	}

	// Rain, snow and splashes.
	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, SkySolidMaterialPath);
	if (SolidMaterial)
	{
		RainInstances = MakeInstanced(MakeBox(0.8f, 0.8f, 60.0f, FLinearColor(0.28f, 0.34f, 0.46f)));
		SnowInstances = MakeInstanced(MakeBox(3.2f, 3.2f, 3.2f, FLinearColor(0.92f, 0.94f, 0.98f)));
		SplashInstances = MakeInstanced(MakeBox(12.0f, 12.0f, 0.6f, FLinearColor(0.7f, 0.78f, 0.9f)));
		const FTransform Hidden(FRotator::ZeroRotator, FVector::ZeroVector, FVector(0.0001f));
		TArray<FTransform> Pool;
		Pool.Init(Hidden, SkyMaxRain);
		RainInstances->AddInstances(Pool, false, true);
		Pool.SetNum(SkyMaxSnow);
		SnowInstances->AddInstances(Pool, false, true);
		Pool.SetNum(SkyMaxSplashes);
		SplashInstances->AddInstances(Pool, false, true);
	}
	else
	{
		UE_LOG(LogSky, Error, TEXT("Could not load %s; no rain or snow"), SkySolidMaterialPath);
	}
	UE_LOG(LogSky, Display, TEXT("Sky: built; the level's own sun, sky light and fog (%d parts) are out, a clear noon is %.1f lux"), LevelParts.Num(), LevelSunLux > 0.5f ? LevelSunLux : SkyDefaultSunLux);
}

void USkySubsystem::DestroyScene()
{
	ShowLevelParts();
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
	Sun = nullptr;
	Moon = nullptr;
	Lightning = nullptr;
	Atmosphere = nullptr;
	SkyLight = nullptr;
	Fog = nullptr;
	Clouds = nullptr;
	CloudMaterial = nullptr;
	Exposure = nullptr;
	RainInstances = nullptr;
	SnowInstances = nullptr;
	SplashInstances = nullptr;
	Rain.Reset();
	Snow.Reset();
	Splashes.Reset();
	bPrecipCentreKnown = false;
	RainDrawn = SnowDrawn = SplashesDrawn = 0;
}

// ---------------------------------------------------------------------------
// Applying the state to the scene.

void USkySubsystem::ApplyState(const FSkyState& New)
{
	State = New;
	if (!Holder)
	{
		return;
	}
	Sun->SetWorldRotation((-State.ToSun).Rotation());
	Sun->SetIntensity(FMath::Max(State.SunLux, 0.001f));
	const bool bSunUp = State.SunLux > 0.02f;
	if (Sun->IsVisible() != bSunUp)
	{
		Sun->SetVisibility(bSunUp);
	}
	Sun->SetCastShadows(State.SunLux > 0.3f);

	Moon->SetWorldRotation((-State.ToMoon).Rotation());
	Moon->SetIntensity(FMath::Max(State.MoonLux, 0.0001f));
	const bool bMoonUp = State.MoonLux > 0.005f;
	if (Moon->IsVisible() != bMoonUp)
	{
		Moon->SetVisibility(bMoonUp);
	}

	SkyLight->SetIntensity(State.SkyLightIntensity);

	// Fog takes the sky's colour: grey-blue by day, darker at night, whiter in snow.
	const FLinearColor DayFog = FMath::Lerp(FLinearColor(0.30f, 0.38f, 0.5f), FLinearColor(0.42f, 0.45f, 0.5f), State.Overcast);
	const FLinearColor SnowFog(0.6f, 0.62f, 0.66f);
	FLinearColor FogColor = FMath::Lerp(DayFog, SnowFog, State.Snow);
	FogColor *= (0.02f + 0.98f * State.Daylight) * GSkyFogLuminance.GetValueOnGameThread();
	Fog->SetFogDensity(State.FogDensity);
	Fog->SetFogInscatteringColor(FogColor);

	if (CloudMaterial)
	{
		const float CoverageOverride = GSkyCloudCoverage.GetValueOnGameThread();
		CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), CoverageOverride > -50.0f ? CoverageOverride : State.CloudCoverage);
		const float DensityOverride = GSkyCloudDensity.GetValueOnGameThread();
		if (DensityOverride >= 0.0f)
		{
			CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalDensity"), DensityOverride);
		}
	}
	++FramesApplied;
}

void USkySubsystem::ApplyWetness(const colony::Sim& Sim, float GameSeconds, float RealDelta)
{
	(void)RealDelta;
	if (LastWetnessSeconds < 0.0f)
	{
		LastWetnessSeconds = GameSeconds;
	}
	const float Elapsed = FMath::Clamp(GameSeconds - LastWetnessSeconds, 0.0f, 7200.0f);
	LastWetnessSeconds = GameSeconds;
	// Rain soaks the ground in about half a game hour and it takes about four hours to dry.
	const float Target = State.Rain > 0.01f ? FMath::Clamp(0.45f + State.Wetness, 0.0f, 1.0f) : 0.0f;
	if (GroundWet < Target)
	{
		GroundWet = FMath::Min(Target, GroundWet + Elapsed / 1800.0f);
	}
	else if (GroundWet > Target)
	{
		GroundWet = FMath::Max(Target, GroundWet - Elapsed / 14400.0f);
	}
	(void)Sim;
	if (UTerrainSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainSubsystem>())
	{
		Terrain->SetGroundWetness(GroundWet);
	}
}

void USkySubsystem::UpdateLightning(const colony::Sim& Sim, float RealDelta)
{
	(void)Sim;
	if (!Lightning)
	{
		return;
	}
	if (State.bLightning)
	{
		NextFlashIn -= RealDelta;
		if (NextFlashIn <= 0.0f && FlashAge > 1.0f)
		{
			FlashAge = 0.0f;
			++Flashes;
			NextFlashIn = Random.FRandRange(3.0f, 12.0f);
			Lightning->SetWorldRotation(FRotator(Random.FRandRange(-75.0f, -50.0f), Random.FRandRange(0.0f, 360.0f), 0.0f));
		}
	}
	if (FlashAge <= 1.0f)
	{
		FlashAge += RealDelta;
		// A hard flash, a gap, a second softer one, then it fades out.
		const float T = FlashAge;
		float Envelope = 0.0f;
		if (T < 0.08f)
		{
			Envelope = 1.0f;
		}
		else if (T < 0.15f)
		{
			Envelope = 0.15f;
		}
		else if (T < 0.35f)
		{
			Envelope = 0.7f * (1.0f - (T - 0.15f) / 0.2f);
		}
		FlashIntensity = SkyLightningLux * Envelope;
	}
	else
	{
		FlashIntensity = 0.0f;
	}
	const bool bOn = FlashIntensity > 0.01f;
	if (Lightning->IsVisible() != bOn)
	{
		Lightning->SetVisibility(bOn);
	}
	if (bOn)
	{
		Lightning->SetIntensity(FlashIntensity);
	}
}

// ---------------------------------------------------------------------------
// Rain and snow.

double USkySubsystem::GroundZ(const colony::Sim& Sim, const USimSubsystem& Source, double X, double Y) const
{
	const FVector Origin = Source.GetWorldOrigin();
	const colony::Map& Map = Sim.map();
	const double XM = (X - Origin.X) * 0.01;
	const double YM = (Y - Origin.Y) * 0.01;
	const int32 CX = FMath::Clamp(FMath::FloorToInt32(XM), 0, Map.width() - 1);
	const int32 CY = FMath::Clamp(FMath::FloorToInt32(YM), 0, Map.height() - 1);
	return Origin.Z + static_cast<double>(Map.groundHeight(CX, CY)) * 100.0 + TerrainDraw::SurfaceBonusCm(Map, XM, YM);
}

void USkySubsystem::RespawnDrop(FDrop& Drop, const colony::Sim& Sim, const USimSubsystem& Source, bool bAnywhere)
{
	const double X = PrecipCentre.X + Random.FRandRange(-SkyPrecipHalfSizeCm, SkyPrecipHalfSizeCm);
	const double Y = PrecipCentre.Y + Random.FRandRange(-SkyPrecipHalfSizeCm, SkyPrecipHalfSizeCm);
	const double Floor = GroundZ(Sim, Source, X, Y);
	const double Top = PrecipCentre.Z + SkyPrecipHeightCm;
	Drop.Position = FVector(X, Y, bAnywhere ? Random.FRandRange(Floor, Top) : Top + Random.FRandRange(0.0f, 300.0f));
	Drop.Seed = Random.FRand();
	Drop.Speed = 0.0f;
	const FVector Origin = Source.GetWorldOrigin();
	const colony::CellPos Cell{FMath::FloorToInt32((X - Origin.X) * 0.01), FMath::FloorToInt32((Y - Origin.Y) * 0.01), 0};
	Drop.bShown = Sim.map().inBounds(Cell) && Sim.weather().outdoors(Cell);
}

void USkySubsystem::ResetPrecipitation()
{
	Rain.Reset();
	Snow.Reset();
	Splashes.Reset();
	bPrecipCentreKnown = false;
}

void USkySubsystem::UpdatePrecipitation(const colony::Sim& Sim, USimSubsystem& Source, float Delta)
{
	if (!RainInstances || !SnowInstances || !SplashInstances)
	{
		return;
	}
	const bool bEnabled = GSkyPrecipitation.GetValueOnGameThread() != 0;
	const int32 WantRain = bEnabled ? FMath::RoundToInt(State.Rain * SkyMaxRain) : 0;
	const int32 WantSnow = bEnabled ? FMath::RoundToInt(State.Snow * SkyMaxSnow) : 0;

	// The block of air follows where the camera looks: the point on the ground the view's middle meets.
	APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	FVector CameraPosition = FVector::ZeroVector;
	if (CameraManager)
	{
		const FVector CamLocation = CameraManager->GetCameraLocation();
		CameraPosition = CamLocation;
		const FVector Forward = CameraManager->GetCameraRotation().Vector();
		const double Ground = GroundZ(Sim, Source, CamLocation.X, CamLocation.Y);
		FVector Centre;
		if (Forward.Z < -0.12)
		{
			const double T = FMath::Min((Ground - CamLocation.Z) / Forward.Z, 6000.0);
			Centre = CamLocation + Forward * T;
		}
		else
		{
			Centre = CamLocation + Forward.GetSafeNormal2D() * 2500.0;
		}
		Centre.Z = GroundZ(Sim, Source, Centre.X, Centre.Y);
		PrecipCentre = Centre;
		bPrecipCentreKnown = true;
	}
	if (!bPrecipCentreKnown)
	{
		return;
	}

	const float Wind = static_cast<float>(Sim.weather().wind());
	RoofedHidden = 0;
	const double H = SkyPrecipHalfSizeCm;
	const auto Wrap = [H](double Value, double Centre)
	{
		double Rel = Value - Centre;
		while (Rel > H)
		{
			Rel -= 2.0 * H;
		}
		while (Rel < -H)
		{
			Rel += 2.0 * H;
		}
		return Centre + Rel;
	};
	const FVector Origin = Source.GetWorldOrigin();
	const auto Roofed = [&](const FVector& P)
	{
		const colony::CellPos Cell{FMath::FloorToInt32((P.X - Origin.X) * 0.01), FMath::FloorToInt32((P.Y - Origin.Y) * 0.01), 0};
		return !(Sim.map().inBounds(Cell) && Sim.weather().outdoors(Cell));
	};

	const auto Fall = [&](TArray<FDrop>& Drops, int32 Want, int32 Max, bool bRain, UInstancedStaticMeshComponent* Instances, int32& OutDrawn)
	{
		const int32 Previous = Drops.Num();
		if (Drops.Num() > Want)
		{
			Drops.SetNum(Want, EAllowShrinking::No);
		}
		while (Drops.Num() < FMath::Min(Want, Max))
		{
			FDrop Drop;
			RespawnDrop(Drop, Sim, Source, true);
			Drops.Add(Drop);
		}
		int32 Shown = 0;
		TArray<FTransform> Transforms;
		Transforms.Reserve(FMath::Max(Previous, Drops.Num()));
		const FVector Wind2D(0.0, 1.0, 0.0);
		for (FDrop& Drop : Drops)
		{
			const float Fast = bRain ? SkyRainSpeedCm * (0.85f + 0.3f * Drop.Seed) : SkySnowSpeedCm * (0.7f + 0.6f * Drop.Seed);
			const float Drift = bRain ? Wind * 160.0f : Wind * 90.0f + FMath::Sin(Drop.Seed * 37.0f + static_cast<float>(FPlatformTime::Seconds()) * (0.8f + Drop.Seed)) * 40.0f;
			Drop.Position += FVector(bRain ? 0.0 : FMath::Cos(Drop.Seed * 50.0f + static_cast<float>(FPlatformTime::Seconds()) * 0.7f) * 25.0 * Delta, Drift * Delta, -Fast * Delta);
			const double X = Wrap(Drop.Position.X, PrecipCentre.X);
			const double Y = Wrap(Drop.Position.Y, PrecipCentre.Y);
			const bool bWrapped = X != Drop.Position.X || Y != Drop.Position.Y;
			Drop.Position.X = X;
			Drop.Position.Y = Y;
			const double Floor = GroundZ(Sim, Source, X, Y);
			if (Drop.Position.Z < Floor || Drop.Position.Z > PrecipCentre.Z + SkyPrecipHeightCm + 600.0)
			{
				if (bRain && Drop.bShown && Drop.Position.Z < Floor && Splashes.Num() < SkyMaxSplashes && Random.FRand() < 0.16f)
				{
					FSplash Splash;
					Splash.Position = FVector(X, Y, Floor + 0.5);
					Splash.Age = 0.0f;
					Splashes.Add(Splash);
				}
				RespawnDrop(Drop, Sim, Source, false);
			}
			else if (bWrapped)
			{
				Drop.bShown = !Roofed(Drop.Position);
			}
			if (!Drop.bShown)
			{
				Transforms.Add(FTransform(FRotator::ZeroRotator, Drop.Position, FVector(0.0001f)));
				++RoofedHidden;
				continue;
			}
			++Shown;
			const float Tilt = bRain ? FMath::RadiansToDegrees(FMath::Atan2(Drift, Fast)) : 0.0f;
			// Nothing right in front of the lens: a streak 60 cm long would look like a pole. Rain fades in between 2.5 and 8 m
			// from the camera, snow between 1 and 3 m.
			const float Distance = static_cast<float>(FVector::Dist(Drop.Position, CameraPosition));
			const float Near = bRain ? FMath::Clamp((Distance - 250.0f) / 550.0f, 0.0f, 1.0f) : FMath::Clamp((Distance - 100.0f) / 200.0f, 0.0f, 1.0f);
			const float Size = Near * (bRain ? 1.0f : 0.6f + 0.8f * Drop.Seed);
			Transforms.Add(FTransform(FRotator(0.0f, 0.0f, Tilt), Drop.Position, FVector(FMath::Max(Size, 0.0001f))));
		}
		// Whatever was drawn last frame and is not now goes out of sight.
		const FTransform Hidden(FRotator::ZeroRotator, FVector::ZeroVector, FVector(0.0001f));
		for (int32 I = Transforms.Num(); I < Previous; ++I)
		{
			Transforms.Add(Hidden);
		}
		if (!Transforms.IsEmpty())
		{
			Instances->BatchUpdateInstancesTransforms(0, Transforms, true, true, true);
		}
		OutDrawn = Shown;
	};
	Fall(Rain, WantRain, SkyMaxRain, true, RainInstances, RainDrawn);
	Fall(Snow, WantSnow, SkyMaxSnow, false, SnowInstances, SnowDrawn);

	// Splashes grow into rings of a hand's width and fade.
	TArray<FTransform> SplashTransforms;
	int32 Alive = 0;
	for (int32 Index = Splashes.Num() - 1; Index >= 0; --Index)
	{
		Splashes[Index].Age += Delta;
		if (Splashes[Index].Age >= SkySplashSeconds)
		{
			Splashes.RemoveAtSwap(Index);
		}
	}
	const FTransform Hidden(FRotator::ZeroRotator, FVector::ZeroVector, FVector(0.0001f));
	for (const FSplash& Splash : Splashes)
	{
		const float T = Splash.Age / SkySplashSeconds;
		SplashTransforms.Add(FTransform(FRotator::ZeroRotator, Splash.Position, FVector((0.3f + 0.9f * T) * (1.0f - T * T))));
		++Alive;
	}
	const int32 PreviousSplashes = SplashesDrawn;
	for (int32 I = SplashTransforms.Num(); I < PreviousSplashes; ++I)
	{
		SplashTransforms.Add(Hidden);
	}
	if (!SplashTransforms.IsEmpty())
	{
		SplashInstances->BatchUpdateInstancesTransforms(0, SplashTransforms, true, true, true);
	}
	SplashesDrawn = Alive;
}

// ---------------------------------------------------------------------------

void USkySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || GSkyEnable.GetValueOnGameThread() == 0)
	{
		if (Holder)
		{
			DestroyScene();
		}
		return;
	}
	const colony::Sim& Sim = *Source->GetSim();
	if (!Holder)
	{
		BuildScene();
		BuiltGeneration = Source->GetSimGeneration();
	}
	else if (BuiltGeneration != Source->GetSimGeneration())
	{
		// A new game or a load: the weather starts over, so the particles and the soak do too.
		BuiltGeneration = Source->GetSimGeneration();
		ResetPrecipitation();
		GroundWet = 0.0f;
		LastWetnessSeconds = -1.0f;
		Flashes = 0;
		FlashAge = 99.0f;
	}
	if (!Holder)
	{
		return;
	}

	// The sim's clock, carried on between steps so the sun moves smoothly.
	const double Seconds = static_cast<double>(Source->GetGameSeconds()) + static_cast<double>(Source->GetSpeed() > 0 ? Source->GetBlendAlpha() : 0.0f);
	float Hour = static_cast<float>(FMath::Fmod(Seconds, static_cast<double>(colony::kSecondsPerDay)) / colony::kSecondsPerHour);
	float DayOfYear = static_cast<float>(FMath::Fmod(Seconds / colony::kSecondsPerDay, static_cast<double>(colony::kDaysPerYear)));
	if (GSkyHour.GetValueOnGameThread() >= 0.0f)
	{
		Hour = FMath::Fmod(GSkyHour.GetValueOnGameThread(), 24.0f);
	}
	if (GSkyDayOfYear.GetValueOnGameThread() >= 0.0f)
	{
		DayOfYear = FMath::Fmod(GSkyDayOfYear.GetValueOnGameThread(), SkyDaysPerYear);
	}
	ApplyState(ComputeState(Sim, Hour, DayOfYear));
	ApplyWetness(Sim, static_cast<float>(Source->GetGameSeconds()), DeltaTime);

	// The air moves in real time, and holds still while the game is paused.
	const float Delta = Source->GetSpeed() > 0 ? FMath::Min(DeltaTime, 0.1f) : 0.0f;
	UpdateLightning(Sim, Delta);
	UpdatePrecipitation(Sim, *Source, Delta);
}
