#include "PawnSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "PoseSearch/PoseSearchDatabase.h"

#include "PawnActor.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogSimPawns, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GPawnsFullDetail(
		TEXT("Pawns.FullDetail"), 20, TEXT("How many pawns nearest the camera get full motion matching."), ECVF_Default);
	TAutoConsoleVariable<float> GPawnsNearDistance(
		TEXT("Pawns.NearDistance"), 40.0f, TEXT("Metres: past this, pawns update their animation 8 times a second instead of 20."), ECVF_Default);
	TAutoConsoleVariable<int32> GPawnsMetaHumans(
		TEXT("Pawns.MetaHumans"), 0, TEXT("1 draws pawns as MetaHumans (driven by a hidden mannequin), 0 as mannequins."), ECVF_Default);
	TAutoConsoleVariable<int32> GPawnsShowDetail(
		TEXT("Pawns.ShowDetail"), 0, TEXT("1 marks each pawn's animation detail over its head: green full, yellow near, red far."), ECVF_Default);

	constexpr float PawnNearTickInterval = 1.0f / 20.0f;
	constexpr float PawnFarTickInterval = 1.0f / 8.0f;

	const TCHAR* const PawnBodyMeshPath = TEXT("/Game/Characters/UEFN_Mannequin/Meshes/SKM_UEFN_Mannequin.SKM_UEFN_Mannequin");
	const TCHAR* const PawnMetaHumanClassPath = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");
	const TCHAR* const PawnIdlePath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Idle/M_Neutral_Stand_Idle_Loop.M_Neutral_Stand_Idle_Loop");
	const TCHAR* const PawnWalkPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Walk/M_Neutral_Walk_Loop_F.M_Neutral_Walk_Loop_F");
	const TCHAR* const PawnRunPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Run/M_Neutral_Run_Loop_F.M_Neutral_Run_Loop_F");
	// Placeholder for picking up and putting down until there are real action animations: a short crouch.
	const TCHAR* const PawnCrouchPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Idle/M_Neutral_Crouch_Idle_Loop.M_Neutral_Crouch_Idle_Loop");

	// The sample's standing locomotion databases (its crouch, jump and traversal ones are left out).
	const TCHAR* const PawnDatabaseNames[] = {
		TEXT("PSD_Dense_Stand_Idles"),
		TEXT("PSD_Dense_Stand_TurnInPlace"),
		TEXT("PSD_Dense_Stand_Walk_Loops"),
		TEXT("PSD_Dense_Stand_Walk_Starts"),
		TEXT("PSD_Dense_Stand_Walk_Stops"),
		TEXT("PSD_Dense_Stand_Walk_Pivots"),
		TEXT("PSD_Dense_Stand_Run_Loops"),
		TEXT("PSD_Dense_Stand_Run_Starts"),
		TEXT("PSD_Dense_Stand_Run_Stops"),
		TEXT("PSD_Dense_Stand_Run_Pivots"),
	};
	const TCHAR* const PawnDatabaseFolder = TEXT("/Game/Characters/UEFN_Mannequin/Animations/MotionMatchingData/Databases/Dense/");

	uint32 PawnKey(colony::Entity Thing)
	{
		return static_cast<uint32>(entt::to_integral(Thing));
	}

	/** Ground speed of a loop from its root motion, cm per second. */
	float PawnLoopSpeed(const UAnimSequence* Loop, float Fallback)
	{
		if (!Loop || Loop->GetPlayLength() <= 0.0f)
		{
			return Fallback;
		}
		const FTransform Motion = Loop->ExtractRootMotionFromRange(0.0, Loop->GetPlayLength(), FAnimExtractContext());
		const float Speed = Motion.GetTranslation().Size2D() / Loop->GetPlayLength();
		return Speed > 10.0f ? Speed : Fallback;
	}
}

bool USimPawnSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USimPawnSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USimPawnSubsystem, STATGROUP_Tickables);
}

void USimPawnSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &USimPawnSubsystem::OnSimEvent);
		EventSource = Bridge;
	}
}

void USimPawnSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = EventSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	// The world is going away and takes the actors with it.
	Pawns.Reset();
	Super::Deinitialize();
}

void USimPawnSubsystem::LoadAssets()
{
	if (bAssetsTried)
	{
		return;
	}
	bAssetsTried = true;
	BodyMesh = LoadObject<USkeletalMesh>(nullptr, PawnBodyMeshPath);
	IdleLoop = LoadObject<UAnimSequence>(nullptr, PawnIdlePath);
	WalkLoop = LoadObject<UAnimSequence>(nullptr, PawnWalkPath);
	RunLoop = LoadObject<UAnimSequence>(nullptr, PawnRunPath);
	CrouchPose = LoadObject<UAnimSequence>(nullptr, PawnCrouchPath);
	for (const TCHAR* Name : PawnDatabaseNames)
	{
		const FString Path = FString::Printf(TEXT("%s%s.%s"), PawnDatabaseFolder, Name, Name);
		if (const UPoseSearchDatabase* Database = LoadObject<UPoseSearchDatabase>(nullptr, *Path))
		{
			Databases.Add(Database);
		}
		else
		{
			UE_LOG(LogSimPawns, Warning, TEXT("Motion matching database missing: %s"), *Path);
		}
	}
	WalkLoopSpeed = PawnLoopSpeed(WalkLoop, 150.0f);
	RunLoopSpeed = PawnLoopSpeed(RunLoop, 400.0f);
	bAssetsLoaded = BodyMesh && IdleLoop && WalkLoop && RunLoop && !Databases.IsEmpty();
	if (bAssetsLoaded)
	{
		UE_LOG(LogSimPawns, Display, TEXT("Pawn animation: %d motion matching databases, walk loop %.0f cm/s, run loop %.0f cm/s"),
			Databases.Num(), WalkLoopSpeed, RunLoopSpeed);
	}
	else
	{
		UE_LOG(LogSimPawns, Error, TEXT("Pawn assets missing (mesh %d, loops %d %d %d, %d databases): is the Game Animation Sample in game/Content?"),
			BodyMesh != nullptr, IdleLoop != nullptr, WalkLoop != nullptr, RunLoop != nullptr, Databases.Num());
	}
}

UAnimSequence* USimPawnSubsystem::GetActionAnimation(colony::PawnAction Action) const
{
	switch (Action)
	{
		case colony::PawnAction::PickUp:
		case colony::PawnAction::PutDown:
		case colony::PawnAction::Eat:
		case colony::PawnAction::LieDown:
		case colony::PawnAction::GetUp:
			return CrouchPose;
	}
	return CrouchPose;
}

ASimPawnActor* USimPawnSubsystem::FindPawn(colony::Entity Thing) const
{
	const TObjectPtr<ASimPawnActor>* Found = Pawns.Find(PawnKey(Thing));
	return Found ? Found->Get() : nullptr;
}

void USimPawnSubsystem::RemoveAll()
{
	for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	Pawns.Reset();
	DetailCounts = FSimPawnDetailCounts();
}

void USimPawnSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::PawnAction)
	{
		return;
	}
	++ActionEvents;
	if (ASimPawnActor* Pawn = FindPawn(Event.thing))
	{
		const int32 Before = Pawn->GetActionsPlayed();
		Pawn->PlayAction(Event.action);
		ActionsStarted += Pawn->GetActionsPlayed() - Before;
	}
}

void USimPawnSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !Bridge->GetSim())
	{
		RemoveAll();
		return;
	}
	// A new sim reuses entity numbers; a new look needs new actors.
	const bool bMetaHumans = GPawnsMetaHumans.GetValueOnGameThread() != 0;
	if (Bridge->GetSim() != LastSim || bMetaHumans != bMetaHumansShown)
	{
		RemoveAll();
		LastSim = Bridge->GetSim();
		bMetaHumansShown = bMetaHumans;
	}
	LoadAssets();
	if (!bAssetsLoaded)
	{
		return;
	}
	if (bMetaHumans && !MetaHumanClass)
	{
		MetaHumanClass = LoadClass<AActor>(nullptr, PawnMetaHumanClassPath);
		if (!MetaHumanClass)
		{
			UE_LOG(LogSimPawns, Error, TEXT("Could not load %s; drawing mannequins"), PawnMetaHumanClassPath);
		}
	}

	const colony::Snapshot& Now = Bridge->GetCurrentSnapshot();
	const colony::Snapshot& Before = Bridge->GetPreviousSnapshot();
	const colony::DefDatabase* Defs = Bridge->GetDefs().get();
	TSet<uint32> Seen;
	for (const colony::ThingView& View : Now.things)
	{
		if (View.category != colony::ThingCategory::Pawn)
		{
			continue;
		}
		const uint32 Key = PawnKey(View.id);
		Seen.Add(Key);
		TObjectPtr<ASimPawnActor>& Pawn = Pawns.FindOrAdd(Key);
		if (!IsValid(Pawn))
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			FVector Location;
			Bridge->GetBlendedLocation(View.id, Location);
			Pawn = GetWorld()->SpawnActor<ASimPawnActor>(Location, FRotator::ZeroRotator, Params);
			const double MetresPerStep = Defs && View.def.valid() ? Defs->get(View.def).number("stats.moveSpeed", 0.0) : 0.0;
			const float Cruise = MetresPerStep > 0.0 ? static_cast<float>(MetresPerStep * colony::kStepsPerRealSecondAt1x * 100.0) : 400.0f;
			Pawn->Setup(*this, View.id, Cruise, bMetaHumans);
		}
		Pawn->UpdateFromSim(*Bridge, View, Before.find(View.id), DeltaTime, Bridge->GetSpeed());
	}
	for (auto It = Pawns.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			if (IsValid(It.Value()))
			{
				It.Value()->Destroy();
			}
			It.RemoveCurrent();
		}
	}

	FVector Camera = FVector::ZeroVector;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
	{
		Camera = PC->PlayerCameraManager->GetCameraLocation();
	}
	AssignDetail(Camera);
}

void USimPawnSubsystem::AssignDetail(const FVector& CameraLocation)
{
	TArray<TPair<double, ASimPawnActor*>> ByDistance;
	ByDistance.Reserve(Pawns.Num());
	for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns)
	{
		ByDistance.Emplace(FVector::DistSquared(Pair.Value->GetActorLocation(), CameraLocation), Pair.Value.Get());
	}
	ByDistance.Sort([](const TPair<double, ASimPawnActor*>& A, const TPair<double, ASimPawnActor*>& B)
	{
		return A.Key < B.Key;
	});

	const int32 FullCount = FMath::Max(0, GPawnsFullDetail.GetValueOnGameThread());
	const double Near = FMath::Square(GPawnsNearDistance.GetValueOnGameThread() * 100.0);
	const bool bShow = GPawnsShowDetail.GetValueOnGameThread() != 0;
	DetailCounts = FSimPawnDetailCounts();
	for (int32 i = 0; i < ByDistance.Num(); ++i)
	{
		ASimPawnActor* Pawn = ByDistance[i].Value;
		FColor Mark;
		if (i < FullCount)
		{
			Pawn->SetDetail(ESimPawnDetail::Full, 0.0f);
			++DetailCounts.Full;
			Mark = FColor::Green;
		}
		else if (ByDistance[i].Key < Near)
		{
			Pawn->SetDetail(ESimPawnDetail::Simple, PawnNearTickInterval);
			++DetailCounts.Near;
			Mark = FColor::Yellow;
		}
		else
		{
			Pawn->SetDetail(ESimPawnDetail::Simple, PawnFarTickInterval);
			++DetailCounts.Far;
			Mark = FColor::Red;
		}
		if (bShow)
		{
			DrawDebugPoint(GetWorld(), Pawn->GetActorLocation() + FVector(0.0, 0.0, 220.0), 12.0f, Mark);
		}
	}
}
