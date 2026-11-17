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
};

/**
 * The animation of one sim pawn, all in C++ (no Animation Blueprint): a montage slot over a
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

	/**
	 * Free hand (docs/design/free-hand.md): the right wrist reaches `Target` (this mesh's component space, cm), the elbow
	 * bent towards `Elbow` (same space); the palm turned towards `PalmFacing` (component space, unit; zero keeps the
	 * wrist as animated) by `PalmWeight` (0 to 1); the fingers curled by `Curl` (0 as animated .. 1 a loose grip).
	 * Alpha 0 is off (costs nothing). Set once a frame before the animation updates; followers through the leader pose
	 * and MakeHuman bodies retargeted from this mesh take the same arm.
	 */
	void SetFreeHand(float Alpha, const FVector& Target, const FVector& Elbow, const FVector& PalmFacing, float PalmWeight, float Curl)
	{
		FreeHandAlpha = Alpha;
		FreeHandTarget = Target;
		FreeHandElbow = Elbow;
		FreeHandPalm = PalmFacing;
		FreeHandPalmWeight = PalmWeight;
		FreeHandCurl = Curl;
	}
	float GetFreeHandAlpha() const { return FreeHandAlpha; }

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

	float FreeHandAlpha = 0.0f;
	FVector FreeHandTarget = FVector::ZeroVector;
	FVector FreeHandElbow = FVector::ZeroVector;
	FVector FreeHandPalm = FVector::ZeroVector;
	float FreeHandPalmWeight = 0.0f;
	float FreeHandCurl = 0.0f;

	FSimPawnAnimStats Stats;
};
