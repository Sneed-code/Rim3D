// Smoke test `sky` (task U13): the sun, moon, sky, clouds, fog, rain, snow, wet ground, lightning and the date readout, drawn from the sim's clock and weather, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "SnowSubsystem.h"
#include "TerrainSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Weather.h"

#include <optional>
#include <string>

namespace
{
	const FVector SkySmokeOrigin(-1100000.0, 0.0, 0.0);
	constexpr float SkySmokeMidsummer = 22.5f;
	constexpr float SkySmokeMidwinter = 52.5f;

	struct FSkySmokeState
	{
		/** Game seconds the phase still has to run, and how many it ran. */
		int64 StepsWanted = 0;
		int64 StepsRun = 0;
		int64 SnowCellsAtEnd = 0;
		float SummerNoonElevation = 0.0f;
		float MaxFogDensity = 0.0f;
		int32 MaxRainDrawn = 0;
		int32 MaxSnowDrawn = 0;
		int32 MaxSplashes = 0;
		float MaxWet = 0.0f;
		int32 FlashesAtStart = 0;
		double SlowestFrameMs = 0.0;
		colony::CellPos Home = colony::CellPos{128, 128, 0};
		/** A place where the mountain's roof (the sim's thick roof) is about half of what is within 12 m. */
		colony::CellPos RockEdge = colony::CellPos{128, 128, 0};
		int32 RoofedBefore = 0;
	};

	void SkySmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}
}

void USmokeTestSubsystem::BuildSkyPhases()
{
	const TSharedRef<FSkySmokeState> State = MakeShared<FSkySmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Sky = [this]() { return GetWorld()->GetSubsystem<USkySubsystem>(); };
	const auto Snow = [this]() { return GetWorld()->GetSubsystem<USnowSubsystem>(); };
	const auto Terrain = [this]() { return GetWorld()->GetSubsystem<UTerrainSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto Aim = [=, this](float Notches, bool bAtRock = false)
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (Pawn)
		{
			const colony::CellPos At = bAtRock ? State->RockEdge : State->Home;
			Pawn->ResetView();
			Pawn->Zoom(Notches);
			Pawn->FocusOn(Bridge()->SimToWorld(At.x + 0.5f, At.y + 0.5f, 0));
			Pawn->SnapCamera();
		}
	};
	const auto SetWeather = [=, this](const char* Name, float Hours)
	{
		Bridge()->Submit(colony::SetWeatherCommand{Name, Hours});
	};
	const auto SetClock = [](float Hour, float DayOfYear)
	{
		SkySmokeSet(TEXT("Sky.Hour"), Hour);
		SkySmokeSet(TEXT("Sky.DayOfYear"), DayOfYear);
	};
	// Runs the sim ahead during a phase, a little each frame so the frames stay alive: `Wanted` steps in all.
	const auto Advance = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim() || State->StepsRun >= State->StepsWanted)
		{
			return;
		}
		const double Start = FPlatformTime::Seconds();
		int32 Count = 0;
		while (State->StepsRun < State->StepsWanted && (FPlatformTime::Seconds() - Start) < 0.03 && Count < 400)
		{
			B->StepOnce();
			++State->StepsRun;
			++Count;
		}
	};
	// What the sky does every frame: keep the largest of the numbers a phase reports.
	const auto Watch = [=, this]()
	{
		USkySubsystem* S = Sky();
		UTerrainSubsystem* T = Terrain();
		if (!S || !S->IsSceneBuilt())
		{
			return;
		}
		State->MaxFogDensity = FMath::Max(State->MaxFogDensity, S->GetState().FogDensity);
		State->MaxRainDrawn = FMath::Max(State->MaxRainDrawn, S->GetRainDrawn());
		State->MaxSnowDrawn = FMath::Max(State->MaxSnowDrawn, S->GetSnowDrawn());
		State->MaxSplashes = FMath::Max(State->MaxSplashes, S->GetSplashesDrawn());
		State->MaxWet = FMath::Max(State->MaxWet, T ? T->GetGroundWetness() : 0.0f);
	};
	const auto Reset = [=]()
	{
		State->StepsWanted = 0;
		State->StepsRun = 0;
		State->MaxFogDensity = 0.0f;
		State->MaxRainDrawn = 0;
		State->MaxSnowDrawn = 0;
		State->MaxSplashes = 0;
		State->MaxWet = 0.0f;
	};

	// 1. A new sim, the overseer view, a clear midsummer noon: the sky has its own sun and the level's is out.
	FPhase Day;
	Day.Name = TEXT("sky_summer_noon");
	Day.WarmupSeconds = 6.0f;
	Day.MeasureSeconds = 3.0f;
	Day.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Sky() || !Snow() || !Terrain() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, USkySubsystem, USnowSubsystem, UTerrainSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		SkySmokeSet(TEXT("Terrain.Roofs"), 0.0f);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(SkySmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(1);
		if (!Overseer()->EnterOverseerMode())
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		if (const std::optional<colony::CellPos> Stockpile = B->GetSim()->stockpileCell())
		{
			State->Home = *Stockpile;
		}
		// The nearest place to home where about half the cells within 12 m are the mountain's roof.
		{
			const colony::Map& Map = B->GetSim()->map();
			double Best = 1.0e18;
			for (int32 Y = 12; Y < Map.height() - 12; Y += 3)
			{
				for (int32 X = 12; X < Map.width() - 12; X += 3)
				{
					int32 Roofed = 0;
					for (int32 DY = -12; DY <= 12; DY += 2)
					{
						for (int32 DX = -12; DX <= 12; DX += 2)
						{
							Roofed += Map.roof(colony::CellPos{X + DX, Y + DY, 0}) != colony::Roof::None ? 1 : 0;
						}
					}
					const int32 Total = 13 * 13;
					if (Roofed > Total * 4 / 10 && Roofed < Total * 6 / 10)
					{
						const double Dist = FVector2D::DistSquared(FVector2D(X, Y), FVector2D(State->Home.x, State->Home.y));
						if (Dist < Best)
						{
							Best = Dist;
							State->RockEdge = colony::CellPos{X, Y, 0};
						}
					}
				}
			}
			Notes.Add(FString::Printf(TEXT("sky_summer_noon: the mountain's edge nearest home is at (%d, %d)"), State->RockEdge.x, State->RockEdge.y));
		}
		Reset();
		SetClock(12.0f, SkySmokeMidsummer);
		SetWeather("Clear", 48.0f);
		Aim(3.0f);
	};
	Day.OnFrame = Watch;
	Day.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S || !S->IsSceneBuilt())
		{
			Fail(TEXT("sky_summer_noon: the sky was never built"));
			return;
		}
		const FSkyState& K = S->GetState();
		State->SummerNoonElevation = K.SunElevation;
		Notes.Add(FString::Printf(TEXT("sky_summer_noon: sun %.1f degrees up at bearing %.0f, %.1f lux, sky light %.2f, fog %.4f, cloud cover %.2f; the level's own sun, sky light and fog: %d parts put out"),
			K.SunElevation, K.SunAzimuth, K.SunLux, K.SkyLightIntensity, K.FogDensity, K.CloudCoverage, S->GetLevelLightsHidden()));
		if (K.SunElevation < 66.0f || K.SunElevation > 70.0f)
		{
			Fail(FString::Printf(TEXT("sky_summer_noon: the midsummer noon sun is %.1f degrees up, wanted about 68"), K.SunElevation));
		}
		if (FMath::Abs(K.SunAzimuth - 180.0f) > 1.0f)
		{
			Fail(FString::Printf(TEXT("sky_summer_noon: the noon sun is at bearing %.0f, wanted south (180)"), K.SunAzimuth));
		}
		if (!S->GetSun() || FVector::DotProduct(-S->GetSun()->GetForwardVector(), K.ToSun) < 0.999f)
		{
			Fail(TEXT("sky_summer_noon: the sun light does not point from the sun's place in the sky"));
		}
		else if (S->GetSun()->Intensity < 4.0f || !S->GetSun()->IsVisible())
		{
			Fail(FString::Printf(TEXT("sky_summer_noon: the noon sun's light is %.2f lux (visible %d)"), S->GetSun()->Intensity, S->GetSun()->IsVisible()));
		}
		if (!S->GetAtmosphere() || !S->GetFog() || !S->GetMoon())
		{
			Fail(TEXT("sky_summer_noon: the atmosphere, fog or moon is missing"));
		}
		if (!S->GetClouds())
		{
			Fail(TEXT("sky_summer_noon: the clouds are missing (their material did not load?)"));
		}
		if (State->MaxFogDensity > 0.01f || S->GetRainDrawn() != 0 || S->GetSnowDrawn() != 0)
		{
			Fail(FString::Printf(TEXT("sky_summer_noon: a clear day has fog %.4f, %d rain and %d snow drawn"), State->MaxFogDensity, S->GetRainDrawn(), S->GetSnowDrawn()));
		}
		if (State->MaxWet > 0.01f)
		{
			Fail(TEXT("sky_summer_noon: the ground is wet on a clear first day"));
		}
		// The date and the weather as the HUD gives them.
		const colony::Sim* Core = SimOf();
		const FString Date = USkySubsystem::DescribeDate(*Core);
		const FString Climate = USkySubsystem::DescribeClimate(*Core);
		Notes.Add(FString::Printf(TEXT("sky_summer_noon: the readout says \"%s   %s\""), *Date, *Climate));
		if (!Date.StartsWith(TEXT("Spring 1, Year 1")) || !Climate.Contains(TEXT("°C")))
		{
			Fail(FString::Printf(TEXT("sky_summer_noon: the readout \"%s\" / \"%s\" is not 'Spring 1, Year 1' and a temperature"), *Date, *Climate));
		}
	};
	Phases.Add(MoveTemp(Day));

	// 2. Midwinter noon: the sun is lower than in summer.
	FPhase Winter;
	Winter.Name = TEXT("sky_winter_noon");
	Winter.WarmupSeconds = 3.0f;
	Winter.MeasureSeconds = 2.0f;
	Winter.Setup = [=, this]()
	{
		SetClock(12.0f, SkySmokeMidwinter);
		Aim(3.0f);
	};
	Winter.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		const float WinterElevation = S->GetState().SunElevation;
		Notes.Add(FString::Printf(TEXT("sky_winter_noon: sun %.1f degrees up (summer %.1f)"), WinterElevation, State->SummerNoonElevation));
		if (WinterElevation < 26.0f || WinterElevation > 30.0f || WinterElevation > State->SummerNoonElevation - 30.0f)
		{
			Fail(FString::Printf(TEXT("sky_winter_noon: the midwinter noon sun is %.1f degrees up (summer %.1f), wanted about 28"), WinterElevation, State->SummerNoonElevation));
		}
	};
	Phases.Add(MoveTemp(Winter));

	// 3. Rain: the sim's rain comes in over the weather's transition, streaks fall, splashes jump, the ground soaks.
	FPhase Rain;
	Rain.Name = TEXT("sky_rain");
	Rain.WarmupSeconds = 22.0f;
	Rain.MeasureSeconds = 4.0f;
	Rain.Setup = [=, this]()
	{
		Reset();
		SetClock(14.0f, SkySmokeMidsummer);
		SetWeather("Rain", 30.0f);
		State->StepsWanted = 3 * colony::kSecondsPerHour;
		// At the foot of the mountain, where the rain falls half on open ground and half on its roof.
		Aim(3.0f, true);
	};
	Rain.OnFrame = [=]() { Advance(); Watch(); };
	Rain.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("sky_rain: %lld of %lld steps run, up to %d streaks and %d splashes drawn (%d hidden under roofs), fog %.4f, ground wetness %.2f"),
			State->StepsRun, State->StepsWanted, State->MaxRainDrawn, State->MaxSplashes, S->GetRoofedHidden(), State->MaxFogDensity, State->MaxWet));
		if (State->StepsRun < State->StepsWanted)
		{
			Fail(TEXT("sky_rain: the sim did not run the three game hours in time"));
		}
		if (State->MaxRainDrawn < 1500 || State->MaxSplashes < 5)
		{
			Fail(FString::Printf(TEXT("sky_rain: only %d rain streaks and %d splashes were ever drawn"), State->MaxRainDrawn, State->MaxSplashes));
		}
		if (State->MaxWet < 0.3f)
		{
			Fail(FString::Printf(TEXT("sky_rain: the ground only got to wetness %.2f after three hours of rain"), State->MaxWet));
		}
		if (S->GetRoofedHidden() < 200)
		{
			Fail(FString::Printf(TEXT("sky_rain: only %d drops were hidden for being over the mountain's roof (%d, %d)"), S->GetRoofedHidden(), State->RockEdge.x, State->RockEdge.y));
		}
		if (State->MaxFogDensity < 0.0015f || S->GetState().Overcast < 0.2f)
		{
			Fail(FString::Printf(TEXT("sky_rain: rain brought no cloud or haze (fog %.4f, overcast %.2f)"), State->MaxFogDensity, S->GetState().Overcast));
		}
		if (S->GetSnowDrawn() != 0)
		{
			Fail(TEXT("sky_rain: snow is falling in the rain"));
		}
		const FVector Centre = S->GetPrecipitationCentre();
		if (Centre.IsNearlyZero())
		{
			Fail(TEXT("sky_rain: the block of rain never found the camera"));
		}
	};
	Phases.Add(MoveTemp(Rain));

	// 4. Heavy snow in midwinter: flakes in the air, and a skin of snow grows on the ground.
	FPhase SnowPhase;
	SnowPhase.Name = TEXT("sky_snow");
	SnowPhase.WarmupSeconds = 40.0f;
	SnowPhase.MeasureSeconds = 4.0f;
	SnowPhase.Setup = [=, this]()
	{
		Reset();
		SetClock(11.0f, SkySmokeMidwinter);
		SetWeather("SnowHard", 40.0f);
		// The sim's day is the first spring day, so the afternoon melts what the night laid: stop in the morning.
		State->StepsWanted = 8 * colony::kSecondsPerHour;
		Aim(3.0f);
	};
	SnowPhase.OnFrame = [=]() { Advance(); Watch(); };
	SnowPhase.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		USnowSubsystem* W = Snow();
		const colony::Sim* Core = SimOf();
		if (!S || !W || !Core)
		{
			return;
		}
		W->SyncAll();
		// What the sim says, counted the way the skin is drawn: cells with 8 percent or more that are not rock or water.
		int64 Expected = 0;
		float Deepest = 0.0f;
		const colony::Map& Map = Core->map();
		for (int32 Y = 0; Y < Map.height(); ++Y)
		{
			for (int32 X = 0; X < Map.width(); ++X)
			{
				const float D = Core->weather().snowDepth(colony::CellPos{X, Y, 0});
				if (D >= 8.0f && Map.terrain(X, Y) != colony::Terrain::Rock && Map.terrain(X, Y) != colony::Terrain::Water)
				{
					++Expected;
					Deepest = FMath::Max(Deepest, D);
				}
			}
		}
		Notes.Add(FString::Printf(TEXT("sky_snow: %lld of %lld steps run, up to %d flakes drawn, the sim has %lld snow cells (deepest %.0f%%), %lld drawn in %d chunks (%lld vertices, %lld rebuilds, slowest %.1f ms)"),
			State->StepsRun, State->StepsWanted, State->MaxSnowDrawn, Expected, Deepest, W->GetCellsDrawn(), W->GetChunksWithSnow(), W->GetVertices(), W->GetRebuilds(), W->GetSlowestRebuildMs()));
		State->SnowCellsAtEnd = Expected;
		if (State->StepsRun < State->StepsWanted)
		{
			Fail(TEXT("sky_snow: the sim did not run the eight game hours in time"));
		}
		if (State->MaxSnowDrawn < 1500)
		{
			Fail(FString::Printf(TEXT("sky_snow: only %d flakes were ever drawn"), State->MaxSnowDrawn));
		}
		if (Expected < 1000 || Deepest < 20.0f)
		{
			Fail(FString::Printf(TEXT("sky_snow: the sim has only %lld cells of snow (deepest %.0f%%) after eight hours of heavy snow"), Expected, Deepest));
		}
		if (W->GetCellsDrawn() != Expected)
		{
			Fail(FString::Printf(TEXT("sky_snow: %lld cells drawn but the sim has snow on %lld"), W->GetCellsDrawn(), Expected));
		}
		if (W->GetSlowestRebuildMs() > 60.0)
		{
			Fail(FString::Printf(TEXT("sky_snow: a chunk of snow took %.1f ms to make"), W->GetSlowestRebuildMs()));
		}
		if (S->GetRainDrawn() != 0)
		{
			Fail(TEXT("sky_snow: rain is falling in the snow"));
		}
	};
	Phases.Add(MoveTemp(SnowPhase));

	// 5. The snow stays when the weather clears.
	FPhase Lying;
	Lying.Name = TEXT("sky_snow_lying");
	Lying.WarmupSeconds = 3.0f;
	Lying.MeasureSeconds = 3.0f;
	Lying.Setup = [=, this]()
	{
		Reset();
		SetClock(11.0f, SkySmokeMidwinter);
		SetWeather("Clear", 24.0f);
		// The snow stops falling over the weather's transition (an hour and a half) and then only melts a little.
		State->StepsWanted = 2 * colony::kSecondsPerHour;
		Aim(1.5f);
	};
	Lying.OnFrame = [=]() { Advance(); Watch(); };
	Lying.Verify = [=, this]()
	{
		USnowSubsystem* W = Snow();
		USkySubsystem* S = Sky();
		if (!W || !S)
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("sky_snow_lying: %lld snow cells still drawn in %d chunks (%lld a moment ago), %d flakes in the air"), W->GetCellsDrawn(), W->GetChunksWithSnow(), State->SnowCellsAtEnd, S->GetSnowDrawn()));
		if (W->GetCellsDrawn() < State->SnowCellsAtEnd / 4 || !W->IsUpToDate())
		{
			Fail(FString::Printf(TEXT("sky_snow_lying: the snow is gone or stale (%lld cells drawn, %lld before, up to date: %d)"), W->GetCellsDrawn(), State->SnowCellsAtEnd, W->IsUpToDate()));
		}
		if (S->GetSnowDrawn() != 0)
		{
			Fail(TEXT("sky_snow_lying: flakes are still falling after the weather cleared"));
		}
	};
	Phases.Add(MoveTemp(Lying));

	// 5b. The snow from the ground, in sunshine.
	FPhase Ground;
	Ground.Name = TEXT("sky_snow_ground");
	Ground.WarmupSeconds = 4.0f;
	Ground.MeasureSeconds = 2.0f;
	Ground.Setup = [=, this]()
	{
		Reset();
		SetClock(11.0f, SkySmokeMidwinter);
		const FVector Eye = Bridge()->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 200.0);
		ViewFrom(Eye, FRotator(-6.0f, 100.0f, 0.0f));
	};
	Phases.Add(MoveTemp(Ground));

	// 6. A thunderstorm flashes.
	FPhase Storm;
	Storm.Name = TEXT("sky_storm");
	Storm.WarmupSeconds = 24.0f;
	Storm.MeasureSeconds = 3.0f;
	Storm.Setup = [=, this]()
	{
		Reset();
		State->FlashesAtStart = Sky() ? Sky()->GetFlashes() : 0;
		SetClock(16.0f, SkySmokeMidsummer);
		SetWeather("RainyThunderstorm", 24.0f);
		State->StepsWanted = 2 * colony::kSecondsPerHour;
		const FVector Eye = Bridge()->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 260.0);
		ViewFrom(Eye, FRotator(10.0f, 100.0f, 0.0f));
	};
	Storm.OnFrame = [=]() { Advance(); Watch(); };
	Storm.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		const int32 Flashes = S->GetFlashes() - State->FlashesAtStart;
		Notes.Add(FString::Printf(TEXT("sky_storm: %d lightning flashes, sky %.2f overcast, sun %.2f lux, up to %d streaks, fog %.4f"), Flashes, S->GetState().Overcast, S->GetState().SunLux, State->MaxRainDrawn, State->MaxFogDensity));
		if (Flashes < 1)
		{
			Fail(TEXT("sky_storm: no lightning in the storm"));
		}
		// A clear noon's sun is data/look.json's clearNoonLux; a storm lets through well under a third of it.
		if (S->GetState().Overcast < 0.8f || S->GetState().SunLux > 0.3f * S->GetLook().SunClearNoonLux)
		{
			Fail(FString::Printf(TEXT("sky_storm: the storm sky is not dark (overcast %.2f, sun %.2f lux)"), S->GetState().Overcast, S->GetState().SunLux));
		}
		if (State->MaxRainDrawn < 3000)
		{
			Fail(FString::Printf(TEXT("sky_storm: only %d rain streaks in a storm"), State->MaxRainDrawn));
		}
	};
	Phases.Add(MoveTemp(Storm));

	// 7. Fog.
	FPhase Fog;
	Fog.Name = TEXT("sky_fog");
	Fog.WarmupSeconds = 14.0f;
	Fog.MeasureSeconds = 3.0f;
	Fog.Setup = [=, this]()
	{
		Reset();
		SetClock(10.0f, SkySmokeMidsummer);
		SetWeather("Fog", 24.0f);
		State->StepsWanted = 2 * colony::kSecondsPerHour;
		const FVector Eye = Bridge()->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 260.0);
		ViewFrom(Eye, FRotator(6.0f, 100.0f, 0.0f));
	};
	Fog.OnFrame = [=]() { Advance(); Watch(); };
	Fog.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("sky_fog: fog density %.4f"), State->MaxFogDensity));
		if (State->MaxFogDensity < 0.012f)
		{
			Fail(FString::Printf(TEXT("sky_fog: the fog only got to density %.4f"), State->MaxFogDensity));
		}
		if (State->MaxRainDrawn > 0 && S->GetRainDrawn() > 0)
		{
			Fail(TEXT("sky_fog: rain is falling in the fog"));
		}
	};
	Phases.Add(MoveTemp(Fog));

	// 8. From the ground: a clear morning's sky with a little cloud, then the same sky under rain.
	FPhase Morning;
	Morning.Name = TEXT("sky_horizon_clear");
	Morning.WarmupSeconds = 8.0f;
	Morning.MeasureSeconds = 3.0f;
	Morning.Setup = [=, this]()
	{
		Reset();
		SetClock(9.0f, SkySmokeMidsummer);
		SetWeather("Clear", 24.0f);
		State->StepsWanted = 2 * colony::kSecondsPerHour;
		const FVector Eye = Bridge()->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 260.0);
		ViewFrom(Eye, FRotator(14.0f, 100.0f, 0.0f));
	};
	Morning.OnFrame = [=]() { Advance(); Watch(); };
	Morning.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (S)
		{
			Notes.Add(FString::Printf(TEXT("sky_horizon_clear: sun %.1f degrees up at bearing %.0f, cloud cover %.2f, fog %.4f"), S->GetState().SunElevation, S->GetState().SunAzimuth, S->GetState().CloudCoverage, S->GetState().FogDensity));
		}
	};
	Phases.Add(MoveTemp(Morning));

	FPhase Overcast;
	Overcast.Name = TEXT("sky_horizon_rain");
	Overcast.WarmupSeconds = 14.0f;
	Overcast.MeasureSeconds = 3.0f;
	Overcast.Setup = [=, this]()
	{
		Reset();
		SetClock(14.0f, SkySmokeMidsummer);
		SetWeather("Rain", 24.0f);
		State->StepsWanted = 2 * colony::kSecondsPerHour;
		const FVector Eye = Bridge()->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 260.0);
		ViewFrom(Eye, FRotator(10.0f, 100.0f, 0.0f));
	};
	Overcast.OnFrame = [=]() { Advance(); Watch(); };
	Overcast.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (S)
		{
			Notes.Add(FString::Printf(TEXT("sky_horizon_rain: overcast %.2f, cloud cover %.2f, fog %.4f, %d streaks, ground wetness %.2f"), S->GetState().Overcast, S->GetState().CloudCoverage, S->GetState().FogDensity, S->GetRainDrawn(), State->MaxWet));
		}
	};
	Phases.Add(MoveTemp(Overcast));

	// 9. Dusk from the ground, looking at the sun: low in the west.
	FPhase Dusk;
	Dusk.Name = TEXT("sky_dusk");
	Dusk.WarmupSeconds = 5.0f;
	Dusk.MeasureSeconds = 3.0f;
	Dusk.Setup = [=, this]()
	{
		Reset();
		SetClock(17.7f, SkySmokeMidsummer);
		SetWeather("Clear", 24.0f);
		State->StepsWanted = 2 * colony::kSecondsPerHour;
		USimSubsystem* B = Bridge();
		const FVector Eye = B->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 180.0);
		// Yaw 270 looks west (yaw 0 is north, 90 east): the sun's bearing at sunset.
		ViewFrom(Eye, FRotator(8.0f, 270.0f, 0.0f));
	};
	Dusk.OnFrame = [=]() { Advance(); Watch(); };
	Dusk.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		const FSkyState& K = S->GetState();
		Notes.Add(FString::Printf(TEXT("sky_dusk: sun %.1f degrees up at bearing %.0f, %.2f lux"), K.SunElevation, K.SunAzimuth, K.SunLux));
		if (K.SunElevation < -1.0f || K.SunElevation > 12.0f || K.SunAzimuth < 250.0f || K.SunAzimuth > 275.0f)
		{
			Fail(FString::Printf(TEXT("sky_dusk: the sun at 17:42 should be low in the west (got %.1f degrees up, bearing %.0f)"), K.SunElevation, K.SunAzimuth));
		}
	};
	Phases.Add(MoveTemp(Dusk));

	// 10. Night over the colony, then the moon.
	FPhase NightGround;
	NightGround.Name = TEXT("sky_night_ground");
	NightGround.WarmupSeconds = 4.0f;
	NightGround.MeasureSeconds = 2.0f;
	NightGround.Setup = [=, this]()
	{
		Reset();
		SetClock(0.5f, SkySmokeMidsummer);
		SetWeather("Clear", 24.0f);
		const FVector Eye = Bridge()->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(-1800.0, 0.0, 1800.0);
		ViewFrom(Eye, FRotator(-45.0f, 0.0f, 0.0f));
	};
	Phases.Add(MoveTemp(NightGround));

	FPhase Night;
	Night.Name = TEXT("sky_night");
	Night.WarmupSeconds = 5.0f;
	Night.MeasureSeconds = 3.0f;
	Night.Setup = [=, this]()
	{
		Reset();
		SetClock(0.5f, SkySmokeMidsummer);
		USimSubsystem* B = Bridge();
		const FVector Eye = B->SimToWorld(State->Home.x + 0.5f, State->Home.y + 0.5f, 0) + FVector(0.0, 0.0, 180.0);
		// The moon is across the sky from the sun: south of high, at about 47 degrees.
		ViewFrom(Eye, FRotator(35.0f, 187.0f, 0.0f));
	};
	Night.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		if (!S)
		{
			return;
		}
		const FSkyState& K = S->GetState();
		Notes.Add(FString::Printf(TEXT("sky_night: sun %.1f degrees (%.2f lux), moon %.1f degrees up (%.2f lux, %.0f%% full), sky light %.2f"), K.SunElevation, K.SunLux, K.MoonElevation, K.MoonLux, K.MoonFullness * 100.0f, K.SkyLightIntensity));
		if (K.SunElevation > -10.0f || K.SunLux > 0.01f)
		{
			Fail(FString::Printf(TEXT("sky_night: the sun is %.1f degrees up and gives %.2f lux at half past midnight"), K.SunElevation, K.SunLux));
		}
		if (K.MoonElevation < 20.0f || K.MoonLux < 0.1f || !S->GetMoon()->IsVisible())
		{
			Fail(FString::Printf(TEXT("sky_night: no moon (%.1f degrees up, %.3f lux)"), K.MoonElevation, K.MoonLux));
		}
		// Since L1 the sky light is the atmosphere's own (lit by the moon at night); what makes the night dark is the exposure,
		// which follows the light only part of the way: the night must be held at least 8 stops brighter than noon (a lower EV)
		// but stay dark on screen.
		const float NightEV = K.ExposureEV;
		const float NoonEV = S->ComputeState(*SimOf(), 12.0f, SkySmokeMidsummer).ExposureEV;
		Notes.Add(FString::Printf(TEXT("sky_night: exposure EV %.2f at night, %.2f at a clear noon; %.2f lux on the ground"), NightEV, NoonEV, K.GroundLux));
		if (NoonEV - NightEV < 8.0f)
		{
			Fail(FString::Printf(TEXT("sky_night: the exposure is EV %.2f at night and %.2f at noon"), NightEV, NoonEV));
		}
		if (State->MaxRainDrawn > 0 && S->GetRainDrawn() > 0)
		{
			Fail(TEXT("sky_night: rain is falling on a clear night"));
		}
	};
	Phases.Add(MoveTemp(Night));

	// 10. Put everything back to follow the sim and check the level's own sky comes back with Sky.Enable 0.
	FPhase Restore;
	Restore.Name = TEXT("sky_restore");
	Restore.WarmupSeconds = 2.0f;
	Restore.MeasureSeconds = 1.0f;
	Restore.Setup = [=, this]()
	{
		SkySmokeSet(TEXT("Sky.Hour"), -1.0f);
		SkySmokeSet(TEXT("Sky.DayOfYear"), -1.0f);
		SkySmokeSet(TEXT("Sky.Enable"), 0.0f);
	};
	Restore.Verify = [=, this]()
	{
		USkySubsystem* S = Sky();
		const bool bGone = S && !S->IsSceneBuilt();
		SkySmokeSet(TEXT("Sky.Enable"), 1.0f);
		SkySmokeSet(TEXT("Terrain.Roofs"), -1.0f);
		if (!bGone)
		{
			Fail(TEXT("sky_restore: Sky.Enable 0 did not take the sim's sky down"));
		}
	};
	Phases.Add(MoveTemp(Restore));
}
