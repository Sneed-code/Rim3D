#include "WorkToolSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Snapshot.h"

#include "HoldDraw.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorkTools, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GWorkTools(TEXT("Work.Tools"), 1,
		TEXT("1 draws the hand axe and plays the chopping clip while a pawn fells a tree; 0 off."));
	TAutoConsoleVariable<int32> GWorkChopClip(TEXT("Work.ChopClip"), 0,
		TEXT("Which chopping clip: 0 the two-handed mocap chop (A_ChopAxe), 1 Mixamo's horizontal axe swing."));
	TAutoConsoleVariable<float> GWorkChopRate(TEXT("Work.ChopRate"), 1.0f, TEXT("Play rate of the chopping clip."));
	TAutoConsoleVariable<float> GWorkAxeRoll(TEXT("Work.AxeRoll"), 0.0f, TEXT("Degrees the axe is rolled about its handle in the fist."));
	TAutoConsoleVariable<float> GWorkAxePitch(TEXT("Work.AxePitch"), 0.0f, TEXT("Degrees the axe is pitched in the fist."));
	TAutoConsoleVariable<float> GWorkAxeYaw(TEXT("Work.AxeYaw"), 0.0f, TEXT("Degrees the axe is turned in the fist."));

	const TCHAR* const WorkSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const WorkChopClipPaths[2] = {
		TEXT("/Game/Rim3D/Generated/Animations/ChopAxe/A_ChopAxe.A_ChopAxe"),
		TEXT("/Game/Rim3D/Generated/Animations/Mixamo/MeleeAxe/A_ProMeleeAxeStandingMeleeAttackHorizontal.A_ProMeleeAxeStandingMeleeAttackHorizontal"),
	};
}

bool UWorkToolSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWorkToolSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWorkToolSubsystem, STATGROUP_Tickables);
}

void UWorkToolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	BridgeSource = Collection.InitializeDependency<USimSubsystem>();
	Collection.InitializeDependency<USimPawnSubsystem>();
}

void UWorkToolSubsystem::Deinitialize()
{
	for (const TPair<TWeakObjectPtr<ASimPawnActor>, TWeakObjectPtr<UStaticMeshComponent>>& Pair : Axes)
	{
		if (Pair.Value.IsValid())
		{
			Pair.Value->DestroyComponent();
		}
	}
	Axes.Reset();
	Super::Deinitialize();
}

UStaticMeshComponent* UWorkToolSubsystem::GetAxe(const ASimPawnActor* Actor) const
{
	for (const TPair<TWeakObjectPtr<ASimPawnActor>, TWeakObjectPtr<UStaticMeshComponent>>& Pair : Axes)
	{
		if (Pair.Key.Get() == Actor)
		{
			return Pair.Value.Get();
		}
	}
	return nullptr;
}

UStaticMesh* UWorkToolSubsystem::AxeMesh()
{
	if (Axe)
	{
		return Axe;
	}
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::DefDatabase* Defs = Bridge ? Bridge->GetDefs().get() : nullptr;
	const colony::Def* Tools = Defs ? Defs->find("Rules", "WorkTools") : nullptr;
	if (!Solid)
	{
		Solid = LoadObject<UMaterialInterface>(nullptr, WorkSolidMaterialPath);
	}
	if (!Tools || !Solid)
	{
		return nullptr;
	}
	TArray<VisualMesh::FPart> Parts;
	VisualMesh::ReadParts(*Tools, "fellingAxe.visual", FVector2f(100.0f, 100.0f), Parts);
	if (Parts.IsEmpty())
	{
		UE_LOG(LogWorkTools, Warning, TEXT("WorkTools has no fellingAxe.visual.parts: no axe drawn"));
		return nullptr;
	}
	Axe = VisualMesh::MakeMesh(this, Solid, Parts, VisualMesh::EPartSet::All, nullptr, false);
	return Axe;
}

UAnimSequence* UWorkToolSubsystem::ChopClip()
{
	const int32 Which = FMath::Clamp(GWorkChopClip.GetValueOnGameThread(), 0, 1);
	if (!bClipTried[Which])
	{
		bClipTried[Which] = true;
		Clips[Which] = LoadObject<UAnimSequence>(nullptr, WorkChopClipPaths[Which]);
		if (!Clips[Which])
		{
			UE_LOG(LogWorkTools, Warning, TEXT("No chopping clip at %s (MakeHumanArt -Clips makes it); felling plays no clip"), WorkChopClipPaths[Which]);
		}
	}
	return Clips[Which];
}

void UWorkToolSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	TSet<TWeakObjectPtr<ASimPawnActor>> Felling;
	if (Bridge && Bridge->GetSim() && Pawns && GWorkTools.GetValueOnGameThread() != 0)
	{
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			if (!View.felling || View.downed || View.dead)
			{
				continue;
			}
			ASimPawnActor* Actor = Pawns->FindPawn(View.id);
			if (!Actor || Actor->IsRagdoll())
			{
				continue;
			}
			Felling.Add(Actor);
			// The chop on a loop.
			if (UAnimSequence* Clip = ChopClip(); Clip && !Actor->HasWorkLoop())
			{
				Actor->SetWorkLoop(Clip, FMath::Max(0.1f, GWorkChopRate.GetValueOnGameThread()));
				++ChopsStarted;
			}
			// The axe in the right fist, the handle out of it the way a blade is held.
			USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(*Actor);
			FTransform Fist;
			UStaticMesh* AxeShape = AxeMesh();
			if (!Mesh || !AxeShape || !HoldDraw::BladeFrame(Mesh, true, Fist))
			{
				continue;
			}
			TWeakObjectPtr<UStaticMeshComponent>& Slot = Axes.FindOrAdd(Actor);
			if (!Slot.IsValid())
			{
				UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor);
				Component->SetStaticMesh(AxeShape);
				Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Component->SetCastShadow(true);
				Component->RegisterComponent();
				Component->AttachToComponent(Mesh, FAttachmentTransformRules::KeepWorldTransform, FName(TEXT("hand_r")));
				Slot = Component;
				++AxesShown;
			}
			const FRotator Turn(GWorkAxePitch.GetValueOnGameThread(), GWorkAxeYaw.GetValueOnGameThread(), GWorkAxeRoll.GetValueOnGameThread());
			FTransform World = FTransform(Turn) * FTransform(Fist.GetRotation(), Fist.GetLocation());
			World.SetScale3D(FVector::OneVector);
			Slot->SetWorldTransform(World);
			Slot->SetVisibility(true);
		}
	}
	// Done felling (or gone): the axe goes and the clip blends out.
	for (auto It = Axes.CreateIterator(); It; ++It)
	{
		if (Felling.Contains(It->Key))
		{
			continue;
		}
		if (It->Value.IsValid())
		{
			It->Value->DestroyComponent();
		}
		if (ASimPawnActor* Actor = It->Key.Get())
		{
			Actor->StopWorkLoop();
		}
		It.RemoveCurrent();
	}
	// A felling pawn whose axe could not be drawn still loses its loop when it stops.
	if (Pawns)
	{
		for (const TWeakObjectPtr<ASimPawnActor>& Looping : Looped)
		{
			if (Looping.IsValid() && !Felling.Contains(Looping))
			{
				Looping->StopWorkLoop();
			}
		}
	}
	Looped = Felling;
}
