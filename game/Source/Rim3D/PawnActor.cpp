#include "PawnActor.h"
#include "WaterDraw.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Retargeter/IKRetargeter.h"
#include "MakeHumanAnimInstance.h"
#include "MakeHumanArt.h"
#include "GarmentCoverData.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/DecalComponent.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GroomAsset.h"
#include "HAL/IConsoleManager.h"
#include "GroomBindingAsset.h"
#include "GroomComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"
#include "PhysicsEngine/PhysicsAsset.h"

#include "PawnExpressions.h"
#include "PawnFaces.h"
#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/BodyMotion.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Health.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	TAutoConsoleVariable<int32> GPawnsHairCards(
		TEXT("Pawns.HairCards"), 1, TEXT("1 draws hair cards on pawns away from full detail (their strands only show at full detail), 0 none."), ECVF_Default);

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
	/** cm the ragdoll's floor sits above the drawn ground (the flesh outside the physics shapes). */
	constexpr float PawnRagdollFloorLift = 4.0f;
	const TCHAR* const PawnRagdollPhysicsAssetPath = TEXT("/Game/Characters/UEFN_Mannequin/Rigs/PA_UEFN_Mannequin.PA_UEFN_Mannequin");
	const TCHAR* const PawnCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
	// The mannequin faces +Y in its own space.
	const FRotator PawnBodyRelativeRotation(0.0, -90.0, 0.0);
	// A clip the sim moves the body along (docs/design/body-motion.md): blended in short (its blow is timed to the
	// sim's step), a little longer from a clip still playing (the next swing, a stagger on a stagger: 0.08 s was a
	// visible snap), out over 0.25 s. Blending never shifts the clip's moment, only how much of it shows.
	const float PawnMotionBlendIn = 0.1f;
	const float PawnMotionFollowBlendIn = 0.16f;
	const float PawnMotionBlendOut = 0.25f;
	// A react to a hit (upper body): blended in over 0.12 s, out over 0.3 s, and not restarted within 0.35 s.
	const float PawnHitClipBlendIn = 0.12f;
	const float PawnHitClipBlendOut = 0.3f;
	const float PawnHitClipMinGap = 0.35f;
	// The facing drawn: a damped spring onto the sim's facing, PawnDrawnYawRate radians a second, at most
	// PawnDrawnYawMaxLag degrees behind.
	const float PawnDrawnYawRate = 20.0f;
	const float PawnDrawnYawMaxLag = 30.0f;

	/** A clip or a push the sim is carrying the body along (walking waits); false for the player's own swings, which
	 *  leave the feet to the keys (docs/design/body-motion.md "Upper-body clips"). */
	bool PawnMotionHolds(const colony::ThingView& View)
	{
		return (View.motionClip.valid() || View.motionPush) && !View.motionFreeFeet;
	}
	// The hit layer (docs/design/body-health.md part E). A leg limps from 85% health down, fully at 35% and less.
	constexpr float PawnLimpFrom = 0.85f;
	constexpr float PawnLimpFull = 0.35f;
	// The hurt legs are looked up this often (real seconds).
	constexpr float PawnHurtInterval = 0.3f;
	// The crawl: the clips' folder, the blend, and the speed (cm/s) a clip that stays in place is taken to crawl at.
	const TCHAR* const PawnCrawlFolder = TEXT("/Game/Rim3D/Generated/Animations/Mixamo/Crawling");
	constexpr float PawnCrawlBlend = 0.3f;
	constexpr float PawnCrawlDefaultSpeed = 40.0f;
	// Slower than this (cm/s) a crawling pawn lies still (and keeps facing the way it faced).
	constexpr float PawnCrawlStill = 4.0f;
	/** The player's body when this pawn is the colonist it drives, aiming a gun (not rolling, climbing or down). */
	const AProtagonistPawn* PawnAimingPlayer(const UWorld* World, colony::Entity Thing, const colony::ThingView& View)
	{
		const UProtagonistSubsystem* Protagonist = World ? World->GetSubsystem<UProtagonistSubsystem>() : nullptr;
		if (!Protagonist || !Protagonist->IsDirectMode() || Protagonist->GetControlled() != Thing || View.rollPhase != 0 ||
			View.downed || View.climb > 0.0f)
		{
			return nullptr;
		}
		const AProtagonistPawn* Player = Protagonist->GetBody();
		return Player && Player->GetAimBlend() > 0.01f && Player->HasGunDrawn() && !Player->IsClimbing() ? Player : nullptr;
	}
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
		Anim->SetAssets(Pawns.GetDatabases(), Pawns.GetIdleLoops(), Pawns.GetIdleFeet(), Pawns.GetWalkLoop(), Pawns.GetWalkLoopSpeed(),
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
				// V2: the face with the face shape morphs.
				else if (Mesh->GetName() == TEXT("Face") && Mesh->GetSkeletalMeshAsset())
				{
					if (USkeletalMesh* Face = Cast<USkeletalMesh>(Pawns.GetFacesAsset(PawnFacesMeshName(Mesh->GetSkeletalMeshAsset()->GetName()))))
					{
						Mesh->SetSkeletalMeshAsset(Face);
						FaceMesh = Mesh;
					}
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
						if (Mesh == FaceMesh)
						{
							FaceSkinSlots.Add(Slot, Skin);  // V2b: a face skin of the pawn's gender replaces it (ApplyFace)
						}
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
			// Strand hair (HairStrands plugin).
			if (Cast<UGroomComponent>(Component))
			{
				Grooms.Add(Component);
			}
		}
	}
	if (FaceMesh)
	{
		// The eyes go on a copy of their material that may be used with morph targets (one per eye colour); until
		// the looks come, Kellan's colour. Kellan's own grooms are bound to the shaped face too.
		UMaterialInterface* Eye = Cast<UMaterialInterface>(Pawns.GetFacesAsset(PawnFacesEyeDefault));
		const TArray<FSkeletalMaterial>& Slots = FaceMesh->GetSkeletalMeshAsset()->GetMaterials();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			const FString Name = Slots[Slot].MaterialSlotName.ToString();
			if (Name.Contains(TEXT("eyeLeft")) || Name.Contains(TEXT("eyeRight")))
			{
				EyeSlots.Add(Slot);
				if (Eye)
				{
					FaceMesh->SetMaterial(Slot, Eye);
				}
			}
		}
		for (USceneComponent* Component : Grooms)
		{
			UGroomComponent* Groom = Cast<UGroomComponent>(Component);
			UGroomBindingAsset* Binding = Groom && Groom->GroomAsset
				? Cast<UGroomBindingAsset>(Pawns.GetFacesAsset(PawnFacesKellanBindingName(Groom->GroomAsset->GetName()))) : nullptr;
			if (Binding)
			{
				Groom->SetGroomAsset(Groom->GroomAsset, Binding);
			}
		}
		MarkMaterial = Cast<UMaterialInterface>(Pawns.GetFacesAsset(PawnFacesMarkMaterial));
		for (int32 Style = 0; UTexture* Texture = Cast<UTexture>(Pawns.GetFacesAsset(PawnFacesTattooTexture(Style))); ++Style)
		{
			TattooTextures.Add(Texture);
		}
		for (int32 Style = 0; UTexture* Texture = Cast<UTexture>(Pawns.GetFacesAsset(PawnFacesScarTexture(Style))); ++Style)
		{
			ScarTextures.Add(Texture);
		}
		BodyHairTexture = Cast<UTexture>(Pawns.GetFacesAsset(PawnFacesBodyHairTexture));
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
	UpdateMeshTickGroups();

	// Metres per step to cm per second at 1x, blended like the position.
	const float Alpha = Bridge.GetBlendAlpha();
	float Vx = Before ? FMath::Lerp(Before->vx, Now.vx, Alpha) : Now.vx;
	float Vy = Before ? FMath::Lerp(Before->vy, Now.vy, Alpha) : Now.vy;
	// On stairs or a ladder the sim's velocity is nought while the pawn moves along the climb: it walks (and faces) that way.
	if (Before && (Now.climb > 0.0f || Before->climb > 0.0f))
	{
		Vx = Now.x - Before->x;
		Vy = Now.y - Before->y;
	}
	FVector Velocity(Vx * colony::kStepsPerRealSecondAt1x * 100.0f, Vy * colony::kStepsPerRealSecondAt1x * 100.0f, 0.0f);
	// The pace its animation aims for: the sim's own for this pawn now (drafted, hurt, in the dark ...), else its def's.
	float Pace = Now.pace > 0.0f ? Now.pace * colony::kStepsPerRealSecondAt1x * 100.0f : CruiseSpeed;
	// The player's colonist: the sim moves it by the steps the keys send, which come unevenly (two or three frames a step),
	// so it would jerk and its animation would see a stuttering speed. It is drawn where the player's body is (moved
	// every frame, at most a metre from the sim's: ProtagonistPawn reconciles) at its smooth velocity.
	if (const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
		Protagonist && Protagonist->IsDirectMode() && Protagonist->GetControlled() == Thing && Now.rollPhase == 0 && Now.climb <= 0.0f &&
		!PawnMotionHolds(Now))
	{
		if (const AProtagonistPawn* Player = Protagonist->GetBody(); Player && Player->GetSimLevel() == Now.level && !Player->IsClimbing())
		{
			const FVector Local = Bridge.SimToWorld(static_cast<float>(Player->GetSimPosition().X), static_cast<float>(Player->GetSimPosition().Y), Now.level);
			Location.X = Local.X;
			Location.Y = Local.Y;
			Velocity = FVector(Player->GetLocalVelocity().X * 100.0, Player->GetLocalVelocity().Y * 100.0, 0.0);
			Pace = Player->GetTargetSpeed() > 0.1f ? Player->GetTargetSpeed() * 100.0f : CruiseSpeed;
		}
	}
	GroundSpeed = Velocity.Size2D();

	// Whether the body keeps a facing of its own while it moves (a clip, a lock-on): the walk then steps sideways or
	// backwards under it instead of turning the hips the way it goes.
	bool bHoldFacing = false;
	JumpHeightFactor = Now.jumpHeight;
	if (Now.wallClimb && !bRagdoll && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		// On a wall: facing it, up or down (docs/design/climbing.md).
		const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
		bHoldFacing = true;
	}
	else if (Now.motionClip.valid() && !bRagdoll && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		// A clip the sim moves the body along plays facing the way the sim has the body face, not the way it is
		// thrown: a stagger back must not turn the body round to face its own push (docs/design/body-motion.md).
		const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
		bHoldFacing = true;
	}
	else if (const AProtagonistPawn* Aimer = PawnAimingPlayer(GetWorld(), Thing, Now))
	{
		// The player's colonist aiming a gun: the body turns with the view and strafes, as in any over-the-shoulder
		// shooter (PC run 11: standing still it kept its old facing and the rifle pointed off to the side of the view).
		const float Wanted = Aimer->GetYaw();
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
		bHoldFacing = true;
	}
	else if (Now.focusTarget != colony::kNoEntity && Now.rollPhase == 0 && !Now.downed && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		// Locked on (docs/design/focus.md): the body keeps facing its focus and walks sideways or backwards (strafes).
		const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
		bHoldFacing = true;
	}
	else if (Velocity.Size2D() > (Now.crawling ? PawnCrawlStill : PawnTurnSpeed))
	{
		const float Wanted = Velocity.Rotation().Yaw;
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * Speed);
	}
	else if ((Now.shotReadyIn > 0 || Now.fighting || Now.meleePhase != colony::MeleePhase::Ready || Now.blocking || Now.shieldUp) && FMath::Abs(Now.facingX) + FMath::Abs(Now.facingY) > 0.1f)
	{
		// Standing and shooting or swinging: face where the sim says the pawn aims (docs/design/fights.md).
		const float Wanted = FMath::RadiansToDegrees(FMath::Atan2(Now.facingY, Now.facingX));
		Yaw = bTeleported ? Wanted : FMath::FixedTurn(Yaw, Wanted, PawnTurnRate * DeltaTime * FMath::Max(Speed, 1));
	}
	// Swimming: the body floats with its shoulders at the surface (docs/design/water.md, W4).
	SwimLiftCm = WaterDraw::SwimLiftEase(SwimLiftCm, Bridge.GetSim() ? WaterDraw::SwimLiftTargetCm(Now, Bridge.GetSim()->bodyHeightM(Thing)) : 0.0f, DeltaTime, bTeleported);
	Location.Z += SwimLiftCm;
	// The facing drawn follows Yaw as a damped spring (PawnDrawnYawRate): the sim turns a body up to 30 degrees in a few
	// frames, and a facing that starts or stops turning at once jolts every bone (docs/design/pawns.md "No pops").
	if (bTeleported || DeltaTime * Speed <= 0.0f)
	{
		DrawnYaw = bTeleported ? Yaw : DrawnYaw;
		DrawnYawRate = bTeleported ? 0.0f : DrawnYawRate;
	}
	else
	{
		const float YawDelta = DeltaTime * Speed;
		const int32 YawSteps = FMath::Clamp(FMath::CeilToInt32(YawDelta * 120.0f), 1, 30);
		for (int32 i = 0; i < YawSteps; ++i)
		{
			const float Off = FMath::FindDeltaAngleDegrees(DrawnYaw, Yaw);
			DrawnYawRate += (PawnDrawnYawRate * PawnDrawnYawRate * Off - 2.0f * PawnDrawnYawRate * DrawnYawRate) * (YawDelta / YawSteps);
			DrawnYaw = FRotator::NormalizeAxis(DrawnYaw + DrawnYawRate * (YawDelta / YawSteps));
		}
		const float Behind = FMath::FindDeltaAngleDegrees(Yaw, DrawnYaw);
		if (FMath::Abs(Behind) > PawnDrawnYawMaxLag)
		{
			DrawnYaw = FRotator::NormalizeAxis(Yaw + FMath::Sign(Behind) * PawnDrawnYawMaxLag);
		}
	}
	SetActorLocationAndRotation(Location, FRotator(0.0, DrawnYaw, 0.0));
	ApplySevered(Now.severed);
	UpdateFightFace(Bridge, Now, DeltaTime * Speed);
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
	// A clip the sim is moving the body along plays as itself: no procedural tumble, swing or sidestep lean on top.
	if (UpdateMotionClip(Bridge, Now, DeltaTime * Speed))
	{
		RollProgress = 0.0f;
		RollAngle = 0.0f;
		Swing.Abort();
		DodgeAge = kDodgeSeconds;
	}
	UpdateCrawl(Now, DeltaTime * Speed);
	UpdateWallClimb(Bridge, Now);
	UpdateHurt(Bridge, Now, DeltaTime);

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
	ApplyBodyPose(GameDelta, (Now.blocking || Now.shieldUp) && !Now.downed && !Now.dead);

	// Animation runs at the game's speed and stops while paused (and during a hit-stop).
	Body->GlobalAnimRateScale = HitStopLeft > 0.0f ? 0.0f : static_cast<float>(Speed);
	if (USimPawnAnimInstance* Anim = GetAnim())
	{
		FSimPawnAnimInputs& In = Anim->Inputs;
		In.Location = Location;
		In.Yaw = DrawnYaw;
		In.Velocity = Velocity;
		In.CruiseSpeed = Pace;
		In.Detail = Detail;
		In.bTeleported = bTeleported;
		In.bHoldFacing = bHoldFacing;
		In.bStandTwist = !(IsInMotionClip() && !bMotionUpper) && RollProgress <= 0.0f && !IsJumping() && !Now.crawling && !Now.downed;
		In.bLegsFromClip = LegsClipWeight > 0.001f || ActionEndSeconds > 0.0 || (IsInMotionClip() && !bMotionUpper);
		In.bHitLayer = true;
		In.LimpLeft = LimpLeft;
		In.LimpRight = LimpRight;
		In.Patch = bPatchPosed ? 1.0f : 0.0f;
		In.PatchZone = PatchZone;
		In.Route.Reset();
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		for (uint32 i = 0; i < Now.waypointCount && In.Route.Num() < 8; ++i)
		{
			const colony::PathPointView& Point = Snapshot.waypoints[Now.firstWaypoint + i];
			In.Route.Add(Bridge.SimToWorld(Point.x, Point.y, Now.level));
		}

		if (HitMontage.IsValid())
		{
			HitClipAge += DeltaTime * Speed;
			if (HitClipEndSeconds > 0.0f && HitClipAge >= HitClipEndSeconds)
			{
				Anim->Montage_Stop(PawnHitClipBlendOut, HitMontage.Get());
				HitClipEndSeconds = 0.0f;
			}
			else if (HitClipEndSeconds <= 0.0f && !Anim->Montage_IsPlaying(HitMontage.Get()))
			{
				HitMontage.Reset();
			}
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
		RefreshGroomVisibility();
		// Skin marks only near the camera.
		if (bHasLooks && (!MarkDecals.IsEmpty() || Detail == ESimPawnDetail::Full))
		{
			ApplyMarks();
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
	if (!Anim || !Animation || WorkLoopClip.IsValid())
	{
		return;  // a work loop (felling) owns the slot
	}
	if (Anim->PlaySlotAnimationAsDynamicMontage(Animation, FAnimSlotGroup::DefaultSlotName, PawnActionBlendIn, PawnActionBlendOut))
	{
		ActionEndSeconds = PawnActionBlendIn + PawnActionHold;
		++ActionsPlayed;
	}
}

void ASimPawnActor::SetWorkLoop(UAnimSequence* Clip, float Rate)
{
	USimPawnAnimInstance* Anim = GetAnim();
	if (!Anim || !Clip || WorkLoopClip.Get() == Clip)
	{
		return;
	}
	// Loops until stopped (a felling takes a minute or so of game time; the count is only a ceiling).
	if (Anim->PlaySlotAnimationAsDynamicMontage(Clip, FAnimSlotGroup::DefaultSlotName, PawnActionBlendIn, PawnActionBlendOut, Rate, 10000))
	{
		WorkLoopClip = Clip;
		ActionEndSeconds = 0.0;  // a short action it replaced must not stop it
	}
}

void ASimPawnActor::StopWorkLoop()
{
	if (!WorkLoopClip.IsValid())
	{
		return;
	}
	WorkLoopClip.Reset();
	if (USimPawnAnimInstance* Anim = GetAnim())
	{
		Anim->StopSlotAnimation(PawnActionBlendOut, FAnimSlotGroup::DefaultSlotName);
	}
}

namespace
{
	const TCHAR* const PawnJumpFolder = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Jump/");

	UAnimSequence* PawnJumpAnimation(const FString& Name)
	{
		return LoadObject<UAnimSequence>(nullptr, *(FString(PawnJumpFolder) + Name + TEXT(".") + Name));
	}

	/** The sample's gait names by ground speed (cm/s). */
	const TCHAR* PawnJumpGait(float Speed)
	{
		return Speed < 60.0f ? TEXT("Stand") : (Speed < 260.0f ? TEXT("Walk") : (Speed < 520.0f ? TEXT("Run") : TEXT("Sprint")));
	}
}

bool ASimPawnActor::StartJump()
{
	USimPawnAnimInstance* Anim = GetAnim();
	if (IsJumping() || bRagdoll || StanceHeight < 0.9f || RollProgress > 0.0f || !Anim)
	{
		return false;
	}
	JumpAge = 0.0f;
	JumpSpeed = GroundSpeed;
	JumpFactor = FMath::Clamp(JumpHeightFactor, 0.5f, 3.0f);
	bJumpFalling = false;
	JumpStartLength = 0.0f;
	if (UAnimSequence* Start = PawnJumpAnimation(FString::Printf(TEXT("M_Neutral_Jump_F_Start_%s_Lfoot"), PawnJumpGait(JumpSpeed))))
	{
		if (Anim->PlaySlotAnimationAsDynamicMontage(Start, FAnimSlotGroup::DefaultSlotName, 0.08f, 0.15f))
		{
			JumpStartLength = Start->GetPlayLength();
		}
	}
	ActionEndSeconds = 0.0;
	++JumpsPlayed;
	return true;
}

float ASimPawnActor::GetJumpHeight() const
{
	if (!IsJumping())
	{
		return 0.0f;
	}
	// A thrown body: up at the speed that reaches kJumpHeight halfway through, back down by kJumpSeconds. A higher jump
	// (the lineage's factor) takes longer by its square root, as a thrown body does.
	const float T = FMath::Clamp(JumpAge / (kJumpSeconds * FMath::Sqrt(JumpFactor)), 0.0f, 1.0f);
	return 4.0f * kJumpHeight * JumpFactor * T * (1.0f - T);
}

void ASimPawnActor::AdvanceJump(float GameDelta)
{
	if (!IsJumping())
	{
		return;
	}
	USimPawnAnimInstance* Anim = GetAnim();
	JumpAge += GameDelta;
	const float Left = kJumpSeconds * FMath::Sqrt(JumpFactor) - JumpAge;
	// The take-off animation runs out before the ground comes: the fall loop holds the body in the air.
	if (Anim && !bJumpFalling && JumpAge > JumpStartLength - 0.15f && Left > 0.2f)
	{
		bJumpFalling = true;
		if (UAnimSequence* Fall = PawnJumpAnimation(TEXT("M_Neutral_Jump_Loop_Fall")))
		{
			Anim->PlaySlotAnimationAsDynamicMontage(Fall, FAnimSlotGroup::DefaultSlotName, 0.15f, 0.1f, 1.0f, 100);
		}
	}
	if (Left <= 0.0f)
	{
		JumpAge = -1.0f;
		if (!Anim)
		{
			return;
		}
		// The landing for how fast it is going now; the walk or run takes over again soon after.
		const float Speed = GroundSpeed;
		const TCHAR* Gait = PawnJumpGait(Speed);
		if (UAnimSequence* Land = PawnJumpAnimation(FString::Printf(TEXT("M_Neutral_Jump_F_Land_%s_Light_Lfoot"), Gait)))
		{
			if (Anim->PlaySlotAnimationAsDynamicMontage(Land, FAnimSlotGroup::DefaultSlotName, 0.05f, 0.25f))
			{
				ActionEndSeconds = Speed < 60.0f ? 0.6 : 0.35;
			}
		}
		else
		{
			Anim->StopSlotAnimation(0.2f, FAnimSlotGroup::DefaultSlotName);
		}
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
	if (Looks.MakeHuman != AppliedMakeHuman)
	{
		ApplyMakeHuman(Pawns);
	}

	for (USkeletalMeshComponent* Mesh : ShapedMeshes)
	{
		for (const TPair<FName, float>& Morph : Looks.MorphWeights())
		{
			Mesh->SetMorphTarget(Morph.Key, Morph.Value);
		}
	}
	// The shaped face's neck follows the body type too (the real body); a face without those morphs ignores them.
	if (FaceMesh)
	{
		for (const TPair<FName, float>& Morph : Looks.MorphWeights())
		{
			FaceMesh->SetMorphTarget(Morph.Key, Morph.Value);
		}
	}
	const FVector Skin(Looks.Skin.R, Looks.Skin.G, Looks.Skin.B);
	for (USkeletalMeshComponent* Mesh : SkinMeshes)
	{
		Mesh->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, Skin);
	}
	if (FaceMesh)
	{
		ApplyFace(Pawns);
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
	if (HairCardsMaterial)
	{
		HairCardsMaterial->SetVectorParameterValue(PawnFacesParamHairColor, Looks.Hair);
	}

	// The clothes and the skin under them share the body's shape and the pawn's skin colour.
	for (UMaterialInstanceDynamic* BareSkin : BareSkins)
	{
		BareSkin->SetVectorParameterValue(PawnLooksParamSkinColor, Looks.Skin);
	}
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
	ApplyMarks();
}

// ---------------------------------------------------------------------------
// Faces, hair and marks (V2, V3; docs/design/looks.md).

void ASimPawnActor::ApplyMakeHuman(USimPawnSubsystem& Pawns)
{
	AppliedMakeHuman = Looks.MakeHuman;
	UIKRetargeter* Retargeter = nullptr;
	USkeletalMesh* Mesh = Looks.MakeHuman.IsEmpty() ? nullptr : Pawns.GetMakeHumanMesh(Looks.MakeHuman, Retargeter);
	if (!Mesh || !Retargeter)
	{
		if (!Looks.MakeHuman.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("MakeHuman character %s is not imported (run the MakeHumanArt commandlet, docs/design/makehuman.md)"), *Looks.MakeHuman);
		}
		if (MakeHumanMesh)
		{
			DestroyWorn();  // its garments go with it; the MetaHuman dresses again
			Apparel = FSimPawnApparel();
			GarmentCuts.Reset();
			CoveredHairSections.Reset();
			CoveredPubicSections.Reset();
			bHeadwearDrawn = false;
			bApparelApplied = false;
			MakeHumanMesh->DestroyComponent();
			MakeHumanMesh = nullptr;
		}
		if (MetaHuman)
		{
			MetaHuman->SetActorHiddenInGame(false);
		}
		return;
	}
	if (!MakeHumanMesh)
	{
		// Attached to the hidden mannequin: the Game Animation Sample's generic retarget animation takes the pose of the mesh it is attached to.
		MakeHumanMesh = NewObject<USkeletalMeshComponent>(this, TEXT("MakeHuman"));
		MakeHumanMesh->SetupAttachment(Body);
		MakeHumanMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MakeHumanMesh->SetCanEverAffectNavigation(false);
		MakeHumanMesh->bReceivesDecals = true;
		MakeHumanMesh->RegisterComponent();
	}
	if (MakeHumanMesh->GetSkeletalMeshAsset() != Mesh)
	{
		// Garments are made per body: a new body puts its clothes on afresh (the pawn subsystem applies them again).
		DestroyWorn();
		Apparel = FSimPawnApparel();
		GarmentCuts.Reset();
		CoveredHairSections.Reset();
		CoveredPubicSections.Reset();
		bHeadwearDrawn = false;
		SetMakeHumanCoverBits(0);
		bApparelApplied = false;
	}
	MakeHumanMesh->SetSkeletalMeshAsset(Mesh);
	MakeHumanMesh->SetAnimInstanceClass(UMakeHumanAnimInstance::StaticClass());
	if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(MakeHumanMesh->GetAnimInstance()))
	{
		Anim->SetRetargeter(Retargeter);
	}
	MakeHumanMesh->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, FVector(Looks.Skin.R, Looks.Skin.G, Looks.Skin.B));
	if (MetaHuman)
	{
		MetaHuman->SetActorHiddenInGame(true);
	}
}

void ASimPawnActor::ApplyFace(USimPawnSubsystem& Pawns)
{
	if (!Looks.HasFace())
	{
		return;  // a pawn from an older save keeps Kellan's face, eyes and grooms
	}
	if (!AppliedFacePreset.IsNone())
	{
		FaceMesh->SetMorphTarget(AppliedFacePreset, 0.0f);
		AppliedFacePreset = NAME_None;
	}
	for (const TPair<FName, float>& Morph : Looks.FaceMorphWeights())
	{
		FaceMesh->SetMorphTarget(Morph.Key, Morph.Value);
	}
	if (!Looks.FacePreset.IsEmpty())
	{
		AppliedFacePreset = PawnFacesPresetMorph(Looks.bFemale, Looks.FacePreset);
	}
	if (UMaterialInterface* Eye = Cast<UMaterialInterface>(Pawns.GetFacesAsset(PawnFacesEyeInstanceName(Looks.Eyes))))
	{
		for (const int32 Slot : EyeSlots)
		{
			FaceMesh->SetMaterial(Slot, Eye);
		}
	}

	// V2b: the face texture, one of the gender's Creator preset skins picked by the face sliders (so it never changes for
	// a pawn); men can also keep Kellan's. Without the skins everyone keeps Kellan's.
	int32 Skins = 0;
	while (Skins < 64 && Pawns.GetFacesAsset(PawnFacesSkinName(Looks.bFemale, Skins)))
	{
		++Skins;
	}
	const float Sliders[] = {Looks.FaceShape, Looks.Brow, Looks.Nose, Looks.Jaw};
	const int32 Choices = Skins + (Looks.bFemale ? 0 : 1);
	const int32 SkinPick = Looks.FaceSkin >= 0 && Looks.FaceSkin < Skins ? Looks.FaceSkin
		: Choices > 0 ? static_cast<int32>(FCrc::MemCrc32(Sliders, sizeof(Sliders)) % static_cast<uint32>(Choices)) : 0;
	UMaterialInterface* FaceSkin = SkinPick < Skins ? Pawns.GetShinySkin(Cast<UMaterialInterface>(Pawns.GetFacesAsset(PawnFacesSkinName(Looks.bFemale, SkinPick)))) : nullptr;
	for (const TPair<int32, TObjectPtr<UMaterialInterface>>& Slot : FaceSkinSlots)
	{
		FaceMesh->SetMaterial(Slot.Key, FaceSkin ? FaceSkin : Slot.Value.Get());
	}

	SetGroom(Pawns, TEXT("Hair"), Looks.HairStyle == TEXT("Bald") ? FString() : PawnFacesGroomName(TEXT("Hair"), Looks.HairStyle));
	SetHairCards(Pawns);
	const bool bBeard = !Looks.Beard.IsEmpty() && Looks.Beard != TEXT("None");
	SetGroom(Pawns, TEXT("Beard"), bBeard ? PawnFacesGroomName(TEXT("Beard"), Looks.Beard) : FString());
	SetGroom(Pawns, TEXT("Mustache"), bBeard ? PawnFacesGroomName(TEXT("Moustache"), Looks.Beard) : FString());
	// Eyebrows: the brow slider picks among the gender's styles (finer first for women, plainer first for men).
	const TCHAR* Gender = Looks.bFemale ? TEXT("F") : TEXT("M");
	int32 Brows = 0;
	while (Brows < 16 && Pawns.GetFacesAsset(PawnFacesGroomName(TEXT("Brows"), FString::Printf(TEXT("%s%d"), Gender, Brows))))
	{
		++Brows;
	}
	if (Brows > 0)
	{
		const int32 Pick = FMath::Clamp(FMath::FloorToInt32(Looks.Brow * Brows), 0, Brows - 1);
		SetGroom(Pawns, TEXT("Eyebrows"), PawnFacesGroomName(TEXT("Brows"), FString::Printf(TEXT("%s%d"), Gender, Pick)));
	}
	RefreshGroomVisibility();
}

UMaterialInterface* ASimPawnActor::GetFaceSkin() const
{
	for (const TPair<int32, TObjectPtr<UMaterialInterface>>& Slot : FaceSkinSlots)
	{
		return FaceMesh ? FaceMesh->GetMaterial(Slot.Key) : nullptr;
	}
	return nullptr;
}

void ASimPawnActor::SetHairCards(USimPawnSubsystem& Pawns)
{
	// V2b: far pawns draw the style's hair cards (strands only show at full detail); none for bald, or without the assets.
	UStaticMesh* Mesh = Looks.HairStyle == TEXT("Bald") ? nullptr : Cast<UStaticMesh>(Pawns.GetFacesAsset(PawnFacesHairCardsName(Looks.HairStyle)));
	if (Mesh && !HairCards && FaceMesh)
	{
		HairCards = NewObject<UStaticMeshComponent>(this, TEXT("HairCards"));
		HairCards->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HairCards->SetCanEverAffectNavigation(false);
		HairCards->SetCastShadow(false);
		HairCards->SetupAttachment(FaceMesh, TEXT("head"));
		HairCards->RegisterComponent();
	}
	if (HairCards)
	{
		HairCards->SetStaticMesh(Mesh);
		if (Mesh && Mesh->GetMaterial(0))
		{
			HairCardsMaterial = UMaterialInstanceDynamic::Create(Mesh->GetMaterial(0), HairCards);
			HairCardsMaterial->SetVectorParameterValue(PawnFacesParamHairColor, Looks.Hair);
			HairCards->SetMaterial(0, HairCardsMaterial);
		}
	}
}

void ASimPawnActor::SetGroom(USimPawnSubsystem& Pawns, FName Component, const FString& GroomName)
{
	UGroomComponent* Groom = nullptr;
	for (USceneComponent* Candidate : Grooms)
	{
		if (Candidate && Candidate->GetFName() == Component)
		{
			Groom = Cast<UGroomComponent>(Candidate);
		}
	}
	if (!Groom)
	{
		return;
	}
	// A style the commandlet made no groom for (a moustache has no beard) draws none.
	UGroomAsset* Asset = GroomName.IsEmpty() ? nullptr : Cast<UGroomAsset>(Pawns.GetFacesAsset(GroomName));
	UGroomBindingAsset* Binding = Asset ? Cast<UGroomBindingAsset>(Pawns.GetFacesAsset(PawnFacesBindingName(GroomName))) : nullptr;
	if (Groom->GroomAsset != Asset || Groom->BindingAsset != Binding)
	{
		Groom->SetGroomAsset(Asset, Binding);
	}
	if (Asset)
	{
		EmptyGrooms.Remove(Component);
	}
	else
	{
		EmptyGrooms.Add(Component);
	}
}

FString ASimPawnActor::GetGroomName(FName Component) const
{
	for (const USceneComponent* Candidate : Grooms)
	{
		const UGroomComponent* Groom = Cast<UGroomComponent>(Candidate);
		if (Groom && Groom->GetFName() == Component)
		{
			return Groom->GroomAsset && !EmptyGrooms.Contains(Component) ? Groom->GroomAsset->GetName() : FString();
		}
	}
	return FString();
}

void ASimPawnActor::RefreshGroomVisibility()
{
	const bool bHeadOff = (SeveredShown & colony::kSeveredHead) != 0;  // the hair went with the head
	for (USceneComponent* Groom : Grooms)
	{
		if (Groom)
		{
			// Hair under a helmet or a tuque stays hidden at every detail.
			const bool bCovered = bHairHidden && Groom->GetName().Contains(TEXT("Hair"));
			Groom->SetVisibility(!bHeadOff && Detail == ESimPawnDetail::Full && !bCovered && !EmptyGrooms.Contains(Groom->GetFName()));
		}
	}
	// Hair cards stand in for the hair strands away from full detail.
	if (HairCards)
	{
		HairCards->SetVisibility(!bHeadOff && Detail != ESimPawnDetail::Full && !bHairHidden && HairCards->GetStaticMesh() != nullptr && GPawnsHairCards.GetValueOnGameThread() != 0);
	}
}

namespace
{
	// Where marks go on the MetaHuman body (its reference pose, mesh space: cm, +Y forward, +Z up): a region is a chain of
	// bones the mark's `v` runs along, with the body's radius there; `u` goes once around (0 the front).
	struct FPawnMarkRegion
	{
		TArray<FName> Bones;
		TArray<float> Radius;  // at each bone
	};

	const FPawnMarkRegion& PawnMarkRegion(uint8 Region)
	{
		static const FPawnMarkRegion Regions[] = {
			{{TEXT("neck_02"), TEXT("head")}, {8.5f, 9.0f}},                                     // Face (front half only)
			{{TEXT("neck_01"), TEXT("neck_02")}, {6.0f, 5.5f}},                                  // Neck
			{{TEXT("spine_01"), TEXT("spine_03"), TEXT("spine_05")}, {14.0f, 14.0f, 15.0f}},     // Torso
			{{TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l")}, {5.5f, 4.2f, 3.2f}},      // LeftArm
			{{TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r")}, {5.5f, 4.2f, 3.2f}},      // RightArm
			{{TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l")}, {8.5f, 5.8f, 4.5f}},             // LeftLeg
			{{TEXT("thigh_r"), TEXT("calf_r"), TEXT("foot_r")}, {8.5f, 5.8f, 4.5f}},             // RightLeg
		};
		return Regions[FMath::Min<int32>(Region, UE_ARRAY_COUNT(Regions) - 1)];
	}

	constexpr float PawnTattooHalfSize = 4.0f;   // cm, before the mark's size
	constexpr float PawnScarHalfSize = 3.0f;
	const FLinearColor PawnTattooInk(0.012f, 0.016f, 0.03f);
	// Marks fade out when the pawn is smaller than this share of the screen.
	constexpr float PawnMarkFadeScreenSize = 0.004f;
}

void ASimPawnActor::ApplyMarks()
{
	for (UDecalComponent* Decal : MarkDecals)
	{
		if (Decal)
		{
			Decal->DestroyComponent();
		}
	}
	MarkDecals.Reset();
	USkeletalMeshComponent* Skin = MetaHumanBody.Get();
	const USkeletalMesh* SkinMesh = Skin ? Skin->GetSkeletalMeshAsset() : nullptr;
	const bool bAny = bHasLooks && Looks.HasFace() && (Looks.BodyHair > 0.05f || !Looks.Tattoos.IsEmpty() || !Looks.Scars.IsEmpty());
	// The skin draws into custom depth so the marks land only where skin is the nearest surface (not on clothes).
	const bool bDraw = bAny && MarkMaterial && SkinMesh && Detail == ESimPawnDetail::Full;
	for (USkeletalMeshComponent* Mesh : TArray<USkeletalMeshComponent*>{MetaHumanBody.Get(), FaceMesh.Get()})
	{
		if (Mesh)
		{
			Mesh->SetRenderCustomDepth(bDraw);
		}
	}
	for (USkeletalMeshComponent* Layer : BareLayer)
	{
		Layer->SetRenderCustomDepth(bDraw);
	}
	if (!bDraw)
	{
		return;
	}

	const FReferenceSkeleton& Ref = SkinMesh->GetRefSkeleton();
	const auto Place = [&](uint8 Region, float U, float V, float HalfSize, UTexture* Mask, const FLinearColor& Color, float Opacity)
	{
		const FPawnMarkRegion& Chain = PawnMarkRegion(Region);
		TArray<int32> Bones;
		for (const FName& Name : Chain.Bones)
		{
			Bones.Add(Ref.FindBoneIndex(Name));
		}
		if (!Mask || Bones.Contains(INDEX_NONE))
		{
			return;
		}
		// Along the chain: which segment, and how far along it.
		const float Along = FMath::Clamp(V, 0.0f, 1.0f) * (Bones.Num() - 1);
		const int32 Segment = FMath::Min(FMath::FloorToInt32(Along), Bones.Num() - 2);
		const float T = Along - Segment;
		const FTransform A = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bones[Segment]);
		const FTransform B = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bones[Segment + 1]);
		const FVector Axis = (B.GetLocation() - A.GetLocation()).GetSafeNormal();
		const FVector OnAxis = FMath::Lerp(A.GetLocation(), B.GetLocation(), T);
		const float Radius = FMath::Lerp(Chain.Radius[Segment], Chain.Radius[Segment + 1], T);
		// Around: the face only has a front half; elsewhere u goes once around, 0 at the front (+Y).
		const float Angle = Region == static_cast<uint8>(colony::BodyRegion::Face) ? (U - 0.5f) * PI : U * 2.0f * PI;
		FVector Front = FVector::YAxisVector - Axis * FVector::DotProduct(FVector::YAxisVector, Axis);
		Front = Front.IsNearlyZero() ? FVector::ZAxisVector : Front.GetSafeNormal();
		const FVector Side = FVector::CrossProduct(Axis, Front);
		const FVector Out = Front * FMath::Cos(Angle) + Side * FMath::Sin(Angle);
		// The decal projects along its X: into the body, centred on the skin, deep enough for a heavy body's belly.
		const FMatrix Frame = FRotationMatrix::MakeFromXZ(-Out, Axis);
		const FTransform InMesh(Frame.Rotator(), OnAxis + Out * Radius);
		UDecalComponent* Decal = NewObject<UDecalComponent>(this);
		Decal->SetupAttachment(Skin, Chain.Bones[Segment]);
		Decal->SetRelativeTransform(InMesh.GetRelativeTransform(A));
		Decal->DecalSize = FVector(Radius * 0.9f + 4.0f, HalfSize, HalfSize);
		Decal->SetFadeScreenSize(PawnMarkFadeScreenSize);
		UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(MarkMaterial, Decal);
		Material->SetTextureParameterValue(PawnFacesParamMask, Mask);
		Material->SetVectorParameterValue(PawnFacesParamColor, Color);
		Material->SetScalarParameterValue(PawnFacesParamOpacity, Opacity);
		Decal->SetDecalMaterial(Material);
		Decal->RegisterComponent();
		MarkDecals.Add(Decal);
	};

	// Body hair: forearms and shins, and on men chest and belly, as dense as the pawn's amount, in its hair colour.
	if (Looks.BodyHair > 0.05f && BodyHairTexture)
	{
		const float Opacity = FMath::Clamp(Looks.BodyHair, 0.0f, 1.0f) * 0.7f;
		const uint8 Torso = static_cast<uint8>(colony::BodyRegion::Torso);
		if (!Looks.bFemale)
		{
			Place(Torso, 0.0f, 0.82f, 11.0f, BodyHairTexture, Looks.Hair, Opacity);
			Place(Torso, 0.0f, 0.3f, 9.0f, BodyHairTexture, Looks.Hair, Opacity * 0.8f);
		}
		for (const colony::BodyRegion Limb : {colony::BodyRegion::LeftArm, colony::BodyRegion::RightArm})
		{
			Place(static_cast<uint8>(Limb), 0.25f, 0.72f, 7.0f, BodyHairTexture, Looks.Hair, Opacity);
		}
		for (const colony::BodyRegion Limb : {colony::BodyRegion::LeftLeg, colony::BodyRegion::RightLeg})
		{
			Place(static_cast<uint8>(Limb), 0.0f, 0.68f, 10.0f, BodyHairTexture, Looks.Hair, Opacity);
		}
	}
	// Scars: paler and pinker than the skin around them.
	const FLinearColor ScarColor = FLinearColor::LerpUsingHSV(Looks.Skin, FLinearColor(0.55f, 0.32f, 0.3f), 0.45f);
	for (const FSimPawnMark& Mark : Looks.Scars)
	{
		Place(Mark.Region, Mark.U, Mark.V, PawnScarHalfSize * Mark.Size, ScarTextures.IsValidIndex(Mark.Style) ? ScarTextures[Mark.Style].Get() : nullptr, ScarColor, 0.75f);
	}
	for (const FSimPawnMark& Mark : Looks.Tattoos)
	{
		Place(Mark.Region, Mark.U, Mark.V, PawnTattooHalfSize * Mark.Size, TattooTextures.IsValidIndex(Mark.Style) ? TattooTextures[Mark.Style].Get() : nullptr,
			PawnTattooInk, 0.85f);
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
		else if (Name.Contains(TEXT("shoes")))
		{
			DefaultShoes = Mesh;
		}
	}
	if (!DefaultTop || !DefaultTrousers)
	{
		return;
	}
	// The real body (character creator step 1): the MetaHuman's body is the engine's whole body, all skin, so it is
	// what shows where nothing is worn. Kellan's own clothes go once the clothes assets are there to replace them.
	const USkeletalMesh* BodyMesh = MetaHumanBody->GetSkeletalMeshAsset();
	if (BodyMesh && BodyMesh->FindMorphTarget(PawnLooksFullBodyMarker) && Pawns.GetClothesAsset(PawnBareMeshes[0]))
	{
		bFullBody = true;
		DefaultTop->SetVisibility(false);
		DefaultTrousers->SetVisibility(false);
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
			BareSkins.Reset();
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
		// V2b: lit like the face and hands (subsurface skin) and in the pawn's skin colour, on its own dynamic instance.
		if (UMaterialInterface* BareSkin = Cast<UMaterialInterface>(Pawns.GetFacesAsset(PawnFacesBareSkinMaterial)))
		{
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(BareSkin, Layer);
			Instance->SetVectorParameterValue(PawnLooksParamSkinColor, Skin);
			Pawns.ApplySkinShine(Instance);
			for (int32 Slot = 0; Slot < Layer->GetNumMaterials(); ++Slot)
			{
				Layer->SetMaterial(Slot, Instance);
			}
			BareSkins.Add(Instance);
		}
		Layer->SetRenderCustomDepth(!MarkDecals.IsEmpty());  // skin marks land on it (ApplyMarks)
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
	RefreshGroomVisibility();
}

namespace
{
	/** A worn piece's cut on a MakeHuman body: one of its `visual.cuts` whose garments are all made for the base, picked by
	 *  the item (so the same shirt always looks the same, and two shirts of a kind can differ). Empty if none fits. */
	FString PawnGarmentCut(USimPawnSubsystem& Pawns, const FString& Base, const FSimWornPiece& Piece)
	{
		TArray<const FString*> Fits;
		for (const FString& Cut : Piece.Cuts)
		{
			TArray<FString> Garments;
			Cut.ParseIntoArray(Garments, TEXT("+"));
			bool bAll = !Garments.IsEmpty();
			for (const FString& Garment : Garments)
			{
				bAll = bAll && Pawns.GetGarmentMesh(Base, Garment) != nullptr;
			}
			if (bAll)
			{
				Fits.Add(&Cut);
			}
		}
		if (Fits.IsEmpty())
		{
			return FString();
		}
		const uint32 Seed[2] = {Piece.Item, Piece.Def};
		return *Fits[FCrc::MemCrc32(Seed, sizeof(Seed)) % static_cast<uint32>(Fits.Num())];
	}

	/** Slot names of a MakeHuman body's scalp hair styles (tools/makehuman: long01, bob02, short02, ponytail01, braid01...). */
	bool PawnIsScalpHairSlot(const FString& Slot)
	{
		static const TCHAR* const Keys[] = {TEXT("long0"), TEXT("bob0"), TEXT("short0"), TEXT("ponytail"), TEXT("braid"), TEXT("afro"),
			TEXT("_hair"), TEXT("twintail"), TEXT("messy")};
		const FString Lower = Slot.ToLower();
		if (Lower.Contains(TEXT("pubic")) || Lower.Contains(TEXT("brow")) || Lower.Contains(TEXT("lash")) || Lower.Contains(TEXT("beard")))
		{
			return false;
		}
		for (const TCHAR* Key : Keys)
		{
			if (Lower.Contains(Key))
			{
				return true;
			}
		}
		return false;
	}
}

void ASimPawnActor::ApplyGarments(USimPawnSubsystem& Pawns, const FSimPawnApparel& Wanted)
{
	const FString Base = Looks.MakeHuman;
	if (!Wanted.SameStructure(Apparel))
	{
		DestroyWorn();
		GarmentCuts.Reset();
		++ApparelRebuilds;
		for (const FSimWornPiece& Piece : Wanted.Pieces)
		{
			const FString Cut = PawnGarmentCut(Pawns, Base, Piece);
			UE_LOG(LogTemp, Display, TEXT("Garments: %s on %s wears item %u (%d cuts) as '%s'"), *GetName(), *Base, Piece.Item, Piece.Cuts.Num(), *Cut);
			if (Cut.IsEmpty())
			{
				continue;  // nothing made for this piece yet: it is worn (and counts) but not drawn
			}
			GarmentCuts.Add(Piece.Item, Cut);
			TArray<FString> Garments;
			Cut.ParseIntoArray(Garments, TEXT("+"));
			for (const FString& Garment : Garments)
			{
				USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(this);
				Component->SetSkeletalMeshAsset(Pawns.GetGarmentMesh(Base, Garment));
				Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Component->SetCanEverAffectNavigation(false);
				Component->bReceivesDecals = false;
				Component->SetupAttachment(MakeHumanMesh);
				Component->RegisterComponent();
				Component->SetLeaderPoseComponent(MakeHumanMesh);
				WornMeshes.Add(Component);
				WornMeshItems.Add(Piece.Item);
			}
		}
		GarmentMorphHash = 0;  // the new garments take the body's morphs below
		// Skin under the clothes is cut away, so the body never pokes through (docs/design/clothes-draw.md).
		uint32 CoverBits = 0;
		for (UMeshComponent* Worn : WornMeshes)
		{
			const USkeletalMeshComponent* Garment = Cast<USkeletalMeshComponent>(Worn);
			USkeletalMesh* GarmentMesh = Garment ? Garment->GetSkeletalMeshAsset() : nullptr;
			const UGarmentCoverData* Cover = GarmentMesh ? GarmentMesh->GetAssetUserData<UGarmentCoverData>() : nullptr;
			if (Cover && Cover->CoverBit >= 0 && Cover->CoverBit < 32)
			{
				CoverBits |= 1u << Cover->CoverBit;
			}
		}
		SetMakeHumanCoverBits(CoverBits);
	}
	Apparel = Wanted;
	for (const FSimWornPiece& Piece : Apparel.Pieces)
	{
		for (int32 I = 0; I < WornMeshItems.Num(); ++I)
		{
			if (WornMeshItems[I] == Piece.Item && WornMeshes.IsValidIndex(I) && WornMeshes[I])
			{
				StyleGarment(WornMeshes[I], Piece);
			}
		}
	}
	SyncGarmentMorphs();
	bool bCovered = false;
	bool bCrotchCovered = false;
	bHeadwearDrawn = false;
	for (const FSimWornPiece& Piece : Apparel.Pieces)
	{
		bHeadwearDrawn = bHeadwearDrawn || (Piece.bHeadwear && GarmentCuts.Contains(Piece.Item));
		bCovered = bCovered || (Piece.bHidesHair && GarmentCuts.Contains(Piece.Item));
		bCrotchCovered = bCrotchCovered || (Piece.bCoversCrotch && GarmentCuts.Contains(Piece.Item));
	}
	SetMakeHumanHairCovered(bCovered);
	SetMakeHumanPubicCovered(bCrotchCovered);
	if (UMakeHumanAnimInstance* MhAnim = MakeHumanMesh ? Cast<UMakeHumanAnimInstance>(MakeHumanMesh->GetAnimInstance()) : nullptr)
	{
		MhAnim->SetGenitalTucked(bCrotchCovered);
	}
}

void ASimPawnActor::SetMakeHumanPubicCovered(bool bCovered)
{
	if (!MakeHumanMesh || bCovered == !CoveredPubicSections.IsEmpty())
	{
		return;
	}
	if (bCovered)
	{
		const TArray<FSkeletalMaterial>& Slots = MakeHumanMesh->GetSkeletalMeshAsset() ? MakeHumanMesh->GetSkeletalMeshAsset()->GetMaterials() : TArray<FSkeletalMaterial>();
		for (int32 I = 0; I < Slots.Num(); ++I)
		{
			if (Slots[I].MaterialSlotName.ToString().ToLower().Contains(TEXT("pubic")) && MakeHumanMesh->IsMaterialSectionShown(I, 0))
			{
				MakeHumanMesh->ShowMaterialSection(I, 0, false, 0);
				CoveredPubicSections.Add(I);
			}
		}
	}
	else
	{
		for (const int32 I : CoveredPubicSections)
		{
			MakeHumanMesh->ShowMaterialSection(I, 0, true, 0);
		}
		CoveredPubicSections.Reset();
	}
}

void ASimPawnActor::SetMakeHumanCoverBits(uint32 Bits)
{
	if (MakeHumanMesh)
	{
		MakeHumanMesh->SetCustomPrimitiveDataFloat(MakeHumanCoverLowIndex, static_cast<float>(Bits & 0xFFFFu));
		MakeHumanMesh->SetCustomPrimitiveDataFloat(MakeHumanCoverHighIndex, static_cast<float>(Bits >> 16));
	}
}

void ASimPawnActor::StyleGarment(UMeshComponent* Mesh, const FSimWornPiece& Piece) const
{
	// Colour is a look, not a sign of the stuff (sneed 2026-11-20): each made-of-stuff garment takes a clothing colour
	// picked from its item id, so it never changes. The stuff stays hidden in the numbers. Gear without a stuff
	// (flak, plasteel) keeps its own colours.
	static const FLinearColor Palette[] = {
		FLinearColor(0.05f, 0.07f, 0.13f), FLinearColor(0.16f, 0.18f, 0.09f), FLinearColor(0.45f, 0.38f, 0.25f), FLinearColor(0.22f, 0.22f, 0.22f),
		FLinearColor(0.20f, 0.12f, 0.07f), FLinearColor(0.10f, 0.16f, 0.28f), FLinearColor(0.55f, 0.53f, 0.48f), FLinearColor(0.30f, 0.07f, 0.06f),
		FLinearColor(0.08f, 0.08f, 0.08f), FLinearColor(0.62f, 0.60f, 0.55f), FLinearColor(0.12f, 0.20f, 0.17f), FLinearColor(0.35f, 0.22f, 0.12f),
		FLinearColor(0.25f, 0.27f, 0.30f), FLinearColor(0.40f, 0.30f, 0.32f), FLinearColor(0.28f, 0.30f, 0.20f), FLinearColor(0.15f, 0.10f, 0.18f)};
	const uint32 Pick = FCrc::MemCrc32(&Piece.Item, sizeof(Piece.Item), 0x7c01u) % UE_ARRAY_COUNT(Palette);
	const FLinearColor Colour = Palette[Pick];
	Mesh->SetCustomPrimitiveDataVector3(MakeHumanGarmentTintIndex, FVector(Colour.R, Colour.G, Colour.B));
	Mesh->SetCustomPrimitiveDataFloat(MakeHumanGarmentWearIndex, Piece.Wear);
	Mesh->SetCustomPrimitiveDataFloat(MakeHumanGarmentTintAmountIndex, Piece.bHasTint ? 1.0f : 0.0f);
}

void ASimPawnActor::SyncGarmentMorphs()
{
	if (!MakeHumanMesh || WornMeshes.IsEmpty())
	{
		return;
	}
	const TMap<FName, float>& Curves = MakeHumanMesh->GetMorphTargetCurves();
	uint32 Hash = static_cast<uint32>(Curves.Num()) + 1u;
	for (const TPair<FName, float>& Morph : Curves)
	{
		Hash = HashCombineFast(Hash, HashCombineFast(GetTypeHash(Morph.Key), GetTypeHash(FMath::RoundToInt32(Morph.Value * 1000.0f))));
	}
	if (Hash == GarmentMorphHash)
	{
		return;
	}
	GarmentMorphHash = Hash;
	for (UMeshComponent* Mesh : WornMeshes)
	{
		if (USkeletalMeshComponent* Garment = Cast<USkeletalMeshComponent>(Mesh))
		{
			Garment->ClearMorphTargets();
			for (const TPair<FName, float>& Morph : Curves)
			{
				Garment->SetMorphTarget(Morph.Key, Morph.Value, false);
			}
		}
	}
}

void ASimPawnActor::SetMakeHumanHairCovered(bool bCovered)
{
	if (!MakeHumanMesh || bCovered == !CoveredHairSections.IsEmpty())
	{
		return;
	}
	if (bCovered)
	{
		const TArray<FSkeletalMaterial>& Slots = MakeHumanMesh->GetSkeletalMeshAsset() ? MakeHumanMesh->GetSkeletalMeshAsset()->GetMaterials() : TArray<FSkeletalMaterial>();
		for (int32 I = 0; I < Slots.Num(); ++I)
		{
			if (PawnIsScalpHairSlot(Slots[I].MaterialSlotName.ToString()) && MakeHumanMesh->IsMaterialSectionShown(I, 0))
			{
				MakeHumanMesh->ShowMaterialSection(I, 0, false, 0);
				CoveredHairSections.Add(I);
			}
		}
	}
	else
	{
		for (const int32 I : CoveredHairSections)
		{
			MakeHumanMesh->ShowMaterialSection(I, 0, true, 0);
		}
		CoveredHairSections.Reset();
	}
}

void ASimPawnActor::ApplyApparel(USimPawnSubsystem& Pawns, const FSimPawnApparel& Wanted, uint64 Key)
{
	ApparelKey = Key;
	bApparelApplied = true;
	if (MakeHumanMesh)
	{
		ApplyGarments(Pawns, Wanted);
		return;
	}
	if (!MetaHuman || !MetaHumanBody)
	{
		return;  // a mannequin has no clothes to change
	}
	EnsureBareLayer(Pawns);
	if ((BareLayer.IsEmpty() && !bFullBody) || !DefaultTop || !DefaultTrousers)
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
	// With the whole body, a pawn that wears nothing at all goes barefoot (the sim has no footwear: dressed pawns keep Kellan's shoes).
	if (bFullBody && DefaultShoes)
	{
		DefaultShoes->SetVisibility(!Apparel.Pieces.IsEmpty());
	}

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
	LeanCarry = ReactionAge < kHitReactionSeconds ? LeanShown : FQuat::Identity;
	ReactionAge = 0.0f;
	++HitReactions;
	// Every hit that reaches the body (or its armour) shows a moment of pain on the face (docs/design/focus.md "Faces").
	PainFaceLeft = PawnPainFaceSeconds;
	++PainFacesShown;
}

// ---------------------------------------------------------------------------
// Fight faces (sneed, docs/design/focus.md "Faces"). Cosmetic.

void ASimPawnActor::UpdateFightFace(const USimSubsystem& Bridge, const colony::ThingView& Now, float GameDelta)
{
	// Angry while it fights hand to hand: swinging, blocking, or locked on with a melee weapon or bare hands (a gun in the
	// hands is not a melee fight), or in a fist fight; held a few seconds so it does not drop between blows.
	const bool bGun = Now.weapon.valid() && Bridge.GetDefs() && Bridge.GetDefs()->get(Now.weapon).has("verbs");
	const bool bMelee = Now.meleePhase != colony::MeleePhase::Ready || (Now.blocking && !bGun) || Now.socialFight ||
		(Now.focusTarget != colony::kNoEntity && !bGun);
	if (Now.downed || Now.dead)
	{
		AngryHoldLeft = 0.0f;
	}
	else if (bMelee)
	{
		AngryHoldLeft = PawnAngryHoldSeconds;
	}
	else
	{
		AngryHoldLeft = FMath::Max(0.0f, AngryHoldLeft - GameDelta);
	}
	PainFaceLeft = Now.dead ? 0.0f : FMath::Max(0.0f, PainFaceLeft - GameDelta);
	const float PainWanted = PainFaceLeft > 0.0f ? 1.0f : 0.0f;
	const float AngryWanted = AngryHoldLeft > 0.0f ? 1.0f : 0.0f;
	const float PainRate = GameDelta / (PainWanted > PainFace ? PawnPainFaceIn : PawnPainFaceOut);
	const float AngryRate = GameDelta / (AngryWanted > AngryFace ? PawnAngryFaceIn : PawnAngryFaceOut);
	const float OldPain = PainFace;
	const float OldAngry = AngryFace;
	PainFace = FMath::Clamp(PainFace + FMath::Clamp(PainWanted - PainFace, -PainRate, PainRate), 0.0f, 1.0f);
	AngryFace = FMath::Clamp(AngryFace + FMath::Clamp(AngryWanted - AngryFace, -AngryRate, AngryRate), 0.0f, 1.0f);
	if (PainFace != OldPain || AngryFace != OldAngry || (FaceBaseMesh.Get() != MakeHumanMesh.Get() && (PainFace > 0.0f || AngryFace > 0.0f)))
	{
		ApplyFightFace();
	}
}

void ASimPawnActor::ApplyFightFace()
{
	USkeletalMeshComponent* Mesh = MakeHumanMesh.Get();
	// The body changed under the faces (looks applied again): its own face is the new base.
	if (FaceBaseMesh.Get() != Mesh)
	{
		FaceBase.Reset();
		FaceBaseMesh = Mesh;
	}
	if (!Mesh || !Mesh->GetSkeletalMeshAsset())
	{
		return;
	}
	const bool bShowing = PainFace > 0.0f || AngryFace > 0.0f;
	if (FaceBase.IsEmpty())
	{
		if (!bShowing)
		{
			return;
		}
		// Keep the face it has now (the creator's or randomizer's expression units): the fight faces go on top of it.
		for (const FPawnExpressionUnit& Unit : PawnExpressionPain)
		{
			FaceBase.Add(Unit.Morph, Mesh->GetMorphTarget(Unit.Morph));
		}
		for (const FPawnExpressionUnit& Unit : PawnExpressionAngry)
		{
			FaceBase.Add(Unit.Morph, Mesh->GetMorphTarget(Unit.Morph));
		}
		const TArray<float>& Data = Mesh->GetCustomPrimitiveData().Data;
		const auto At = [&Data](int32 i) { return Data.IsValidIndex(i) ? Data[i] : 0.0f; };
		FaceBaseWrinkles = FVector4f(At(8), At(9), At(10), At(11));
	}
	// Pain wins over anger while it lasts.
	const float Pain = PainFace;
	const float Angry = AngryFace * (1.0f - PainFace);
	TMap<FName, float> Face = FaceBase;
	for (const FPawnExpressionUnit& Unit : PawnExpressionPain)
	{
		Face.FindOrAdd(Unit.Morph) += Unit.Weight * Pain;
	}
	for (const FPawnExpressionUnit& Unit : PawnExpressionAngry)
	{
		Face.FindOrAdd(Unit.Morph) += Unit.Weight * Angry;
	}
	for (const TPair<FName, float>& Morph : Face)
	{
		Mesh->SetMorphTarget(Morph.Key, FMath::Clamp(Morph.Value, 0.0f, 1.0f), false);
	}
	// The skin's wrinkle map (custom primitive data 8 to 11: forehead, brow furrow, smile, nose), as the creator sets it.
	const float Furrow = FMath::Max(FaceBaseWrinkles.Y, FMath::Max(Pain * 0.9f, Angry));
	const float Nose = FMath::Max(FaceBaseWrinkles.W, FMath::Max(Pain * 0.25f, Angry * 0.5f));
	const float Forehead = FMath::Max(FaceBaseWrinkles.X, Pain * 0.1f);
	Mesh->SetCustomPrimitiveDataVector4(8, FVector4(Forehead, Furrow, FaceBaseWrinkles.Z, Nose));
	if (!bShowing)
	{
		FaceBase.Reset();  // back to its own face; the next fight takes it afresh
	}
}

namespace
{
	// Whole-body react clips by the side the shot came from: front, right, back, left (Mixamo, retargeted).
	const TCHAR* const PawnHitClipSmall[4] = {TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromFront"), TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromRight"),
		TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromBack"), TEXT("Mixamo/Magic/A_ProMagicStandingReactSmallFromLeft")};
	const TCHAR* const PawnHitClipLarge[4] = {TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromFront"), TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromRight"),
		TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromBack"), TEXT("Mixamo/Magic/A_ProMagicStandingReactLargeFromLeft")};

	UAnimSequence* PawnHitClipLoad(const TCHAR* Rel)
	{
		const FString Name = FPaths::GetBaseFilename(Rel);
		return LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("/Game/Rim3D/Generated/Animations/%s.%s"), Rel, *Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
}

bool ASimPawnActor::PlayHitClip(const FVector& ShotDirection, bool bStaggered)
{
	USimPawnAnimInstance* Anim = GetAnim();
	if (!Anim || bRagdoll || IsJumping() || StanceHeight < 0.9f || RollProgress > 0.0f || Swing.IsActive())
	{
		return false;
	}
	// The side the shot came from, in the pawn's own frame: it travels along ShotDirection, so it came from -ShotDirection.
	const FVector From = -FVector(ShotDirection.X, ShotDirection.Y, 0.0).GetSafeNormal();
	if (From.IsNearlyZero())
	{
		return false;
	}
	const double Ahead = FVector::DotProduct(From, GetActorForwardVector());
	const double Right = FVector::DotProduct(From, GetActorRightVector());
	const int32 Side = FMath::Abs(Ahead) >= FMath::Abs(Right) ? (Ahead >= 0.0 ? 0 : 2) : (Right >= 0.0 ? 1 : 3);
	// A clip the sim moves the body along (a stagger, a swing) already shows the hit: it is not cut off for a react.
	// Nor is a react restarted by every bullet of a burst (each restart snapped the body back to the clip's start):
	// the hit layer's wobble shows the later ones.
	if (IsInMotionClip() || (HitMontage.IsValid() && Anim->Montage_IsPlaying(HitMontage.Get()) && HitClipAge < PawnHitClipMinGap))
	{
		return false;
	}
	UAnimSequence* Clip = PawnHitClipLoad(bStaggered ? PawnHitClipLarge[Side] : PawnHitClipSmall[Side]);
	// On the upper body only (sneed 2026-11-18: no animation resets): the legs keep walking or standing as they were
	// and the react blends in over the pose the body is in, instead of the whole body jumping into the clip's stance
	// and stepping where the sim does not move it.
	UAnimMontage* Montage = Clip ? Anim->PlaySlotAnimationAsDynamicMontage(Clip, USimPawnAnimInstance::UpperBodySlot, PawnHitClipBlendIn, PawnHitClipBlendOut) : nullptr;
	if (!Montage)
	{
		return false;
	}
	HitMontage = Montage;
	HitClipAge = 0.0f;
	// The stumble lasts while the sim slows the walk (its stagger is about 1.6 s at 1x), the flinch a short moment; the
	// walk or idle takes over again after.
	HitClipEndSeconds = FMath::Min(Clip->GetPlayLength(), bStaggered ? 1.3f : 0.7f);
	++HitClipsPlayed;
	return true;
}

namespace
{
	/** A bone's place in the animation's own mesh space at `Time`: its own and every parent's transform, multiplied up. */
	FVector PawnMotionBoneAt(const UAnimSequence& Clip, int32 Bone, double Time)
	{
		const FReferenceSkeleton& Ref = Clip.GetSkeleton()->GetReferenceSkeleton();
		const FAnimExtractContext Context(Time);
		FTransform Out = FTransform::Identity;
		for (int32 At = Bone; At != INDEX_NONE; At = Ref.GetParentIndex(At))
		{
			FTransform Local;
			Clip.GetBoneTransform(Local, FSkeletonPoseBoneIndex(At), Context, false);
			Out = Out * Local;
		}
		return Out.GetLocation();
	}
}

/**
 * Plays the clip the sim is moving this body along (docs/design/body-motion.md), at the sim's own moment of it. The
 * clip's travel is in its hips, and the sim has already put that travel into the pawn's position, so the visible body
 * is set back by how far the drawn animation has carried its hips from the animation's first frame (a trimmed clip's
 * travel before its `from` too), times the montage's blend weight: the hips stay exactly on the sim's position while
 * the clip blends in, plays and blends out, whatever stopped the body on the way. True while the sim has a clip.
 */
bool ASimPawnActor::UpdateMotionClip(const USimSubsystem& Bridge, const colony::ThingView& Now, float GameDelta)
{
	USimPawnAnimInstance* Anim = GetAnim();
	const colony::Sim* Sim = Bridge.GetSim();
	const colony::MotionClip* Clip = Sim && Anim && !bRagdoll && Now.motionClip.valid() ? Sim->combat().motion().clip(Now.motionClip) : nullptr;
	UAnimSequence* Asset = nullptr;
	if (Clip && !Clip->anim.empty())
	{
		const FString Rel = UTF8_TO_TCHAR(Clip->anim.c_str());
		Asset = LoadObject<UAnimSequence>(nullptr, *FString::Printf(TEXT("/Game/Rim3D/Generated/Animations/%s.%s"), *Rel, *FPaths::GetBaseFilename(Rel)), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Asset)
	{
		if (MotionClipDef != MAX_uint32)
		{
			// Over: the clip blends out, and the set-back fades with its weight below.
			MotionClipDef = MAX_uint32;
			if (Anim && MotionMontage.IsValid())
			{
				Anim->Montage_Stop(PawnMotionBlendOut, MotionMontage.Get());
			}
		}
	}
	else
	{
		// The clip's moment drawn: between the last two steps' moments, as the body's place is between their places (the
		// newest step's moment alone jumped 1/30 s at every step, and the catch-up below sped the clip up and down 30 times
		// a second: every bone jittered).
		float ShownTime = Now.motionTime;
		if (const colony::ThingView* Prev = Bridge.GetPreviousSnapshot().find(Thing);
			Prev && Prev->motionClip.value == Now.motionClip.value && Prev->motionTime <= Now.motionTime)
		{
			ShownTime = FMath::Lerp(Prev->motionTime, Now.motionTime, Bridge.GetBlendAlpha());
		}
		const float Want = Clip->from + ShownTime;
		const float Rate = FMath::Max(0.05f, Now.motionRate);
		MotionClipFrom = Clip->from;
		MotionClipHit = Clip->hitSeconds;
		if (MotionClipDef != Now.motionClip.value || Now.motionTime < MotionClipTime - 0.02f || Now.motionUpper != bMotionUpper || !MotionMontage.IsValid() ||
			!Anim->Montage_IsPlaying(MotionMontage.Get()))
		{
			// A stagger, a flinch or the player's own swing plays on the upper body, over legs that walk the way the body
			// really goes (sneed 2026-11-18); a roll, a sidestep, a knockdown or an AI's lunging swing plays whole.
			// A clip that follows another (the next swing, a stagger on a stagger) cross-fades from it a little longer, and
			// the one going out keeps its share of the set-back below while it fades, so the body does not jump.
			const bool bFollows = MotionMontage.IsValid() && Anim->Montage_IsPlaying(MotionMontage.Get());
			if (bFollows && !bMotionUpper && MotionAsset.IsValid() && MotionPelvisBone != INDEX_NONE)
			{
				MotionOutMontage = MotionMontage;
				MotionOutAsset = MotionAsset;
				MotionOutPelvisBone = MotionPelvisBone;
				MotionOutPelvisStart = MotionPelvisStart;
			}
			bMotionUpper = Now.motionUpper;
			MotionMontage = Anim->PlaySlotAnimationAsDynamicMontage(Asset, bMotionUpper ? USimPawnAnimInstance::UpperBodySlot : FAnimSlotGroup::DefaultSlotName,
				bFollows ? PawnMotionFollowBlendIn : PawnMotionBlendIn, PawnMotionBlendOut, Rate, 1, -1.0f, Want);
			MotionClipDef = Now.motionClip.value;
			ActionEndSeconds = 0.0;
			++MotionClipsPlayed;
			MotionAsset = Asset;
			const USkeleton* Skeleton = Asset->GetSkeleton();
			MotionPelvisBone = Skeleton ? Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(TEXT("pelvis"))) : INDEX_NONE;
			MotionPelvisStart = MotionPelvisBone != INDEX_NONE ? PawnMotionBoneAt(*Asset, MotionPelvisBone, 0.0) : FVector::ZeroVector;
		}
		else
		{
			// Kept in step with the sim: its rate, and its moment if the picture drifted (a pause, a hitch, a hit-stop). A
			// picture a little behind (a hit-stop froze it while the sim went on) plays faster until it is back, within about
			// 0.1 s, so the blow still lands on its step; the sim's moment is whole steps, so a picture up to a step ahead is right.
			// The montage moves on by this frame's time after this (the meshes tick after the pawns): where it will be drawn.
			const UWorld* ClipWorld = GetWorld();
			const float Ahead = ClipWorld ? ClipWorld->GetDeltaSeconds() * Anim->Montage_GetPlayRate(MotionMontage.Get()) : 0.0f;
			const float Drift = Want - (Anim->Montage_GetPosition(MotionMontage.Get()) + Ahead);
			if (FMath::Abs(Drift) > 0.08f)
			{
				Anim->Montage_SetPosition(MotionMontage.Get(), Want);
				Anim->Montage_SetPlayRate(MotionMontage.Get(), Rate);
			}
			else
			{
				// Gently back in step, either way (the moment drawn is smooth now, so this stays small).
				Anim->Montage_SetPlayRate(MotionMontage.Get(), FMath::Max(0.05f, Rate + FMath::Clamp(Drift / 0.25f, -0.3f * Rate, 0.3f * Rate)));
			}
		}
		MotionClipTime = Now.motionTime;
	}

	// The set-back: the drawn hips' travel at each montage's own moment, as much as each shows (blending in or out): the
	// clip now playing, and the one it took over from while that fades out (without it the body jumped forward by the
	// old clip's travel the moment a second swing or stagger began).
	const auto MontageShown = [Anim](const TWeakObjectPtr<UAnimMontage>& Montage) -> const FAnimMontageInstance*
	{
		const FAnimMontageInstance* Best = nullptr;
		if (Anim && Montage.IsValid())
		{
			for (const FAnimMontageInstance* Each : Anim->MontageInstances)
			{
				if (Each && Each->Montage == Montage.Get() && Each->IsValid() && (!Best || Each->GetWeight() > Best->GetWeight()))
				{
					Best = Each;
				}
			}
		}
		return Best;
	};
	// The mannequin faces +Y in its own space and the actor +X, so the mesh's +Y is the actor's +X and its -X the
	// actor's +Y (right); the body is scaled by the pawn's height, and so is its hips' travel.
	const float Height = bHasLooks ? Looks.Height : 1.0f;
	const UWorld* World = GetWorld();
	const double FrameAhead = World ? static_cast<double>(World->GetDeltaSeconds()) : 0.0;
	const auto SetBack = [Height, FrameAhead](const FAnimMontageInstance& Playing, const UAnimSequence& PlayingAsset, int32 Pelvis, const FVector& PelvisStart)
	{
		const double Ahead = Playing.IsPlaying() ? FrameAhead * static_cast<double>(Playing.GetPlayRate()) : 0.0;
		const double At = FMath::Clamp(static_cast<double>(Playing.GetPosition()) + Ahead, 0.0, static_cast<double>(PlayingAsset.GetPlayLength()));
		const FVector Travel = PawnMotionBoneAt(PlayingAsset, Pelvis, At) - PelvisStart;
		return FVector(-Travel.Y, Travel.X, 0.0) * Playing.GetWeight() * Height;
	};
	// The bodies' meshes tick after the pawns (TG_PostUpdateWork), so the moments and weights read here are a frame
	// old; the hips' travel is read a frame on (the montage's moment plus this frame's play), which is where it is drawn.
	const FAnimMontageInstance* Shown = MontageShown(MotionMontage);
	const UAnimSequence* ShownAsset = MotionAsset.Get();
	MotionOffset = FVector::ZeroVector;
	LegsClipWeight = Shown && !bMotionUpper ? Shown->GetWeight() : 0.0f;
	if (Shown && ShownAsset && MotionPelvisBone != INDEX_NONE && !bMotionUpper)
	{
		MotionOffset += SetBack(*Shown, *ShownAsset, MotionPelvisBone, MotionPelvisStart);
	}
	else if (!Asset)
	{
		MotionMontage.Reset();
		MotionAsset.Reset();
	}
	const FAnimMontageInstance* Out = MotionOutMontage != MotionMontage ? MontageShown(MotionOutMontage) : nullptr;
	if (Out && MotionOutAsset.IsValid() && MotionOutPelvisBone != INDEX_NONE)
	{
		LegsClipWeight = FMath::Max(LegsClipWeight, Out->GetWeight());
		MotionOffset += SetBack(*Out, *MotionOutAsset.Get(), MotionOutPelvisBone, MotionOutPelvisStart);
	}
	else
	{
		MotionOutMontage.Reset();
		MotionOutAsset.Reset();
	}
	return Asset != nullptr;
}

void ASimPawnActor::BeginMeleeSwing(EMeleeStyle Style, float StepsToHit)
{
	Swing.Begin(Style, StepsToHit, StanceHeight);
	SwingMismatch = 0.0f;
}

bool ASimPawnActor::GetMotionClipToHit(float& OutSeconds, float& OutRate) const
{
	const USimPawnAnimInstance* Anim = GetAnim();
	if (!IsInMotionClip() || MotionClipHit <= 0.0f || !Anim || !MotionMontage.IsValid() || !Anim->Montage_IsPlaying(MotionMontage.Get()))
	{
		return false;
	}
	OutSeconds = MotionClipHit - (Anim->Montage_GetPosition(MotionMontage.Get()) - MotionClipFrom);
	OutRate = FMath::Max(0.05f, Anim->Montage_GetPlayRate(MotionMontage.Get()));
	return true;
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

// ---------------------------------------------------------------------------
// The hit layer (docs/design/body-health.md part E).

namespace
{
	/** The crawl clip: the one in the Mixamo crawling folder that crawls forward (not backward, not a way in or out of
	 *  the crawl), an injured or slow one before the others. Null if the folder has none. */
	UAnimSequence* PawnFindCrawlClip()
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(PawnCrawlFolder), Assets, true);
		const FAssetData* Best = nullptr;
		int32 BestScore = -1;
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath != UAnimSequence::StaticClass()->GetClassPathName())
			{
				continue;
			}
			const FString Name = Asset.AssetName.ToString();
			if (!Name.Contains(TEXT("Crawl"), ESearchCase::CaseSensitive) || Name.Contains(TEXT("Back"), ESearchCase::CaseSensitive) || Name.Contains(TEXT("To"), ESearchCase::CaseSensitive) || Name.Contains(TEXT("Stand"), ESearchCase::CaseSensitive) || Name.Contains(TEXT("Idle"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			// Low on the belly before hands and knees; ties go to the name first in the alphabet, so every run picks the same.
			const int32 Score = (Name.Contains(TEXT("Injured"), ESearchCase::CaseSensitive) ? 4 : 0) + (Name.Contains(TEXT("Prone"), ESearchCase::CaseSensitive) || Name.Contains(TEXT("Low"), ESearchCase::CaseSensitive) ? 2 : 0) + (Name.Contains(TEXT("Run"), ESearchCase::CaseSensitive) || Name.Contains(TEXT("Fast"), ESearchCase::CaseSensitive) ? 0 : 1);
			if (Score > BestScore || (Score == BestScore && Best && Name < Best->AssetName.ToString()))
			{
				Best = &Asset;
				BestScore = Score;
			}
		}
		return Best ? Cast<UAnimSequence>(Best->GetAsset()) : nullptr;
	}
}

ESimPawnZone ASimPawnActor::ZoneOfSimZone(const USimSubsystem& Bridge, int32 SimZone) const
{
	const colony::Sim* Sim = Bridge.GetSim();
	if (!Sim || SimZone < 0)
	{
		return ESimPawnZone::Torso;
	}
	// The sim's zones come in the body's order, the left arm and leg before the right.
	const std::vector<std::string> Names = Sim->health().zoneNames(Thing);
	if (static_cast<size_t>(SimZone) >= Names.size())
	{
		return ESimPawnZone::Torso;
	}
	const std::string& Name = Names[static_cast<size_t>(SimZone)];
	bool bSecond = false;
	for (int32 i = 0; i < SimZone; ++i)
	{
		bSecond |= Names[static_cast<size_t>(i)] == Name;
	}
	if (Name == "Head")
	{
		return ESimPawnZone::Head;
	}
	if (Name == "Arm")
	{
		return bSecond ? ESimPawnZone::ArmRight : ESimPawnZone::ArmLeft;
	}
	if (Name == "Leg")
	{
		return bSecond ? ESimPawnZone::LegRight : ESimPawnZone::LegLeft;
	}
	return ESimPawnZone::Torso;
}

void ASimPawnActor::KickZone(const USimSubsystem& Bridge, int32 Part, const FVector& AtWorld, const FVector& DirectionWorld, float Strength)
{
	USimPawnAnimInstance* Anim = GetAnim();
	if (!Anim || bRagdoll)
	{
		return;
	}
	const colony::Sim* Sim = Bridge.GetSim();
	const int32 SimZone = Sim && Part >= 0 ? Sim->health().partZone(Thing, Part) : -1;
	ESimPawnZone Zone = ESimPawnZone::Torso;
	if (SimZone >= 0)
	{
		Zone = ZoneOfSimZone(Bridge, SimZone);
	}
	else
	{
		// By where it landed: how far up the body (as it stands, crouches or lies), and how far to a side.
		const float Height = (bHasLooks ? Looks.Height : 1.0f) * 180.0f * StanceHeight;
		const FVector Local = GetActorTransform().InverseTransformPositionNoScale(AtWorld);
		const float Up = Height > 1.0f ? static_cast<float>(Local.Z) / Height : 0.6f;
		const bool bRightSide = Local.Y > 0.0;
		if (Up > 0.86f)
		{
			Zone = ESimPawnZone::Head;
		}
		else if (Up < 0.5f)
		{
			Zone = bRightSide ? ESimPawnZone::LegRight : ESimPawnZone::LegLeft;
		}
		else if (FMath::Abs(Local.Y) > 18.0 * (bHasLooks ? Looks.Height : 1.0f))
		{
			Zone = bRightSide ? ESimPawnZone::ArmRight : ESimPawnZone::ArmLeft;
		}
	}
	FSimPawnKick Hit;
	Hit.Zone = Zone;
	// Into the mesh's own space, as the body is turned now (leaning, swinging).
	Hit.Direction = Body->GetComponentTransform().InverseTransformVectorNoScale(DirectionWorld.GetSafeNormal());
	Hit.Strength = FMath::Clamp(Strength, 0.0f, 1.0f);
	if (Anim->Inputs.Kicks.Num() < 4)
	{
		Anim->Inputs.Kicks.Add(Hit);
	}
	LastKickZone = Zone;
	++ZoneKicks;
}

void ASimPawnActor::UpdateHurt(const USimSubsystem& Bridge, const colony::ThingView& Now, float DeltaTime)
{
	bPatchPosed = Now.patchZone >= 0 && !Now.downed && !Now.dead;
	if (bPatchPosed)
	{
		PatchZone = ZoneOfSimZone(Bridge, Now.patchZone);
	}
	HurtAge += DeltaTime;
	if (HurtAge < PawnHurtInterval)
	{
		return;
	}
	HurtAge = 0.0f;
	LimpLeft = 0.0f;
	LimpRight = 0.0f;
	const colony::Sim* Sim = Bridge.GetSim();
	if (!Sim || Now.pain <= 0.0f || Now.downed || Now.dead)
	{
		return;
	}
	const std::vector<std::string> Names = Sim->health().zoneNames(Thing);
	bool bSecond = false;
	for (size_t i = 0; i < Names.size(); ++i)
	{
		if (Names[i] != "Leg")
		{
			continue;
		}
		const float Health = Sim->health().zoneHealth(Thing, static_cast<int>(i));
		(bSecond ? LimpRight : LimpLeft) = FMath::Clamp((PawnLimpFrom - Health) / (PawnLimpFrom - PawnLimpFull), 0.0f, 1.0f);
		bSecond = true;
	}
}

/**
 * A pawn downed by its torso and awake crawls (part D): it lies in the crawl clip instead of going limp, the clip
 * running as fast as the sim moves it and holding still when it does not. The clip's travel is in its hips, as with the
 * clips of body-motion.md, so the body is set back by it and stays on the sim's position.
 */
void ASimPawnActor::UpdateCrawl(const colony::ThingView& Now, float GameDelta)
{
	USimPawnAnimInstance* Anim = GetAnim();
	const bool bWanted = Anim && !bRagdoll && Now.crawling && Now.downed && !Now.dead && !Now.motionClip.valid();
	if (bWanted && !bCrawlClipTried)
	{
		bCrawlClipTried = true;
		CrawlClip = PawnFindCrawlClip();
		if (CrawlClip)
		{
			const USkeleton* Skeleton = CrawlClip->GetSkeleton();
			CrawlPelvisBone = Skeleton ? Skeleton->GetReferenceSkeleton().FindBoneIndex(FName(TEXT("pelvis"))) : INDEX_NONE;
			CrawlClipSpeed = PawnCrawlDefaultSpeed;
			if (CrawlPelvisBone != INDEX_NONE && CrawlClip->GetPlayLength() > 0.1f)
			{
				CrawlPelvisStart = PawnMotionBoneAt(*CrawlClip, CrawlPelvisBone, 0.0);
				const double Travel = (PawnMotionBoneAt(*CrawlClip, CrawlPelvisBone, CrawlClip->GetPlayLength()) - CrawlPelvisStart).Size2D();
				if (Travel > 10.0)
				{
					CrawlClipSpeed = static_cast<float>(Travel) / CrawlClip->GetPlayLength();
				}
			}
		}
	}
	if (!bWanted || !CrawlClip)
	{
		if (bCrawling)
		{
			bCrawling = false;
			if (Anim && CrawlMontage.IsValid())
			{
				Anim->Montage_Stop(PawnCrawlBlend, CrawlMontage.Get());
			}
		}
	}
	else
	{
		const float Height = bHasLooks ? Looks.Height : 1.0f;
		const float Rate = GroundSpeed > PawnCrawlStill ? FMath::Clamp(GroundSpeed / (CrawlClipSpeed * Height), 0.4f, 2.0f) : 0.02f;  // all but still: lying where it is
		if (!bCrawling || !CrawlMontage.IsValid() || !Anim->Montage_IsPlaying(CrawlMontage.Get()))
		{
			CrawlMontage = Anim->PlaySlotAnimationAsDynamicMontage(CrawlClip, FAnimSlotGroup::DefaultSlotName, PawnCrawlBlend, PawnCrawlBlend, 1.0f, 2000);
			bCrawling = true;
			ActionEndSeconds = 0.0;
			++CrawlsPlayed;
		}
		if (CrawlMontage.IsValid())
		{
			Anim->Montage_SetPlayRate(CrawlMontage.Get(), Rate);
		}
	}

	// The set-back, as UpdateMotionClip's: the hips' travel since the clip's first frame at the moment that is drawn,
	// as much as the montage shows.
	const FAnimMontageInstance* Shown = nullptr;
	if (Anim && CrawlMontage.IsValid())
	{
		for (const FAnimMontageInstance* Instance : Anim->MontageInstances)
		{
			if (Instance && Instance->Montage == CrawlMontage.Get() && Instance->IsValid() && (!Shown || Instance->GetWeight() > Shown->GetWeight()))
			{
				Shown = Instance;
			}
		}
	}
	if (Shown && CrawlClip && CrawlPelvisBone != INDEX_NONE && CrawlClip->GetPlayLength() > 0.1f)
	{
		const double At = FMath::Fmod(static_cast<double>(Shown->GetPosition()), static_cast<double>(CrawlClip->GetPlayLength()));
		const FVector Travel = PawnMotionBoneAt(*CrawlClip, CrawlPelvisBone, At) - CrawlPelvisStart;
		CrawlOffset = FVector(-Travel.Y, Travel.X, 0.0) * Shown->GetWeight() * (bHasLooks ? Looks.Height : 1.0f);
	}
	else
	{
		CrawlOffset = FVector::ZeroVector;
		if (!bCrawling)
		{
			CrawlMontage.Reset();
		}
	}
}

namespace
{
	/** The Game Animation Sample's climb from standing onto a 2.5 m ledge: jump, catch the edge, pull up and over. */
	const TCHAR* const PawnWallClimbClipPath =
		TEXT("/Game/Characters/UEFN_Mannequin/Animations/Traversal/Climb/M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot.M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot");
	/** Share of the clip up to the moment the hands have the edge: the sim's time on the face plays up to here, its
	 *  mantle (colony::kWallMantleFrom on) the rest. To be tuned by eye on the PC. */
	constexpr float PawnWallClimbGrab = 0.45f;
	/** Share of the clip where the body stands on the top (PC measures, #389 rounds 2 and 3): the rest is settling, not played. */
	constexpr float PawnWallClimbEnd = 0.68f;
	/** The wall's height, cm: a level (the sim's colony::Map::kLevelHeightM). */
	constexpr float PawnWallClimbHeightCm = 300.0f;
	constexpr float PawnWallClimbBlend = 0.2f;
}

void ASimPawnActor::UpdateWallClimb(const USimSubsystem& Bridge, const colony::ThingView& Now)
{
	USimPawnAnimInstance* Anim = GetAnim();
	const bool bOn = Now.wallClimb && Now.climb > 0.0f && Anim && !bRagdoll;
	if (bOn && !bWallClimbClipTried)
	{
		bWallClimbClipTried = true;
		WallClimbClip = LoadObject<UAnimSequence>(nullptr, PawnWallClimbClipPath);
		if (!WallClimbClip)
		{
			UE_LOG(LogTemp, Warning, TEXT("Wall climb: no clip at %s"), PawnWallClimbClipPath);
		}
	}
	if (!bOn || !WallClimbClip || WallClimbClip->GetPlayLength() <= 0.1f)
	{
		if (Anim && WallClimbMontage.IsValid())
		{
			Anim->Montage_Stop(PawnWallClimbBlend, WallClimbMontage.Get());
		}
		WallClimbMontage.Reset();
		WallClimbOffset = FVector::ZeroVector;
		return;
	}
	if (!WallClimbMontage.IsValid() || !Anim->Montage_IsPlaying(WallClimbMontage.Get()))
	{
		WallClimbMontage = Anim->PlaySlotAnimationAsDynamicMontage(WallClimbClip, FAnimSlotGroup::DefaultSlotName, PawnWallClimbBlend,
			PawnWallClimbBlend, 1.0f, 1);
		ActionEndSeconds = 0.0;
		++WallClimbsPlayed;
	}
	if (!WallClimbMontage.IsValid())
	{
		return;
	}
	// The clip's time follows the climb: going up, the face to the catch, then the pull over the top; going down, the
	// same backwards, evenly.
	const float U = Now.wallClimbUp ? Now.climb : 1.0f - Now.climb;
	const float Length = WallClimbClip->GetPlayLength();
	float Share = 0.0f;
	if (U < colony::kWallMantleFrom)
	{
		const float K = U / colony::kWallMantleFrom;
		Share = PawnWallClimbGrab * K;  // even: eased, the catch came early and the end of the face froze (#389 round 3)
	}
	else
	{
		const float K = (U - colony::kWallMantleFrom) / (1.0f - colony::kWallMantleFrom);
		Share = PawnWallClimbGrab + (PawnWallClimbEnd - PawnWallClimbGrab) * K;
	}
	const float At = FMath::Clamp(Share, 0.0f, 1.0f) * Length;
	Anim->Montage_SetPlayRate(WallClimbMontage.Get(), 0.0f);
	Anim->Montage_SetPosition(WallClimbMontage.Get(), At);
	// The pose is drawn about its root, and the clip's hands hold the edge only while the root rises the way the clip's
	// does. So the body is drawn at the clip's own height up the wall (its rise to the end share stretched to the wall),
	// not the sim's straight line: only the height, the sim keeps where it is across the ground.
	const float TotalRise = static_cast<float>(WallClimbClip->ExtractRootMotionFromRange(0.0, PawnWallClimbEnd * Length, FAnimExtractContext()).GetTranslation().Z);
	const float ClipRise = static_cast<float>(WallClimbClip->ExtractRootMotionFromRange(0.0, At, FAnimExtractContext()).GetTranslation().Z);
	const int32 LowLevel = Now.wallClimbUp ? Now.level : Now.level - 1;
	const double FootZ = Bridge.SimToWorld(Now.x, Now.y, 0).Z + LowLevel * PawnWallClimbHeightCm;
	const double SimRise = GetActorLocation().Z - FootZ;
	const double Wanted = TotalRise > 10.0f ? FMath::Clamp(ClipRise / TotalRise, 0.0f, 1.0f) * PawnWallClimbHeightCm : SimRise;
	WallClimbOffset = FVector(0.0, 0.0, Wanted - SimRise);
}

float ASimPawnActor::GetMeleeBodyDegrees() const
{
	const FMeleePose Pose = ComputeMeleePose();
	return FMath::Abs(Pose.Lean) + FMath::Abs(Pose.Tilt) + FMath::Abs(Pose.Twist);
}

void ASimPawnActor::ApplyBodyPose(float GameDelta, bool bBlocking)
{
	// A parry: up fast (a fast swing read late leaves only a few steps between the guard going up and the blow, sneed
	// 2026-11-19), and held a moment after the sim lets go, so the blade meets the blow and doesn't flick.
	if (bBlocking)
	{
		BlockHoldLeft = 0.25f;
	}
	else
	{
		BlockHoldLeft = FMath::Max(0.0f, BlockHoldLeft - GameDelta);
	}
	const bool bGuardUp = bBlocking || BlockHoldLeft > 0.0f;
	BlockBlend = FMath::FInterpConstantTo(BlockBlend, bGuardUp ? 1.0f : 0.0f, GameDelta, bGuardUp ? 20.0f : 8.0f);
	Swing.Advance(GameDelta * colony::kStepsPerRealSecondAt1x);
	if (DodgeAge < kDodgeSeconds)
	{
		DodgeAge = FMath::Min(kDodgeSeconds, DodgeAge + GameDelta);
	}
	const bool bReacting = ReactionAge < kHitReactionSeconds;
	const bool bMelee = Swing.IsActive() || BlockBlend > 0.0f || DodgeAge < kDodgeSeconds;
	const bool bRolling = RollProgress > 0.0f;
	AdvanceJump(GameDelta);
	const float JumpHeight = GetJumpHeight();
	const bool bMotion = !MotionOffset.IsNearlyZero(0.01) || !CrawlOffset.IsNearlyZero(0.01) || !WallClimbOffset.IsNearlyZero(0.01);
	if (!bReacting && !bMelee && !bRolling && !bMotion && JumpHeight <= 0.0f)
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
		// Over in the first 20%, then back; eased at both ends of both, so the body never starts or stops at once
		// (docs/design/pawns.md "No pops").
		const auto Ease = [](float X) { X = FMath::Clamp(X, 0.0f, 1.0f); return X * X * (3.0f - 2.0f * X); };
		const float Envelope = T < 0.2f ? Ease(T / 0.2f) : 1.0f - Ease((T - 0.2f) / 0.8f);
		LeanDegrees = PawnHitLeanDegrees * ReactionStrength * Envelope;
		// The top of the body goes along the shot: turn about up x direction.
		const FVector Axis = FVector::CrossProduct(FVector::UpVector, ReactionDirection).GetSafeNormal();
		LeanWorld = FQuat(Axis, FMath::DegreesToRadians(LeanDegrees));
		// A hit before the last one's lean is over: that lean eases out over the new one's way in (it used to jump back).
		if (!LeanCarry.Equals(FQuat::Identity, 1.0e-5))
		{
			LeanWorld = FQuat::Slerp(LeanCarry, FQuat::Identity, Ease(T / 0.2f)) * LeanWorld;
		}
		LeanShown = LeanWorld;
	}
	else
	{
		LeanDegrees = 0.0f;
		LeanCarry = FQuat::Identity;
		LeanShown = FQuat::Identity;
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
	Offset.Z += JumpHeight;
	Offset += MotionOffset;  // a clip's own travel is taken out: the sim's position already has it (body-motion.md)
	Offset += CrawlOffset;   // and the crawl clip's
	Offset += WallClimbOffset;  // and the wall climb's
	Body->SetRelativeLocationAndRotation(Offset, RootQuat.Inverse() * RollWorld * LeanWorld * RootQuat * MeleeLocal * PawnBodyRelativeRotation.Quaternion());
}

void ASimPawnActor::ResetRagdollFloor(bool bWanted)
{
	if (RagdollFloor)
	{
		RagdollFloor->DestroyComponent();
		RagdollFloor = nullptr;
	}
	if (bWanted && bRagdoll)
	{
		SpawnRagdollFloor();
	}
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
	const FVector SimNormal = FVector((Map.heightAt(X - 1.0f, Y) - Map.heightAt(X + 1.0f, Y)) * 0.5f, (Map.heightAt(X, Y - 1.0f) - Map.heightAt(X, Y + 1.0f)) * 0.5f, 1.0f).GetSafeNormal();
	FVector Ground = Bridge->SimToWorld(X, Y, 0);
	FVector Normal = SimNormal;
	// The ground that is drawn, where it differs from the height map (sneed saw corpses sunk into it).
	FHitResult GroundHit;
	FCollisionQueryParams GroundParams(SCENE_QUERY_STAT(PawnRagdollGround), false, this);
	const FVector Over(GetActorLocation().X, GetActorLocation().Y, Ground.Z);
	if (GetWorld()->LineTraceSingleByChannel(GroundHit, Over + FVector(0, 0, 150), Over - FVector(0, 0, 300), ECC_Visibility, GroundParams) &&
		!Cast<ASimPawnActor>(GroundHit.GetActor()) && FMath::Abs(GroundHit.ImpactPoint.Z - Ground.Z) < 30.0)  // not a plant or a crate
	{
		Ground = GroundHit.ImpactPoint;
		Normal = GroundHit.ImpactNormal.Z > 0.7 ? FVector(GroundHit.ImpactNormal) : SimNormal;
	}
	// A little above it: the body falls on the mannequin's physics shapes, and the drawn flesh (hips, chest, back)
	// sits outside them, so a floor right at the ground lets it sink in.
	Ground += Normal * PawnRagdollFloorLift;

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
	BlockHoldLeft = 0.0f;
	DodgeAge = kDodgeSeconds;
	HitStopLeft = 0.0f;
	bBodyPosed = false;
	bCrawling = false;
	CrawlOffset = FVector::ZeroVector;
	bPatchPosed = false;
	if (USimPawnAnimInstance* Anim = GetAnim())
	{
		// The hit layer forgets its springs: the physics body has the hit now.
		Anim->Inputs.bHitLayer = false;
		Anim->Inputs.Kicks.Reset();
		if (CrawlMontage.IsValid())
		{
			Anim->Montage_Stop(0.0f, CrawlMontage.Get());
		}
		if (WallClimbMontage.IsValid())
		{
			Anim->Montage_Stop(0.0f, WallClimbMontage.Get());
		}
	}
	CrawlMontage.Reset();
	WallClimbMontage.Reset();
	WallClimbOffset = FVector::ZeroVector;
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

void ASimPawnActor::UpdateMeshTickGroups()
{
	// The pawn subsystem moves and turns the actor as a tickable, after TG_PrePhysics: a mesh animated there draws a pose
	// made for last frame's place and facing, so every turn and step swings the feet by one frame's move and back
	// (docs/design/pawns.md "No pops"). TG_PostUpdateWork comes after the tickables: the pose is made for the place it
	// is drawn at. A ragdoll keeps TG_PrePhysics, where physics expects its body.
	const ETickingGroup Group = bRagdoll ? TG_PrePhysics : TG_PostUpdateWork;
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(this);
	if (MetaHuman)
	{
		TInlineComponentArray<USkeletalMeshComponent*> More(MetaHuman.Get());
		Meshes.Append(More);
	}
	if (MeshTickGroup == static_cast<int32>(Group) && MeshTickCount == Meshes.Num())
	{
		return;
	}
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		Mesh->PrimaryComponentTick.TickGroup = Group;
		Mesh->PrimaryComponentTick.EndTickGroup = Group;
		if (Mesh != Body.Get())
		{
			// Copies of the body's pose (the MakeHuman retarget, MetaHuman parts) come after it.
			Mesh->PrimaryComponentTick.AddPrerequisite(Body.Get(), Body->PrimaryComponentTick);
		}
	}
	MeshTickGroup = static_cast<int32>(Group);
	MeshTickCount = Meshes.Num();
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

bool ASimPawnActor::TraceDrawnBody(const FVector& From, const FVector& To, FVector& OutHit)
{
	if (!Body || !Body->GetSkeletalMeshAsset())
	{
		return false;
	}
	if (!Body->GetPhysicsAsset())
	{
		if (UPhysicsAsset* Asset = LoadObject<UPhysicsAsset>(nullptr, PawnRagdollPhysicsAssetPath))
		{
			Body->SetPhysicsAsset(Asset, false);
		}
	}
	if (!Body->GetPhysicsAsset())
	{
		return false;
	}
	// March along the line in 1.5 cm steps; the first step on or in a shape is the skin.
	const FVector Line = To - From;
	const int32 Steps = FMath::Clamp(FMath::CeilToInt(static_cast<float>(Line.Size()) / 1.5f), 1, 200);
	for (int32 i = 0; i <= Steps; ++i)
	{
		const FVector At = From + Line * (static_cast<double>(i) / Steps);
		FClosestPointOnPhysicsAsset Closest;
		if (Body->GetClosestPointOnPhysicsAsset(At, Closest, false) && Closest.Distance <= 0.75f)
		{
			OutHit = At;
			return true;
		}
	}
	return false;
}

USkeletalMeshComponent* ASimPawnActor::GetDecalMesh() const
{
	// The body that is drawn: a MakeHuman body hides the MetaHuman, so wounds and blood stuck to that would never show.
	if (MakeHumanMesh)
	{
		return MakeHumanMesh.Get();
	}
	return MetaHumanBody ? MetaHumanBody.Get() : Body.Get();
}

bool ASimPawnActor::TraceDrawnBody(const FVector& Start, const FVector& Direction, double MaxCm, DrawnHit::FResult& Out, bool& bOutHasSkeleton) const
{
	bOutHasSkeleton = false;
	const USkeletalMeshComponent* Mesh = GetDecalMesh();
	if (Mesh && !DrawnHit::HasSkeleton(*Mesh))
	{
		Mesh = Body.Get();
	}
	if (!Mesh || !DrawnHit::HasSkeleton(*Mesh))
	{
		return false;
	}
	bOutHasSkeleton = true;
	if (IsHidden())
	{
		return false;
	}
	return DrawnHit::Trace(*Mesh, GetBodyScale(), Start, Direction, MaxCm, Out);
}

double ASimPawnActor::DistanceToDrawnBodyCm(const FVector& Point) const
{
	const USkeletalMeshComponent* Mesh = GetDecalMesh();
	if (Mesh && !DrawnHit::HasSkeleton(*Mesh))
	{
		Mesh = Body.Get();
	}
	return Mesh && DrawnHit::HasSkeleton(*Mesh) ? DrawnHit::DistanceCm(*Mesh, GetBodyScale(), Point) : -1.0;
}

bool ASimPawnActor::GetDrawnHeadCentre(FVector& Out) const
{
	const USkeletalMeshComponent* Mesh = GetDecalMesh();
	if (Mesh && !DrawnHit::HasSkeleton(*Mesh))
	{
		Mesh = Body.Get();
	}
	if (!Mesh || !DrawnHit::HasSkeleton(*Mesh))
	{
		return false;
	}
	const FVector Neck = Mesh->GetBoneLocation(TEXT("neck_01"));
	const FVector Head = Mesh->GetBoneLocation(TEXT("head"));
	// Halfway up the head capsule DrawnHit uses (4 to 13 cm over the head bone, for a 1.80 m body).
	Out = Head + (Head - Neck).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector) * (8.5 * FMath::Clamp(GetBodyScale(), 0.5f, 1.5f));
	return true;
}

void ASimPawnActor::SetBodyHidden(bool bHide)
{
	bBodyHidden = bHide;
	const bool bAnyHidden = bBodyHidden || bCutAway;
	SetActorHiddenInGame(bAnyHidden);
	if (MetaHuman)
	{
		// A MakeHuman body replaces the MetaHuman for good: showing the pawn again (third person after first) must not
		// bring the MetaHuman back over it (sneed saw the protagonist's bearded MetaHuman inside the woman).
		MetaHuman->SetActorHiddenInGame(bAnyHidden || MakeHumanMesh != nullptr);
	}
}

void ASimPawnActor::SetCutAway(bool bCut)
{
	if (bCut != bCutAway)
	{
		bCutAway = bCut;
		SetBodyHidden(bBodyHidden);
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
