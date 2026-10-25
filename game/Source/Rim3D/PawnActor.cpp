#include "PawnActor.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/PhysicsAsset.h"

#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
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
	// Where horns grow, in the MetaHuman's mesh space (cm; +Y forward, +Z up): the top of the
	// forehead, measured on Kellan's head (its mesh spans z 135 to 173, y -10 to 14).
	constexpr float PawnHornForward = 6.5f;
	constexpr float PawnHornUp = 166.0f;
	// Horns lean outwards by this much (degrees).
	constexpr float PawnHornTilt = 22.0f;
	// The horn mesh is 1 cm long with a 0.1 cm base radius (the PawnLooks commandlet makes it).
	constexpr float PawnHornMeshRadius = 0.1f;
	// A hit leans the pawn this far at most (degrees).
	constexpr float PawnHitLeanDegrees = 14.0f;
	// A ragdoll's floor: a slab this many metres across and thick, its top at the ground.
	constexpr float PawnRagdollFloorSize = 10.0f;
	constexpr float PawnRagdollFloorThickness = 0.5f;
	const TCHAR* const PawnRagdollPhysicsAssetPath = TEXT("/Game/Characters/UEFN_Mannequin/Rigs/PA_UEFN_Mannequin.PA_UEFN_Mannequin");
	const TCHAR* const PawnCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	// The mannequin faces +Y in its own space.
	const FRotator PawnBodyRelativeRotation(0.0, -90.0, 0.0);
}

ASimPawnActor::ASimPawnActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	// The mannequin faces +Y in its own space.
	Body->SetRelativeRotation(PawnBodyRelativeRotation);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->SetCanEverAffectNavigation(false);
	Body->bReceivesDecals = true;  // wound decals (docs/design/fights.md)
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
			if (USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Component))
			{
				if (Mesh->GetName() == TEXT("Body"))
				{
					Mesh->SetLeaderPoseComponent(Body);
					MetaHumanBody = Mesh;
				}
				// Seeded looks: the copy of each mesh with the body shape morphs, and our skin
				// material on the skin slots (docs/design/looks.md).
				if (USkeletalMesh* Shaped = Pawns.GetMorphMesh(Mesh->GetSkeletalMeshAsset()))
				{
					Mesh->SetSkeletalMeshAsset(Shaped);
					ShapedMeshes.Add(Mesh);
				}
				bool bSkin = false;
				for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
				{
					// The MetaHuman Blueprint gives the face dynamic instances; ours replace what they were made from.
					UMaterialInterface* Current = Mesh->GetMaterial(Slot);
					if (const UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Current))
					{
						Current = Dynamic->Parent;
					}
					if (UMaterialInterface* Skin = Pawns.GetSkinMaterial(Current))
					{
						Mesh->SetMaterial(Slot, Skin);
						bSkin = true;
					}
					else if (UMaterialInterface* Cloth = Pawns.GetClothMaterial(Current))
					{
						Mesh->SetMaterial(Slot, Cloth);
					}
				}
				if (bSkin)
				{
					SkinMeshes.Add(Mesh);
				}
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
	else if ((Now.shotReadyIn > 0 || Now.fighting) && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		// Standing and shooting: face where the sim says the pawn aims (docs/design/fights.md).
		const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
	}
	SetActorLocationAndRotation(Location, FRotator(0.0, Yaw, 0.0));
	if (bRagdoll)
	{
		return;  // the physics body is on its own until the pawn gets up
	}
	ApplyLean(DeltaTime * Speed);

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

void ASimPawnActor::ApplyLooks(USimPawnSubsystem& Pawns, const FSimPawnLooks& InLooks)
{
	if (bHasLooks && Looks == InLooks)
	{
		return;
	}
	Looks = InLooks;
	bHasLooks = true;

	// Height: the whole pawn, MetaHuman and horns included, scales with the mannequin.
	Body->SetRelativeScale3D(FVector(Looks.Height));

	for (USkeletalMeshComponent* Mesh : ShapedMeshes)
	{
		for (const TPair<FName, float>& Morph : Looks.MorphWeights())
		{
			Mesh->SetMorphTarget(Morph.Key, Morph.Value);
		}
	}
	const FVector Skin(Looks.Skin.R, Looks.Skin.G, Looks.Skin.B);
	for (USkeletalMeshComponent* Mesh : SkinMeshes)
	{
		Mesh->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, Skin);
	}
	// Strand hair, eyebrows and beards: the MetaHuman hair material is coloured by melanin and redness.
	const float Lightness = Looks.Hair.GetLuminance();
	const float Melanin = FMath::Clamp(1.0f - FMath::Sqrt(Lightness) * 1.6f, 0.0f, 1.0f);
	const float Redness = FMath::Clamp((Looks.Hair.R - Looks.Hair.B) / FMath::Max(Looks.Hair.R, 0.01f), 0.0f, 1.0f) * 0.8f;
	for (USceneComponent* Groom : Grooms)
	{
		if (UMeshComponent* Mesh = Cast<UMeshComponent>(Groom); Mesh && Groom->GetName() != TEXT("Fuzz") && Groom->GetName() != TEXT("Eyelashes"))
		{
			Mesh->SetScalarParameterValueOnMaterials(TEXT("HairMelanin"), Melanin);
			Mesh->SetScalarParameterValueOnMaterials(TEXT("HairRedness"), Redness);
		}
	}

	// Horns on the head bone of whichever body shows, placed in its reference pose.
	for (UStaticMeshComponent* Horn : Horns)
	{
		if (Horn)
		{
			Horn->DestroyComponent();
		}
	}
	Horns.Reset();
	UStaticMesh* HornMesh = Looks.HornLength > 0.0f ? Pawns.GetHornMesh() : nullptr;
	USkeletalMeshComponent* Head = MetaHumanBody ? MetaHumanBody.Get() : Body.Get();
	const USkeletalMesh* HeadMesh = Head ? Head->GetSkeletalMeshAsset() : nullptr;
	const int32 HeadBone = HeadMesh ? HeadMesh->GetRefSkeleton().FindBoneIndex(TEXT("head")) : INDEX_NONE;
	if (HornMesh && HeadBone != INDEX_NONE)
	{
		const FTransform HeadRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(HeadMesh->GetRefSkeleton(), HeadBone);
		const float Thickness = Looks.HornRadius / PawnHornMeshRadius;
		for (const float Side : {-1.0f, 1.0f})
		{
			UStaticMeshComponent* Horn = NewObject<UStaticMeshComponent>(this);
			Horn->SetStaticMesh(HornMesh);
			Horn->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Horn->SetCanEverAffectNavigation(false);
			Horn->SetCastShadow(true);
			const FTransform InMesh(FRotator(-Side * PawnHornTilt, 0.0, 0.0), FVector(Side * Looks.HornSpread, PawnHornForward, PawnHornUp),
				FVector(Thickness, Thickness, Looks.HornLength));
			Horn->SetupAttachment(Head, TEXT("head"));
			Horn->SetRelativeTransform(InMesh.GetRelativeTransform(HeadRef));
			Horn->RegisterComponent();
			Horn->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, FVector(Looks.HornColor.R, Looks.HornColor.G, Looks.HornColor.B));
			Horns.Add(Horn);
		}
	}
}

void ASimPawnActor::PlayHitReaction(const FVector& ShotDirection, float Strength)
{
	ReactionDirection = FVector(ShotDirection.X, ShotDirection.Y, 0.0).GetSafeNormal();
	if (ReactionDirection.IsNearlyZero())
	{
		ReactionDirection = GetActorForwardVector();
	}
	ReactionStrength = FMath::Clamp(Strength, 0.0f, 1.0f);
	ReactionAge = 0.0f;
	++HitReactions;
}

void ASimPawnActor::ApplyLean(float DeltaTime)
{
	if (ReactionAge >= kHitReactionSeconds)
	{
		if (LeanDegrees != 0.0f)
		{
			LeanDegrees = 0.0f;
			Body->SetRelativeRotation(PawnBodyRelativeRotation);
		}
		return;
	}
	ReactionAge += DeltaTime;
	const float T = FMath::Clamp(ReactionAge / kHitReactionSeconds, 0.0f, 1.0f);
	// Snaps over in the first 15%, then settles back.
	const float Envelope = T < 0.15f ? T / 0.15f : FMath::Square(1.0f - (T - 0.15f) / 0.85f);
	LeanDegrees = PawnHitLeanDegrees * ReactionStrength * Envelope;
	// The top of the body goes along the shot: turn about up x direction.
	const FVector Axis = FVector::CrossProduct(FVector::UpVector, ReactionDirection).GetSafeNormal();
	const FQuat LeanWorld(Axis, FMath::DegreesToRadians(LeanDegrees));
	const FQuat RootQuat = GetActorQuat();
	Body->SetRelativeRotation(RootQuat.Inverse() * LeanWorld * RootQuat * PawnBodyRelativeRotation.Quaternion());
}

void ASimPawnActor::SpawnRagdollFloor()
{
	if (RagdollFloor)
	{
		return;
	}
	const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, PawnCubePath);
	if (!Bridge || !Bridge->GetSim() || !Cube)
	{
		return;
	}
	// Flat to the ground under the pawn: the normal from the height map around it.
	const FVector Here = (GetActorLocation() - Bridge->GetWorldOrigin()) / 100.0;
	const colony::Map& Map = Bridge->GetSim()->map();
	const float X = static_cast<float>(Here.X);
	const float Y = static_cast<float>(Here.Y);
	const FVector Normal = FVector((Map.heightAt(X - 1.0f, Y) - Map.heightAt(X + 1.0f, Y)) * 0.5f, (Map.heightAt(X, Y - 1.0f) - Map.heightAt(X, Y + 1.0f)) * 0.5f, 1.0f).GetSafeNormal();
	const FVector Ground = Bridge->SimToWorld(X, Y, 0);

	RagdollFloor = NewObject<UStaticMeshComponent>(this, TEXT("RagdollFloor"));
	RagdollFloor->SetStaticMesh(Cube);
	RagdollFloor->SetMobility(EComponentMobility::Movable);
	RagdollFloor->SetCollisionProfileName(TEXT("BlockAll"));
	RagdollFloor->SetCollisionObjectType(ECC_WorldStatic);
	RagdollFloor->SetCanEverAffectNavigation(false);
	RagdollFloor->SetCastShadow(false);
	RagdollFloor->SetVisibility(false);
	RagdollFloor->SetHiddenInGame(true);
	RagdollFloor->SetWorldScale3D(FVector(PawnRagdollFloorSize, PawnRagdollFloorSize, PawnRagdollFloorThickness));
	RagdollFloor->RegisterComponent();
	RagdollFloor->SetWorldLocationAndRotation(Ground - Normal * (PawnRagdollFloorThickness * 50.0f), FRotationMatrix::MakeFromZ(Normal).ToQuat());
}

void ASimPawnActor::StartRagdoll(const FVector& Impulse)
{
	if (bRagdoll || !Body->GetSkeletalMeshAsset())
	{
		return;
	}
	if (!Body->GetPhysicsAsset())
	{
		if (UPhysicsAsset* Asset = LoadObject<UPhysicsAsset>(nullptr, PawnRagdollPhysicsAssetPath))
		{
			Body->SetPhysicsAsset(Asset, true);
		}
	}
	if (!Body->GetPhysicsAsset())
	{
		return;
	}
	SpawnRagdollFloor();
	bRagdoll = true;
	++RagdollsStarted;
	ReactionAge = kHitReactionSeconds;
	LeanDegrees = 0.0f;
	Body->SetRelativeRotation(PawnBodyRelativeRotation);
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Body->SetCollisionProfileName(TEXT("Ragdoll"));
	Body->SetAllBodiesSimulatePhysics(true);
	Body->SetAllBodiesPhysicsBlendWeight(1.0f);
	Body->bBlendPhysics = true;
	Body->WakeAllRigidBodies();
	if (!Impulse.IsNearlyZero())
	{
		Body->AddImpulse(Impulse, NAME_None, true);
	}
}

void ASimPawnActor::StopRagdoll()
{
	if (!bRagdoll)
	{
		return;
	}
	bRagdoll = false;
	Body->SetAllBodiesSimulatePhysics(false);
	Body->bBlendPhysics = false;
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->AttachToComponent(Root, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	Body->SetRelativeLocationAndRotation(FVector::ZeroVector, PawnBodyRelativeRotation);
	Body->SetRelativeScale3D(FVector(bHasLooks ? Looks.Height : 1.0f));
	if (RagdollFloor)
	{
		RagdollFloor->DestroyComponent();
		RagdollFloor = nullptr;
	}
}

bool ASimPawnActor::GetRagdollPelvis(FVector& OutLocation) const
{
	if (!bRagdoll)
	{
		return false;
	}
	OutLocation = Body->DoesSocketExist(TEXT("pelvis")) ? Body->GetSocketLocation(TEXT("pelvis")) : Body->GetComponentLocation();
	return true;
}

USkeletalMeshComponent* ASimPawnActor::GetDecalMesh() const
{
	return MetaHumanBody ? MetaHumanBody.Get() : Body.Get();
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
	if (RagdollFloor)
	{
		RagdollFloor->DestroyComponent();
		RagdollFloor = nullptr;
	}
	if (IsValid(MetaHuman))
	{
		MetaHuman->Destroy();
	}
	Super::EndPlay(EndPlayReason);
}
