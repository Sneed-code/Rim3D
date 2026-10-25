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
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "ColonySim/Map.h"
#include "ColonySim/Replay.h"

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
	Sim = Defs ? std::make_unique<colony::Sim>(Settings, Defs) : std::make_unique<colony::Sim>(Settings);
	for (const std::string& Problem : Sim->setupErrors())
	{
		UE_LOG(LogColonySim, Warning, TEXT("Sim setup: %s"), UTF8_TO_TCHAR(Problem.c_str()));
	}

	// Both snapshots start as the opening state, so nothing blends from an empty map.
	Sim->writeSnapshot(Current);
	Previous = Current;
	BlendAlpha = 0.0f;
	StepAccumulator = 0.0;
	LastLoggedHour = -1;
	EventCount = 0;
	Timings = FSimTimings();
	WindowSeconds = 0.0;
	WindowFrames = WindowSteps = 0;
	WindowFrameMs = WindowMaxFrameMs = WindowSnapshotMs = WindowMaxStepMs = WindowStepMs = 0.0;

	UE_LOG(LogColonySim, Log, TEXT("Colony sim started: seed %llu, %d m map, %d defs from %s"),
		static_cast<unsigned long long>(Settings.seed), Settings.mapSize, GetDefCount(), *DataRoot);
	return bDefsOk;
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

void USimSubsystem::SetSpeed(int32 NewSpeed)
{
	Speed = FMath::Clamp(NewSpeed, 0, 3);
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
