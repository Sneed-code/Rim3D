#include "FreeHandSubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include <imgui.h>

#include "MakeHumanAnimInstance.h"
#include "OrdersSubsystem.h"
#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "SoftContactSubsystem.h"

#include "ColonySim/Snapshot.h"

namespace
{
	TAutoConsoleVariable<float> GFreeHandSpeed(TEXT("Hand.MarkerSpeed"), 1.0f,
		TEXT("How fast the free hand's marker follows the mouse (times the mouse turn speed)."), ECVF_Default);
	TAutoConsoleVariable<float> GFreeHandFlesh(TEXT("Hand.FleshPad"), 0.0f,
		TEXT("Centimetres added to every body stand-in the free hand touches (more: the hand stops further out from the bones)."), ECVF_Default);
	TAutoConsoleVariable<float> GFreeHandPress(TEXT("Hand.Press"), 2.0f,
		TEXT("Centimetres the gripping hand presses into a body's skin (the squish moves the skin at most 3 cm)."), ECVF_Default);
	TAutoConsoleVariable<float> GFreeHandMaxKg(TEXT("Hand.MaxGrabKg"), 30.0f,
		TEXT("The heaviest loose physics piece the free hand picks up; heavier ones are only pushed."), ECVF_Default);
	TAutoConsoleVariable<float> GFreeHandKick(TEXT("Hand.Kick"), 1.0f,
		TEXT("How hard the free hand's touches wobble a body's soft parts (times)."), ECVF_Default);
	TAutoConsoleVariable<int32> GFreeHandShowShapes(TEXT("Hand.ShowShapes"), 0,
		TEXT("1 draws the body stand-ins the free hand touches (for tuning)."), ECVF_Default);

	// The marker turns the view when pushed past this far from the centre (of -1..1).
	constexpr float FreeHandEdge = 0.8f;
	// The view is about 80 degrees wide: a degree of mouse turn moves the marker this much.
	constexpr float FreeHandMarkerPerDegree = 1.0f / 40.0f;
	// The wrist sits this far back from the middle of the palm, along the fingers; the palm's face this far out.
	constexpr float FreeHandPalmLength = 7.5f;
	constexpr float FreeHandPalmDepth = 2.5f;

	struct FFreeHandShape
	{
		const TCHAR* From;
		const TCHAR* To;
		float Radius;
		const TCHAR* Ride;  // the bone a held spot rides on
	};
	// Stand-ins for the flesh (cm): a little fuller than the soft-contact cores, the surface the palm rests on.
	const FFreeHandShape FreeHandShapes[] = {
		{TEXT("upperarm_l"), TEXT("lowerarm_l"), 5.5f, TEXT("upperarm_l")},
		{TEXT("upperarm_r"), TEXT("lowerarm_r"), 5.5f, TEXT("upperarm_r")},
		{TEXT("lowerarm_l"), TEXT("hand_l"), 4.2f, TEXT("lowerarm_l")},
		{TEXT("lowerarm_r"), TEXT("hand_r"), 4.2f, TEXT("lowerarm_r")},
		{TEXT("hand_l"), TEXT("hand_l"), 4.5f, TEXT("hand_l")},
		{TEXT("hand_r"), TEXT("hand_r"), 4.5f, TEXT("hand_r")},
		{TEXT("thigh_l"), TEXT("calf_l"), 8.5f, TEXT("thigh_l")},
		{TEXT("thigh_r"), TEXT("calf_r"), 8.5f, TEXT("thigh_r")},
		{TEXT("calf_l"), TEXT("foot_l"), 6.0f, TEXT("calf_l")},
		{TEXT("calf_r"), TEXT("foot_r"), 6.0f, TEXT("calf_r")},
		{TEXT("neck_01"), TEXT("head"), 6.0f, TEXT("neck_01")},
	};

	/** Where a ray first enters a capsule (A to B, radius R): false if it misses. */
	bool FreeHandRayCapsule(const FVector& O, const FVector& D, float MaxT, const FVector& A, const FVector& B, float R, float& OutT, FVector& OutNormal)
	{
		FVector OnRay, OnCore;
		FMath::SegmentDistToSegmentSafe(O, O + D * MaxT, A, B, OnRay, OnCore);
		if (FVector::DistSquared(OnRay, OnCore) > FMath::Square(R))
		{
			return false;
		}
		// The distance to the core along the ray is convex: halve between the start and the nearest point.
		double Lo = 0.0;
		double Hi = FVector::DotProduct(OnRay - O, D);
		const auto Outside = [&](double T)
		{
			const FVector P = O + D * T;
			return FVector::DistSquared(P, FMath::ClosestPointOnSegment(P, A, B)) > FMath::Square(R);
		};
		if (!Outside(Lo))
		{
			return false;  // the ray starts inside (the camera in a body): no touch
		}
		for (int32 i = 0; i < 18; ++i)
		{
			const double Mid = (Lo + Hi) * 0.5;
			(Outside(Mid) ? Lo : Hi) = Mid;
		}
		OutT = static_cast<float>(Hi);
		const FVector P = O + D * Hi;
		OutNormal = (P - FMath::ClosestPointOnSegment(P, A, B)).GetSafeNormal();
		if (OutNormal.IsNearlyZero())
		{
			OutNormal = -D;
		}
		return true;
	}
}

// ---------------------------------------------------------------------------

bool UFreeHandSubsystem::IsActive(const UWorld* World)
{
	const UFreeHandSubsystem* Hand = World ? World->GetSubsystem<UFreeHandSubsystem>() : nullptr;
	return Hand && Hand->bOn;
}

float UFreeHandSubsystem::GetHolsterBlend(const UWorld* World, colony::Entity Pawn)
{
	const UFreeHandSubsystem* Hand = World ? World->GetSubsystem<UFreeHandSubsystem>() : nullptr;
	return Hand && Hand->Controlled == Pawn && Pawn != colony::kNoEntity ? Hand->HolsterBlend : 0.0f;
}

void UFreeHandSubsystem::Deinitialize()
{
	EndGrip();
	Super::Deinitialize();
}

bool UFreeHandSubsystem::Toggle()
{
	SetActive(!bOn);
	return bOn;
}

void UFreeHandSubsystem::SetActive(bool bWant)
{
	if (bWant == bOn)
	{
		return;
	}
	if (!bWant)
	{
		Exit();
		return;
	}
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Protagonist || !Protagonist->IsDirectMode())
	{
		return;
	}
	bOn = true;
	Controlled = Protagonist->GetControlled();
	State = EFreeHandState::Free;
	bHaveHand = false;
	bGripWas = false;
	Marker = FVector2D(0.15, -0.25);
	if (USoftContactSubsystem* Soft = GetWorld()->GetSubsystem<USoftContactSubsystem>())
	{
		Soft->SetForcedOn(true);
	}
	UE_LOG(LogTemp, Display, TEXT("Free hand on"));
}

void UFreeHandSubsystem::Exit()
{
	EndGrip();
	bOn = false;
	State = EFreeHandState::Off;
	if (USoftContactSubsystem* Soft = GetWorld()->GetSubsystem<USoftContactSubsystem>())
	{
		Soft->SetForcedOn(false);
		Soft->SetHandPresser(FVector::ZeroVector, FVector::ZeroVector, 0.0f, nullptr);
	}
	if (Pusher)
	{
		Pusher->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (ASimPawnActor* Actor = AppliedTo.Get())
	{
		Actor->SetFacingOverride(false);
	}
	UE_LOG(LogTemp, Display, TEXT("Free hand off"));
	// Blend stays above nought for a moment: Tick eases the arm back to its animation.
}

void UFreeHandSubsystem::SetScripted(const FVector2D& InMarker, bool bGrip, float Reach)
{
	bScripted = true;
	ScriptMarker = InMarker;
	bScriptGrip = bGrip;
	ScriptReach = Reach;
}

ASimPawnActor* UFreeHandSubsystem::GetPlayerActor() const
{
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	return Pawns && Controlled != colony::kNoEntity ? Pawns->FindPawn(Controlled) : nullptr;
}

USkeletalMeshComponent* UFreeHandSubsystem::GetDrawnMesh(const ASimPawnActor& Actor) const
{
	// The MakeHuman body when there is one; else the mannequin (a MetaHuman follows its bones through the leader pose).
	if (USkeletalMeshComponent* MakeHuman = Actor.GetMakeHumanMesh())
	{
		return MakeHuman;
	}
	return Actor.GetBody();
}

void UFreeHandSubsystem::EnsureComponents(AActor& Owner)
{
	if (Pusher && Pusher->GetOwner() != &Owner)
	{
		EndGrip();
		Pusher->DestroyComponent();
		Pusher = nullptr;
		if (Handle)
		{
			Handle->DestroyComponent();
			Handle = nullptr;
		}
	}
	if (!Pusher)
	{
		// A small kinematic ball at the palm: moved without teleporting, it shoves loose physics things it runs into.
		Pusher = NewObject<USphereComponent>(&Owner, TEXT("FreeHandPusher"));
		Pusher->SetSphereRadius(4.5f);
		Pusher->SetMobility(EComponentMobility::Movable);
		Pusher->SetCollisionObjectType(ECC_WorldDynamic);
		Pusher->SetCollisionResponseToAllChannels(ECR_Ignore);
		Pusher->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
		Pusher->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pusher->SetGenerateOverlapEvents(false);
		Pusher->SetCanEverAffectNavigation(false);
		Pusher->SetHiddenInGame(true);
		Pusher->RegisterComponent();
	}
	if (!Handle)
	{
		Handle = NewObject<UPhysicsHandleComponent>(&Owner, TEXT("FreeHandHandle"));
		Handle->SetLinearStiffness(900.0f);
		Handle->SetLinearDamping(120.0f);
		Handle->SetAngularStiffness(600.0f);
		Handle->SetAngularDamping(300.0f);
		Handle->SetInterpolationSpeed(30.0f);
		Handle->RegisterComponent();
	}
}

void UFreeHandSubsystem::ReadInput(float DeltaTime)
{
	if (bScripted)
	{
		Marker = ScriptMarker;
		bGripHeld = bScriptGrip;
		ReachFraction = FMath::Clamp(ScriptReach, 0.35f, 1.0f);
		return;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	AProtagonistPawn* Body = Protagonist ? Protagonist->GetBody() : nullptr;
	if (!PC || !Body)
	{
		bGripHeld = false;
		return;
	}
	bGripHeld = PC->IsInputKeyDown(EKeys::LeftMouseButton);
	if (PC->WasInputKeyJustPressed(EKeys::MouseScrollUp))
	{
		ReachFraction = FMath::Min(1.0f, ReachFraction + 0.08f);
	}
	if (PC->WasInputKeyJustPressed(EKeys::MouseScrollDown))
	{
		ReachFraction = FMath::Max(0.35f, ReachFraction - 0.08f);
	}
	// Right mouse button held: the view turns (AProtagonistPawn) and the marker stays where it is on screen.
	if (PC->IsInputKeyDown(EKeys::RightMouseButton))
	{
		return;
	}
	float MouseX = 0.0f, MouseY = 0.0f;
	PC->GetInputMouseDelta(MouseX, MouseY);
	const float Turn = FMath::Max(0.01f, AProtagonistPawn::GetMouseTurn()) * FMath::Max(0.05f, GFreeHandSpeed.GetValueOnGameThread());
	Marker.X += MouseX * Turn * FreeHandMarkerPerDegree;
	Marker.Y += MouseY * Turn * FreeHandMarkerPerDegree;
	// Past the edge the view turns instead, so the hand can reach all round.
	const FVector2D Over(Marker.X - FMath::Clamp(Marker.X, -FreeHandEdge, FreeHandEdge), Marker.Y - FMath::Clamp(Marker.Y, -FreeHandEdge, FreeHandEdge));
	if (!Over.IsNearlyZero())
	{
		Body->Look(static_cast<float>(Over.X / FreeHandMarkerPerDegree), static_cast<float>(Over.Y / FreeHandMarkerPerDegree));
		Marker.X = FMath::Clamp(Marker.X, -FreeHandEdge, FreeHandEdge);
		Marker.Y = FMath::Clamp(Marker.Y, -FreeHandEdge, FreeHandEdge);
	}
	(void)DeltaTime;
}

void UFreeHandSubsystem::TraceBodies(const FVector& O, const FVector& D, float MaxT, const ASimPawnActor& Me, FTouch& Best) const
{
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	if (!Pawns)
	{
		return;
	}
	const float Pad = GFreeHandFlesh.GetValueOnGameThread();
	[[maybe_unused]] const bool bShow = GFreeHandShowShapes.GetValueOnGameThread() != 0;
	for (const auto& Pair : Pawns->GetPawns())
	{
		ASimPawnActor* Actor = Pair.Value;
		if (!Actor || Actor == &Me || Actor->IsHidden() || FVector::DistSquared(Actor->GetActorLocation(), Me.GetActorLocation()) > FMath::Square(400.0))
		{
			continue;
		}
		USkeletalMeshComponent* Mesh = GetDrawnMesh(*Actor);
		if (!Mesh)
		{
			continue;
		}
		const auto Bone = [Mesh](const TCHAR* Name, FVector& Out)
		{
			if (Mesh->GetBoneIndex(Name) == INDEX_NONE)
			{
				return false;
			}
			Out = Mesh->GetSocketLocation(Name);
			return true;
		};
		const auto Try = [&](const FVector& A, const FVector& B, float R, const TCHAR* Ride)
		{
#if ENABLE_DRAW_DEBUG
			if (bShow)
			{
				// Hand.ShowShapes: the stand-in's core and its ends, for tuning against the skin.
				DrawDebugLine(GetWorld(), A, B, FColor::Cyan, false, -1.0f, SDPG_Foreground, 0.5f);
				DrawDebugSphere(GetWorld(), A, R + Pad, 10, FColor::Cyan, false, -1.0f, SDPG_World, 0.3f);
				DrawDebugSphere(GetWorld(), B, R + Pad, 10, FColor::Cyan, false, -1.0f, SDPG_World, 0.3f);
			}
#endif
			float T = 0.0f;
			FVector N;
			if (FreeHandRayCapsule(O, D, MaxT, A, B, R + Pad, T, N) && (!Best.bHit || T < Best.Distance))
			{
				Best = FTouch();
				Best.bHit = true;
				Best.Distance = T;
				Best.Point = O + D * T;
				Best.Normal = N;
				Best.BodyMesh = Mesh;
				Best.BodyActor = Actor;
				Best.Bone = Ride;
			}
		};
		for (const FFreeHandShape& Shape : FreeHandShapes)
		{
			FVector A, B;
			if (Bone(Shape.From, A) && Bone(Shape.To, B))
			{
				Try(A, B, Shape.Radius, Shape.Ride);
			}
		}
		// The head: a ball above the head bone (that bone is at the top of the neck), a little forward.
		const FVector Forward = Actor->GetActorForwardVector();
		FVector Neck, Head;
		if (Bone(TEXT("neck_01"), Neck) && Bone(TEXT("head"), Head))
		{
			const FVector Up = (Head - Neck).GetSafeNormal();
			const FVector Centre = Head + Up * 8.5f + Forward * 2.0f;
			Try(Centre - Up * 2.0f, Centre + Up * 2.0f, 9.0f, TEXT("head"));
		}
		// The trunk: from the pelvis to the top of the spine, a little forward of the bones (they lie towards the back).
		FVector Pelvis, Top;
		if (Bone(TEXT("pelvis"), Pelvis) && (Bone(TEXT("spine_05"), Top) || Bone(TEXT("spine_03"), Top)))
		{
			Try(Pelvis + Forward * 4.0f, Top + Forward * 3.0f, 13.5f, TEXT("spine_02"));
		}
	}
}

UFreeHandSubsystem::FTouch UFreeHandSubsystem::Trace(const FVector& O, const FVector& D, float MaxT, const ASimPawnActor& Me) const
{
	FTouch Best;
	// The world: walls, the ground where it has collision, loose physics things (ragdolls, debris).
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	Objects.AddObjectTypesToQuery(ECC_PhysicsBody);
	FCollisionQueryParams Params(TEXT("FreeHand"), false);
	Params.AddIgnoredActor(&Me);
	if (const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>(); Protagonist && Protagonist->GetBody())
	{
		Params.AddIgnoredActor(Protagonist->GetBody());
	}
	if (AActor* MetaHuman = Me.GetMetaHuman())
	{
		Params.AddIgnoredActor(MetaHuman);
	}
	if (UPrimitiveComponent* Carried = Held.Component.Get())
	{
		Params.AddIgnoredComponent(Carried);
	}
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByObjectType(Hit, O, O + D * MaxT, Objects, Params) && Hit.Component.IsValid())
	{
		Best.bHit = true;
		Best.Distance = static_cast<float>(Hit.Distance);
		Best.Point = Hit.ImpactPoint;
		Best.Normal = Hit.ImpactNormal;
		Best.Component = Hit.Component;
		Best.ComponentBone = Hit.BoneName;
	}
	TraceBodies(O, D, Best.bHit ? Best.Distance : MaxT, Me, Best);
	return Best;
}

void UFreeHandSubsystem::KickBody(const FTouch& At, const FVector& Velocity, float Radius)
{
	USkeletalMeshComponent* Mesh = At.BodyMesh.Get();
	UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Anim)
	{
		return;  // a MetaHuman body has no soft parts to wobble
	}
	Anim->AddHitKick(At.Point, Velocity * FMath::Max(0.0f, GFreeHandKick.GetValueOnGameThread()), Radius);
	++Stats.Kicks;
}

void UFreeHandSubsystem::StartGrip(const FTouch& At)
{
	EndGrip();
	if (!At.bHit)
	{
		State = EFreeHandState::GrippingAir;
		return;
	}
	if (USkeletalMeshComponent* Mesh = At.BodyMesh.Get())
	{
		// A body: the hand stays on that spot (riding on the bone) and presses in.
		Held = At;
		const FTransform Bone = Mesh->GetSocketTransform(At.Bone);
		HeldLocal = Bone.InverseTransformPosition(At.Point);
		HeldNormalLocal = Bone.InverseTransformVectorNoScale(At.Normal);
		State = EFreeHandState::HoldingBody;
		KickBody(At, -At.Normal * 90.0f, 16.0f);
		return;
	}
	UPrimitiveComponent* Component = At.Component.Get();
	if (Component && Handle && Component->IsSimulatingPhysics(At.ComponentBone))
	{
		const FBodyInstance* BodyInstance = Component->GetBodyInstance(At.ComponentBone);
		const float Mass = BodyInstance ? BodyInstance->GetBodyMass() : Component->GetMass();
		if (Mass <= GFreeHandMaxKg.GetValueOnGameThread())
		{
			Handle->GrabComponentAtLocation(Component, At.ComponentBone, At.Point);
			Held = At;
			State = EFreeHandState::HoldingThing;
			if (Pusher)
			{
				Pusher->SetCollisionEnabled(ECollisionEnabled::NoCollision);  // it would shove what it carries
			}
			return;
		}
	}
	State = EFreeHandState::GrippingAir;  // a wall or something too heavy: the hand just closes on it
}

void UFreeHandSubsystem::EndGrip()
{
	if (Handle && Handle->GetGrabbedComponent())
	{
		Handle->ReleaseComponent();  // it keeps the hand's speed: a toss
	}
	Held = FTouch();
	if (bOn)
	{
		State = EFreeHandState::Free;
	}
}

void UFreeHandSubsystem::ApplyToBody(ASimPawnActor& Actor, float Alpha, const FVector& Wrist, const FVector& Elbow, const FVector& Facing, float PalmWeight, float Curl)
{
	// The mannequin (and the MetaHuman on it through the leader pose, and a MakeHuman body retargeted from it) ...
	if (USkeletalMeshComponent* Body = Actor.GetBody())
	{
		if (USimPawnAnimInstance* Anim = Cast<USimPawnAnimInstance>(Body->GetAnimInstance()))
		{
			const FTransform& T = Body->GetComponentTransform();
			Anim->SetFreeHand(Alpha, T.InverseTransformPosition(Wrist), T.InverseTransformPosition(Elbow), T.InverseTransformVectorNoScale(Facing), PalmWeight, Curl);
		}
	}
	// ... and the MakeHuman body's own wrist reach puts its hand exactly there (its arms are not the mannequin's length).
	if (USkeletalMeshComponent* MakeHuman = Actor.GetMakeHumanMesh())
	{
		if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(MakeHuman->GetAnimInstance()))
		{
			const FTransform& T = MakeHuman->GetComponentTransform();
			Anim->SetRightHandReach(Alpha, T.InverseTransformPosition(Wrist), T.InverseTransformPosition(Elbow));
		}
	}
}

void UFreeHandSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const UProtagonistSubsystem* Protagonist = World->GetSubsystem<UProtagonistSubsystem>();
	const USimSubsystem* Bridge = World->GetSubsystem<USimSubsystem>();
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	const bool bDirect = Protagonist && Protagonist->IsDirectMode();

	// H: on and off (only in direct control).
	if (bDirect && PC && !bScripted && PC->WasInputKeyJustPressed(EKeys::H))
	{
		Toggle();
	}
	if (bOn)
	{
		const UOrdersSubsystem* Orders = World->GetSubsystem<UOrdersSubsystem>();
		const colony::ThingView* Me = Bridge && Bridge->GetSim() ? Bridge->GetCurrentSnapshot().find(Protagonist ? Protagonist->GetControlled() : colony::kNoEntity) : nullptr;
		// Off when control ends or moves to another colonist, when it goes down, or a building is picked to place.
		if (!bDirect || Protagonist->GetControlled() != Controlled || (Me && (Me->downed || Me->dead)) || (Orders && Orders->IsDirectBuilding()))
		{
			Exit();
		}
	}

	Blend = FMath::FInterpConstantTo(Blend, bOn ? 1.0f : 0.0f, DeltaTime, 5.0f);
	HolsterBlend = FMath::FInterpConstantTo(HolsterBlend, bOn ? 1.0f : 0.0f, DeltaTime, 3.0f);
	ASimPawnActor* Actor = GetPlayerActor();
	if (AppliedTo.IsValid() && AppliedTo.Get() != Actor)
	{
		ApplyToBody(*AppliedTo.Get(), 0.0f, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, 0.0f, 0.0f);
		AppliedTo->SetFacingOverride(false);
		AppliedTo = nullptr;
	}
	if (!Actor || Blend <= 0.0f)
	{
		if (Actor && AppliedTo.Get() == Actor)
		{
			ApplyToBody(*Actor, 0.0f, FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, 0.0f, 0.0f);
			AppliedTo = nullptr;
		}
		bHaveHand = false;
		return;
	}
	USkeletalMeshComponent* Drawn = GetDrawnMesh(*Actor);
	AProtagonistPawn* View = Protagonist ? Protagonist->GetBody() : nullptr;
	if (!Drawn || !View || Drawn->GetBoneIndex(TEXT("upperarm_r")) == INDEX_NONE || Drawn->GetBoneIndex(TEXT("lowerarm_r")) == INDEX_NONE ||
		Drawn->GetBoneIndex(TEXT("hand_r")) == INDEX_NONE)
	{
		return;
	}
	if (bOn)
	{
		++Stats.FramesOn;
		ReadInput(DeltaTime);
		EnsureComponents(*View);
	}

	// The arm: where the shoulder is and how long the arm is (from the drawn body: its own proportions).
	Shoulder = Drawn->GetSocketLocation(TEXT("upperarm_r"));
	const FVector ElbowNow = Drawn->GetSocketLocation(TEXT("lowerarm_r"));
	const FVector WristNow = Drawn->GetSocketLocation(TEXT("hand_r"));
	const float ArmLength = static_cast<float>(FVector::Dist(Shoulder, ElbowNow) + FVector::Dist(ElbowNow, WristNow));
	// What reaches: the wrist plus the palm.
	const float Reach = ArmLength + FreeHandPalmLength * 0.8f;

	// The ray through the marker.
	FVector Origin = View->GetViewLocation();
	FVector Direction = View->GetViewDirection();
	FVector2D Screen(0.0, 0.0);
	bool bOnScreen = false;
	if (PC)
	{
		int32 SizeX = 0, SizeY = 0;
		PC->GetViewportSize(SizeX, SizeY);
		if (SizeX > 0 && SizeY > 0)
		{
			Screen = FVector2D((Marker.X + 1.0) * 0.5 * SizeX, (1.0 - Marker.Y) * 0.5 * SizeY);
			FVector WorldOrigin, WorldDirection;
			if (PC->DeprojectScreenPositionToWorld(Screen.X, Screen.Y, WorldOrigin, WorldDirection))
			{
				Origin = WorldOrigin;
				Direction = WorldDirection.GetSafeNormal();
				bOnScreen = true;
			}
		}
	}
	const float MaxT = static_cast<float>(FVector::Dist(Origin, Shoulder)) + Reach + 30.0f;

	// The hand in the air: on the ball round the shoulder (radius: the wheel's share of the arm), where the ray leaves it.
	const float Radius = Reach * ReachFraction;
	const FVector ToShoulder = Shoulder - Origin;
	const double Along = FVector::DotProduct(ToShoulder, Direction);
	const double Disc = Along * Along - (ToShoulder.SizeSquared() - static_cast<double>(Radius) * Radius);
	FVector AirPalm;
	if (Disc >= 0.0)
	{
		AirPalm = Origin + Direction * (Along + FMath::Sqrt(Disc));
	}
	else
	{
		AirPalm = Shoulder + ((Origin + Direction * Along) - Shoulder).GetSafeNormal() * Radius;
	}

	// What is under the marker.
	FTouch Now = bOn ? Trace(Origin, Direction, MaxT, *Actor) : FTouch();
	const bool bReachable = Now.bHit && FVector::Dist(Now.Point, Shoulder) <= Reach * 0.98f;
	if (!bReachable)
	{
		Now.bHit = false;
	}

	// Grip: pressed starts holding what is touched; let go drops it.
	if (bOn && bGripHeld && !bGripWas)
	{
		StartGrip(Now);
	}
	else if (bOn && !bGripHeld && bGripWas)
	{
		EndGrip();
	}
	bGripWas = bOn && bGripHeld;

	FVector PalmGoal = AirPalm;
	FVector FacingGoal = (AirPalm - Shoulder).GetSafeNormal();  // palm forward-ish in the air: loosely, the way it reaches
	float PalmWeight = 0.25f;
	float CurlGoal = bGripHeld ? 0.85f : 0.15f;
	const float Press = FMath::Clamp(GFreeHandPress.GetValueOnGameThread(), 0.0f, 4.0f);
	switch (State)
	{
	case EFreeHandState::HoldingBody:
	{
		USkeletalMeshComponent* Mesh = Held.BodyMesh.Get();
		if (!Mesh)
		{
			EndGrip();
			break;
		}
		const FTransform Bone = Mesh->GetSocketTransform(Held.Bone);
		const FVector Spot = Bone.TransformPosition(HeldLocal);
		const FVector Normal = Bone.TransformVectorNoScale(HeldNormalLocal).GetSafeNormal();
		if (FVector::Dist(Spot, Shoulder) > Reach * 1.1f)
		{
			EndGrip();  // they moved out of reach
			break;
		}
		Held.Point = Spot;
		Held.Normal = Normal;
		PalmGoal = Spot - Normal * Press;
		FacingGoal = -Normal;
		PalmWeight = 0.85f;
		CurlGoal = 0.45f;  // a hold, not a fist: the fingers rest on the skin
		++Stats.HoldBodyFrames;
		break;
	}
	case EFreeHandState::HoldingThing:
	{
		// The palm goes where the marker says (in the air or on a surface), the carried thing follows the handle.
		PalmGoal = Now.bHit ? Now.Point + Now.Normal * 6.0f : AirPalm;
		FacingGoal = FVector::DownVector;
		PalmWeight = 0.5f;
		CurlGoal = 0.8f;
		if (!Handle || !Handle->GetGrabbedComponent())
		{
			EndGrip();
		}
		++Stats.HoldThingFrames;
		break;
	}
	default:
		if (Now.bHit)
		{
			// Resting on what is there: the palm on the surface, facing it.
			PalmGoal = Now.Point + Now.Normal * 0.5f;
			FacingGoal = -Now.Normal;
			PalmWeight = 0.8f;
			CurlGoal = bGripHeld ? 0.6f : 0.1f;
			if (bOn && !bGripHeld)
			{
				State = EFreeHandState::Touching;
			}
		}
		else if (bOn && !bGripHeld)
		{
			State = EFreeHandState::Free;
		}
		break;
	}
	// Never below the ground at the feet.
	PalmGoal.Z = FMath::Max(PalmGoal.Z, Actor->GetActorLocation().Z + 4.0);

	// Smooth the palm (quick, so it feels like the mouse, but no jumps when the ray slips off an edge).
	const FVector WasPalm = Palm;
	if (!bHaveHand)
	{
		Palm = WristNow;
		PalmFacing = FacingGoal;
		bHaveHand = true;
	}
	Palm = FMath::Lerp(Palm, PalmGoal, 1.0 - FMath::Exp(-18.0 * DeltaTime));
	PalmFacing = FMath::Lerp(PalmFacing, FacingGoal, 1.0 - FMath::Exp(-12.0 * DeltaTime)).GetSafeNormal();
	PalmVelocity = DeltaTime > 0.0f ? (Palm - WasPalm) / DeltaTime : FVector::ZeroVector;
	CurlNow = FMath::FInterpTo(CurlNow, CurlGoal, DeltaTime, 10.0f);

	// The wrist: back from the palm along the fingers, out from the palm's face.
	FVector FingersDir = (Palm - Shoulder).GetSafeNormal();
	if (!PalmFacing.IsNearlyZero() && PalmWeight > 0.5f)
	{
		const FVector Flat = FingersDir - PalmFacing * FVector::DotProduct(FingersDir, PalmFacing);
		FingersDir = Flat.IsNearlyZero() ? FingersDir : Flat.GetSafeNormal();
	}
	WristTarget = Palm - FingersDir * FreeHandPalmLength - (PalmWeight > 0.5f ? PalmFacing * FreeHandPalmDepth : FVector::ZeroVector);
	// The elbow out to the right, down and back, as a reaching arm holds it.
	const FVector Right = Actor->GetActorRightVector();
	const FVector ElbowHint = (Shoulder + WristTarget) * 0.5 + Right * 30.0 - FVector::UpVector * 35.0 - Actor->GetActorForwardVector() * 10.0;
	const float Alpha = FMath::SmoothStep(0.0f, 1.0f, Blend);
	ApplyToBody(*Actor, Alpha, WristTarget, ElbowHint, PalmFacing, PalmWeight, CurlNow);
	AppliedTo = Actor;
	// Standing, the body turns to where the player looks, so the right arm can reach what is ahead.
	Actor->SetFacingOverride(bOn, View->GetYaw());

	// How close the drawn wrist got to the goal (a frame late: the animation of this frame runs after this tick).
	Stats.WristError = static_cast<float>(FVector::Dist(WristNow, WristTarget));
	if (bOn && (State == EFreeHandState::Touching || State == EFreeHandState::HoldingBody) && Blend >= 1.0f)
	{
		Stats.WorstWristError = FMath::Max(Stats.WorstWristError, Stats.WristError);
	}

	if (!bOn)
	{
		return;
	}

	// The palm presses on skin (the squish), and touches wobble the soft parts.
	const FTouch& Contact = State == EFreeHandState::HoldingBody ? Held : Now;
	const bool bTouchingBody = Contact.bHit && Contact.BodyMesh.IsValid() && FVector::Dist(Palm, Contact.Point) < 6.0f;
	if (USoftContactSubsystem* Soft = World->GetSubsystem<USoftContactSubsystem>())
	{
		const FVector PalmCentre = Palm + PalmFacing * 1.5f;
		Soft->SetHandPresser(PalmCentre - FingersDir * 3.0f, PalmCentre + FingersDir * 3.5f, 3.5f, Drawn);
	}
	KickWait = FMath::Max(0.0f, KickWait - DeltaTime);
	if (bTouchingBody)
	{
		++Stats.TouchFrames;
		if (!bWasTouching)
		{
			// The touch lands: as hard as the hand came in, never a slap.
			const float In = FMath::Clamp(static_cast<float>(FVector::DotProduct(PalmVelocity, -Contact.Normal)), 20.0f, 220.0f);
			KickBody(Contact, -Contact.Normal * In * 0.6f, 18.0f);
			KickWait = 0.12f;
		}
		else if (KickWait <= 0.0f && PalmVelocity.SizeSquared() > 400.0)
		{
			// Moving over the skin drags the flesh a little along.
			KickBody(Contact, PalmVelocity.GetClampedToMaxSize(100.0) * 0.35f, 15.0f);
			KickWait = 0.12f;
		}
	}
	bWasTouching = bTouchingBody;

	// The pusher ball follows the palm without teleporting (so it shoves); the handle drags what is carried.
	if (Pusher)
	{
		Pusher->SetCollisionEnabled(State == EFreeHandState::HoldingThing ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
		Pusher->SetWorldLocation(Palm, false, nullptr, ETeleportType::None);
	}
	if (State == EFreeHandState::HoldingThing && Handle && Handle->GetGrabbedComponent())
	{
		Handle->SetTargetLocation(Palm + PalmFacing * 4.0f);
	}

	DrawHud(Screen, bOnScreen);
}

void UFreeHandSubsystem::DrawHud(const FVector2D& ScreenPoint, bool bOnScreen)
{
	if (!bOnScreen)
	{
		return;
	}
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImDrawList* Draw = ImGui::GetForegroundDrawList();
	// The marker: a ring, white in the air, green on something, gold while holding.
	ImU32 Colour = IM_COL32(255, 255, 255, 200);
	switch (State)
	{
	case EFreeHandState::Touching:
		Colour = IM_COL32(120, 235, 130, 230);
		break;
	case EFreeHandState::HoldingBody:
	case EFreeHandState::HoldingThing:
		Colour = IM_COL32(255, 200, 70, 240);
		break;
	case EFreeHandState::GrippingAir:
		Colour = IM_COL32(200, 200, 200, 200);
		break;
	default:
		break;
	}
	// The viewport size from the player controller and ImGui's can differ by the window's DPI: scale between them.
	int32 SizeX = 0, SizeY = 0;
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		PC->GetViewportSize(SizeX, SizeY);
	}
	const float Sx = SizeX > 0 ? Viewport->Size.x / static_cast<float>(SizeX) : 1.0f;
	const float Sy = SizeY > 0 ? Viewport->Size.y / static_cast<float>(SizeY) : 1.0f;
	const ImVec2 At(Viewport->Pos.x + static_cast<float>(ScreenPoint.X) * Sx, Viewport->Pos.y + static_cast<float>(ScreenPoint.Y) * Sy);
	Draw->AddCircle(At, 9.0f * Scale, IM_COL32(0, 0, 0, 120), 24, 3.5f * Scale);
	Draw->AddCircle(At, 9.0f * Scale, Colour, 24, 2.0f * Scale);
	Draw->AddCircleFilled(At, 2.0f * Scale, Colour);
	// The keys, small, at the bottom.
	static const char* const Help = "Free hand   move: mouse   hold: left click   reach: wheel   look: hold right mouse   H: hands back";
	const ImVec2 Size = ImGui::CalcTextSize(Help);
	const ImVec2 TextAt(Viewport->Pos.x + (Viewport->Size.x - Size.x) * 0.5f, Viewport->Pos.y + Viewport->Size.y - Size.y - 64.0f * Scale);
	Draw->AddRectFilled(ImVec2(TextAt.x - 10.0f, TextAt.y - 5.0f), ImVec2(TextAt.x + Size.x + 10.0f, TextAt.y + Size.y + 5.0f), IM_COL32(0, 0, 0, 110), 4.0f);
	Draw->AddText(TextAt, IM_COL32(235, 235, 235, 230), Help);
}
