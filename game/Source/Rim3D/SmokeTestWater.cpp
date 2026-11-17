// Smoke test `water` (docs/design/water.md, W1): the water surface on a seed-42 512 m rainforest map, seen low from a river
// bank and from a lake shore, with the old flat water colour (Water.Enable 0) for comparison and the frame cost of both.
#include "SmokeTest.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "SimWaterSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"

#include <cmath>

namespace
{
	const FVector WaterSmokeOrigin(-1300000.0, 0.0, 0.0);

	void WaterSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}

	bool WaterSmokeOpen(const colony::Map& Map, int32 X, int32 Y)
	{
		const colony::CellPos C{X, Y, 0};
		if (!Map.inBounds(C))
		{
			return false;
		}
		const colony::Terrain T = Map.terrain(C);
		return (T == colony::Terrain::Water || T == colony::Terrain::ShallowWater) && !std::isnan(Map.waterSurface(C));
	}

	float WaterSmokeSpeed(const colony::Map& Map, int32 X, int32 Y)
	{
		float Vx = 0.0f, Vy = 0.0f;
		Map.flowAt(colony::CellPos{X, Y, 0}, Vx, Vy);
		return std::sqrt(Vx * Vx + Vy * Vy);
	}

	/** A shot: where the camera stands (metres, on dry ground) and the water point it looks at. */
	struct FWaterSmokeShot
	{
		bool bFound = false;
		FVector2D Eye = FVector2D::ZeroVector;
		FVector2D Look = FVector2D::ZeroVector;
	};

	/** Walks from a water cell along a direction until dry ground; the camera stands two cells onto it. */
	bool WaterSmokeWalkToBank(const colony::Map& Map, FVector2D From, FVector2D Dir, FVector2D& OutEye)
	{
		Dir.Normalize();
		for (int32 Step = 1; Step < 60; ++Step)
		{
			const FVector2D P = From + Dir * Step;
			if (!Map.inBounds(colony::CellPos{FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y), 0}))
			{
				return false;
			}
			if (!WaterSmokeOpen(Map, FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y)))
			{
				const FVector2D Eye = P + Dir * 5.0;
				const colony::CellPos E{FMath::FloorToInt32(Eye.X), FMath::FloorToInt32(Eye.Y), 0};
				if (!Map.inBounds(E) || Map.terrain(E) == colony::Terrain::Rock)
				{
					return false;
				}
				OutEye = Eye;
				return true;
			}
		}
		return false;
	}

	/** How many open water cells lie along a line from a point (the river's width across its flow). */
	int32 WaterSmokeWidth(const colony::Map& Map, FVector2D From, FVector2D Dir)
	{
		int32 Count = 0;
		for (int32 Sign = -1; Sign <= 1; Sign += 2)
		{
			for (int32 Step = Sign < 0 ? 1 : 0; Step < 40; ++Step)
			{
				const FVector2D P = From + Dir * (Step * Sign);
				if (!WaterSmokeOpen(Map, FMath::FloorToInt32(P.X), FMath::FloorToInt32(P.Y)))
				{
					break;
				}
				++Count;
			}
		}
		return Count;
	}

	/** The river shot: fast mid-channel water on a wide reach with low banks both sides (no gorge), the camera on a bank. */
	FWaterSmokeShot WaterSmokeFindRiver(const colony::Map& Map)
	{
		FWaterSmokeShot Shot;
		double Best = -1.0e30;
		for (int32 Y = 12; Y < Map.height() - 12; ++Y)
		{
			for (int32 X = 12; X < Map.width() - 12; ++X)
			{
				if (!WaterSmokeOpen(Map, X, Y) || WaterSmokeSpeed(Map, X, Y) < 0.8f)
				{
					continue;
				}
				float Vx = 0.0f, Vy = 0.0f;
				Map.flowAt(colony::CellPos{X, Y, 0}, Vx, Vy);
				const FVector2D Water(X + 0.5, Y + 0.5);
				const FVector2D Across = FVector2D(-Vy, Vx).GetSafeNormal();
				const float Surface = Map.waterSurface(colony::CellPos{X, Y, 0});
				for (int32 Side = -1; Side <= 1; Side += 2)
				{
					FVector2D Eye;
					if (!WaterSmokeWalkToBank(Map, Water, Across * Side, Eye))
					{
						continue;
					}
					// The ground at the eye and 12 m past the far bank, over the surface: low both sides.
					const FVector2D Far = Water - Across * Side * (WaterSmokeWidth(Map, Water, Across) + 12.0);
					const colony::CellPos E{FMath::FloorToInt32(Eye.X), FMath::FloorToInt32(Eye.Y), 0};
					const colony::CellPos F{FMath::FloorToInt32(Far.X), FMath::FloorToInt32(Far.Y), 0};
					if (!Map.inBounds(F) || Map.terrain(F) == colony::Terrain::Rock)
					{
						continue;
					}
					const double Rise = FMath::Max(Map.groundHeight(E) - Surface, Map.groundHeight(F) - Surface);
					const double Score = WaterSmokeWidth(Map, Water, Across) - 3.0 * FMath::Max(0.0, Rise - 0.5);
					if (Score > Best)
					{
						Best = Score;
						Shot.bFound = true;
						Shot.Eye = Eye;
						// Look across and a little downstream.
						Shot.Look = Water + FVector2D(Vx, Vy).GetSafeNormal() * 8.0;
					}
				}
			}
		}
		return Shot;
	}

	/** The lake shot: still water with still water all round (3 cells), nearest the middle, the camera on its shore. */
	FWaterSmokeShot WaterSmokeFindLake(const colony::Map& Map)
	{
		FWaterSmokeShot Shot;
		const FVector2D Mid(Map.width() * 0.5, Map.height() * 0.5);
		double Best = 1.0e30;
		for (int32 Y = 10; Y < Map.height() - 10; Y += 2)
		{
			for (int32 X = 10; X < Map.width() - 10; X += 2)
			{
				if (!WaterSmokeOpen(Map, X, Y) || WaterSmokeSpeed(Map, X, Y) > 0.01f)
				{
					continue;
				}
				const double D = FVector2D::DistSquared(FVector2D(X, Y), Mid);
				if (D >= Best)
				{
					continue;
				}
				bool bDeep = true;
				for (int32 DY = -3; DY <= 3 && bDeep; DY += 3)
				{
					for (int32 DX = -3; DX <= 3 && bDeep; DX += 3)
					{
						bDeep = WaterSmokeOpen(Map, X + DX, Y + DY) && WaterSmokeSpeed(Map, X + DX, Y + DY) < 0.01f;
					}
				}
				FVector2D Eye;
				if (bDeep && WaterSmokeWalkToBank(Map, FVector2D(X + 0.5, Y + 0.5), FVector2D(1.0, 0.3), Eye))
				{
					Best = D;
					Shot.bFound = true;
					Shot.Eye = Eye;
					Shot.Look = FVector2D(X + 0.5, Y + 0.5);
				}
			}
		}
		return Shot;
	}

	struct FWaterSmokeState
	{
		FWaterSmokeShot River;
		FWaterSmokeShot Lake;
	};
}

void USmokeTestSubsystem::BuildWaterPhases()
{
	const TSharedRef<FWaterSmokeState> State = MakeShared<FWaterSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Water = [this]() { return GetWorld()->GetSubsystem<USimWaterSubsystem>(); };

	// A camera 1.7 m over the ground at the eye, looking at the water's surface at the look point.
	const auto Frame = [=, this](const FWaterSmokeShot& Shot)
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim() || !Shot.bFound)
		{
			return;
		}
		const colony::Map& Map = B->GetSim()->map();
		const FVector Eye = B->SimToWorld(static_cast<float>(Shot.Eye.X), static_cast<float>(Shot.Eye.Y), 0) + FVector(0.0, 0.0, 250.0);
		// Looking past the water point at the surface's height, so the camera looks along the water and not down into it.
		const FVector2D Past = Shot.Look + (Shot.Look - Shot.Eye).GetSafeNormal() * 15.0;
		FVector Look = B->SimToWorld(static_cast<float>(Past.X), static_cast<float>(Past.Y), 0);
		const float Surface = Map.waterSurface(colony::CellPos{FMath::FloorToInt32(Shot.Look.X), FMath::FloorToInt32(Shot.Look.Y), 0});
		if (!std::isnan(Surface))
		{
			Look.Z = WaterSmokeOrigin.Z + Surface * 100.0;
		}
		ViewFrom(Eye, (Look - Eye).Rotation());
	};
	// Plants between the camera and the water (and right round the camera) are taken out, so the shot sees the water.
	const auto ClearView = [=, this](const FWaterSmokeShot& Shot)
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr;
		if (!Sim || !Shot.bFound)
		{
			return;
		}
		const FVector2D Dir = (Shot.Look - Shot.Eye).GetSafeNormal();
		const double Length = FVector2D::Distance(Shot.Look, Shot.Eye);
		for (double T = -6.0; T <= Length + 4.0; T += 0.5)
		{
			const FVector2D P = Shot.Eye + Dir * T;
			for (int32 DY = -5; DY <= 5; ++DY)
			{
				for (int32 DX = -5; DX <= 5; ++DX)
				{
					const colony::CellPos C{FMath::FloorToInt32(P.X) + DX, FMath::FloorToInt32(P.Y) + DY, 0};
					if (Sim->map().inBounds(C) && Sim->plants().has(C))
					{
						Sim->plants().remove(C);
					}
				}
			}
		}
	};
	const auto NoteStats = [=, this](const TCHAR* Shot)
	{
		if (USimWaterSubsystem* W = Water())
		{
			const FWaterStats& S = W->GetStats();
			Notes.Add(FString::Printf(TEXT("%s: water %d chunks, %lld triangles, %lld vertices, full build %.1f ms, slowest chunk %.2f ms"), Shot, S.Chunks, S.Triangles, S.Vertices,
				S.FullBuildMs, S.SlowestRebuildMs));
		}
	};

	FPhase Prepare;
	Prepare.Name = TEXT("_prepare");
	Prepare.WarmupSeconds = 8.0f;
	Prepare.MeasureSeconds = 0.5f;
	Prepare.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
		if (!B || !Water() || !Overseer)
		{
			Fail(TEXT("USimSubsystem, USimWaterSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		if (!USimWaterSubsystem::WantsSurface())
		{
			Fail(TEXT("water: M_Water missing (run the WaterArt commandlet) or Water.Enable is 0"));
		}
		GEngine->Exec(GetWorld(), TEXT("r.SetRes 1920x1080w"));
		WaterSmokeSet(TEXT("Water.Enable"), 1.0f);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 512;
		Options.Scenario = TEXT("CrashlandedRainforest");
		Options.bWorld = true;
		B->SetWorldOrigin(WaterSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		Overseer->EnterOverseerMode();
		B->Submit(colony::SetWeatherCommand{"Clear", 96.0f});
		B->StepOnce();
		WaterSmokeSet(TEXT("Sky.Hour"), 10.5f);
		const colony::Map& Map = B->GetSim()->map();
		State->River = WaterSmokeFindRiver(Map);
		State->Lake = WaterSmokeFindLake(Map);
		if (!State->River.bFound)
		{
			Fail(TEXT("water: no river bank found on the seed-42 map"));
		}
		else
		{
			Notes.Add(FString::Printf(TEXT("river: camera at (%.1f, %.1f) looking at (%.1f, %.1f)"), State->River.Eye.X, State->River.Eye.Y, State->River.Look.X, State->River.Look.Y));
			ClearView(State->River);
		}
		if (State->Lake.bFound)
		{
			Notes.Add(FString::Printf(TEXT("lake: camera at (%.1f, %.1f) looking at (%.1f, %.1f)"), State->Lake.Eye.X, State->Lake.Eye.Y, State->Lake.Look.X, State->Lake.Look.Y));
			ClearView(State->Lake);
		}
		else
		{
			int32 Still = 0, Open = 0;
			for (int32 Y = 0; Y < Map.height(); ++Y)
			{
				for (int32 X = 0; X < Map.width(); ++X)
				{
					Open += WaterSmokeOpen(Map, X, Y) ? 1 : 0;
					Still += WaterSmokeOpen(Map, X, Y) && WaterSmokeSpeed(Map, X, Y) < 0.01f ? 1 : 0;
				}
			}
			Notes.Add(FString::Printf(TEXT("lake: no still lake on this map (%d open water cells, %d of them still); the lake shots show the river"), Open, Still));
		}
		B->StepOnce();
	};
	Phases.Add(Prepare);

	// The old look first, for comparison and the cost without the surface.
	FPhase Before;
	Before.Name = TEXT("river_before");
	Before.WarmupSeconds = 4.0f;
	Before.MeasureSeconds = 5.0f;
	Before.Setup = [=]()
	{
		WaterSmokeSet(TEXT("Water.Enable"), 0.0f);
		Frame(State->River);
	};
	Before.Verify = [=, this]()
	{
		if (Water() && Water()->IsBuilt())
		{
			Fail(TEXT("water: the surface is still drawn with Water.Enable 0"));
		}
	};
	Phases.Add(Before);

	FPhase River;
	River.Name = TEXT("river");
	River.WarmupSeconds = 4.0f;
	River.MeasureSeconds = 5.0f;
	River.Setup = [=]()
	{
		WaterSmokeSet(TEXT("Water.Enable"), 1.0f);
		Frame(State->River);
	};
	River.Verify = [=, this]()
	{
		USimWaterSubsystem* W = Water();
		if (!W || !W->IsBuilt() || W->GetStats().Chunks <= 0 || W->GetStats().Triangles <= 0)
		{
			Fail(TEXT("water: the subsystem built no water chunks"));
		}
		NoteStats(TEXT("river"));
	};
	Phases.Add(River);

	{
		FPhase Lake;
		Lake.Name = TEXT("lake");
		Lake.WarmupSeconds = 3.0f;
		Lake.MeasureSeconds = 3.0f;
		Lake.Setup = [=]() { Frame(State->Lake.bFound ? State->Lake : State->River); };
		Phases.Add(Lake);

		FPhase LakeBefore;
		LakeBefore.Name = TEXT("lake_before");
		LakeBefore.WarmupSeconds = 4.0f;
		LakeBefore.MeasureSeconds = 2.0f;
		LakeBefore.Setup = [=]()
		{
			WaterSmokeSet(TEXT("Water.Enable"), 0.0f);
			Frame(State->Lake.bFound ? State->Lake : State->River);
		};
		Phases.Add(LakeBefore);

		// From above at the river, the surface's flow and shore seen whole.
		FPhase Above;
		Above.Name = TEXT("river_above");
		Above.WarmupSeconds = 4.0f;
		Above.MeasureSeconds = 2.0f;
		Above.Setup = [=, this]()
		{
			WaterSmokeSet(TEXT("Water.Enable"), 1.0f);
			USimSubsystem* B = Bridge();
			if (!B || !State->River.bFound)
			{
				return;
			}
			const FVector Look = B->SimToWorld(static_cast<float>(State->River.Look.X), static_cast<float>(State->River.Look.Y), 0);
			const FVector Eye = B->SimToWorld(static_cast<float>(State->River.Eye.X), static_cast<float>(State->River.Eye.Y), 0) + FVector(0.0, 0.0, 1000.0);
			ViewFrom(Eye, (Look - Eye).Rotation());
		};
		Phases.Add(Above);
	}
}
