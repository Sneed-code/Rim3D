#include "AnimalActor.h"
#include "WaterDraw.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "AnimalSubsystem.h"
#include "SimSubsystem.h"
#include "ColonySim/Sim.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Snapshot.h"

namespace
{
	// How fast an animal turns to face where it walks, degrees per second at 1x.
	constexpr float AnimalTurnRate = 360.0f;
	// Slower than this (cm/s) an animal keeps facing the way it faced.
	constexpr float AnimalTurnSpeed = 15.0f;
	// Farther than this in one frame is a jump (spawn, load), not a walk.
	constexpr float AnimalTeleportDistance = 300.0f;
	// Above this speed (cm/s) an animal gallops instead of walking.
	constexpr float AnimalGallopSpeed = 280.0f;
	// Share of its adult size an animal is drawn at by life stage (baby, juvenile, adult).
	constexpr float AnimalStageScale[3] = {0.5f, 0.75f, 1.0f};
	// Moving slower than this (cm/s) counts as standing.
	constexpr float AnimalStandingSpeed = 8.0f;
	// Mid-distance animals update their pose this often (seconds).
	constexpr float AnimalMidTickInterval = 0.125f;
}

ASimAnimalActor::ASimAnimalActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->SetRelativeRotation(FRotator(0.0, kMeshYawOffset, 0.0));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->SetCanEverAffectNavigation(false);
	Body->bReceivesDecals = true;
	Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
}

void ASimAnimalActor::Setup(const FSimAnimalModel& InModel, float InArtScale, colony::Entity InThing)
{
	Model = InModel;
	ArtScale = InArtScale * InModel.FitScale;
	Thing = InThing;
	Body->SetSkeletalMesh(Model.Mesh);
	Body->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	for (USkeletalMesh* Piece : Model.Parts)
	{
		// Eyes, teeth, fur cards: drawn on the body's pose, no animation of their own.
		USkeletalMeshComponent* Part = NewObject<USkeletalMeshComponent>(this);
		Part->SetupAttachment(Body);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetGenerateOverlapEvents(false);
		Part->SetCanEverAffectNavigation(false);
		Part->SetSkeletalMesh(Piece);
		Part->RegisterComponent();
		Part->SetLeaderPoseComponent(Body);
	}
	if (Model.bFitFromPose && Model.Idle)
	{
		FitToPose(InArtScale);
	}
	Body->SetRelativeScale3D(FVector(ArtScale));
	DrawScale = ArtScale;
	PlaceBody(false);
}

void ASimAnimalActor::FitToPose(float InArtScale)
{
	// Pose the skeleton on the idle clip's first frame and measure how far its bones reach across the ground.
	Body->SetAnimation(Model.Idle);
	Body->SetPosition(0.0f, false);
	Body->TickAnimation(0.0f, false);
	Body->RefreshBoneTransforms();
	FBox Box(ForceInit);
	for (const FTransform& Bone : Body->GetComponentSpaceTransforms())
	{
		Box += Bone.GetLocation();
	}
	if (!Box.IsValid)
	{
		return;
	}
	const FVector Size = Box.GetSize();
	const double Span = FMath::Max(Size.X, Size.Y);
	if (Span < 0.01)
	{
		return;
	}
	// Bones stop short of the nose and the tail tip: they span about nine tenths of the body.
	const double Fit = Model.WantedLengthCm * 0.9 / Span * Model.SizeFix;
	ArtScale = static_cast<float>(InArtScale * Fit);
	PoseHalfSize = Size * 0.5;
	// The mesh's bounds are in the file's other unit: grow them to the posed size so the body is not culled.
	const double MeshSpan = FMath::Max(Model.Extent.X, Model.Extent.Y) * 2.0;
	Body->SetBoundsScale(static_cast<float>(FMath::Clamp(MeshSpan > 0.01 ? Span / MeshSpan : 1.0, 1.0, 1000.0)));
	static TSet<FName> Logged;
	if (!Logged.Contains(Model.Mesh->GetFName()))
	{
		Logged.Add(Model.Mesh->GetFName());
		UE_LOG(LogTemp, Display, TEXT("Animal %s fitted on its pose: bones span %.1f x %.1f x %.1f cm, fit %.3f"), *Model.Mesh->GetName(), Size.X, Size.Y, Size.Z, Fit);
	}
}

void ASimAnimalActor::PlaceBody(bool bOnSide)
{
	const FQuat Facing = FRotator(0.0, Model.YawOffset, 0.0).Quaternion();
	if (!bOnSide)
	{
		Body->SetRelativeLocationAndRotation(FVector::ZeroVector, Facing);
		return;
	}
	// Rolled onto its side about the actor's forward axis, lifted by half its width so it lies on the ground.
	const FVector Size = !PoseHalfSize.IsNearlyZero() ? PoseHalfSize : Model.Mesh ? Model.Mesh->GetBounds().BoxExtent : FVector(20.0);
	const FVector Side = Facing.RotateVector(FVector(Size.X, Size.Y, 0.0)).GetAbs();
	Body->SetRelativeLocationAndRotation(FVector(0.0, 0.0, Side.Y * DrawScale), FQuat(FVector::ForwardVector, UE_HALF_PI) * Facing);
}

float ASimAnimalActor::GetDrawLengthCm() const
{
	// A pose-fitted model's bones span about nine tenths of the body (FitToPose); the rest are measured on the mesh.
	const double Half = !PoseHalfSize.IsNearlyZero() ? FMath::Max(PoseHalfSize.X, PoseHalfSize.Y) / 0.9 : FMath::Max(Model.Extent.X, Model.Extent.Y);
	return static_cast<float>(Half * 2.0 * DrawScale);
}

const UAnimSequence* ASimAnimalActor::GetClip() const
{
	UAnimSingleNodeInstance* Single = Body->GetSingleNodeInstance();
	return Single ? Cast<UAnimSequence>(Single->GetCurrentAsset()) : nullptr;
}

void ASimAnimalActor::SetPose(ESimAnimalPose NewPose, float PlayRate)
{
	UAnimSingleNodeInstance* Single = Body->GetSingleNodeInstance();
	if (!Single)
	{
		return;
	}
	if (!bPoseSet || NewPose != Pose)
	{
		UAnimSequence* Clip = Model.Idle;
		bool bLoop = true;
		switch (NewPose)
		{
			case ESimAnimalPose::Walk: Clip = Model.Walk; break;
			case ESimAnimalPose::Gallop: Clip = Model.Gallop; break;
			case ESimAnimalPose::Eat: Clip = Model.Eat; break;
			case ESimAnimalPose::Dead: Clip = Model.Death; bLoop = false; break;
			default: break;
		}
		const bool bWasPlaced = bPoseSet;
		const bool bLieDown = NewPose == ESimAnimalPose::Dead && !Model.bHasDeath;
		if (bLieDown || (bPoseSet && Pose == ESimAnimalPose::Dead))
		{
			PlaceBody(bLieDown);
		}
		Pose = NewPose;
		bPoseSet = true;
		Single->SetAnimationAsset(Clip, bLoop);
		Single->SetPlaying(!bLieDown);
		if (NewPose == ESimAnimalPose::Dead && !bWasPlaced && Clip)
		{
			// It was already dead when we first saw it (a load): lie where it fell.
			Single->SetPosition(Clip->GetPlayLength(), false);
		}
		else if (!bWasPlaced && Clip)
		{
			// Not every animal of a herd in step.
			Single->SetPosition(FMath::FRand() * Clip->GetPlayLength(), false);
		}
	}
	Single->SetPlayRate(NewPose == ESimAnimalPose::Sleep ? 0.15f : PlayRate);
}

void ASimAnimalActor::UpdateFromSim(const USimSubsystem& Bridge, const colony::ThingView& Now, const colony::ThingView* Before,
	float DeltaTime, int32 Speed)
{
	FVector Location;
	if (!Bridge.GetBlendedLocation(Thing, Location))
	{
		return;
	}
	const bool bTeleported = !bPlaced || FVector::Dist(Location, GetActorLocation()) > AnimalTeleportDistance;
	bPlaced = true;

	const float Alpha = Bridge.GetBlendAlpha();
	const float Vx = Before ? FMath::Lerp(Before->vx, Now.vx, Alpha) : Now.vx;
	const float Vy = Before ? FMath::Lerp(Before->vy, Now.vy, Alpha) : Now.vy;
	const FVector Velocity(Vx * colony::kStepsPerRealSecondAt1x * 100.0f, Vy * colony::kStepsPerRealSecondAt1x * 100.0f, 0.0f);
	const float Moving = Velocity.Size2D();

	if (Moving > AnimalTurnSpeed)
	{
		const float Wanted = Velocity.Rotation().Yaw;
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, AnimalTurnRate * DeltaTime * FMath::Max(Speed, 1));
	}
	else if (bTeleported && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		Yaw = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
	}
	// Swimming: the body floats with its back at the surface (docs/design/water.md, W4).
	SwimLiftCm = WaterDraw::SwimLiftEase(SwimLiftCm, Bridge.GetSim() ? WaterDraw::SwimLiftTargetCm(Now, Bridge.GetSim()->bodyHeightM(Thing)) : 0.0f, DeltaTime, bTeleported);
	Location.Z += SwimLiftCm;
	SetActorLocationAndRotation(Location, FRotator(0.0, Yaw, 0.0));

	const int32 Stage = FMath::Clamp(Now.lifeStage, 0, 2);
	DrawScale = ArtScale * AnimalStageScale[Stage];
	Body->SetRelativeScale3D(FVector(DrawScale));

	// Animation runs at the game's speed and stops while paused.
	Body->GlobalAnimRateScale = static_cast<float>(Speed);

	if (Now.dead)
	{
		SetPose(ESimAnimalPose::Dead, 1.0f);
	}
	else if (Now.asleep)
	{
		SetPose(ESimAnimalPose::Sleep, 1.0f);
	}
	else if (Moving > AnimalGallopSpeed)
	{
		SetPose(ESimAnimalPose::Gallop, FMath::Clamp(Moving / (Model.GallopClipSpeed * DrawScale), 0.5f, 2.0f));
	}
	else if (Moving > AnimalStandingSpeed)
	{
		SetPose(ESimAnimalPose::Walk, FMath::Clamp(Moving / (Model.WalkClipSpeed * DrawScale), 0.4f, 2.0f));
	}
	else if (Before && Now.food > Before->food + 1.0e-5f)
	{
		SetPose(ESimAnimalPose::Eat, 1.0f);
	}
	else
	{
		SetPose(ESimAnimalPose::Idle, 1.0f);
	}
}

void ASimAnimalActor::SetDetail(ESimAnimalDetail InDetail)
{
	if (InDetail == Detail && bPoseSet)
	{
		return;
	}
	Detail = InDetail;
	const bool bFarOut = Detail == ESimAnimalDetail::Hidden;
	SetActorHiddenInGame(bFarOut);
	// A frozen pose costs no animation; hidden ones cost nothing at all.
	Body->bNoSkeletonUpdate = Detail == ESimAnimalDetail::Far || bFarOut;
	Body->PrimaryComponentTick.TickInterval = Detail == ESimAnimalDetail::Mid ? AnimalMidTickInterval : 0.0f;
}
