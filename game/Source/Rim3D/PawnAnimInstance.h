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
	/** The body keeps a facing of its own (a clip, a lock-on): the walk steps sideways or backwards under it rather than
	 *  predicting a turn towards where it goes. */
	bool bHoldFacing = false;
	/** Standing, a turn twists the upper body over planted feet (docs/design/pawns.md "Standing still"); off while a
	 *  whole-body clip, a roll, a jump or a crawl has the body. */
	bool bStandTwist = true;
	/** A whole-body clip still shows (blending out after the sim's clip ended, or a crouching action): the legs are
	 *  the clip's, so neither the feet holds nor the hips' pull to the sim's position may act on them. */
	bool bLegsFromClip = false;
	/** The legs' whole-body clip is only fading out (the sim's clip is over): the standing hold may take the feet. */
	bool bLegsClipFading = false;
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
	/** Standing still: a standing idle plays and motion matching rests (it shuffled the feet). */
	bool bStandingIdle = false;
	/** Standing idles started (each stop picks one at random) and drifts from one to another while standing. */
	int32 IdlesStarted = 0;
	int32 IdleDrifts = 0;
	/** Seam blends (an idle blended into its own start before its end, so it never wraps on screen). */
	int32 IdleSeams = 0;
	/** Motion matching picks thrown away: a turn, pivot or reface clip, or a run clip at a walking pace. */
	int32 BannedPicks = 0;
	/** Walking feet: how much the walking foot lock shows (0 to 1) and how many feet it holds now. */
	float WalkLock = 0.0f;
	int32 WalkFeetHeld = 0;
	/** Which feet the walking lock holds fully now: bit 0 the left, bit 1 the right. */
	int32 WalkHeldMask = 0;
	/** Steps a held walking foot took because the body left it behind (the leg out of reach, the foot far off the
	 *  animation's, or its facing held too far round). */
	int32 WalkSteps = 0;
	/** Degrees the upper body is twisted over planted feet now (a standing turn), and the most it has been. */
	float StandTwistDegrees = 0.0f;
	float MaxStandTwistDegrees = 0.0f;
	/** Standing feet: how much the hold on them shows (0 to 1), whether a foot is stepping round now, steps so far. */
	float FootHold = 0.0f;
	bool bFootStepping = false;
	int32 FootSteps = 0;
};

/**
 * The animation of one sim pawn, all in C++ (no Animation Blueprint): a hit layer (struck parts wobble and spring
 * back, a limp, the patching bend; body-health.md part E) over an upper-body montage slot over a montage slot over a
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

	/** The databases to search, the standing idles (with the footprint each stands on: USimPawnSubsystem::GetIdleFeet)
	 *  and the loops for simple detail. Call once, before the first update. */
	void SetAssets(const TArray<TObjectPtr<const UPoseSearchDatabase>>& InDatabases, const TArray<TObjectPtr<UAnimSequence>>& InIdles,
		const TArray<int32>& InIdleFeet, UAnimSequence* InWalk, float InWalkSpeed, UAnimSequence* InRun, float InRunSpeed);

	/** Set every frame by the pawn actor before the mesh ticks. */
	FSimPawnAnimInputs Inputs;

	/** The montage slot drawn on the chest, arms and head only, over whatever the legs do (walking, the default slot):
	 *  staggers, flinches and the player's swings (docs/design/body-motion.md "Upper-body clips"). */
	static const FName UpperBodySlot;

	// --- The hold layer (docs/design/weapon-hold.md): the held weapon's stance on the spine and arms, over the walk. ---

	/** The upper body plays Clip (looping) over the locomotion, cross-fading from the clip before over BlendSeconds; null
	 *  for none. Weight is how much it shows (0 to 1), eased towards at WeightRate per second. */
	void SetUpperStance(UAnimSequence* Clip, float Weight, float BlendSeconds = 0.2f, float WeightRate = 5.0f);
	/** Bends the body forward at the waist by this many degrees (a load on the back), eased in and out. */
	void SetSpineBend(float Degrees) { SpineBendTarget = Degrees; }
	UAnimSequence* GetUpperStance() const { return UpperClip; }
	float GetUpperWeight() const { return UpperWeight; }
	float GetSpineBend() const { return SpineBend; }
	/** Advances the layer's clocks and weights (game thread, from the proxy's PreUpdate). */
	void AdvanceUpper(float DeltaSeconds);

	const FSimPawnAnimStats& GetStats() const { return Stats; }

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	friend struct FSimPawnAnimInstanceProxy;

	UPROPERTY(Transient)
	TArray<TObjectPtr<const UPoseSearchDatabase>> Databases;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> IdleLoop;  // the first of IdleLoops

	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimSequence>> IdleLoops;
	TArray<int32> IdleFeet;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> WalkLoop;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> RunLoop;

	float WalkLoopSpeed = 150.0f;
	float RunLoopSpeed = 400.0f;

	FSimPawnAnimStats Stats;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> UpperClip;
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> UpperOldClip;
	float UpperTime = 0.0f;
	float UpperOldTime = 0.0f;
	float UpperBlend = 1.0f;       // the new clip's share against the old one
	float UpperBlendRate = 5.0f;
	float UpperWeight = 0.0f;      // how much the layer shows now
	float UpperWeightTarget = 0.0f;
	float UpperWeightRate = 5.0f;
	float SpineBend = 0.0f;        // degrees now
	float SpineBendTarget = 0.0f;
};
