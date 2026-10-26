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
	else if ((Now.shotReadyIn > 0 || Now.fighting || Now.meleePhase != colony::MeleePhase::Ready || Now.blocking) && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		// Standing and shooting or swinging: face where the sim says the pawn aims (docs/design/fights.md).
		const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
	}
	SetActorLocationAndRotation(Location, FRotator(0.0, Yaw, 0.0));
	if (bRagdoll)
	{
		return;  // the physics body is on its own until the pawn gets up
	}
	StanceHeight = Now.stance == colony::Stance::Crouching ? 0.66f : (Now.stance == colony::Stance::Prone ? 0.24f : 1.0f);
	// A dodge roll (P2): the sim has the pawn start up, roll for 15 steps and recover (27 in all, `rollStepsLeft` counting down);
	// the body turns over once about its middle across the rolling steps, fitted to those steps.
	const float WasRolling = RollProgress;
	if (Now.rollPhase > 0)
	{
		RollProgress = FMath::Clamp(1.0f - static_cast<float>(Now.rollStepsLeft) / 27.0f, 0.001f, 1.0f);
		RollDirection = FVector(Now.rollDirX, Now.rollDirY, 0.0f).GetSafeNormal();
		const float Turn = FMath::Clamp((RollProgress - 3.0f / 27.0f) / (15.0f / 27.0f), 0.0f, 1.0f);
		RollAngle = 360.0f * Turn * Turn * (3.0f - 2.0f * Turn);
		RollsSeen += WasRolling <= 0.0f ? 1 : 0;
	}
	else
	{
		RollProgress = 0.0f;
		RollAngle = 0.0f;
	}

	// The hit-stop holds the swing, the flinch and the animation for a moment of real time.
	float GameDelta = DeltaTime * Speed;
	if (HitStopLeft > 0.0f)
	{
		HitStopLeft = FMath::Max(0.0f, HitStopLeft - (Speed > 0 ? DeltaTime : 0.0f));
		GameDelta = 0.0f;
	}
	// A swing on screen that the sim no longer has (the attacker went down, lost its weapon): recover.
	if (Swing.IsWinding() && Now.meleePhase != colony::MeleePhase::Windup)
	{
		SwingMismatch += DeltaTime;
		if (SwingMismatch > 0.25f)
		{
			Swing.Abort();
		}
	}
	else
	{
		SwingMismatch = 0.0f;
	}
	ApplyBodyPose(GameDelta, Now.blocking && !Now.downed && !Now.dead);

	// Animation runs at the game's speed and stops while paused (and during a hit-stop).
	Body->GlobalAnimRateScale = HitStopLeft > 0.0f ? 0.0f : static_cast<float>(Speed);
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
				// Hair under a helmet or a tuque stays hidden at every detail.
				Groom->SetVisibility(Detail == ESimPawnDetail::Full && !(bHairHidden && Groom->GetName().Contains(TEXT("Hair"))));
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

	// The clothes and the skin under them share the body's shape and the pawn's skin colour.
	for (USkeletalMeshComponent* Layer : BareLayer)
	{
		Layer->SetCustomPrimitiveDataVector3(0, FVector(Looks.Skin.R, Looks.Skin.G, Looks.Skin.B));
		for (const TPair<FName, float>& Morph : Looks.MorphWeights())
		{
			Layer->SetMorphTarget(Morph.Key, Morph.Value);
		}
	}
	for (UMeshComponent* Worn : WornMeshes)
	{
		if (USkeletalMeshComponent* Skinned = Cast<USkeletalMeshComponent>(Worn))
		{
			for (const TPair<FName, float>& Morph : Looks.MorphWeights())
			{
				Skinned->SetMorphTarget(Morph.Key, Morph.Value);
			}
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

// ---------------------------------------------------------------------------
// Worn clothes (docs/design/clothes-draw.md).

namespace
{
	const TCHAR* const PawnBareMeshes[] = {
		TEXT("/Game/Rim3D/Generated/Clothes/SK_Body_Bare_0.SK_Body_Bare_0"),
		TEXT("/Game/Rim3D/Generated/Clothes/SK_Body_Bare_1.SK_Body_Bare_1"),
	};
	// The garment material's custom primitive data: the cloth's colour (RGB) at 0 and the wear at 3.
	constexpr int32 PawnClothColorIndex = 0;
	constexpr int32 PawnClothWearIndex = 3;

	/** The colour a garment mesh has by itself (its material instance's `Color`), for pieces with no stuff. */
	FLinearColor PawnOwnColor(const UMeshComponent* Mesh)
	{
		FLinearColor Color(0.3f, 0.3f, 0.3f);
		if (const UMaterialInterface* Material = Mesh ? Mesh->GetMaterial(0) : nullptr)
		{
			Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Color")), Color);
		}
		return Color;
	}
}

void ASimPawnActor::EnsureBareLayer(USimPawnSubsystem& Pawns)
{
	if (bBareLayerTried || !MetaHuman || !MetaHumanBody)
	{
		return;
	}
	bBareLayerTried = true;
	for (USkeletalMeshComponent* Mesh : ShapedMeshes)
	{
		const FString Name = Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetName() : FString();
		if (Name.Contains(TEXT("hoodie")))
		{
			DefaultTop = Mesh;
		}
		else if (Name.Contains(TEXT("cargopants")))
		{
			DefaultTrousers = Mesh;
		}
	}
	if (!DefaultTop || !DefaultTrousers)
	{
		return;
	}
	USkinnedMeshComponent* Leader = DefaultTop->LeaderPoseComponent.IsValid() ? DefaultTop->LeaderPoseComponent.Get() : MetaHumanBody.Get();
	for (const TCHAR* Path : PawnBareMeshes)
	{
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Pawns.GetClothesAsset(Path));
		if (!Mesh)
		{
			// Without the skin layer Kellan's own clothes must stay: nothing else covers the body.
			for (USkeletalMeshComponent* Layer : BareLayer)
			{
				Layer->DestroyComponent();
			}
			BareLayer.Reset();
			return;
		}
		USkeletalMeshComponent* Layer = NewObject<USkeletalMeshComponent>(MetaHuman);
		Layer->SetSkeletalMeshAsset(Mesh);
		Layer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Layer->SetCanEverAffectNavigation(false);
		Layer->SetupAttachment(MetaHuman->GetRootComponent());
		Layer->RegisterComponent();
		Layer->SetLeaderPoseComponent(Leader);
		const FLinearColor Skin = bHasLooks ? Looks.Skin : FLinearColor(0.4f, 0.28f, 0.22f);
		Layer->SetCustomPrimitiveDataVector3(PawnClothColorIndex, FVector(Skin.R, Skin.G, Skin.B));
		Layer->SetCustomPrimitiveDataFloat(PawnClothWearIndex, 0.0f);
		if (bHasLooks)
		{
			for (const TPair<FName, float>& Morph : Looks.MorphWeights())
			{
				Layer->SetMorphTarget(Morph.Key, Morph.Value);
			}
		}
		BareLayer.Add(Layer);
	}
	// The skin is there now: Kellan's own clothes go.
	if (DefaultTop)
	{
		DefaultTop->SetVisibility(false);
	}
	if (DefaultTrousers)
	{
		DefaultTrousers->SetVisibility(false);
	}
}

void ASimPawnActor::DestroyWorn()
{
	for (UMeshComponent* Mesh : WornMeshes)
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
	}
	WornMeshes.Reset();
	WornMeshItems.Reset();
}

void ASimPawnActor::StyleWornMesh(UMeshComponent* Mesh, const FSimWornPiece& Piece)
{
	const FLinearColor Color = Piece.bHasTint ? Piece.Tint : PawnOwnColor(Mesh);
	Mesh->SetCustomPrimitiveDataVector3(PawnClothColorIndex, FVector(Color.R, Color.G, Color.B));
	Mesh->SetCustomPrimitiveDataFloat(PawnClothWearIndex, Piece.Wear);
	if (USkeletalMeshComponent* Skinned = Cast<USkeletalMeshComponent>(Mesh); Skinned && bHasLooks)
	{
		for (const TPair<FName, float>& Morph : Looks.MorphWeights())
		{
			Skinned->SetMorphTarget(Morph.Key, Morph.Value);
		}
	}
}

void ASimPawnActor::UpdateHairVisibility()
{
	bool bHide = false;
	for (const FSimWornPiece& Piece : Apparel.Pieces)
	{
		bHide = bHide || Piece.bHidesHair;
	}
	bHairHidden = bHide;
	for (USceneComponent* Groom : Grooms)
	{
		if (Groom && Groom->GetName().Contains(TEXT("Hair")))
		{
			Groom->SetVisibility(!bHide && Detail == ESimPawnDetail::Full);
		}
	}
}

void ASimPawnActor::ApplyApparel(USimPawnSubsystem& Pawns, const FSimPawnApparel& Wanted, uint64 Key)
{
	ApparelKey = Key;
	bApparelApplied = true;
	if (!MetaHuman || !MetaHumanBody)
	{
		return;  // a mannequin has no clothes to change
	}
	EnsureBareLayer(Pawns);
	if (BareLayer.IsEmpty() || !DefaultTop || !DefaultTrousers)
	{
		return;  // no clothes assets, or no looks meshes to put away: Kellan's own clothes stay
	}

	if (!Wanted.SameStructure(Apparel))
	{
		DestroyWorn();
		++ApparelRebuilds;
		USkinnedMeshComponent* Leader = DefaultTop && DefaultTop->LeaderPoseComponent.IsValid() ? DefaultTop->LeaderPoseComponent.Get() : MetaHumanBody.Get();
		for (const FSimWornPiece& Piece : Wanted.Pieces)
		{
			for (const FSimWornMesh& Part : Piece.Meshes)
			{
				UObject* Asset = Pawns.GetClothesAsset(Part.Path);
				UMeshComponent* Made = nullptr;
				if (USkeletalMesh* Skinned = Cast<USkeletalMesh>(Asset); Skinned && Part.Bone.IsNone())
				{
					USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(MetaHuman);
					Component->SetSkeletalMeshAsset(Skinned);
					Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Component->SetCanEverAffectNavigation(false);
					Component->SetupAttachment(MetaHuman->GetRootComponent());
					Component->RegisterComponent();
					Component->SetLeaderPoseComponent(Leader);
					Made = Component;
				}
				else if (UStaticMesh* Prop = Cast<UStaticMesh>(Asset); Prop && !Part.Bone.IsNone())
				{
					UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(MetaHuman);
					Component->SetStaticMesh(Prop);
					Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Component->SetCanEverAffectNavigation(false);
					Component->SetupAttachment(MetaHumanBody, Part.Bone);
					Component->RegisterComponent();
					Made = Component;
				}
				if (Made)
				{
					WornMeshes.Add(Made);
					WornMeshItems.Add(Piece.Item);
				}
			}
		}
	}
	Apparel = Wanted;

	// Colour and wear of every mesh, rebuilt or not.
	for (const FSimWornPiece& Piece : Apparel.Pieces)
	{
		for (int32 I = 0; I < WornMeshItems.Num(); ++I)
		{
			if (WornMeshItems[I] == Piece.Item && WornMeshes.IsValidIndex(I) && WornMeshes[I])
			{
				StyleWornMesh(WornMeshes[I], Piece);
			}
		}
	}
	UpdateHairVisibility();
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

void ASimPawnActor::BeginMeleeSwing(EMeleeStyle Style, float StepsToHit)
{
	Swing.Begin(Style, StepsToHit, StanceHeight);
	SwingMismatch = 0.0f;
}

void ASimPawnActor::MeleeHitMoment()
{
	Swing.HitMoment();
}

void ASimPawnActor::PlayDodge(const FVector& TowardWorld)
{
	const FVector Delta = TowardWorld - GetActorLocation();
	DodgeSide = FVector::DotProduct(Delta, GetActorRightVector()) >= 0.0 ? 1.0f : -1.0f;
	DodgeAge = 0.0f;
	++DodgesPlayed;
}

void ASimPawnActor::HitStop(float Seconds)
{
	HitStopLeft = FMath::Max(HitStopLeft, Seconds);
	++HitStops;
}

FMeleePose ASimPawnActor::ComputeMeleePose() const
{
	FMeleePose Pose = MeleePose::Rest(StanceHeight);
	if (Swing.IsActive())
	{
		Pose = Swing.Current;
	}
	else if (BlockBlend > 0.0f)
	{
		Pose = FMeleePose::Lerp(Pose, MeleePose::Block(StanceHeight), MeleePose::EaseInOut(BlockBlend));
	}
	if (DodgeAge < kDodgeSeconds)
	{
		// Up and over in a third of a second: the body drops and tips into the step.
		const float Envelope = FMath::Sin(PI * DodgeAge / kDodgeSeconds);
		Pose.Tilt += DodgeSide * 16.0f * Envelope;
		Pose.Dip += 9.0f * Envelope;
		Pose.Lean += 6.0f * Envelope;
		Pose.WeaponLocation.Y += DodgeSide * 10.0 * Envelope;
	}
	return Pose;
}

void ASimPawnActor::GetMeleeWeaponPose(FVector& OutLocation, FRotator& OutRotation) const
{
	const FMeleePose Pose = ComputeMeleePose();
	// The weapon goes with the body: twisted about the feet, carried forward and down with it.
	const FQuat Twist(FVector::ZAxisVector, FMath::DegreesToRadians(Pose.Twist));
	OutLocation = Twist.RotateVector(Pose.WeaponLocation) + FVector(Pose.Lunge, 0.0, -Pose.Dip);
	OutRotation = (Twist * Pose.WeaponRotation.Quaternion()).Rotator();
}

float ASimPawnActor::GetMeleeBodyDegrees() const
{
	const FMeleePose Pose = ComputeMeleePose();
	return FMath::Abs(Pose.Lean) + FMath::Abs(Pose.Tilt) + FMath::Abs(Pose.Twist);
}

void ASimPawnActor::ApplyBodyPose(float GameDelta, bool bBlocking)
{
	BlockBlend = FMath::FInterpConstantTo(BlockBlend, bBlocking ? 1.0f : 0.0f, GameDelta, 8.0f);
	Swing.Advance(GameDelta * colony::kStepsPerRealSecondAt1x);
	if (DodgeAge < kDodgeSeconds)
	{
		DodgeAge = FMath::Min(kDodgeSeconds, DodgeAge + GameDelta);
	}
	const bool bReacting = ReactionAge < kHitReactionSeconds;
	const bool bMelee = Swing.IsActive() || BlockBlend > 0.0f || DodgeAge < kDodgeSeconds;
	const bool bRolling = RollProgress > 0.0f;
	if (!bReacting && !bMelee && !bRolling)
	{
		if (bBodyPosed)
		{
			bBodyPosed = false;
			LeanDegrees = 0.0f;
			Body->SetRelativeLocationAndRotation(FVector::ZeroVector, PawnBodyRelativeRotation);
		}
		return;
	}
	bBodyPosed = true;

	FQuat LeanWorld = FQuat::Identity;
	if (bReacting)
	{
		ReactionAge += GameDelta;
		const float T = FMath::Clamp(ReactionAge / kHitReactionSeconds, 0.0f, 1.0f);
		// Snaps over in the first 15%, then settles back.
		const float Envelope = T < 0.15f ? T / 0.15f : FMath::Square(1.0f - (T - 0.15f) / 0.85f);
		LeanDegrees = PawnHitLeanDegrees * ReactionStrength * Envelope;
		// The top of the body goes along the shot: turn about up x direction.
		const FVector Axis = FVector::CrossProduct(FVector::UpVector, ReactionDirection).GetSafeNormal();
		LeanWorld = FQuat(Axis, FMath::DegreesToRadians(LeanDegrees));
	}
	else
	{
		LeanDegrees = 0.0f;
	}

	FQuat MeleeLocal = FQuat::Identity;
	FVector Offset = FVector::ZeroVector;
	if (bMelee)
	{
		const FMeleePose Pose = ComputeMeleePose();
		// Twist about up, lean forward about y (z toward x), tip to the right about -x (z toward y).
		MeleeLocal = FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(Pose.Twist)) * FQuat(FVector::YAxisVector, FMath::DegreesToRadians(Pose.Lean)) *
			FQuat(FVector::XAxisVector, -FMath::DegreesToRadians(Pose.Tilt));
		Offset = FVector(Pose.Lunge, 0.0, -Pose.Dip);
	}
	const FQuat RootQuat = GetActorQuat();
	// The roll: tumble forward over the way the sim is moving the pawn, about the middle of the body, ducking into it.
	FQuat RollWorld = FQuat::Identity;
	if (bRolling)
	{
		const FVector Axis = FVector::CrossProduct(FVector::UpVector, RollDirection).GetSafeNormal();
		RollWorld = FQuat(Axis, FMath::DegreesToRadians(RollAngle));
		const float Height = bHasLooks ? Looks.Height : 1.0f;
		const float PivotZ = 90.0f * Height;
		const float Duck = FMath::Sin(PI * RollProgress) * 22.0f * Height;
		const FVector World = FVector(0.0, 0.0, PivotZ) - RollWorld.RotateVector(FVector(0.0, 0.0, PivotZ)) - FVector(0.0, 0.0, Duck);
		Offset += RootQuat.Inverse().RotateVector(World);
	}
	Body->SetRelativeLocationAndRotation(Offset, RootQuat.Inverse() * RollWorld * LeanWorld * RootQuat * MeleeLocal * PawnBodyRelativeRotation.Quaternion());
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
	Swing.Reset();
	BlockBlend = 0.0f;
	DodgeAge = kDodgeSeconds;
	HitStopLeft = 0.0f;
	bBodyPosed = false;
	Body->SetRelativeLocationAndRotation(FVector::ZeroVector, PawnBodyRelativeRotation);
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
