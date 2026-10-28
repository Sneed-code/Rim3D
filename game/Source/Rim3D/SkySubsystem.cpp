#include "SkySubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DirectionalLightComponent.h"
#include "Dom/JsonObject.h"
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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Scalability.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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
	TAutoConsoleVariable<int32> GLookQuality(
		TEXT("Look.Quality"), 2, TEXT("How the world is drawn: 0 Low, 1 Medium, 2 High (Lumen light and reflections, volumetric fog), 3 Epic (data/look.json \"quality\")."), ECVF_Default);
	TAutoConsoleVariable<float> GLookExposure(
		TEXT("Look.Exposure"), -99.0f, TEXT("An exposure (EV100) to hold the camera at instead of the one that follows the light; below -50 follows the light."), ECVF_Default);
	TAutoConsoleVariable<int32> GLookReload(
		TEXT("Look.Reload"), 0, TEXT("Set to 1 to read data/look.json again (it goes back to 0)."), ECVF_Default);

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
	// The clear midsummer noon the exposure is measured from: the sun this high, plus the sky's own light.
	constexpr float SkyReferenceNoonDeg = 68.0f;
	// The exposure jumps to its target, instead of easing there, when the target moves more than this in a frame.
	constexpr float SkyExposureJumpEV = 2.0f;

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

	// Reading the look file: a missing number keeps the default.
	TSharedPtr<FJsonObject> LookObject(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		return Parent.IsValid() && Parent->TryGetObjectField(Key, Child) ? *Child : nullptr;
	}

	void LookNumber(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key, float& Out)
	{
		double Value = 0.0;
		if (Parent.IsValid() && Parent->TryGetNumberField(Key, Value))
		{
			Out = static_cast<float>(Value);
		}
	}

	void LookColor(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key, FLinearColor& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Parent.IsValid() && Parent->TryGetArrayField(Key, Values) && Values->Num() >= 3)
		{
			Out = FLinearColor(static_cast<float>((*Values)[0]->AsNumber()), static_cast<float>((*Values)[1]->AsNumber()), static_cast<float>((*Values)[2]->AsNumber()));
		}
	}

	const TCHAR* const LookSeasonNames[4] = {TEXT("spring"), TEXT("summer"), TEXT("autumn"), TEXT("winter")};
}

// ---------------------------------------------------------------------------
// The look file (data/look.json).

FLookConfig::FLookConfig()
{
	Seasons[0] = {6500.0f, 1.04f, 1.02f, FLinearColor(1.0f, 1.0f, 0.99f)};
	Seasons[1] = {6700.0f, 1.07f, 1.05f, FLinearColor(1.01f, 1.0f, 0.97f)};
	Seasons[2] = {6900.0f, 1.0f, 1.04f, FLinearColor(1.03f, 0.99f, 0.94f)};
	Seasons[3] = {6350.0f, 0.9f, 1.0f, FLinearColor(0.98f, 0.99f, 1.02f)};
	Quality.Add({TEXT("Low"), 0, false, false, false, 16});
	Quality.Add({TEXT("Medium"), 1, false, true, true, 16});
	Quality.Add({TEXT("High"), 2, true, true, true, 12});
	Quality.Add({TEXT("Epic"), 3, true, true, true, 8});
}

bool FLookConfig::Load(const FString& Path, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("%s not found"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("%s is not valid JSON"), *Path);
		return false;
	}
	const TSharedPtr<FJsonObject> SunJson = LookObject(Root, TEXT("sun"));
	LookNumber(SunJson, TEXT("clearNoonLux"), SunClearNoonLux);
	LookColor(SunJson, TEXT("color"), SunColor);
	LookNumber(SunJson, TEXT("fadeBelowDeg"), SunFadeBelowDeg);
	LookNumber(SunJson, TEXT("fadeAboveDeg"), SunFadeAboveDeg);
	LookNumber(SunJson, TEXT("contactShadowLength"), ContactShadowLength);
	LookNumber(SunJson, TEXT("shadowDistanceCm"), ShadowDistanceCm);
	LookNumber(SunJson, TEXT("volumetricScattering"), SunVolumetricScattering);
	const TSharedPtr<FJsonObject> MoonJson = LookObject(Root, TEXT("moon"));
	LookNumber(MoonJson, TEXT("fullLux"), MoonFullLux);
	LookNumber(MoonJson, TEXT("newMoonShare"), MoonNewShare);
	LookColor(MoonJson, TEXT("color"), MoonColor);
	const TSharedPtr<FJsonObject> SkyJson = LookObject(Root, TEXT("skyLight"));
	LookNumber(SkyJson, TEXT("day"), SkyLightDay);
	LookNumber(SkyJson, TEXT("night"), SkyLightNight);
	LookNumber(SkyJson, TEXT("overcast"), SkyLightOvercast);
	LookNumber(Root, TEXT("lightningLux"), LightningLux);

	const TSharedPtr<FJsonObject> ExposureJson = LookObject(Root, TEXT("exposure"));
	LookNumber(ExposureJson, TEXT("noonEV"), NoonEV);
	LookNumber(ExposureJson, TEXT("follow"), Follow);
	LookNumber(ExposureJson, TEXT("minEV"), MinEV);
	LookNumber(ExposureJson, TEXT("maxEV"), MaxEV);
	LookNumber(ExposureJson, TEXT("brighterEV"), BrighterEV);
	LookNumber(ExposureJson, TEXT("darkerEV"), DarkerEV);
	LookNumber(ExposureJson, TEXT("speedUp"), SpeedUp);
	LookNumber(ExposureJson, TEXT("speedDown"), SpeedDown);
	LookNumber(ExposureJson, TEXT("skyShareOfSun"), SkyShareOfSun);

	const TSharedPtr<FJsonObject> FogJson = LookObject(Root, TEXT("fog"));
	LookNumber(FogJson, TEXT("clearDensity"), FogClearDensity);
	LookNumber(FogJson, TEXT("sightDensity"), FogSightDensity);
	LookNumber(FogJson, TEXT("rainDensity"), FogRainDensity);
	LookNumber(FogJson, TEXT("maxDensity"), FogMaxDensity);
	LookNumber(FogJson, TEXT("heightFalloff"), FogHeightFalloff);
	LookNumber(FogJson, TEXT("startDistanceCm"), FogStartDistanceCm);
	LookColor(FogJson, TEXT("overcastColor"), FogOvercastColor);
	LookColor(FogJson, TEXT("snowColor"), FogSnowColor);
	LookNumber(FogJson, TEXT("veilAlbedo"), FogVeilAlbedo);
	LookColor(FogJson, TEXT("volumetricAlbedo"), VolumetricAlbedo);
	LookNumber(FogJson, TEXT("volumetricExtinction"), VolumetricExtinction);
	LookNumber(FogJson, TEXT("volumetricScatteringDistribution"), VolumetricScatteringDistribution);
	LookNumber(FogJson, TEXT("volumetricDistanceCm"), VolumetricDistanceCm);
	LookNumber(FogJson, TEXT("lowSunMist"), LowSunMist);
	LookNumber(FogJson, TEXT("lowSunMistFromDeg"), LowSunMistFromDeg);
	LookNumber(FogJson, TEXT("lowSunMistToDeg"), LowSunMistToDeg);

	const TSharedPtr<FJsonObject> FilmJson = LookObject(Root, TEXT("film"));
	LookNumber(FilmJson, TEXT("slope"), FilmSlope);
	LookNumber(FilmJson, TEXT("toe"), FilmToe);
	LookNumber(FilmJson, TEXT("shoulder"), FilmShoulder);
	LookNumber(FilmJson, TEXT("blackClip"), FilmBlackClip);
	LookNumber(FilmJson, TEXT("whiteClip"), FilmWhiteClip);
	LookNumber(FilmJson, TEXT("bloom"), Bloom);
	LookNumber(FilmJson, TEXT("vignette"), Vignette);
	LookNumber(FilmJson, TEXT("ambientOcclusion"), AmbientOcclusion);
	LookNumber(FilmJson, TEXT("ambientOcclusionRadiusCm"), AmbientOcclusionRadiusCm);
	LookNumber(FilmJson, TEXT("localExposureHighlights"), LocalExposureHighlights);
	LookNumber(FilmJson, TEXT("localExposureShadows"), LocalExposureShadows);

	const TSharedPtr<FJsonObject> SeasonsJson = LookObject(Root, TEXT("seasons"));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const TSharedPtr<FJsonObject> One = LookObject(SeasonsJson, LookSeasonNames[Index]);
		LookNumber(One, TEXT("white"), Seasons[Index].White);
		LookNumber(One, TEXT("saturation"), Seasons[Index].Saturation);
		LookNumber(One, TEXT("contrast"), Seasons[Index].Contrast);
		LookColor(One, TEXT("gain"), Seasons[Index].Gain);
	}

	const TArray<TSharedPtr<FJsonValue>>* Levels = nullptr;
	if (const TSharedPtr<FJsonObject> QualityJson = LookObject(Root, TEXT("quality")); QualityJson.IsValid() && QualityJson->TryGetArrayField(TEXT("levels"), Levels) && !Levels->IsEmpty())
	{
		Quality.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Levels)
		{
			const TSharedPtr<FJsonObject> Level = Value->AsObject();
			if (!Level.IsValid())
			{
				continue;
			}
			FLookQuality Q;
			Level->TryGetStringField(TEXT("name"), Q.Name);
			Level->TryGetNumberField(TEXT("scalability"), Q.Scalability);
			Level->TryGetBoolField(TEXT("volumetricFog"), Q.bVolumetricFog);
			Level->TryGetBoolField(TEXT("contactShadows"), Q.bContactShadows);
			Level->TryGetBoolField(TEXT("clouds"), Q.bClouds);
			Level->TryGetNumberField(TEXT("fogGridPixels"), Q.FogGridPixels);
			Quality.Add(Q);
		}
	}
	return true;
}

FLookSeason FLookConfig::GradeFor(float DayOfYear) const
{
	// The seasons' middle days are 7.5, 22.5, 37.5 and 52.5; between two of them the grade blends.
	const float Position = FMath::Fmod(DayOfYear - 7.5f + 60.0f, 60.0f) / 15.0f;
	const int32 From = FMath::Clamp(FMath::FloorToInt32(Position), 0, 3);
	const int32 To = (From + 1) % 4;
	const float T = SkySmooth(0.0f, 1.0f, Position - From);
	FLookSeason Out;
	Out.White = FMath::Lerp(Seasons[From].White, Seasons[To].White, T);
	Out.Saturation = FMath::Lerp(Seasons[From].Saturation, Seasons[To].Saturation, T);
	Out.Contrast = FMath::Lerp(Seasons[From].Contrast, Seasons[To].Contrast, T);
	Out.Gain = FMath::Lerp(Seasons[From].Gain, Seasons[To].Gain, T);
	return Out;
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

FString USkySubsystem::GetQualityName() const
{
	return Look.Quality.IsValidIndex(AppliedQuality) ? Look.Quality[AppliedQuality].Name : FString(TEXT("?"));
}

const FPostProcessSettings* USkySubsystem::GetPostProcess() const
{
	return Exposure ? &Exposure->Settings : nullptr;
}

void USkySubsystem::LoadLook()
{
	USimSubsystem* Source = Bridge.Get();
	const FString Path = Source ? Source->GetDataRoot() / TEXT("look.json") : FString();
	FLookConfig Fresh;
	FString Error;
	bLookLoaded = Fresh.Load(Path, Error);
	Look = Fresh;
	if (bLookLoaded)
	{
		UE_LOG(LogSky, Display, TEXT("Look: read %s (sun %.0f lux at a clear noon, noon exposure EV %.1f)"), *Path, Look.SunClearNoonLux, Look.NoonEV);
	}
	else
	{
		UE_LOG(LogSky, Warning, TEXT("Look: %s; the built-in numbers are used"), *Error);
	}
	AppliedQuality = -1;
}

void USkySubsystem::ApplyQuality(int32 Level)
{
	if (Look.Quality.IsEmpty())
	{
		return;
	}
	Level = FMath::Clamp(Level, 0, Look.Quality.Num() - 1);
	const FLookQuality& Q = Look.Quality[Level];
	// Every scalability group follows the step, but the render resolution stays the player's.
	Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
	const float Resolution = Levels.ResolutionQuality;
	Levels.SetFromSingleQualityLevel(Q.Scalability);
	Levels.ResolutionQuality = Resolution;
	Scalability::SetQualityLevels(Levels);
	if (IConsoleVariable* Grid = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog.GridPixelSize")))
	{
		Grid->Set(Q.FogGridPixels, ECVF_SetByCode);
	}
	AppliedQuality = Level;
	UE_LOG(LogSky, Display, TEXT("Look.Quality %d (%s): scalability %d, volumetric fog %d, contact shadows %d, clouds %d"), Level, *Q.Name, Q.Scalability,
		Q.bVolumetricFog ? 1 : 0, Q.bContactShadows ? 1 : 0, Q.bClouds ? 1 : 0);
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

	// Light in real-world units (data/look.json): the sun fades out just as it sets (the sky atmosphere already dims and reddens
	// it near the horizon), clouds take away the sky glow's square, and the moon is a real moon's few tenths of a lux.
	const float Glow = FMath::Clamp(Effects.skyGlow, 0.0f, 1.0f);
	S.SunLux = Look.SunClearNoonLux * SkySmooth(Look.SunFadeBelowDeg, Look.SunFadeAboveDeg, Elevation) * FMath::Square(Glow);
	S.MoonLux = Look.MoonFullLux * (Look.MoonNewShare + (1.0f - Look.MoonNewShare) * S.MoonFullness) * SkySmooth(0.0f, 12.0f, S.MoonElevation) * (1.0f - S.Daylight) *
		FMath::Lerp(1.0f, 0.3f, S.Overcast);
	S.SkyLightIntensity = FMath::Lerp(Look.SkyLightNight, Look.SkyLightDay * FMath::Lerp(1.0f, Look.SkyLightOvercast, S.Overcast), S.Daylight);

	// The exposure follows the light on open ground (the sun's share that falls on it, the sky's own light through the day and
	// twilight, the moon), but only `Follow` of the way, so a dusk looks like a dusk and a night like a night.
	const float SkyLux = Look.SkyShareOfSun * Look.SunClearNoonLux * SkySmooth(-6.0f, 30.0f, Elevation) * Glow;
	S.GroundLux = S.SunLux * FMath::Max(static_cast<float>(S.ToSun.Z), 0.0f) + SkyLux + S.MoonLux * (0.2f + FMath::Max(static_cast<float>(S.ToMoon.Z), 0.0f));
	const float NoonLux = Look.SunClearNoonLux * (FMath::Sin(FMath::DegreesToRadians(SkyReferenceNoonDeg)) + Look.SkyShareOfSun);
	S.ExposureEV = FMath::Clamp(Look.NoonEV + Look.Follow * FMath::Log2(FMath::Max(S.GroundLux, 1.0e-4f) / NoonLux), Look.MinEV, Look.MaxEV);

	// How far a pawn sees through the air under the open sky is how far you see: the fog is the weather's sight factor turned
	// into a density (0.5 is thick fog), with a little more for rain and a faint haze when it is clear. (The engine divides
	// the density by 1000, per centimetre: 0.3 makes a mist you cannot see 50 m through.)
	const float Sight = FMath::Clamp(Effects.sightFactor, 0.2f, 1.0f);
	S.FogDensity = FMath::Min(Look.FogClearDensity + Look.FogSightDensity * (1.0f / Sight - 1.0f) + Look.FogRainDensity * Effects.rainRate, Look.FogMaxDensity);
	// A low mist while the sun is near the horizon, morning and evening, when it is not raining (the volumetric fog's light shafts).
	S.FogDensity += Look.LowSunMist * SkySmooth(Look.LowSunMistFromDeg, Look.LowSunMistFromDeg + 3.0f, Elevation) *
		(1.0f - SkySmooth(Look.LowSunMistToDeg - 6.0f, Look.LowSunMistToDeg, Elevation)) * (1.0f - S.Rain);
	// The fog's colour is the sky atmosphere's own; as the sky clouds over, snows or fogs up, a grey (or snow-white) veil lit by
	// the sky's light is mixed in.
	const float Veil = FMath::Max3(S.Overcast, S.Snow, FMath::Clamp((1.0f - Sight) / 0.6f, 0.0f, 1.0f));
	const float VeilLuminance = Look.FogVeilAlbedo * (SkyLux + 0.2f * S.MoonLux) / PI;
	S.FogVeil = FMath::Lerp(Look.FogOvercastColor, Look.FogSnowColor, S.Snow) * (VeilLuminance * Veil);
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
	LoadLook();

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
	Sun = MakeLight(Look.SunClearNoonLux, Look.SunColor, 0, true);
	Sun->SetDynamicShadowDistanceMovableLight(Look.ShadowDistanceCm);
	Sun->SetVolumetricScatteringIntensity(Look.SunVolumetricScattering);
	Sun->bCastCloudShadows = true;
	Sun->ContactShadowLength = Look.ContactShadowLength;
	Sun->MarkRenderStateDirty();
	Moon = MakeLight(0.0f, Look.MoonColor, 1, false);
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
	Fog->SetFogHeightFalloff(Look.FogHeightFalloff);
	Fog->SetStartDistance(Look.FogStartDistanceCm);
	Fog->SetFogMaxOpacity(0.95f);
	Fog->SetFogInscatteringColor(FLinearColor::Black);
	Fog->SetSkyAtmosphereAmbientContributionColorScale(FLinearColor::White);
	Fog->SetVolumetricFogScatteringDistribution(Look.VolumetricScatteringDistribution);
	Fog->SetVolumetricFogAlbedo(Look.VolumetricAlbedo.ToFColor(true));
	Fog->SetVolumetricFogExtinctionScale(Look.VolumetricExtinction);
	Fog->SetVolumetricFogDistance(Look.VolumetricDistanceCm);
	Fog->SetupAttachment(Root);
	Fog->RegisterComponent();

	// The camera: an exposure held near the one the light calls for, the film curve and the season's grade (ApplyState).
	Exposure = NewObject<UPostProcessComponent>(Holder);
	Exposure->bUnbound = true;
	Exposure->Priority = 5.0f;
	{
		FPostProcessSettings& P = Exposure->Settings;
		P.bOverride_AutoExposureMethod = true;
		P.AutoExposureMethod = AEM_Histogram;
		P.bOverride_AutoExposureBias = true;
		P.AutoExposureBias = 0.0f;
		P.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		P.AutoExposureApplyPhysicalCameraExposure = false;
		P.bOverride_AutoExposureMinBrightness = true;
		P.bOverride_AutoExposureMaxBrightness = true;
		P.bOverride_AutoExposureSpeedUp = true;
		P.AutoExposureSpeedUp = Look.SpeedUp;
		P.bOverride_AutoExposureSpeedDown = true;
		P.AutoExposureSpeedDown = Look.SpeedDown;
		P.bOverride_FilmSlope = true;
		P.FilmSlope = Look.FilmSlope;
		P.bOverride_FilmToe = true;
		P.FilmToe = Look.FilmToe;
		P.bOverride_FilmShoulder = true;
		P.FilmShoulder = Look.FilmShoulder;
		P.bOverride_FilmBlackClip = true;
		P.FilmBlackClip = Look.FilmBlackClip;
		P.bOverride_FilmWhiteClip = true;
		P.FilmWhiteClip = Look.FilmWhiteClip;
		P.bOverride_BloomIntensity = true;
		P.BloomIntensity = Look.Bloom;
		P.bOverride_VignetteIntensity = true;
		P.VignetteIntensity = Look.Vignette;
		P.bOverride_LensFlareIntensity = true;
		P.LensFlareIntensity = 0.0f;
		P.bOverride_FilmGrainIntensity = true;
		P.FilmGrainIntensity = 0.0f;
		P.bOverride_AmbientOcclusionIntensity = true;
		P.AmbientOcclusionIntensity = Look.AmbientOcclusion;
		P.bOverride_AmbientOcclusionRadius = true;
		P.AmbientOcclusionRadius = Look.AmbientOcclusionRadiusCm;
		P.bOverride_LocalExposureHighlightContrastScale = true;
		P.LocalExposureHighlightContrastScale = Look.LocalExposureHighlights;
		P.bOverride_LocalExposureShadowContrastScale = true;
		P.LocalExposureShadowContrastScale = Look.LocalExposureShadows;
		P.bOverride_WhiteTemp = true;
		P.bOverride_ColorSaturation = true;
		P.bOverride_ColorContrast = true;
		P.bOverride_ColorGain = true;
	}
	Exposure->SetupAttachment(Root);
	Exposure->RegisterComponent();
	LastExposureEV = -100.0f;

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
	UE_LOG(LogSky, Display, TEXT("Sky: built; the level's own sun, sky light and fog (%d parts) are out, a clear noon is %.0f lux"), LevelParts.Num(), Look.SunClearNoonLux);
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
	const bool bSunUp = State.SunLux > 1.0f;
	if (Sun->IsVisible() != bSunUp)
	{
		Sun->SetVisibility(bSunUp);
	}
	Sun->SetCastShadows(State.SunLux > 200.0f);

	Moon->SetWorldRotation((-State.ToMoon).Rotation());
	Moon->SetIntensity(FMath::Max(State.MoonLux, 0.0001f));
	const bool bMoonUp = State.MoonLux > 0.005f;
	if (Moon->IsVisible() != bMoonUp)
	{
		Moon->SetVisibility(bMoonUp);
	}

	SkyLight->SetIntensity(State.SkyLightIntensity);

	// Fog takes the sky atmosphere's colour; the veil is the grey of an overcast sky or the white of snow (ComputeState).
	Fog->SetFogDensity(State.FogDensity);
	Fog->SetFogInscatteringColor(State.FogVeil * GSkyFogLuminance.GetValueOnGameThread());

	// The weather's sky glow already dims the sun under cloud, so the clouds' own shadow on the ground fades out as the sky
	// closes over (else an overcast day is darkened twice); scattered clouds keep their moving shadows.
	const float CloudShadow = FMath::Lerp(1.0f, 0.1f, State.Overcast);
	if (FMath::Abs(Sun->CloudShadowOnSurfaceStrength - CloudShadow) > 0.02f)
	{
		Sun->CloudShadowOnSurfaceStrength = CloudShadow;
		Sun->MarkRenderStateDirty();
	}

	// What the Look.Quality step turns on.
	if (Look.Quality.IsValidIndex(AppliedQuality))
	{
		const FLookQuality& Q = Look.Quality[AppliedQuality];
		const float Contact = Q.bContactShadows ? Look.ContactShadowLength : 0.0f;
		if (Sun->ContactShadowLength != Contact)
		{
			Sun->ContactShadowLength = Contact;
			Sun->MarkRenderStateDirty();
		}
		if (Fog->bEnableVolumetricFog != Q.bVolumetricFog)
		{
			Fog->SetVolumetricFog(Q.bVolumetricFog);
		}
		if (Clouds && Clouds->IsVisible() != Q.bClouds)
		{
			Clouds->SetVisibility(Q.bClouds);
		}
	}

	// The camera's exposure: the auto exposure may move a little either side of the target (darker rooms, bright snow), and
	// jumps straight there when the target moves a lot at once (a time jump, a load) instead of easing over seconds.
	const float ForcedEV = GLookExposure.GetValueOnGameThread();
	if (ForcedEV > -50.0f)
	{
		State.ExposureEV = ForcedEV;
	}
	if (Exposure)
	{
		FPostProcessSettings& P = Exposure->Settings;
		P.AutoExposureMinBrightness = State.ExposureEV - Look.BrighterEV;
		P.AutoExposureMaxBrightness = State.ExposureEV + Look.DarkerEV;
		const FLookSeason Grade = Look.GradeFor(State.DayOfYear);
		P.WhiteTemp = Grade.White;
		P.ColorSaturation = FVector4(Grade.Saturation, Grade.Saturation, Grade.Saturation, 1.0);
		P.ColorContrast = FVector4(Grade.Contrast, Grade.Contrast, Grade.Contrast, 1.0);
		P.ColorGain = FVector4(Grade.Gain.R, Grade.Gain.G, Grade.Gain.B, 1.0);
		if (LastExposureEV < -50.0f || FMath::Abs(State.ExposureEV - LastExposureEV) > SkyExposureJumpEV)
		{
			if (APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0))
			{
				CameraManager->SetGameCameraCutThisFrame();
				++ExposureCuts;
			}
		}
		LastExposureEV = State.ExposureEV;
	}

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
		FlashIntensity = Look.LightningLux * Envelope;
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
	if (GLookReload.GetValueOnGameThread() != 0)
	{
		if (IConsoleVariable* Reload = IConsoleManager::Get().FindConsoleVariable(TEXT("Look.Reload")))
		{
			Reload->Set(0, ECVF_SetByCode);
		}
		LoadLook();
	}
	if (const int32 Wanted = FMath::Clamp(GLookQuality.GetValueOnGameThread(), 0, FMath::Max(Look.Quality.Num() - 1, 0)); AppliedQuality != Wanted)
	{
		ApplyQuality(Wanted);
	}
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
