#include "AnimalActor.h"
#include "WaterDraw.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

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
	const TArray<FTransform>& Bones = Body->GetComponentSpaceTransforms();
	if (Model.bRecentre && Bones.Num() >= 8)
	{
		// The body is far from the file's origin but its top bones stay there (the leopard's RL_BoneRoot and pivots):
		// measure only the bones near the skeleton's middle (the median, per axis), within three times their median
		// distance from it, so a few far-off bones don't stretch the box.
		TArray<double> Xs, Ys, Zs;
		for (const FTransform& Bone : Bones)
		{
			Xs.Add(Bone.GetLocation().X);
			Ys.Add(Bone.GetLocation().Y);
			Zs.Add(Bone.GetLocation().Z);
		}
		Xs.Sort();
		Ys.Sort();
		Zs.Sort();
		const FVector Middle(Xs[Xs.Num() / 2], Ys[Ys.Num() / 2], Zs[Zs.Num() / 2]);
		TArray<double> Distances;
		for (const FTransform& Bone : Bones)
		{
			Distances.Add(FVector::Dist(Bone.GetLocation(), Middle));
		}
		Distances.Sort();
		const double Reach = FMath::Max(3.0 * Distances[Distances.Num() / 2], 1.0);
		for (const FTransform& Bone : Bones)
		{
			if (FVector::Dist(Bone.GetLocation(), Middle) <= Reach)
			{
				Box += Bone.GetLocation();
			}
		}
	}
	else
	{
		for (const FTransform& Bone : Bones)
		{
			Box += Bone.GetLocation();
		}
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
	if (Model.bRecentre)
	{
		PoseCentre = FVector(Box.GetCenter().X, Box.GetCenter().Y, 0.0);
	}
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
	bBodyOnSide = bOnSide;
	if (!bOnSide)
	{
		Body->SetRelativeLocationAndRotation(BodyOffset(Facing), Facing);
		return;
	}
	// Rolled onto its side about the actor's forward axis, lifted by half its width so it lies on the ground.
	const FVector Size = !PoseHalfSize.IsNearlyZero() ? PoseHalfSize : Model.Mesh ? Model.Mesh->GetBounds().BoxExtent : FVector(20.0);
	const FVector Side = Facing.RotateVector(FVector(Size.X, Size.Y, 0.0)).GetAbs();
	const FQuat OnSide = FQuat(FVector::ForwardVector, UE_HALF_PI) * Facing;
	Body->SetRelativeLocationAndRotation(FVector(0.0, 0.0, Side.Y * DrawScale) - OnSide.RotateVector(PoseCentre * DrawScale), OnSide);
}

FVector ASimAnimalActor::BodyOffset(const FQuat& Facing) const
{
	return PoseCentre.IsNearlyZero() ? FVector::ZeroVector : -Facing.RotateVector(PoseCentre * DrawScale);
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
			case ESimAnimalPose::Attack: Clip = Model.Attack ? Model.Attack.Get() : Model.Idle.Get(); bLoop = false; break;
			case ESimAnimalPose::Shoot: Clip = Model.Shoot ? Model.Shoot.Get() : Model.Idle.Get(); bLoop = false; break;
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
		else if (NewPose == ESimAnimalPose::Attack || NewPose == ESimAnimalPose::Shoot)
		{
			Single->SetPosition(0.0f, false);
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

	// A war machine (no life stage) is drawn full size.
	const int32 Stage = Now.lifeStage < 0 ? 2 : FMath::Clamp(Now.lifeStage, 0, 2);
	DrawScale = ArtScale * AnimalStageScale[Stage];
	Body->SetRelativeScale3D(FVector(DrawScale));
	if (!PoseCentre.IsNearlyZero() && !bBodyOnSide)
	{
		Body->SetRelativeLocation(BodyOffset(FRotator(0.0, Model.YawOffset, 0.0).Quaternion()));
	}

	// Animation runs at the game's speed and stops while paused.
	Body->GlobalAnimRateScale = static_cast<float>(Speed);

	// Fight clips (the remnants): a strike as a melee swing winds up, a shot as the gun fires (its cooldown starts
	// again); each plays once over whatever the body was doing, then the usual pose comes back.
	const float GameTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	if (!Now.dead && !Now.downed && Before)
	{
		const bool bStrike = Model.Attack && Now.meleePhase == colony::MeleePhase::Windup && Before->meleePhase != colony::MeleePhase::Windup;
		const bool bShot = Model.Shoot && Now.shotReadyIn > Before->shotReadyIn;
		if (bStrike || bShot)
		{
			UAnimSequence* Clip = bStrike ? Model.Attack.Get() : Model.Shoot.Get();
			OneShotStart = GameTime;
			OneShotLength = Clip->GetPlayLength() / FMath::Max(static_cast<float>(Speed), 1.0f);
			const ESimAnimalPose Wanted = bStrike ? ESimAnimalPose::Attack : ESimAnimalPose::Shoot;
			if (bPoseSet && Pose == Wanted)
			{
				Pose = ESimAnimalPose::Idle;  // so SetPose starts the clip again
			}
			SetPose(Wanted, 1.0f);
		}
	}
	if (OneShotStart >= 0.0f && !Now.dead && !Now.downed && GameTime - OneShotStart < OneShotLength)
	{
		return;  // the strike or shot plays out
	}
	OneShotStart = -1.0f;
	if (Now.dead || (Now.lifeStage < 0 && Now.downed))
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
