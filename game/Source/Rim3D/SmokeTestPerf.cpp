// Smoke test `perf` (task G3, docs/design/perf-check.md): the performance colony of `simrun --perf` drawn in the game
// on a 1 km map, 30 colonists and about 580 animals at 3x, from the overseer camera; then rain, a grass fire and a raid of
// about 40 pirates against the armed colonists. Every phase records frame, game thread, GPU and sim cost, the worst
// frames and the frames that ran the hourly plant pass. Meant for a packaged build (tools/smoke.ps1 -Packaged).
#include "SmokeTest.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UnrealClient.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fire.h"
#include "ColonySim/Map.h"
#include "ColonySim/PerfColony.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{
	const FVector PerfSmokeOrigin(-2000000.0, 0.0, 0.0);
	constexpr int64 PerfSmokeStartHour = 7;     // the colony's day starts: colonists awake and working
	constexpr float PerfSmokeRaidPoints = 2700.0f;  // about 40 pirates (67 points each)
	constexpr float PerfSmokeSlowFrameMs = 1000.0f / 30.0f;

	struct FPerfSmokeState
	{
		colony::CellPos Home = colony::CellPos{512, 512, 0};
		colony::CellPos FireAt = colony::CellPos{512, 512, 0};
		int64 StepsWanted = 0;
		int64 LastFedHour = -1;
		int64 LastHour = -1;
		bool bHourRan = false;  // the last frame ran an hourly step, so this frame's delta is that frame's length
		bool bProfiling = false;
		colony::StepProfile Profile;
		std::vector<double> ProfileAtStart;
		int64 StepsAtStart = 0;
		TArray<float> HourFrameMs;
		double MaxStepMs = 0.0;
		double SimFrameMsSum = 0.0;
		int32 SimFrameCount = 0;
		int32 MaxFireCells = 0;
		int32 Raiders = 0;
		int32 Shots = 0;
		int32 Downed = 0;
		int32 Died = 0;
		bool bListening = false;
	};

	void PerfSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}

	double PerfSmokePercentile(TArray<float> Values, double P)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		return Values[FMath::Clamp(static_cast<int32>(P * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}
}

void USmokeTestSubsystem::BuildPerfPhases()
{
	const TSharedRef<FPerfSmokeState> State = MakeShared<FPerfSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	// The overseer camera on a cell, `Notches` zoomed in from its usual distance (negative: out).
	const auto Aim = [=, this](colony::CellPos At, float Notches)
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (Pawn)
		{
			Pawn->ResetView();
			Pawn->Zoom(Notches);
			Pawn->FocusOn(Bridge()->SimToWorld(At.x + 0.5f, At.y + 0.5f, 0));
			Pawn->SnapCamera();
		}
	};
	// The tame animals eat once a game hour, as in simrun --perf.
	const auto Feed = [=, this]()
	{
		colony::Sim* Sim = SimOf();
		if (Sim && Sim->time().seconds / colony::kSecondsPerHour != State->LastFedHour)
		{
			State->LastFedHour = Sim->time().seconds / colony::kSecondsPerHour;
			colony::feedPerfColonyAnimals(*Sim);
		}
	};
	// Every frame of a measured phase: the hourly frames, the bridge's sim cost, the fire's size.
	const auto Watch = [=, this]()
	{
		USimSubsystem* B = Bridge();
		const colony::Sim* Sim = B ? B->GetSim() : nullptr;
		if (!Sim)
		{
			return;
		}
		if (State->bHourRan)
		{
			State->HourFrameMs.Add(FApp::GetDeltaTime() * 1000.0f);
			State->bHourRan = false;
		}
		const int64 Hour = Sim->time().seconds / colony::kSecondsPerHour;
		if (State->LastHour >= 0 && Hour != State->LastHour)
		{
			State->bHourRan = true;
		}
		State->LastHour = Hour;
		Feed();
		const FSimTimings& T = B->GetTimings();
		State->MaxStepMs = FMath::Max(State->MaxStepMs, T.MaxStepMs);
		State->SimFrameMsSum += T.AvgFrameMs;
		++State->SimFrameCount;
		State->MaxFireCells = FMath::Max(State->MaxFireCells, static_cast<int32>(Sim->fire().count()));
	};
	const auto BeginMeasure = [=]()
	{
		State->HourFrameMs.Reset();
		State->MaxStepMs = 0.0;
		State->SimFrameMsSum = 0.0;
		State->SimFrameCount = 0;
		State->MaxFireCells = 0;
		State->ProfileAtStart = State->Profile.ms;
		State->StepsAtStart = State->Profile.steps;
	};
	// What every measured phase reports, on top of the harness's fps and thread times.
	const auto Report = [=, this](const TCHAR* Phase)
	{
		TArray<float> Frames;
		int32 Slow = 0;
		for (const FFrameSample& S : Samples)
		{
			Frames.Add(S.FrameMs);
			Slow += S.FrameMs > PerfSmokeSlowFrameMs ? 1 : 0;
		}
		PhaseNumbers.Add(TEXT("p99FrameMs"), PerfSmokePercentile(Frames, 0.99));
		PhaseNumbers.Add(TEXT("maxFrameMs"), PerfSmokePercentile(Frames, 1.0));
		PhaseNumbers.Add(TEXT("framesOver33Ms"), Slow);
		const int64 Steps = State->Profile.steps - State->StepsAtStart;
		std::vector<std::pair<double, const char*>> Systems;
		double StepTotal = 0.0;
		for (size_t I = 0; I < State->Profile.names.size(); ++I)
		{
			const double Spent = State->Profile.ms[I] - (I < State->ProfileAtStart.size() ? State->ProfileAtStart[I] : 0.0);
			if (std::string(State->Profile.names[I]) == "total")
			{
				StepTotal = Spent;
			}
			else
			{
				Systems.emplace_back(Spent, State->Profile.names[I]);
			}
		}
		std::sort(Systems.begin(), Systems.end(), [](const auto& A, const auto& Z) { return A.first > Z.first; });
		const double AvgStep = Steps > 0 ? StepTotal / static_cast<double>(Steps) : 0.0;
		PhaseNumbers.Add(TEXT("simSteps"), static_cast<double>(Steps));
		PhaseNumbers.Add(TEXT("simAvgStepMs"), AvgStep);
		PhaseNumbers.Add(TEXT("simMaxStepMs"), State->MaxStepMs);
		PhaseNumbers.Add(TEXT("simMsPerFrame"), State->SimFrameCount > 0 ? State->SimFrameMsSum / State->SimFrameCount : 0.0);
		FString Top;
		for (size_t I = 0; I < Systems.size() && I < 4; ++I)
		{
			Top += FString::Printf(TEXT("%s%s %.3f"), I == 0 ? TEXT("") : TEXT(", "), UTF8_TO_TCHAR(Systems[I].second),
				Steps > 0 ? Systems[I].first / static_cast<double>(Steps) : 0.0);
		}
		FString Hourly;
		for (const float Ms : State->HourFrameMs)
		{
			Hourly += FString::Printf(TEXT(" %.1f"), Ms);
		}
		if (!State->HourFrameMs.IsEmpty())
		{
			PhaseNumbers.Add(TEXT("hourlyFrameMs"), PerfSmokePercentile(State->HourFrameMs, 1.0));
		}
		Notes.Add(FString::Printf(TEXT("%s: %lld sim steps, %.3f ms a step (max %.1f), sim %.2f ms a frame; dearest systems (ms/step): %s; frames over 33 ms: %d, max frame %.1f ms; hourly-pass frames (ms):%s"),
			Phase, Steps, AvgStep, State->MaxStepMs, State->SimFrameCount > 0 ? State->SimFrameMsSum / State->SimFrameCount : 0.0, *Top, Slow,
			PerfSmokePercentile(Frames, 1.0), Hourly.IsEmpty() ? TEXT(" none") : *Hourly));
	};
	const auto AddMeasured = [=, this](const TCHAR* Name, float Warmup, float Measure, TFunction<void()> Setup, TFunction<void()> Verify = nullptr)
	{
		FPhase Phase;
		Phase.Name = Name;
		Phase.WarmupSeconds = Warmup;
		Phase.MeasureSeconds = Measure;
		Phase.bScreenshotInWarmup = true;
		Phase.Setup = MoveTemp(Setup);
		const TSharedRef<bool> bStarted = MakeShared<bool>(false);
		Phase.OnFrame = [=, this]()
		{
			if (!*bStarted && PhaseTime >= Phases[PhaseIndex].WarmupSeconds)
			{
				*bStarted = true;
				BeginMeasure();
			}
			Watch();
		};
		const FString PhaseName = Name;
		Phase.Verify = [=]()
		{
			Report(*PhaseName);
			if (Verify)
			{
				Verify();
			}
		};
		Phases.Add(MoveTemp(Phase));
	};

	// 0. The colony on a 1 km map, run ahead (paused, as fast as it goes) to the morning of its first day.
	FPhase Setup;
	Setup.Name = TEXT("perf_setup");
	Setup.WarmupSeconds = 75.0f;
	Setup.MeasureSeconds = 1.0f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Overseer())
		{
			Fail(TEXT("USimSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		// Offscreen draws at a small size unless asked for more; try for the size a player would have.
		if (GEngine)
		{
			GEngine->Exec(GetWorld(), TEXT("r.SetRes 2560x1440w"));
		}
		PerfSmokeSet(TEXT("Terrain.Roofs"), 0.0f);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 1024;
		Options.Scenario = TEXT("PerfColony");
		Options.WildAnimals = 100;
		B->SetWorldOrigin(PerfSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		colony::PerfColonyCounts Counts;
		const std::string Error = colony::setUpPerfColony(*SimOf(), Counts);
		if (!Error.empty())
		{
			Fail(FString::Printf(TEXT("perf_setup: %s"), UTF8_TO_TCHAR(Error.c_str())));
			return;
		}
		State->Home = *B->GetSim()->stockpileCell();
		State->StepsWanted = PerfSmokeStartHour * colony::kSecondsPerHour;
		Notes.Add(FString::Printf(TEXT("perf_setup: %d colonists, %d tame and %d wild animals"), Counts.colonists, Counts.tameAnimals, Counts.wildAnimals));
		if (!Overseer()->EnterOverseerMode())
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		Aim(State->Home, 0.0f);
		if (!State->bListening)
		{
			State->bListening = true;
			B->OnSimEvent.AddLambda([State](const colony::Event& E)
			{
				switch (E.type)
				{
				case colony::EventType::RaidArrived: State->Raiders += E.count; break;
				case colony::EventType::ShotFired: ++State->Shots; break;
				case colony::EventType::PawnDowned: ++State->Downed; break;
				case colony::EventType::PawnDied: ++State->Died; break;
				default: break;
				}
			});
		}
	};
	Setup.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const double Start = FPlatformTime::Seconds();
		int32 Count = 0;
		while (B->GetSim()->time().seconds < State->StepsWanted && (FPlatformTime::Seconds() - Start) < 0.1 && Count < 3000)
		{
			Feed();
			B->StepOnce();
			++Count;
		}
	};
	Setup.Verify = [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		if (!Sim)
		{
			return;
		}
		FIntPoint Size(0, 0);
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
		{
			Size = GEngine->GameViewport->Viewport->GetSizeXY();
		}
		Notes.Add(FString::Printf(TEXT("perf_setup: the sim reached %02lld:%02lld of day 1; drawing at %d x %d"),
			static_cast<long long>(Sim->time().seconds / colony::kSecondsPerHour), static_cast<long long>((Sim->time().seconds / 60) % 60), Size.X, Size.Y));
		if (Sim->time().seconds < State->StepsWanted)
		{
			Fail(FString::Printf(TEXT("perf_setup: the sim only reached %lld of %lld steps in the warm-up"), static_cast<long long>(Sim->time().seconds),
				static_cast<long long>(State->StepsWanted)));
		}
		// From here on every step is timed by system.
		const_cast<colony::Sim*>(Sim)->setProfile(&State->Profile);
		State->bProfiling = true;
	};
	Phases.Add(MoveTemp(Setup));

	// 1. The colony at work at 3x from the overseer's usual view.
	AddMeasured(TEXT("perf_colony"), 6.0f, 20.0f, [=, this]()
	{
		Bridge()->Submit(colony::SetWeatherCommand{"Clear", 24.0f});  // the same sky every run, and dry ground for the fire
		Bridge()->SetSpeed(3);
		Aim(State->Home, 0.0f);
		if (GEngine)
		{
			GEngine->Exec(GetWorld(), TEXT("csvprofile start"));
		}
	});

	// 2. Close in on the colonists (the nearest get full animation and MetaHumans).
	AddMeasured(TEXT("perf_colony_close"), 3.0f, 15.0f, [=, this]() { Aim(State->Home, 5.0f); });

	// 3. Zoomed right out: most of the map, its plants and animals in view.
	AddMeasured(TEXT("perf_overview"), 4.0f, 15.0f, [=, this]() { Aim(State->Home, -12.0f); });

	// 4. A grass fire 15 to 30 m from the landing that the colonists go out to beat.
	AddMeasured(TEXT("perf_fire"), 5.0f, 20.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const colony::Sim* Sim = B->GetSim();
		int32 Lit = 0;
		for (int32 R = 15; R <= 30 && Lit < 6; ++R)
		{
			for (int32 D = -R; D <= R && Lit < 6; D += 3)
			{
				const colony::CellPos Cell{State->Home.x + D, State->Home.y - R, 0};
				if (Sim->map().inBounds(Cell) && Sim->plants().has(Cell))
				{
					B->Submit(colony::IgniteCommand{Cell, 0.0f, colony::kNoEntity, false});
					State->FireAt = Cell;
					++Lit;
				}
			}
		}
		if (Lit == 0)
		{
			Fail(TEXT("perf_fire: no plants south of the landing to light"));
		}
		Aim(State->FireAt, 1.0f);
	}, [=, this]()
	{
		if (State->MaxFireCells < 3)
		{
			Fail(FString::Printf(TEXT("perf_fire: the fire covered at most %d cells"), State->MaxFireCells));
		}
		Notes.Add(FString::Printf(TEXT("perf_fire: up to %d burning cells"), State->MaxFireCells));
	});

	// 5. Rain over the colony and what is left of the fire.
	AddMeasured(TEXT("perf_rain"), 5.0f, 20.0f, [=, this]()
	{
		Bridge()->Submit(colony::SetWeatherCommand{"Rain", 6.0f});
		Aim(State->FireAt, -1.0f);
	});

	// 6. About 40 pirates walk in at a map edge; the colonists have rifles.
	AddMeasured(TEXT("perf_raid_approach"), 2.0f, 15.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const int32 Armed = colony::armPerfColonists(*SimOf());
		Notes.Add(FString::Printf(TEXT("perf_raid: %d colonists armed"), Armed));
		B->Submit(colony::StartStrategyRaidCommand{"Pirate", PerfSmokeRaidPoints, -1, "Assault", -1.0f});
		Aim(State->Home, -2.0f);
	}, [=, this]()
	{
		Notes.Add(FString::Printf(TEXT("perf_raid_approach: %d raiders arrived"), State->Raiders));
		if (State->Raiders < 30)
		{
			Fail(FString::Printf(TEXT("perf_raid_approach: %d raiders, wanted about 40"), State->Raiders));
		}
	});

	// The rest of the walk, paused and as fast as it goes, until someone fires.
	FPhase Walk;
	Walk.Name = TEXT("perf_raid_walk");
	Walk.WarmupSeconds = 60.0f;
	Walk.MeasureSeconds = 0.5f;
	Walk.Setup = [=, this]()
	{
		Bridge()->SetSpeed(0);
		State->StepsWanted = Bridge()->GetSim()->time().seconds + 4 * colony::kSecondsPerHour;
	};
	Walk.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		const double Start = FPlatformTime::Seconds();
		int32 Count = 0;
		while (State->Shots == 0 && B->GetSim()->time().seconds < State->StepsWanted && (FPlatformTime::Seconds() - Start) < 0.1 && Count < 3000)
		{
			Feed();
			B->StepOnce();
			++Count;
		}
	};
	Walk.Verify = [=, this]()
	{
		const colony::Sim* Sim = Bridge()->GetSim();
		Notes.Add(FString::Printf(TEXT("perf_raid_walk: the first shot at %02lld:%02lld"), static_cast<long long>(Sim->time().seconds / colony::kSecondsPerHour),
			static_cast<long long>((Sim->time().seconds / 60) % 60)));
		if (State->Shots == 0)
		{
			Fail(TEXT("perf_raid_walk: nobody fired in four game hours"));
		}
	};
	Phases.Add(MoveTemp(Walk));

	// 7. The fight at the colony.
	AddMeasured(TEXT("perf_raid_fight"), 2.0f, 30.0f, [=, this]()
	{
		Bridge()->SetSpeed(3);
		Aim(State->Home, -1.0f);
	}, [=, this]()
	{
		if (GEngine)
		{
			GEngine->Exec(GetWorld(), TEXT("csvprofile stop"));
		}
		Notes.Add(FString::Printf(TEXT("perf_raid: %d shots, %d pawns downed, %d died over the raid"), State->Shots, State->Downed, State->Died));
		if (State->Shots < 50)
		{
			Fail(FString::Printf(TEXT("perf_raid_fight: only %d shots, so there was no real fight"), State->Shots));
		}
		if (colony::Sim* Sim = SimOf())
		{
			Sim->setProfile(nullptr);
		}
	});
}
