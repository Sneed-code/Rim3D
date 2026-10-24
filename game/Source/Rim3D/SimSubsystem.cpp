#include "SimSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogColonySim, Log, All);

void USimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	colony::SimSettings Settings;
	Settings.seed = 42;
	Settings.mapSize = 256;
	Sim = std::make_unique<colony::Sim>(Settings);
	UE_LOG(LogColonySim, Log, TEXT("Colony sim started: seed %llu, %d m map"), static_cast<unsigned long long>(Settings.seed), Settings.mapSize);
}

void USimSubsystem::Deinitialize()
{
	Sim.reset();
	Super::Deinitialize();
}

void USimSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Sim || Speed <= 0)
	{
		return;
	}

	StepAccumulator += static_cast<double>(DeltaTime) * colony::kStepsPerRealSecondAt1x * Speed;
	// Cap catch-up after a long hitch so one slow frame can't stall the next.
	const double MaxStepsPerFrame = static_cast<double>(colony::kStepsPerRealSecondAt1x) * 3.0;
	StepAccumulator = FMath::Min(StepAccumulator, MaxStepsPerFrame);
	const int64 Steps = static_cast<int64>(StepAccumulator);
	StepAccumulator -= static_cast<double>(Steps);
	Sim->stepMany(Steps);

	const int64 Hour = Sim->time().seconds / colony::kSecondsPerHour;
	if (Hour != LastLoggedHour)
	{
		LastLoggedHour = Hour;
		UE_LOG(LogColonySim, Log, TEXT("Day %lld, %02d:00"), static_cast<long long>(Sim->time().day()), Sim->time().hourOfDay());
	}
}

TStatId USimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USimSubsystem, STATGROUP_Tickables);
}

void USimSubsystem::SetSpeed(int32 NewSpeed)
{
	Speed = FMath::Clamp(NewSpeed, 0, 3);
}

int64 USimSubsystem::GetGameSeconds() const
{
	return Sim ? Sim->time().seconds : 0;
}

bool USimSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
