#include "BodyCarrySubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

#include "HoldDraw.h"
#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Snapshot.h"

static TAutoConsoleVariable<int32> GBodyCarry(TEXT("Pawns.CarryOnBack"), 1,
	TEXT("1: a carried downed pawn or corpse rides on its carrier's back (its ragdoll held there), 0: left where it lies."));

namespace
{
	const FName BodyCarryChest(TEXT("spine_03"));
	/** How far behind the carrier's chest bone the carried chest goes, and how far up (cm, for a 1.0 body). */
	constexpr float BodyCarryBack = 24.0f;
	constexpr float BodyCarryUp = 2.0f;
	/** Further than this (cm) from where it should be, the carried body is moved there at once (a teleport). */
	constexpr float BodyCarrySnap = 150.0f;

	/** Moves every body of a ragdoll so the chest lands on To, the rest kept as it lies relative to it, all still. */
	void BodyCarryTeleport(USkeletalMeshComponent& Mesh, const FTransform& To)
	{
		const FBodyInstance* Chest = Mesh.GetBodyInstance(BodyCarryChest);
		if (!Chest || !Chest->IsValidBodyInstance())
		{
			return;
		}
		FTransform From = Chest->GetUnrealWorldTransform();
		From.SetScale3D(FVector::OneVector);
		FTransform Target = To;
		Target.SetScale3D(FVector::OneVector);
		const FTransform Delta = From.Inverse() * Target;
		for (FBodyInstance* Body : Mesh.Bodies)
		{
			if (!Body || !Body->IsValidBodyInstance())
			{
				continue;
			}
			const FTransform Old = Body->GetUnrealWorldTransform();
			FTransform New = FTransform(Old.GetRotation(), Old.GetLocation()) * Delta;
			New.SetScale3D(Old.GetScale3D());
			Body->SetBodyTransform(New, ETeleportType::TeleportPhysics);
			Body->SetLinearVelocity(FVector::ZeroVector, false);
			Body->SetAngularVelocityInRadians(FVector::ZeroVector, false);
		}
	}
}

TStatId UBodyCarrySubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBodyCarrySubsystem, STATGROUP_Tickables);
}

void UBodyCarrySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	BridgeSource = Collection.InitializeDependency<USimSubsystem>();
	Collection.InitializeDependency<USimPawnSubsystem>();
}

bool UBodyCarrySubsystem::BackFrame(const ASimPawnActor& Carrier, FTransform& Out) const
{
	// The carrier's own mannequin (it drives the MakeHuman body, and the carried ragdoll is a mannequin too, so the chest
	// bones turn alike): the carried chest goes behind it, turned as it is, stooping with it.
	const USkeletalMeshComponent* Mesh = Carrier.GetBody();
	if (!Mesh || Mesh->GetBoneIndex(BodyCarryChest) == INDEX_NONE)
	{
		return false;
	}
	const FTransform Chest = Mesh->GetSocketTransform(BodyCarryChest);
	const float Scale = Carrier.GetBodyScale();
	const FVector Forward = Carrier.GetActorForwardVector();
	Out = FTransform(Chest.GetRotation(), Chest.GetLocation() - Forward * (BodyCarryBack * Scale) + FVector::UpVector * (BodyCarryUp * Scale));
	return true;
}

bool UBodyCarrySubsystem::Begin(FCarry& Carry)
{
	ASimPawnActor* Carried = Carry.Carried.Get();
	ASimPawnActor* Carrier = Carry.Carrier.Get();
	USkeletalMeshComponent* Ragdoll = Carried ? Carried->GetBody() : nullptr;
	FTransform Back;
	if (!Carrier || !Ragdoll || !Carried->IsRagdoll() || !Ragdoll->GetBodyInstance(BodyCarryChest) || !BackFrame(*Carrier, Back))
	{
		return false;
	}
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("BodyCarryRoot"));
		Holder->SetRootComponent(Root);
		Root->RegisterComponent();
	}
	// The ragdoll onto the back first, so the joint holds it as it is there (chest to back, turned with the carrier).
	Carried->ResetRagdollFloor(false);
	BodyCarryTeleport(*Ragdoll, Back);
	Ragdoll->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);

	// A point that follows the carrier (moved, not simulated, touching nothing) and a joint from it to the chest.
	USphereComponent* Anchor = NewObject<USphereComponent>(Holder);
	Anchor->SetSphereRadius(4.0f);
	Anchor->SetMobility(EComponentMobility::Movable);
	Anchor->SetCollisionEnabled(ECollisionEnabled::PhysicsOnly);
	Anchor->SetCollisionResponseToAllChannels(ECR_Ignore);
	Anchor->SetHiddenInGame(true);
	Anchor->RegisterComponent();
	Anchor->SetWorldTransform(Back);
	Anchor->SetSimulatePhysics(false);

	UPhysicsConstraintComponent* Joint = NewObject<UPhysicsConstraintComponent>(Holder);
	Joint->SetMobility(EComponentMobility::Movable);
	Joint->RegisterComponent();
	Joint->SetWorldTransform(Back);
	Joint->SetDisableCollision(true);
	Joint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
	Joint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
	Joint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
	// The chest may sway a little; the arms, head and legs hang free from it by the ragdoll's own joints.
	Joint->SetAngularSwing1Limit(EAngularConstraintMotion::ACM_Limited, 20.0f);
	Joint->SetAngularSwing2Limit(EAngularConstraintMotion::ACM_Limited, 20.0f);
	Joint->SetAngularTwistLimit(EAngularConstraintMotion::ACM_Limited, 15.0f);
	Joint->SetConstrainedComponents(Anchor, NAME_None, Ragdoll, BodyCarryChest);

	Carry.Anchor = Anchor;
	Carry.Joint = Joint;
	++PickUps;
	return true;
}

void UBodyCarrySubsystem::Hold(FCarry& Carry)
{
	ASimPawnActor* Carrier = Carry.Carrier.Get();
	ASimPawnActor* Carried = Carry.Carried.Get();
	USphereComponent* Anchor = Carry.Anchor.Get();
	USkeletalMeshComponent* Ragdoll = Carried ? Carried->GetBody() : nullptr;
	FTransform Back;
	if (!Carrier || !Anchor || !Ragdoll || !BackFrame(*Carrier, Back))
	{
		return;
	}
	// A carrier that jumped (a teleport, a load) takes the body with it at once; otherwise the joint pulls it along.
	if (const FBodyInstance* Chest = Ragdoll->GetBodyInstance(BodyCarryChest); Chest && FVector::Dist(Chest->GetUnrealWorldTransform().GetLocation(), Back.GetLocation()) > BodyCarrySnap)
	{
		Anchor->SetWorldTransform(Back, false, nullptr, ETeleportType::TeleportPhysics);
		BodyCarryTeleport(*Ragdoll, Back);
	}
	else
	{
		Anchor->SetWorldTransform(Back);
	}

	// The carrier's hands back under the carried thighs (the MakeHuman body's arm IK; a mannequin-only body has none).
	USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(*Carrier);
	UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (Anim && Ragdoll->GetBoneIndex(TEXT("thigh_l")) != INDEX_NONE && Ragdoll->GetBoneIndex(TEXT("thigh_r")) != INDEX_NONE)
	{
		const FTransform MeshWorld = Mesh->GetComponentTransform();
		const FVector Right = Carrier->GetActorRightVector();
		const FVector Forward = Carrier->GetActorForwardVector();
		// A third of the way down the thigh from the hip, under it.
		const auto Under = [Ragdoll](const TCHAR* Thigh, const TCHAR* Calf)
		{
			const FVector Hip = Ragdoll->GetSocketLocation(Thigh);
			const FVector Knee = Ragdoll->GetBoneIndex(Calf) != INDEX_NONE ? Ragdoll->GetSocketLocation(Calf) : Hip;
			return FMath::Lerp(Hip, Knee, 0.35) - FVector::UpVector * 6.0;
		};
		const FVector RightHand = Under(TEXT("thigh_r"), TEXT("calf_r"));
		const FVector LeftHand = Under(TEXT("thigh_l"), TEXT("calf_l"));
		Anim->SetRightHandReach(1.0f, MeshWorld.InverseTransformPosition(RightHand), MeshWorld.InverseTransformPosition(RightHand + Right * 30.0 + Forward * 10.0));
		Anim->SetLeftHandReach(1.0f, MeshWorld.InverseTransformPosition(LeftHand), MeshWorld.InverseTransformPosition(LeftHand - Right * 30.0 + Forward * 10.0));
		Carry.bHands = true;
	}
}

void UBodyCarrySubsystem::End(FCarry& Carry)
{
	if (UPhysicsConstraintComponent* Joint = Carry.Joint.Get())
	{
		Joint->BreakConstraint();
		Joint->DestroyComponent();
	}
	if (USphereComponent* Anchor = Carry.Anchor.Get())
	{
		Anchor->DestroyComponent();
	}
	if (ASimPawnActor* Carried = Carry.Carried.Get())
	{
		if (USkeletalMeshComponent* Ragdoll = Carried->GetBody(); Ragdoll && Carried->IsRagdoll())
		{
			Ragdoll->SetCollisionProfileName(TEXT("Ragdoll"));
			// It falls where it was laid: the floor under it there now.
			Carried->ResetRagdollFloor(true);
			Ragdoll->WakeAllRigidBodies();
		}
	}
	if (ASimPawnActor* Carrier = Carry.Carrier.Get(); Carrier && Carry.bHands)
	{
		USkeletalMeshComponent* Mesh = HoldDraw::VisibleMesh(*Carrier);
		if (UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr)
		{
			Anim->SetRightHandReach(0.0f, FVector::ZeroVector, FVector::ZeroVector);
			Anim->SetLeftHandGrip(0.0f, FVector::ZeroVector, FVector::ZeroVector);
		}
	}
	Carry = FCarry{};
}

void UBodyCarrySubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	TSet<uint32> Seen;
	if (Bridge && Pawns && Bridge->GetSim() && GBodyCarry.GetValueOnGameThread() != 0)
	{
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			if (View.carriedBy == colony::kNoEntity)
			{
				continue;
			}
			ASimPawnActor* Carried = Pawns->FindPawn(View.id);
			ASimPawnActor* Carrier = Pawns->FindPawn(View.carriedBy);
			if (!Carried || !Carrier || !Carried->IsRagdoll())
			{
				continue;  // not limp yet (the ragdoll starts with the fight effects); tried again next frame
			}
			const uint32 Key = entt::to_integral(View.id);
			FCarry* Carry = Carries.Find(Key);
			if (Carry && Carry->Carrier.Get() != Carrier)
			{
				End(*Carry);  // handed to someone else
				Carries.Remove(Key);
				Carry = nullptr;
			}
			if (!Carry)
			{
				FCarry Made;
				Made.Carrier = Carrier;
				Made.Carried = Carried;
				if (!Begin(Made))
				{
					continue;
				}
				Carry = &Carries.Add(Key, Made);
			}
			Seen.Add(Key);
			Hold(*Carry);
		}
	}
	for (auto It = Carries.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			End(It.Value());
			It.RemoveCurrent();
		}
	}
}
