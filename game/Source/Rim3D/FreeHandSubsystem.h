#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"

#include "FreeHandSubsystem.generated.h"

class AActor;
class ASimPawnActor;
class UPhysicsHandleComponent;
class UPrimitiveComponent;
class USkeletalMeshComponent;
class USphereComponent;

/** What the free hand is doing now. */
enum class EFreeHandState : uint8
{
	Off,
	/** In the air: nothing within reach under the hand's marker. */
	Free,
	/** Resting on something: a body's skin, a wall, a loose thing. */
	Touching,
	/** Holding on to a body (the hand stays on that spot and presses in). */
	HoldingBody,
	/** Carrying a loose physics thing (a ragdoll's limb, debris). */
	HoldingThing,
	/** Gripping with nothing in the hand (a loose fist). */
	GrippingAir,
};

/** Numbers for the tests (smoke test gunrange with -FreeHandScript). */
struct FFreeHandStats
{
	int32 FramesOn = 0;
	int32 TouchFrames = 0;
	int32 HoldBodyFrames = 0;
	int32 HoldThingFrames = 0;
	int32 Kicks = 0;
	/** How far the drawn wrist was from where it was asked to go, cm, the largest while touching or holding. */
	float WorstWristError = 0.0f;
	/** The last wrist error, cm. */
	float WristError = 0.0f;
};

/**
 * Free hand mode (docs/design/free-hand.md): in direct control, J frees the right hand. The mouse moves a marker and the
 * hand reaches for whatever is under it within arm's length (a two-bone reach on the arm, the palm turned to the
 * surface); holding the left mouse button grips. Nothing is made for one thing: the hand only follows the marker and the
 * world reacts with the physics already there:
 * - a body's skin gives where the palm presses (the soft-contact squish, USoftContactSubsystem) and its soft parts wobble
 *   from the touch (UMakeHumanAnimInstance kicks); gripping a body keeps the hand on that spot and presses in a little;
 * - loose physics things (ragdolls, debris) are pushed by the hand and, gripped, carried (a physics handle) and dropped or
 *   tossed when let go; heavy ones only get pushed;
 * - walls and the ground stop the hand.
 * Wheel: how far the hand reaches into the air. Right mouse button held: look around (the marker stays put). The marker
 * pushed past the screen's edge turns the view. The weapon in hand goes to its holster meanwhile, and the mouse buttons
 * don't shoot, swing or block. Pure looks: nothing here reaches the sim.
 */
UCLASS()
class RIM3D_API UFreeHandSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UFreeHandSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual void Deinitialize() override;

	/** Whether free hand mode is on in this world (other input stays off the mouse buttons meanwhile). */
	static bool IsActive(const UWorld* World);
	/** 0 to 1: how far the weapon of this pawn should be put away for the free hand (the controlled pawn only). */
	static float GetHolsterBlend(const UWorld* World, colony::Entity Pawn);

	/** J: on or off. Only in direct control with the colonist up. Returns whether it is on now. */
	bool Toggle();
	void SetActive(bool bOn);
	bool IsOn() const { return bOn; }
	EFreeHandState GetState() const { return State; }
	const FFreeHandStats& GetStats() const { return Stats; }

	/** Test input: the marker at (X, Y), -1 to 1 across the view from the centre (Y up), the grip held or not, and the
	 *  reach into the air (0.35 to 1 of the arm) instead of the mouse, until ClearScripted. */
	void SetScripted(const FVector2D& Marker, bool bGrip, float Reach = 0.8f);
	void ClearScripted() { bScripted = false; }
	/** Where the wrist is asked to go now (world cm), and the shoulder. */
	FVector GetWristTarget() const { return WristTarget; }
	FVector GetShoulder() const { return Shoulder; }

private:
	struct FTouch
	{
		bool bHit = false;
		FVector Point = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
		/** A body's drawn mesh and the bone the spot rides on (body touches). */
		TWeakObjectPtr<USkeletalMeshComponent> BodyMesh;
		TWeakObjectPtr<ASimPawnActor> BodyActor;
		FName Bone;
		/** A world thing (wall, ground, loose physics thing). */
		TWeakObjectPtr<UPrimitiveComponent> Component;
		FName ComponentBone;
		float Distance = 0.0f;  // along the ray, cm
	};

	void Exit();
	ASimPawnActor* GetPlayerActor() const;
	USkeletalMeshComponent* GetDrawnMesh(const ASimPawnActor& Actor) const;
	void EnsureComponents(AActor& Owner);
	void ReadInput(float DeltaTime);
	/** The nearest body (capsule stand-ins for its limbs, trunk and head) or world surface along the ray. */
	FTouch Trace(const FVector& Origin, const FVector& Direction, float MaxDistance, const ASimPawnActor& Me) const;
	void TraceBodies(const FVector& Origin, const FVector& Direction, float MaxDistance, const ASimPawnActor& Me, FTouch& Best) const;
	void ApplyToBody(ASimPawnActor& Actor, float Alpha, const FVector& Wrist, const FVector& Elbow, const FVector& PalmFacing, float PalmWeight, float Curl);
	void KickBody(const FTouch& At, const FVector& Velocity, float Radius);
	void DrawHud(const FVector2D& ScreenPoint, bool bOnScreen);
	void StartGrip(const FTouch& At);
	void EndGrip();

	bool bOn = false;
	EFreeHandState State = EFreeHandState::Off;
	FFreeHandStats Stats;
	colony::Entity Controlled = colony::kNoEntity;
	TWeakObjectPtr<ASimPawnActor> AppliedTo;

	// Input.
	FVector2D Marker = FVector2D(0.15, -0.25);  // -1..1 across the view, from the centre, Y up
	float ReachFraction = 0.8f;
	bool bGripHeld = false;
	bool bGripWas = false;
	bool bScripted = false;
	FVector2D ScriptMarker = FVector2D::ZeroVector;
	bool bScriptGrip = false;
	float ScriptReach = 0.8f;

	// The hand.
	float Blend = 0.0f;  // 0 off .. 1 fully the free hand's (eased in and out)
	float HolsterBlend = 0.0f;
	float CurlNow = 0.15f;
	bool bHaveHand = false;
	FVector Palm = FVector::ZeroVector;          // where the palm is going, world, smoothed
	FVector PalmVelocity = FVector::ZeroVector;  // cm/s
	FVector PalmFacing = FVector::ZeroVector;    // the way the palm faces (smoothed), world
	FVector WristTarget = FVector::ZeroVector;
	FVector Shoulder = FVector::ZeroVector;
	bool bWasTouching = false;
	float KickWait = 0.0f;

	// Holding.
	FTouch Held;
	FVector HeldLocal = FVector::ZeroVector;        // the spot in the held bone's space (bodies)
	FVector HeldNormalLocal = FVector::UpVector;

	UPROPERTY(Transient)
	TObjectPtr<USphereComponent> Pusher;
	UPROPERTY(Transient)
	TObjectPtr<UPhysicsHandleComponent> Handle;
};
