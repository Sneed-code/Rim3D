#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"

#include "PawnAnimInstance.generated.h"

class UAnimSequence;
class UPoseSearchDatabase;

/** How much animation a pawn gets, picked by its distance to the camera (USimPawnSubsystem). */
UENUM()
enum class ESimPawnDetail : uint8
{
	/** Motion matching on the sample's databases, every frame. */
	Full,
	/** An idle, walk or run loop picked by speed, played faster or slower to match it. */
	Simple,
};

/** The body zones the picture knows (docs/design/body-health.md part E): where a hit lands, what limps, what is patched. */
enum class ESimPawnZone : uint8
{
	Head,
	Torso,
	ArmLeft,
	ArmRight,
	LegLeft,
	LegRight,
	Count,
};

/** A blow or a bullet landing on a zone: the struck part is knocked along `Direction` and springs back. */
struct FSimPawnKick
{
	ESimPawnZone Zone = ESimPawnZone::Torso;
	/** The way the blow travels, in the mesh's own space (the mannequin faces +Y there), unit length. */
	FVector Direction = FVector::YAxisVector;
	/** 0 to 1. */
	float Strength = 0.5f;
};

/** What a pawn actor tells its animation before each update (game thread). World units (cm). */
struct FSimPawnAnimInputs
{
	FVector Location = FVector::ZeroVector;
	/** The actor's yaw, degrees. The mesh faces the same way. */
	float Yaw = 0.0f;
	/** Ground velocity in cm per second at 1x (the animation runs faster at 2x and 3x). */
	FVector Velocity = FVector::ZeroVector;
	/** How fast the pawn walks on flat open ground, cm per second at 1x. */
	float CruiseSpeed = 400.0f;
	/** The sim's route still ahead, nearest point first. */
	TArray<FVector, TInlineAllocator<8>> Route;
	ESimPawnDetail Detail = ESimPawnDetail::Full;
	/** Set for one update when the pawn jumped (spawned, loaded): forgets the trajectory. */
	bool bTeleported = true;
	// --- The hit layer (docs/design/body-health.md part E), on top of whatever animation plays. ---
	/** Hits since the animation last took them: it empties the list when it has. */
	TArray<FSimPawnKick, TInlineAllocator<4>> Kicks;
	/** How badly each leg is hurt, 0 (whole) to 1: the body lurches over a hurt leg while that foot is down. */
	float LimpLeft = 0.0f;
	float LimpRight = 0.0f;
	/** 0 to 1: bent over a wound it is patching, and the zone. */
	float Patch = 0.0f;
	ESimPawnZone PatchZone = ESimPawnZone::Torso;
	/** Off for a ragdoll and for far pawns: the layer does nothing and forgets its springs. */
	bool bHitLayer = true;
};

/** What the animation did, copied back after each update. Read by the smoke test. */
struct FSimPawnAnimStats
{
	ESimPawnDetail Detail = ESimPawnDetail::Simple;
	/** The animation the blend stack plays now, and the database motion matching took it from. */
	FName PlayingAsset;
	FName Database;
	float PlayRate = 1.0f;
	/** Motion matching searches run and how many of them started a new animation. */
	int32 Searches = 0;
	int32 Jumps = 0;
	// The hit layer: kicks taken so far, how far the most bent zone is off its animation now (degrees), the largest it
	// has been, and how far the body leans over a hurt leg now (0 to 1).
	int32 Kicks = 0;
	float WobbleDegrees = 0.0f;
	float MaxWobbleDegrees = 0.0f;
	float LimpNow = 0.0f;
	float MaxLimp = 0.0f;
	/** Standing still: the breathing idle plays and motion matching rests (it shuffled the feet). */
	bool bStandingIdle = false;
};

/**
 * The animation of one sim pawn, all in C++ (no Animation Blueprint): a hit layer (struck parts wobble and spring
 * back, a limp, the patching bend; body-health.md part E) over a montage slot over a
 * locomotion node that either motion matches (ESimPawnDetail::Full) or plays loops by speed
 * (Simple), through Epic's blend stack. The trajectory for motion matching is the pawn's real
 * history plus a prediction along the sim's route. See docs/design/pawns.md.
 */
UCLASS(Transient, NotBlueprintable)
class RIM3D_API USimPawnAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	USimPawnAnimInstance();

	/** The databases to search and the loops for simple detail. Call once, before the first update. */
	void SetAssets(const TArray<TObjectPtr<const UPoseSearchDatabase>>& InDatabases, UAnimSequence* InIdle,
		UAnimSequence* InWalk, float InWalkSpeed, UAnimSequence* InRun, float InRunSpeed);

	/** Set every frame by the pawn actor before the mesh ticks. */
	FSimPawnAnimInputs Inputs;

	const FSimPawnAnimStats& GetStats() const { return Stats; }

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	friend struct FSimPawnAnimInstanceProxy;

	UPROPERTY(Transient)
	TArray<TObjectPtr<const UPoseSearchDatabase>> Databases;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> IdleLoop;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> WalkLoop;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> RunLoop;

	float WalkLoopSpeed = 150.0f;
	float RunLoopSpeed = 400.0f;

	FSimPawnAnimStats Stats;
};
