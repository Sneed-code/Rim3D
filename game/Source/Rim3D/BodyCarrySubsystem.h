#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "BodyCarrySubsystem.generated.h"

class AActor;
class ASimPawnActor;
class UPhysicsConstraintComponent;
class USimSubsystem;
class USphereComponent;

/**
 * Carrying a body (docs/design/weapon-hold.md): a downed pawn or a corpse the sim says is carried (`carriedBy`) rides
 * on the carrier's back. Its ragdoll is moved there once and held by the chest to a point behind the carrier's
 * shoulders (a joint that keeps it roughly upright, chest to back); the arms, head and legs hang loose. The carrier
 * stoops (the hold layer's spine bend, WeaponDrawSubsystem) and reaches back under the legs with both hands. Laid down,
 * the joint lets go and the body falls where it is. Cosmetic only: the sim already moves the carried pawn with its
 * carrier.
 */
UCLASS()
class RIM3D_API UBodyCarrySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Bodies held on a back now, and how many were picked up so far (for the smoke tests). */
	int32 GetCarriedNow() const { return Carries.Num(); }
	int32 GetPickUps() const { return PickUps; }

private:
	struct FCarry
	{
		TWeakObjectPtr<ASimPawnActor> Carrier;
		TWeakObjectPtr<ASimPawnActor> Carried;
		TWeakObjectPtr<USphereComponent> Anchor;
		TWeakObjectPtr<UPhysicsConstraintComponent> Joint;
		bool bHands = false;
	};

	/** Where the carried chest goes: behind the carrier's upper back, turned as it is. False without the bones. */
	bool BackFrame(const ASimPawnActor& Carrier, FTransform& Out) const;
	bool Begin(FCarry& Carry);
	void Hold(FCarry& Carry);
	void End(FCarry& Carry);

	TWeakObjectPtr<USimSubsystem> BridgeSource;
	UPROPERTY()
	TObjectPtr<AActor> Holder;
	/** By the carried pawn's entity. */
	TMap<uint32, FCarry> Carries;
	int32 PickUps = 0;
};
