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

#include "PawnLooks.h"
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
	// Where horns grow, in the MetaHuman's mesh space (cm; +Y forward, +Z up): the top of the
	// forehead, measured on Kellan's head (its mesh spans z 135 to 173, y -10 to 14).
	constexpr float PawnHornForward = 6.5f;
	constexpr float PawnHornUp = 166.0f;
	// Horns lean outwards by this much (degrees).
	constexpr float PawnHornTilt = 22.0f;
	// The horn mesh is 1 cm long with a 0.1 cm base radius (the PawnLooks commandlet makes it).
	constexpr float PawnHornMeshRadius = 0.1f;
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
