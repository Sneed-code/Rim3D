#include "PawnActor.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Snapshot.h"

namespace
{
	// How fast a pawn turns to face where it walks, degrees per second at 1x.
	constexpr float PawnTurnRate = 720.0f;
	// Slower than this (cm/s) a pawn keeps facing the way it faced.
	constexpr float PawnTurnSpeed = 30.0f;
	// Farther than this in one frame is a jump (spawn, load), not a walk.
	constexpr float PawnTeleportDistance = 300.0f;
	// The action animation: blend in, hold, blend out (seconds at 1x).
	constexpr float PawnActionBlendIn = 0.25f;
	constexpr float PawnActionHold = 0.35f;
	constexpr float PawnActionBlendOut = 0.35f;
}

ASimPawnActor::ASimPawnActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	// The mannequin faces +Y in its own space.
	Body->SetRelativeRotation(FRotator(0.0, -90.0, 0.0));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->SetCanEverAffectNavigation(false);
	Body->bReceivesDecals = false;
	Body->SetAnimInstanceClass(USimPawnAnimInstance::StaticClass());
}

void ASimPawnActor::Setup(USimPawnSubsystem& Pawns, colony::Entity InThing, float InCruiseSpeed, bool bMetaHuman)
{
	Thing = InThing;
	CruiseSpeed = InCruiseSpeed;
	Body->SetSkeletalMesh(Pawns.GetBodyMesh());
	Body->SetAnimInstanceClass(USimPawnAnimInstance::StaticClass());
	if (USimPawnAnimInstance* Anim = GetAnim())
	{
		Anim->SetAssets(Pawns.GetDatabases(), Pawns.GetIdleLoop(), Pawns.GetWalkLoop(), Pawns.GetWalkLoopSpeed(),
			Pawns.GetRunLoop(), Pawns.GetRunLoopSpeed());
	}

	UClass* MetaHumanClass = bMetaHuman ? Pawns.GetMetaHumanClass() : nullptr;
	if (MetaHumanClass)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		MetaHuman = GetWorld()->SpawnActor<AActor>(MetaHumanClass, GetActorTransform(), Params);
	}
	if (MetaHuman)
	{
		// The hidden mannequin keeps animating; the MetaHuman body copies its pose.
		Body->SetVisibility(false, false);
		Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		MetaHuman->AttachToComponent(Body, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		TArray<USceneComponent*> Components;
		MetaHuman->GetComponents(Components);
		for (USceneComponent* Component : Components)
		{
			if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Component); Mesh && Mesh->GetName() == TEXT("Body"))
			{
				Mesh->SetLeaderPoseComponent(Body);
			}
			// Strand hair (HairStrands plugin), found by class name so the module needn't be linked.
			if (Component->GetClass()->GetName() == TEXT("GroomComponent"))
			{
				Grooms.Add(Component);
			}
		}
	}
}

USimPawnAnimInstance* ASimPawnActor::GetAnim() const
{
	return Cast<USimPawnAnimInstance>(Body->GetAnimInstance());
}

void ASimPawnActor::UpdateFromSim(const USimSubsystem& Bridge, const colony::ThingView& Now, const colony::ThingView* Before,
	float DeltaTime, int32 Speed)
{
	FVector Location;
	if (!Bridge.GetBlendedLocation(Thing, Location))
	{
		return;
	}
	const bool bTeleported = !bPlaced || FVector::Dist(Location, GetActorLocation()) > PawnTeleportDistance;
	bPlaced = true;

	// Metres per step to cm per second at 1x, blended like the position.
	const float Alpha = Bridge.GetBlendAlpha();
	const float Vx = Before ? FMath::Lerp(Before->vx, Now.vx, Alpha) : Now.vx;
	const float Vy = Before ? FMath::Lerp(Before->vy, Now.vy, Alpha) : Now.vy;
	const FVector Velocity(Vx * colony::kStepsPerRealSecondAt1x * 100.0f, Vy * colony::kStepsPerRealSecondAt1x * 100.0f, 0.0f);

	if (Velocity.Size2D() > PawnTurnSpeed)
	{
		const float Wanted = Velocity.Rotation().Yaw;
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * Speed);
	}
	SetActorLocationAndRotation(Location, FRotator(0.0, Yaw, 0.0));

	// Animation runs at the game's speed and stops while paused.
	Body->GlobalAnimRateScale = static_cast<float>(Speed);
	if (USimPawnAnimInstance* Anim = GetAnim())
	{
		FSimPawnAnimInputs& In = Anim->Inputs;
		In.Location = Location;
		In.Yaw = Yaw;
		In.Velocity = Velocity;
		In.CruiseSpeed = CruiseSpeed;
		In.Detail = Detail;
		In.bTeleported = bTeleported;
		In.Route.Reset();
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		for (uint32 i = 0; i < Now.waypointCount && In.Route.Num() < 8; ++i)
		{
			const colony::PathPointView& Point = Snapshot.waypoints[Now.firstWaypoint + i];
			In.Route.Add(Bridge.SimToWorld(Point.x, Point.y, Now.level));
		}

		if (ActionEndSeconds > 0.0)
		{
			ActionEndSeconds -= DeltaTime * Speed;
			if (ActionEndSeconds <= 0.0)
			{
				Anim->StopSlotAnimation(PawnActionBlendOut, FAnimSlotGroup::DefaultSlotName);
				ActionEndSeconds = 0.0;
			}
		}
	}
}

void ASimPawnActor::SetDetail(ESimPawnDetail InDetail, float TickInterval)
{
	if (Detail != InDetail)
	{
		Detail = InDetail;
		for (USceneComponent* Groom : Grooms)
		{
			if (Groom)
			{
				Groom->SetVisibility(Detail == ESimPawnDetail::Full);
			}
		}
	}
	if (!FMath::IsNearlyEqual(Body->GetComponentTickInterval(), TickInterval))
	{
		Body->SetComponentTickInterval(TickInterval);
	}
}

void ASimPawnActor::PlayAction(colony::PawnAction Action)
{
	USimPawnAnimInstance* Anim = GetAnim();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	UAnimSequence* Animation = Pawns ? Pawns->GetActionAnimation(Action) : nullptr;
	if (!Anim || !Animation)
	{
		return;
	}
	if (Anim->PlaySlotAnimationAsDynamicMontage(Animation, FAnimSlotGroup::DefaultSlotName, PawnActionBlendIn, PawnActionBlendOut))
	{
		ActionEndSeconds = PawnActionBlendIn + PawnActionHold;
		++ActionsPlayed;
	}
}

void ASimPawnActor::SetBodyHidden(bool bHide)
{
	SetActorHiddenInGame(bHide);
	if (MetaHuman)
	{
		MetaHuman->SetActorHiddenInGame(bHide);
	}
}

bool ASimPawnActor::IsActionPlaying() const
{
	const USimPawnAnimInstance* Anim = GetAnim();
	return Anim && Anim->IsAnyMontagePlaying();
}

int32 ASimPawnActor::CountVisibleGrooms() const
{
	int32 Visible = 0;
	for (const USceneComponent* Groom : Grooms)
	{
		Visible += Groom && Groom->IsVisible() ? 1 : 0;
	}
	return Visible;
}

void ASimPawnActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsValid(MetaHuman))
	{
		MetaHuman->Destroy();
	}
	Super::EndPlay(EndPlayReason);
}
