#include "SimSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ColonySim/Map.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Version.h"

#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogColonySim, Log, All);

namespace
{
	// After a long hitch the sim catches up by at most this many steps, so one slow frame can't stall the next.
	constexpr double SimBridgeMaxCatchUpSteps = static_cast<double>(colony::kStepsPerRealSecondAt1x) * 3.0;
	// What the whole sim may cost in one frame at 3x (Stage 1 target).
	constexpr double SimBridgeFrameBudgetMs = 5.0;
	// Levels stack about 3 m apart.
	constexpr float SimBridgeLevelHeightM = 3.0f;
	// Keys for on-screen messages that are replaced every frame.
	constexpr uint64 SimBridgeMessageKey = 0x51317100;

	TAutoConsoleVariable<int32> GSimBridgeShowTimings(
		TEXT("Sim.ShowTimings"), 1, TEXT("1 shows the sim's speed and cost on screen, 0 hides it."), ECVF_Default);
	TAutoConsoleVariable<int32> GSimBridgeDebugDraw(
		TEXT("Sim.DebugDraw"), 0, TEXT("1 draws every thing in the snapshot as a coloured shape, at its blended position."), ECVF_Default);

	std::string SimBridgeUtf8(const FString& Text)
	{
		return std::string(TCHAR_TO_UTF8(*Text));
	}

	// Ground height in metres at a point, interpolated between the cell corners the way the
	// terrain mesh is (a vertex per cell corner), so things stand on the mesh.
	float SimBridgeGroundHeight(const colony::Map& Map, float X, float Y)
	{
		const auto Corner = [&Map](int32 CX, int32 CY)
		{
			CX = FMath::Clamp(CX, 0, Map.width() - 1);
			CY = FMath::Clamp(CY, 0, Map.height() - 1);
			return Map.groundHeight(CX, CY);
		};
		const int32 X0 = FMath::FloorToInt(X);
		const int32 Y0 = FMath::FloorToInt(Y);
		const float FX = X - static_cast<float>(X0);
		const float FY = Y - static_cast<float>(Y0);
		const float Low = FMath::Lerp(Corner(X0, Y0), Corner(X0 + 1, Y0), FX);
		const float High = FMath::Lerp(Corner(X0, Y0 + 1), Corner(X0 + 1, Y0 + 1), FX);
		return FMath::Lerp(Low, High, FY);
	}

	FString SimBridgeDefaultDataRoot()
	{
		FString Root;
		if (FParse::Value(FCommandLine::Get(), TEXT("SimData="), Root) && !Root.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(Root);
		}
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../data"));
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	FSimStartOptions SimBridgeOptionsFromCommandLine()
	{
		FSimStartOptions Options;
		const TCHAR* Line = FCommandLine::Get();
		int64 Seed = 0;
		if (FParse::Value(Line, TEXT("SimSeed="), Seed))
		{
			Options.Seed = static_cast<uint64>(Seed);
		}
		FParse::Value(Line, TEXT("SimMapSize="), Options.MapSize);
		FParse::Value(Line, TEXT("SimPawns="), Options.Pawns);
		FParse::Value(Line, TEXT("SimItems="), Options.Items);
		Options.bFlatMap = FParse::Param(Line, TEXT("SimFlat"));
		FParse::Value(Line, TEXT("SimScenario="), Options.Scenario);
		return Options;
	}

	void SimBridgeScreenMessage(int32 Slot, float Seconds, const FColor& Color, const FString& Text)
	{
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(SimBridgeMessageKey + Slot, Seconds, Color, Text);
		}
	}

	USimSubsystem* SimBridgeForWorld(UWorld* World)
	{
		return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
	}

	FAutoConsoleCommandWithWorldAndArgs GSimBridgeSpeedCommand(
		TEXT("Sim.Speed"), TEXT("Sim.Speed <0-3>: 0 pauses, 1 to 3 are the normal speeds."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (USimSubsystem* Bridge = SimBridgeForWorld(World); Bridge && Args.Num() == 1)
			{
				Bridge->SetSpeed(FCString::Atoi(*Args[0]));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GSimBridgeSpawnCommand(
		TEXT("Sim.Spawn"), TEXT("Sim.Spawn <Def> <x> <y> [count]: dev command, spawns a thing at a cell of level 0."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			USimSubsystem* Bridge = SimBridgeForWorld(World);
			if (!Bridge || Args.Num() < 3)
			{
				UE_LOG(LogColonySim, Warning, TEXT("Usage: Sim.Spawn <Def> <x> <y> [count]"));
				return;
			}
			colony::SpawnThingCommand Command;
			Command.def = SimBridgeUtf8(Args[0]);
			Command.cell = colony::CellPos{FCString::Atoi(*Args[1]), FCString::Atoi(*Args[2]), 0};
			Command.count = Args.Num() > 3 ? FCString::Atoi(*Args[3]) : 1;
			Bridge->Submit(Command);
		}));

	// Slot names become file names: letters, digits, '-' and '_' only.
	FString SimBridgeCleanSlot(const FString& Slot)
	{
		FString Clean;
		for (const TCHAR Letter : Slot)
		{
			if (FChar::IsAlnum(Letter) || Letter == TEXT('-') || Letter == TEXT('_'))
			{
				Clean.AppendChar(Letter);
			}
		}
		return Clean;
	}

	FAutoConsoleCommandWithWorldAndArgs GSimBridgeSaveCommand(
		TEXT("Sim.Save"), TEXT("Sim.Save [slot]: saves the game to Saved/Saves/<slot>.save (default quicksave)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (USimSubsystem* Bridge = SimBridgeForWorld(World))
			{
				Bridge->SaveGame(Args.Num() > 0 ? Args[0] : TEXT("quicksave"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GSimBridgeLoadCommand(
		TEXT("Sim.Load"), TEXT("Sim.Load [slot]: loads Saved/Saves/<slot>.save (default quicksave)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (USimSubsystem* Bridge = SimBridgeForWorld(World))
			{
				Bridge->LoadGame(Args.Num() > 0 ? Args[0] : TEXT("quicksave"));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GSimBridgeBugReportCommand(
		TEXT("Sim.BugReport"), TEXT("Writes a bug report folder (replay, save, readable dump) in Saved/BugReports/."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (USimSubsystem* Bridge = SimBridgeForWorld(World))
			{
				Bridge->WriteBugReport();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs GSimBridgeReplayCommand(
		TEXT("Sim.SaveReplay"), TEXT("Writes the session so far as a replay file in Saved/Replays/."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (USimSubsystem* Bridge = SimBridgeForWorld(World))
			{
				Bridge->SaveReplay();
			}
		}));
}

// Passes the speed and dev keys to the subsystem without needing a player controller, so they
// work in any level and in the smoke tests. Never swallows the key.
class FSimInputProcessor : public IInputProcessor
{
public:
	explicit FSimInputProcessor(USimSubsystem* InOwner) : Owner(InOwner) {}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& KeyEvent) override
	{
		if (KeyEvent.IsRepeat())
		{
			return false;
		}
		// Typing into the console (a "1" in a Sim.Spawn count) must not change the speed.
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->ViewportConsole && GEngine->GameViewport->ViewportConsole->ConsoleActive())
		{
			return false;
		}
		if (USimSubsystem* Bridge = Owner.Get())
		{
			Bridge->HandleKey(KeyEvent.GetKey());
		}
		return false;
	}

	virtual const TCHAR* GetDebugName() const override { return TEXT("SimInputProcessor"); }

private:
	TWeakObjectPtr<USimSubsystem> Owner;
};

void USimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	StartSim(SimBridgeOptionsFromCommandLine());

	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FSimInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}
}

void USimSubsystem::Deinitialize()
{
	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();
	Sim.reset();
	Defs.reset();
	Super::Deinitialize();
}

bool USimSubsystem::StartSim(const FSimStartOptions& Options)
{
	StartOptions = Options;
	DataRoot = Options.DataRoot.IsEmpty() ? SimBridgeDefaultDataRoot() : Options.DataRoot;

	// A broken data folder is an error and the sim starts without defs; a missing one is a warning.
	bool bDefsOk = true;
	Defs.reset();
	if (IFileManager::Get().DirectoryExists(*(DataRoot / TEXT("defs"))))
	{
		auto Loaded = std::make_shared<colony::DefDatabase>();
		std::vector<std::string> Errors;
		if (colony::DefDatabase::loadDirectory(SimBridgeUtf8(DataRoot), *Loaded, Errors))
		{
			Defs = std::move(Loaded);
		}
		else
		{
			bDefsOk = false;
			for (const std::string& Error : Errors)
			{
				UE_LOG(LogColonySim, Error, TEXT("Defs: %s"), UTF8_TO_TCHAR(Error.c_str()));
			}
		}
	}
	else
	{
		UE_LOG(LogColonySim, Warning, TEXT("No defs folder in %s; the sim has no things"), *DataRoot);
	}

	colony::SimSettings Settings;
	Settings.seed = Options.Seed;
	Settings.mapSize = Options.MapSize;
	Settings.pawns = Options.Pawns;
	Settings.items = Options.Items;
	Settings.flatMap = Options.bFlatMap;
	Settings.scenario = SimBridgeUtf8(Options.Scenario);
	Sim = Defs ? std::make_unique<colony::Sim>(Settings, Defs) : std::make_unique<colony::Sim>(Settings);
	for (const std::string& Problem : Sim->setupErrors())
	{
		UE_LOG(LogColonySim, Warning, TEXT("Sim setup: %s"), UTF8_TO_TCHAR(Problem.c_str()));
	}

	ResetAfterSimChange();

	UE_LOG(LogColonySim, Log, TEXT("Colony sim started: seed %llu, %d m map, %d defs from %s"),
		static_cast<unsigned long long>(Settings.seed), Settings.mapSize, GetDefCount(), *DataRoot);
	return bDefsOk;
}

void USimSubsystem::ResetAfterSimChange()
{
	++SimGeneration;
	// Both snapshots start as the current state, so nothing blends from an empty map.
	Sim->writeSnapshot(Current);
	Previous = Current;
	BlendAlpha = 0.0f;
	StepAccumulator = 0.0;
	LastLoggedHour = -1;
	EventCount = 0;
	AutosaveCount = 0;
	LastAutosaveBucket = AutosaveInterval > 0 ? Sim->time().seconds / AutosaveInterval : 0;
	Timings = FSimTimings();
	WindowSeconds = 0.0;
	WindowFrames = WindowSteps = 0;
	WindowFrameMs = WindowMaxFrameMs = WindowSnapshotMs = WindowMaxStepMs = WindowStepMs = 0.0;
}

void USimSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Sim)
	{
		return;
	}

	if (Speed > 0)
	{
		StepAccumulator += static_cast<double>(DeltaTime) * colony::kStepsPerRealSecondAt1x * Speed;
		StepAccumulator = FMath::Min(StepAccumulator, SimBridgeMaxCatchUpSteps);
		const int64 Steps = static_cast<int64>(StepAccumulator);
		StepAccumulator -= static_cast<double>(Steps);
		RunSteps(Steps);
		// Fixed timestep as in Gaffer on Games: show the blend between the last two steps by how far
		// the leftover time is into the next one. The picture lags the sim by less than one step.
		BlendAlpha = static_cast<float>(StepAccumulator);
		AdvanceTimingWindow(DeltaTime);
	}

	if (GSimBridgeShowTimings.GetValueOnGameThread() != 0)
	{
		ShowTimings();
	}
	if (GSimBridgeDebugDraw.GetValueOnGameThread() != 0)
	{
		DrawDebug();
	}
}

TStatId USimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USimSubsystem, STATGROUP_Tickables);
}

void USimSubsystem::RunSteps(int64 Steps)
{
	if (!Sim || Steps <= 0)
	{
		return;
	}
	const double FrameStart = FPlatformTime::Seconds();
	double StepMsTotal = 0.0;
	double SnapshotMs = 0.0;
	for (int64 Index = 0; Index < Steps; ++Index)
	{
		OnBeforeStep.Broadcast();
		const double StepStart = FPlatformTime::Seconds();
		Sim->step();
		const double StepMs = (FPlatformTime::Seconds() - StepStart) * 1000.0;
		StepMsTotal += StepMs;
		WindowMaxStepMs = FMath::Max(WindowMaxStepMs, StepMs);

		HandleEvents();

		// Only the last two steps of a frame are ever shown, so only they are copied.
		if (Index >= Steps - 2)
		{
			const double SnapshotStart = FPlatformTime::Seconds();
			if (Steps == 1)
			{
				Swap(Previous, Current);  // the old newest becomes the one before it
			}
			Sim->writeSnapshot(Index == Steps - 1 ? Current : Previous);
			SnapshotMs += (FPlatformTime::Seconds() - SnapshotStart) * 1000.0;
		}
	}

	const double FrameMs = (FPlatformTime::Seconds() - FrameStart) * 1000.0;
	WindowSteps += Steps;
	WindowStepMs += StepMsTotal;
	WindowSnapshotMs += SnapshotMs;
	WindowFrameMs += FrameMs;
	WindowMaxFrameMs = FMath::Max(WindowMaxFrameMs, FrameMs);

	const int64 Hour = Sim->time().seconds / colony::kSecondsPerHour;
	if (Hour != LastLoggedHour)
	{
		LastLoggedHour = Hour;
		UE_LOG(LogColonySim, Log, TEXT("Day %lld, %02d:00"), static_cast<long long>(Sim->time().day()), Sim->time().hourOfDay());
	}
	MaybeAutosave();
}

void USimSubsystem::HandleEvents()
{
	for (const colony::Event& Event : Sim->events())
	{
		++EventCount;
		if (Event.type == colony::EventType::CommandFailed)
		{
			UE_LOG(LogColonySim, Warning, TEXT("Command %llu failed: %s"), static_cast<unsigned long long>(Event.command), UTF8_TO_TCHAR(Event.message.c_str()));
			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 6.0f, FColor::Red, FString::Printf(TEXT("Command failed: %s"), UTF8_TO_TCHAR(Event.message.c_str())));
			}
		}
		OnSimEvent.Broadcast(Event);
	}
}

void USimSubsystem::AdvanceTimingWindow(double DeltaTime)
{
	++WindowFrames;
	WindowSeconds += DeltaTime;
	if (WindowSeconds < 1.0)
	{
		return;
	}
	const double Frames = static_cast<double>(WindowFrames);
	Timings.StepsPerFrame = static_cast<double>(WindowSteps) / Frames;
	Timings.AvgFrameMs = WindowFrameMs / Frames;
	Timings.MaxFrameMs = WindowMaxFrameMs;
	Timings.AvgStepMs = WindowSteps > 0 ? WindowStepMs / static_cast<double>(WindowSteps) : 0.0;
	Timings.MaxStepMs = WindowMaxStepMs;
	Timings.AvgSnapshotMs = WindowSnapshotMs / Frames;
	WindowSeconds = 0.0;
	WindowFrames = WindowSteps = 0;
	WindowFrameMs = WindowMaxFrameMs = WindowSnapshotMs = WindowMaxStepMs = WindowStepMs = 0.0;
}

void USimSubsystem::ShowTimings() const
{
	if (!Sim)
	{
		return;
	}
	const colony::GameTime& Time = Sim->time();
	const FString SpeedText = Speed > 0 ? FString::Printf(TEXT("%dx"), Speed) : FString(TEXT("PAUSED"));
	// Unreal lists messages with the highest key on top, so the first line gets the highest slot.
	SimBridgeScreenMessage(2, 0.25f, FColor::Cyan,
		FString::Printf(TEXT("Sim: day %lld %02d:%02d   %s   %d things on the map   %lld events"),
			static_cast<long long>(Time.day() + 1), Time.hourOfDay(), static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute),
			*SpeedText, static_cast<int32>(Current.things.size()), static_cast<long long>(EventCount)));
	const bool bOverBudget = Timings.AvgFrameMs > SimBridgeFrameBudgetMs;
	SimBridgeScreenMessage(1, 0.25f, bOverBudget ? FColor::Red : FColor::Cyan,
		FString::Printf(TEXT("Sim cost per frame: avg %.2f ms, max %.2f ms (budget %.0f ms), %.1f steps per frame"),
			Timings.AvgFrameMs, Timings.MaxFrameMs, SimBridgeFrameBudgetMs, Timings.StepsPerFrame));
	SimBridgeScreenMessage(0, 0.25f, FColor::Cyan,
		FString::Printf(TEXT("Per step: avg %.3f ms, max %.3f ms; snapshots %.3f ms per frame"),
			Timings.AvgStepMs, Timings.MaxStepMs, Timings.AvgSnapshotMs));
}

void USimSubsystem::DrawDebug() const
{
	UWorld* World = GetWorld();
	if (!Sim || !World)
	{
		return;
	}
	for (const colony::ThingView& View : Current.things)
	{
		FVector Location;
		if (!GetBlendedLocation(View.id, Location))
		{
			continue;
		}
		switch (View.category)
		{
			case colony::ThingCategory::Pawn:
				DrawDebugCapsule(World, Location + FVector(0.0, 0.0, 90.0), 90.0f, 30.0f, FQuat::Identity, FColor::Green);
				break;
			case colony::ThingCategory::Item:
				DrawDebugBox(World, Location + FVector(0.0, 0.0, 15.0), FVector(20.0, 20.0, 15.0), FColor::Yellow);
				break;
			case colony::ThingCategory::Building:
				DrawDebugBox(World, Location + FVector(0.0, 0.0, 150.0), FVector(View.sizeX * 50.0, View.sizeY * 50.0, 150.0), FColor(150, 150, 160));
				break;
		}
	}
}

void USimSubsystem::SetMaxSpeed(int32 NewMax)
{
	MaxSpeed = FMath::Clamp(NewMax, 1, 3);
	SpeedBeforePause = FMath::Min(SpeedBeforePause, MaxSpeed);
	if (Speed > MaxSpeed)
	{
		SetSpeed(MaxSpeed);
	}
}

void USimSubsystem::SetSpeed(int32 NewSpeed)
{
	Speed = FMath::Clamp(NewSpeed, 0, MaxSpeed);
	if (Speed > 0)
	{
		SpeedBeforePause = Speed;
	}
	else
	{
		BlendAlpha = 1.0f;  // paused: show the newest state exactly
	}
}

void USimSubsystem::TogglePause()
{
	SetSpeed(Speed > 0 ? 0 : SpeedBeforePause);
}

void USimSubsystem::StepOnce()
{
	if (Sim)
	{
		RunSteps(1);
		BlendAlpha = 1.0f;
	}
}

int64 USimSubsystem::GetGameSeconds() const
{
	return Sim ? Sim->time().seconds : 0;
}

FString USimSubsystem::SaveReplay()
{
	if (!Sim)
	{
		return FString();
	}
	const colony::Replay Replay = colony::recordReplay(*Sim);
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Replays"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = Directory / FString::Printf(TEXT("replay-%s-day%lld.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")), static_cast<long long>(Sim->time().day() + 1));
	std::string Error;
	if (!colony::saveReplayFile(SimBridgeUtf8(Path), Replay, Error))
	{
		UE_LOG(LogColonySim, Error, TEXT("Could not save the replay: %s"), UTF8_TO_TCHAR(Error.c_str()));
		SimBridgeScreenMessage(10, 6.0f, FColor::Red, TEXT("Replay not saved, see the log"));
		return FString();
	}
	UE_LOG(LogColonySim, Display, TEXT("Replay saved: %s (%d commands, step %lld)"), *Path, static_cast<int32>(Replay.commands.size()), static_cast<long long>(Replay.endStep));
	SimBridgeScreenMessage(10, 6.0f, FColor::Green, FString::Printf(TEXT("Replay saved: %s"), *Path));
	return Path;
}

FString USimSubsystem::GetSaveDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Saves"));
}

FString USimSubsystem::SaveGame(const FString& Slot)
{
	const FString Clean = SimBridgeCleanSlot(Slot);
	if (!Sim || Clean.IsEmpty())
	{
		return FString();
	}
	const FString Directory = GetSaveDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = Directory / (Clean + TEXT(".save"));
	// Write beside the old file and swap, so a crash or full disk never leaves half a save.
	const FString TempPath = Path + TEXT(".tmp");
	const double Start = FPlatformTime::Seconds();
	const std::vector<uint8_t> Bytes = colony::saveSim(*Sim);
	std::string Error;
	if (!colony::writeSaveFile(SimBridgeUtf8(TempPath), Bytes, Error) || !IFileManager::Get().Move(*Path, *TempPath, true))
	{
		IFileManager::Get().Delete(*TempPath);
		UE_LOG(LogColonySim, Error, TEXT("Could not save %s: %s"), *Path, UTF8_TO_TCHAR(Error.c_str()));
		SimBridgeScreenMessage(11, 6.0f, FColor::Red, FString::Printf(TEXT("Save failed (%s), see the log"), *Clean));
		return FString();
	}
	UE_LOG(LogColonySim, Display, TEXT("Saved %s (%d KB, %.1f ms, day %lld)"), *Path, static_cast<int32>(Bytes.size() / 1024),
		(FPlatformTime::Seconds() - Start) * 1000.0, static_cast<long long>(Sim->time().day() + 1));
	SimBridgeScreenMessage(11, 3.0f, FColor::Green, FString::Printf(TEXT("Saved: %s"), *Clean));
	return Path;
}

bool USimSubsystem::LoadGame(const FString& Slot)
{
	const FString Clean = SimBridgeCleanSlot(Slot);
	const FString Path = GetSaveDirectory() / (Clean + TEXT(".save"));
	const auto Refuse = [&](const FString& Why)
	{
		UE_LOG(LogColonySim, Warning, TEXT("Could not load %s: %s"), *Path, *Why);
		SimBridgeScreenMessage(11, 6.0f, FColor::Red, FString::Printf(TEXT("Load failed (%s): %s"), *Clean, *Why));
		return false;
	};
	if (Clean.IsEmpty() || !IFileManager::Get().FileExists(*Path))
	{
		return Refuse(TEXT("there is no such save"));
	}
	if (!Defs)
	{
		return Refuse(TEXT("the game has no defs loaded"));
	}
	std::vector<uint8_t> Bytes;
	std::string Error;
	if (!colony::readSaveFile(SimBridgeUtf8(Path), Bytes, Error))
	{
		return Refuse(UTF8_TO_TCHAR(Error.c_str()));
	}
	colony::SaveLoadResult Result = colony::loadSim(Bytes, Defs);
	if (!Result.sim)
	{
		return Refuse(UTF8_TO_TCHAR(Result.error.c_str()));
	}
	for (const std::string& Warning : Result.warnings)
	{
		UE_LOG(LogColonySim, Warning, TEXT("Load %s: %s"), *Clean, UTF8_TO_TCHAR(Warning.c_str()));
	}

	// The old sim goes only after the new one exists. The new one lives at another address, which
	// is how the pawn actors notice the sim changed and rebuild themselves from the snapshot.
	Sim = std::move(Result.sim);
	const colony::SimSettings& Settings = Sim->settings();
	StartOptions.Seed = Settings.seed;
	StartOptions.MapSize = Settings.mapSize;
	StartOptions.Pawns = Settings.pawns;
	StartOptions.Items = Settings.items;
	StartOptions.bFlatMap = Settings.flatMap;
	ResetAfterSimChange();
	UE_LOG(LogColonySim, Display, TEXT("Loaded %s: day %lld, %d things"), *Path, static_cast<long long>(Sim->time().day() + 1), static_cast<int32>(Current.things.size()));
	SimBridgeScreenMessage(11, 3.0f, FColor::Green, FString::Printf(TEXT("Loaded: %s"), *Clean));
	return true;
}

void USimSubsystem::SetAutosaveInterval(int64 GameSeconds)
{
	AutosaveInterval = FMath::Max<int64>(GameSeconds, 0);
	LastAutosaveBucket = (Sim && AutosaveInterval > 0) ? Sim->time().seconds / AutosaveInterval : 0;
}

void USimSubsystem::MaybeAutosave()
{
	if (!Sim || AutosaveInterval <= 0)
	{
		return;
	}
	const int64 Bucket = Sim->time().seconds / AutosaveInterval;
	if (Bucket == LastAutosaveBucket)
	{
		return;
	}
	LastAutosaveBucket = Bucket;
	// Three files in turn, so a bad autosave never costs the two before it.
	if (!SaveGame(FString::Printf(TEXT("autosave-%d"), static_cast<int32>(Bucket % 3) + 1)).IsEmpty())
	{
		++AutosaveCount;
	}
}

FString USimSubsystem::WriteBugReport()
{
	if (!Sim)
	{
		return FString();
	}
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BugReports")
		/ FString::Printf(TEXT("%s-day%lld"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")), static_cast<long long>(Sim->time().day() + 1)));
	IFileManager::Get().MakeDirectory(*Folder, true);

	std::string Error;
	const colony::Replay Replay = colony::recordReplay(*Sim);
	bool bOk = colony::saveReplayFile(SimBridgeUtf8(Folder / TEXT("replay.json")), Replay, Error);
	bOk = bOk && colony::writeSaveFile(SimBridgeUtf8(Folder / TEXT("state.save")), colony::saveSim(*Sim), Error);
	bOk = bOk && FFileHelper::SaveStringToFile(UTF8_TO_TCHAR(colony::saveToJson(*Sim).c_str()), *(Folder / TEXT("state.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!bOk)
	{
		UE_LOG(LogColonySim, Error, TEXT("Could not write the bug report in %s: %s"), *Folder, UTF8_TO_TCHAR(Error.c_str()));
		SimBridgeScreenMessage(12, 6.0f, FColor::Red, TEXT("Bug report not written, see the log"));
		return FString();
	}

	const FString Note = FString::Printf(TEXT("Rim3D bug report\nWritten: %s\nSim version: %s\nSeed: %llu, map %d m, scenario '%s'\nGame second: %lld (day %lld)\nState hash: %llu\nCommands so far: %d\n\n")
		TEXT("Run it in the cloud:  simrun --replay replay.json\nOr continue from the save:  simrun --load state.save --days 1\nstate.json is the same save in readable form; unreal.log is the game's log if it could be copied.\n"),
		*FDateTime::Now().ToString(), UTF8_TO_TCHAR(colony::kSimVersion), static_cast<unsigned long long>(Sim->seed()), Sim->settings().mapSize,
		UTF8_TO_TCHAR(Sim->settings().scenario.c_str()), static_cast<long long>(Sim->time().seconds), static_cast<long long>(Sim->time().day() + 1),
		static_cast<unsigned long long>(Sim->stateHash()), static_cast<int32>(Sim->commandLog().size()));
	FFileHelper::SaveStringToFile(Note, *(Folder / TEXT("notes.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	// Best effort: the log may be locked or missing.
	IFileManager::Get().Copy(*(Folder / TEXT("unreal.log")), *(FPaths::ProjectLogDir() / (FString(FApp::GetProjectName()) + TEXT(".log"))));

	UE_LOG(LogColonySim, Display, TEXT("Bug report written: %s"), *Folder);
	SimBridgeScreenMessage(12, 10.0f, FColor::Green, FString::Printf(TEXT("Bug report written: %s"), *Folder));
	return Folder;
}

bool USimSubsystem::HandleKey(const FKey& Key)
{
	if (Key == EKeys::SpaceBar)
	{
		TogglePause();
	}
	else if (Key == EKeys::One || Key == EKeys::NumPadOne)
	{
		SetSpeed(1);
	}
	else if (Key == EKeys::Two || Key == EKeys::NumPadTwo)
	{
		SetSpeed(2);
	}
	else if (Key == EKeys::Three || Key == EKeys::NumPadThree)
	{
		SetSpeed(3);
	}
	else if (Key == EKeys::Period)
	{
		if (Speed == 0)
		{
			StepOnce();
		}
	}
	else if (Key == EKeys::F7)
	{
		SaveReplay();
	}
	else if (Key == EKeys::F5)
	{
		QuickSave();
	}
	else if (Key == EKeys::F9)
	{
		QuickLoad();
	}
	else if (Key == EKeys::F8)
	{
		WriteBugReport();
	}
	else
	{
		return false;
	}
	return true;
}

uint64 USimSubsystem::Submit(colony::Command Command)
{
	return Sim ? Sim->submit(std::move(Command)) : 0;
}

bool USimSubsystem::GetBlendedLocation(colony::Entity Thing, FVector& OutLocation) const
{
	const colony::ThingView* Newest = Current.find(Thing);
	if (!Newest)
	{
		return false;
	}
	float X = Newest->x;
	float Y = Newest->y;
	// A thing that appeared this step has no earlier position to blend from.
	const colony::ThingView* Older = Previous.find(Thing);
	if (Older && Older->level == Newest->level)
	{
		X = FMath::Lerp(Older->x, Newest->x, BlendAlpha);
		Y = FMath::Lerp(Older->y, Newest->y, BlendAlpha);
	}
	OutLocation = SimToWorld(X, Y, Newest->level);
	return true;
}

FVector USimSubsystem::SimToWorld(float X, float Y, int32 Level) const
{
	const float GroundM = Sim ? SimBridgeGroundHeight(Sim->map(), X, Y) : 0.0f;
	return WorldOrigin + FVector(X * 100.0, Y * 100.0, (GroundM + Level * SimBridgeLevelHeightM) * 100.0);
}

bool USimSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
