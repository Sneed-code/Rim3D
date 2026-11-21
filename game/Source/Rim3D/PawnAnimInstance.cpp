#include "PawnAnimInstance.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/TrajectoryTypes.h"
#include "BlendStack/AnimNode_BlendStack.h"
#include "BonePose.h"
#include "IObjectChooser.h"
#include "PoseSearch/PoseSearchContext.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchEvent.h"
#include "PoseSearch/PoseSearchHistory.h"
#include "PoseSearch/PoseSearchLibrary.h"
#include "PoseSearch/PoseSearchResult.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchTrajectoryLibrary.h"
#if WITH_EDITOR
#include "PoseSearch/PoseSearchDerivedData.h"
#endif

namespace
{
	// Trajectory sampling, as the Game Animation Sample's characters use: 10 past samples 0.04 s
	// apart, 15 future samples 0.1 s apart (1.5 s ahead).
	constexpr int32 PawnHistorySamples = 10;
	constexpr float PawnHistoryInterval = 0.04f;
	constexpr int32 PawnPredictionSamples = 15;
	constexpr float PawnPredictionInterval = 0.1f;
	// Poses kept for the pose features of the search.
	constexpr int32 PawnHistoryPoses = 10;
	constexpr float PawnHistoryPoseInterval = 0.04f;

	// The sim speeds a pawn up and brakes by 20% of its speed per step, 30 steps a second at 1x.
	constexpr float PawnAccelerationPerSpeed = 0.2f * 30.0f;

	constexpr float PawnMotionMatchBlendTime = 0.25f;
	constexpr float PawnLoopBlendTime = 0.3f;
	// Below this a pawn stands (cm/s).
	constexpr float PawnStandSpeed = 30.0f;
	// Standing this long (s) with no route ahead, a full-detail pawn leaves motion matching for the breathing idle, blended
	// in over PawnStandBlendTime; the short wait lets a walk's stop clip plant the feet first.
	constexpr float PawnStandDelay = 0.3f;
	constexpr float PawnStandBlendTime = 0.4f;
	// A route shorter than this (cm) is no walk: the pawn is where it wants to be.
	constexpr float PawnStandRoute = 25.0f;
	// Still this long (s) with a route ahead (waiting at a door, for a friend to pass, between swings), the pawn stands
	// in its idle too: motion matching has no idle clips any more (they shuffled), so it must not be left to it.
	constexpr float PawnStillDelay = 0.6f;
	// A pawn standing a long time drifts to another standing idle now and then (s), blended slowly.
	constexpr float PawnIdleDriftMin = 14.0f;
	constexpr float PawnIdleDriftMax = 40.0f;
	constexpr float PawnIdleDriftBlendTime = 1.2f;
	// Standing turns (docs/design/pawns.md "Standing still"): the feet stay planted and the upper body twists towards the
	// new facing. Past PawnStandStepAt degrees the pawn takes two small steps (one foot, then the other, PawnStandStepTime
	// each, lifted PawnStandStepLift cm at the middle) to where its idle wants the feet for the new facing. The twist
	// never passes PawnStandTwistMax. Leaving the idle, the twist and the planted feet fade out over PawnStandRelease.
	constexpr float PawnStandStepAt = 40.0f;
	constexpr float PawnStandStepAgain = 6.0f;
	constexpr float PawnStandStepTime = 0.3f;
	constexpr float PawnStandStepLift = 5.0f;
	constexpr float PawnStandTwistMax = 60.0f;
	constexpr float PawnStandRelease = 0.2f;
	// A foot further than this (cm) from where the idle wants it steps too (an idle drift, a body that crept).
	constexpr float PawnStandStepFar = 22.0f;
	// A held foot further from its hip than this share of the leg's length steps (the leg straight is 1).
	constexpr double PawnStandReachShare = 0.97;
	constexpr float PawnTwistShare[4] = {0.35f, 0.25f, 0.2f, 0.2f};
	// Planted feet (docs/design/pawns.md "Standing still"): standing, each foot is held where it was when the pawn
	// stopped, the legs bent to reach it, so the blend from the walk's stop into a standing idle (whose feet stand a
	// little differently), a drift between idles and a standing turn move the body over the feet and never slide them.
	// The hold lets go if the body drifts this far from where it stopped (cm).
	constexpr float PawnFootLockDrift = 25.0f;
	// Motion matching: no new clip sooner than this after the last jump (s), unless the clip playing runs out; and the
	// play rate may follow the real speed this far, so a planted foot does not slide when the pace is off the clip's.
	constexpr float PawnMotionMatchMinGap = 0.15f;
	constexpr float PawnMotionMatchRateMin = 0.6f;
	constexpr float PawnMotionMatchRateMax = 1.6f;
	// How fast the drawn play rate follows the wanted one (s, the time constant of an exponential ease).
	constexpr float PawnPlayRateEase = 0.2f;
	// The locomotion's pose follows its animation with this time constant (s): one key's time (PawnKeySmooth in the hold
	// layer). A bone's speed that the keys change at once then changes over about this long.
	constexpr float PawnKeySmooth = 1.0f / 30.0f;
	// Enough blends at once that none is ever dropped half shown (dropping one was a pop).
	constexpr int32 PawnBlendStackMax = 12;
	// A standing idle never wraps from its last frame to its first (most do not match up: a pop every loop). Before
	// its end it blends over PawnIdleSeamBlend into itself from the start, so the old copy wraps unseen.
	constexpr float PawnIdleSeamBlend = 0.5f;
	// Walking feet (docs/design/pawns.md "No pops"): a foot down on the ground in the animation (within
	// PawnWalkLockContact cm of the lowest it has been lately, the low point creeping up PawnWalkLockGroundRise cm a
	// second) is held where it came down, the leg bent to reach it, so a pace off the clip's or a blend between clips
	// cannot slide it. Nothing about it ever changes speed at once (a jump in a foot's speed is a pop):
	// - it lands with the speed it came down at, which dies away over PawnWalkLockSettle;
	// - a hold more than PawnWalkLockSoftOff cm behind the animation's foot (a shove, a pace off the clip's) follows it,
	//   PawnWalkLockFollowRate times the excess a second, so the foot gathers speed instead of being dragged from rest
	//   at the body's speed (that was 900 to 2800 m/s^2 in the PC test); PawnWalkLockMaxOff cm is the hard limit;
	// - lifted, it lets go over PawnWalkLockRelease along a curve that starts at the speed it had and ends on the
	//   animation's foot.
	// The whole lock fades in and out over PawnWalkLockBlend (off for clips that move the whole body, jumps, rolls,
	// crawling).
	constexpr float PawnWalkLockContact = 3.0f;
	constexpr float PawnWalkLockGroundRise = 2.0f;
	constexpr float PawnWalkLockSettle = 0.025f;
	constexpr float PawnWalkLockSoftOff = 8.0f;
	constexpr float PawnWalkLockFollowRate = 22.0f;
	constexpr float PawnWalkLockMaxOff = 50.0f;
	constexpr float PawnWalkLockRelease = 0.15f;
	constexpr float PawnWalkLockMaxSpeed = 1500.0f;
	constexpr float PawnWalkLockBlend = 0.15f;
	// The hips stay over the sim's position (docs/design/body-motion.md): the sample's stop and transition clips lead or
	// trail their root by up to 20 cm, and the root is not where the sim has the body; past PawnHipsReach cm from the
	// mesh's origin the hips are pulled back to it (the held feet stay put, the legs reach).
	constexpr float PawnHipsReach = 8.0f;
	// The pull starts gently past PawnHipsEase cm and nears PawnHipsReach without ever passing it (a tanh curve, so the
	// hips' speed never changes at once where the pull begins: a hard stop at 8 cm was a one-frame jolt of every bone).
	constexpr float PawnHipsEase = 4.0f;
	// A foot only lands while the animation's foot moves slower than this over the ground (cm/s, world): a running foot
	// swinging low past the ground is not a landing (holding it cut its speed at once, a foot pop, and dragged it).
	constexpr float PawnWalkLockLandSpeed = 150.0f;
	// A planted foot keeps its facing too (degrees, world): the body turning over it, or the animation swivelling it,
	// would otherwise spin the toes round the ankle (a slide at the toes). Held up to these many degrees off the
	// animation's facing; a let-go foot turns back to the animation's facing over PawnWalkLockRelease, at the rate it had.
	constexpr float PawnWalkYawMax = 35.0f;
	constexpr float PawnStandYawMax = 60.0f;
	constexpr float PawnWalkYawMaxRate = 540.0f;
	// The foot's drawn turn follows the holds' as a critically damped spring at this rate (1/s).
	constexpr float PawnFootTurnRate = 30.0f;
	constexpr float PawnFootTurnFeed = 1500.0f;

	/** The facing of a foot (degrees, world yaw) from its ankle to its ball. */
	float PawnFootYaw(const FVector& Ankle, const FVector& Ball)
	{
		const FVector Way = Ball - Ankle;
		return FMath::RadiansToDegrees(static_cast<float>(FMath::Atan2(Way.Y, Way.X)));
	}

	/** The let-go facing's offset (degrees) closing to nought as Fade goes 1 to 0: the same cubic as PawnWalkRelease. */
	void PawnWalkYawRelease(float Gap, float GapRate, float Fade, float& OutOffset, float& OutRate)
	{
		const float S = 1.0f - FMath::Clamp(Fade, 0.0f, 1.0f);
		const float T = PawnWalkLockRelease;
		OutOffset = Gap * (2.0f * S * S * S - 3.0f * S * S + 1.0f) + GapRate * (T * (S * S * S - 2.0f * S * S + S));
		OutRate = Gap * ((6.0f * S * S - 6.0f * S) / T) + GapRate * (3.0f * S * S - 4.0f * S + 1.0f);
	}

	float PawnSmooth(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	/**
	 * Clips motion matching must never play here: turns, pivots and refaces turn the body by their own root, and the
	 * sim turns the actor, so under a pawn that keeps its facing they spun the legs and slid the feet (Stand_Turn_180
	 * slid them 24 m/s in the PC test); at a walking pace, run and sprint clips (stops, transitions) too.
	 */
	bool PawnMotionClipBanned(const UObject* Asset, bool bWalkPace)
	{
		if (!Asset)
		{
			return false;
		}
		const FString Name = Asset->GetName();
		for (const TCHAR* Word : {TEXT("Turn"), TEXT("Pivot"), TEXT("Reface")})
		{
			if (Name.Contains(Word))
			{
				return true;
			}
		}
		return bWalkPace && (Name.Contains(TEXT("Run")) || Name.Contains(TEXT("Sprint")));
	}

	/**
	 * A let-go walking foot's offset from the animation's foot, and how fast that offset changes: a cubic from Gap,
	 * changing at GapVel (cm/s), to nought and still, over PawnWalkLockRelease as Fade goes from 1 to 0.
	 */
	void PawnWalkRelease(const FVector& Gap, const FVector& GapVel, float Fade, FVector& OutOffset, FVector& OutRate)
	{
		const double S = 1.0 - FMath::Clamp(static_cast<double>(Fade), 0.0, 1.0);
		const double T = PawnWalkLockRelease;
		OutOffset = Gap * (2.0 * S * S * S - 3.0 * S * S + 1.0) + GapVel * (T * (S * S * S - 2.0 * S * S + S));
		OutRate = Gap * ((6.0 * S * S - 6.0 * S) / T) + GapVel * (3.0 * S * S - 4.0 * S + 1.0);
	}

	/** The mesh faces +Y in its own space, so it is turned -90 degrees under the actor. */
	FQuat PawnMeshFacing(float ActorYaw)
	{
		return FRotator(0.0, ActorYaw - 90.0, 0.0).Quaternion();
	}

	float PawnApproach(float Value, float Target, float MaxStep)
	{
		return Value < Target ? FMath::Min(Value + MaxStep, Target) : FMath::Max(Value - MaxStep, Target);
	}

	// --- The hit layer (docs/design/body-health.md part E). ---
	// A struck zone is a damped spring about its joint: how fast it swings back (radians a second), how soon the swing
	// dies (0 never, 1 without a wobble), the furthest it may bend (radians), and the longest step it is worked out in.
	constexpr float PawnKickFrequency = 20.0f;
	constexpr float PawnKickDamping = 0.28f;
	constexpr float PawnKickMaxAngle = 0.6f;
	constexpr float PawnKickStep = 1.0f / 120.0f;
	// How far a full-strength hit bends each zone at the top of its first swing (radians), by ESimPawnZone.
	// (Head 0.28 at first: a rifle's 9 damage turned it 7 degrees, which nobody could see; sneed 2026-11-20 wants the head to
	// snap back like the knee does.)
	constexpr float PawnKickPeak[static_cast<int32>(ESimPawnZone::Count)] = {0.5f, 0.16f, 0.38f, 0.38f, 0.2f, 0.2f};
	// A spring this damped reaches about 0.7 of (start speed / frequency) on its first swing.
	constexpr float PawnKickPeakShare = 0.7f;
	// The limp: over a hurt leg the body tips to that side and forward, the head stays level and the hips drop.
	constexpr float PawnLimpSideDegrees = 8.0f;
	constexpr float PawnLimpForwardDegrees = 4.0f;
	constexpr float PawnLimpHeadDegrees = 3.5f;
	constexpr float PawnLimpDipCm = 2.5f;
	// One foot this much lower than the other (cm) carries all the weight.
	constexpr float PawnLimpFootSpan = 6.0f;

	FQuat PawnRotationOf(const FVector& RotationVector)
	{
		const double Angle = RotationVector.Size();
		return Angle < 1.0e-5 ? FQuat::Identity : FQuat(RotationVector / Angle, Angle);
	}

	/** Turning about `Up x Along` by `Degrees`: the top of what turns goes along `Along` (mesh space). */
	FQuat PawnLean(const FVector& Along, float Degrees)
	{
		const FVector Axis = FVector::CrossProduct(FVector::ZAxisVector, Along).GetSafeNormal();
		return Axis.IsNearlyZero() ? FQuat::Identity : FQuat(Axis, FMath::DegreesToRadians(Degrees));
	}

	bool PawnBoneValid(const FCompactPose& Pose, FCompactPoseBoneIndex Bone)
	{
		return Bone.GetInt() != INDEX_NONE && Pose.IsValidIndex(Bone);
	}

	/** The rotation of a bone's parent in the mesh's space: every parent's own, multiplied down from the root. */
	FQuat PawnParentRotation(const FCompactPose& Pose, FCompactPoseBoneIndex Bone)
	{
		FQuat Rotation = FQuat::Identity;
		for (FCompactPoseBoneIndex Parent = Pose.GetParentBoneIndex(Bone); Parent.GetInt() != INDEX_NONE; Parent = Pose.GetParentBoneIndex(Parent))
		{
			Rotation = Pose[Parent].GetRotation() * Rotation;
		}
		return Rotation;
	}

	/** Turns a bone, and all that hangs on it, by `Delta` given in the mesh's space, about the bone's own joint. */
	void PawnTurnBone(FCompactPose& Pose, FCompactPoseBoneIndex Bone, const FQuat& Delta)
	{
		if (!PawnBoneValid(Pose, Bone) || Delta.Equals(FQuat::Identity, 1.0e-6))
		{
			return;
		}
		const FQuat Parent = PawnParentRotation(Pose, Bone);
		FTransform& Local = Pose[Bone];
		Local.SetRotation((Parent.Inverse() * Delta * Parent * Local.GetRotation()).GetNormalized());
	}

	/** Where a bone is in the mesh's space. */
	FVector PawnBonePlace(const FCompactPose& Pose, FCompactPoseBoneIndex Bone)
	{
		FTransform Place = Pose[Bone];
		for (FCompactPoseBoneIndex Parent = Pose.GetParentBoneIndex(Bone); Parent.GetInt() != INDEX_NONE; Parent = Pose.GetParentBoneIndex(Parent))
		{
			Place = Place * Pose[Parent];
		}
		return Place.GetLocation();
	}

	/** A bone's whole transform in the mesh's space. */
	FTransform PawnBoneInMesh(const FCompactPose& Pose, FCompactPoseBoneIndex Bone)
	{
		FTransform Place = Pose[Bone];
		for (FCompactPoseBoneIndex Parent = Pose.GetParentBoneIndex(Bone); Parent.GetInt() != INDEX_NONE; Parent = Pose.GetParentBoneIndex(Parent))
		{
			Place = Place * Pose[Parent];
		}
		return Place;
	}

	/**
	 * Bends a leg (thigh, calf, foot; all valid, the calf the thigh's child and the foot the calf's) so the foot reaches
	 * Target (mesh space), the knee staying on the side it is on now; the foot keeps its own turn in the mesh's space.
	 */
	void PawnReachLeg(FCompactPose& Pose, FCompactPoseBoneIndex Thigh, FCompactPoseBoneIndex Calf, FCompactPoseBoneIndex Foot, const FVector& Target)
	{
		const FCompactPoseBoneIndex Hip = Pose.GetParentBoneIndex(Thigh);
		const FTransform HipMesh = Hip.GetInt() != INDEX_NONE ? PawnBoneInMesh(Pose, Hip) : FTransform::Identity;
		const FTransform ThighMesh = Pose[Thigh] * HipMesh;
		const FTransform CalfMesh = Pose[Calf] * ThighMesh;
		const FTransform FootMesh = Pose[Foot] * CalfMesh;
		const FVector A = ThighMesh.GetLocation();
		const FVector B = CalfMesh.GetLocation();
		const FVector C = FootMesh.GetLocation();
		const double Upper = (B - A).Size();
		const double Lower = (C - B).Size();
		if (Upper < 1.0 || Lower < 1.0)
		{
			return;
		}
		FVector Along = Target - A;
		const double Reach = FMath::Clamp(Along.Size(), FMath::Abs(Upper - Lower) + 0.5, Upper + Lower - 0.05);
		if (!Along.Normalize())
		{
			return;
		}
		// The knee's side: where it is now, off the hip-to-foot line.
		FVector Knee = (B - A) - Along * FVector::DotProduct(B - A, Along);
		if (!Knee.Normalize())
		{
			Knee = FVector::YAxisVector;  // forward in the mesh's space
		}
		const double CosHip = FMath::Clamp((Upper * Upper + Reach * Reach - Lower * Lower) / (2.0 * Upper * Reach), -1.0, 1.0);
		const FVector NewB = A + Along * (Upper * CosHip) + Knee * (Upper * FMath::Sqrt(FMath::Max(0.0, 1.0 - CosHip * CosHip)));
		const FVector NewC = A + Along * Reach;
		// Turn the thigh onto the new knee, then the calf onto the new foot.
		const FQuat ThighTurn = FQuat::FindBetweenNormals((B - A).GetSafeNormal(), (NewB - A).GetSafeNormal());
		FTransform NewThigh = ThighMesh;
		NewThigh.SetRotation((ThighTurn * ThighMesh.GetRotation()).GetNormalized());
		const FVector CalfWay = ThighTurn.RotateVector((C - B).GetSafeNormal());
		const FQuat CalfTurn = FQuat::FindBetweenNormals(CalfWay, (NewC - NewB).GetSafeNormal());
		FTransform NewCalf = CalfMesh;
		NewCalf.SetLocation(NewB);
		NewCalf.SetRotation((CalfTurn * ThighTurn * CalfMesh.GetRotation()).GetNormalized());
		FTransform NewFoot = FootMesh;
		NewFoot.SetLocation(NewC);
		// Back to each bone's own space under its parent.
		Pose[Thigh] = NewThigh.GetRelativeTransform(HipMesh);
		Pose[Calf] = NewCalf.GetRelativeTransform(NewThigh);
		Pose[Foot] = NewFoot.GetRelativeTransform(NewCalf);
		Pose[Thigh].NormalizeRotation();
		Pose[Calf].NormalizeRotation();
		Pose[Foot].NormalizeRotation();
	}
}

/**
 * Locomotion: motion matching or loops, both through a blend stack, plus the pose history and
 * trajectory motion matching needs. Runs on animation worker threads; it reads only the proxy.
 */
struct FSimPawnLocomotionNode : public FAnimNode_Base
{
	struct FSimPawnAnimInstanceProxy* Proxy = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

	FSimPawnAnimStats Stats;

private:
	void UpdateTrajectory(const FSimPawnAnimInputs& In, float DeltaTime);
	void PredictAlongRoute(const FSimPawnAnimInputs& In, float DeltaTime);
	void UpdateMotionMatching(const FAnimationUpdateContext& Context, float DeltaTime);
	void UpdateLoops(const FAnimationUpdateContext& Context, const FSimPawnAnimInputs& In, float BlendTime);
	/** Whether the pawn stands where it wants to be: hardly moving and no route ahead. */
	static bool IsStanding(const FSimPawnAnimInputs& In);
	/** Plays this pawn's standing idle (picking one when it starts standing, drifting to another on the same footprint
	 *  after a while); false without one. */
	bool UpdateStandingIdle(const FAnimationUpdateContext& Context, float BlendTime, float DeltaTime);
	/** Whether Asset is one of the standing idles. */
	bool IsIdleAsset(const UAnimationAsset* Asset) const;
	/** A standing idle at random, not the one played last if there is a choice; with Feet >= 0 only those on that footprint. */
	int32 PickIdle(int32 Feet);

	FAnimNode_BlendStack_Standalone BlendStack;
	UE::PoseSearch::FPoseHistory History;
	FTransformTrajectory Trajectory;
	FMotionMatchingState State;
	bool bCacheBones = true;
	bool bResetHistory = true;
	/** The ground speed a moment ago (smoothed over about 0.15 s), cm/s: whether the pawn is speeding up or holding its pace. */
	float RecentSpeed = 0.0f;
	/** The velocity smoothed over about 0.12 s: the player's pawn moves by the steps the keys send, uneven from step to step. */
	FVector SmoothVelocity = FVector::ZeroVector;
	/** How long the pawn has stood still (s); 0 while it moves. StillTime the same, route or not. */
	float StandTime = 0.0f;
	float StillTime = 0.0f;
	/** The standing idle this pawn plays (index into the proxy's IdleLoops), INDEX_NONE before the first. */
	int32 IdleIndex = INDEX_NONE;
	/** Seconds until it drifts to another idle on the same footprint while it keeps standing. */
	float IdleDriftIn = 0.0f;
	/** Cosmetic randomness (which idle, where in it to start, when to drift): not the sim's. */
	FRandomStream Random;
	/** Seconds since motion matching last jumped to a new clip. */
	float SinceJump = 1.0f;
	/** The play rate drawn (PawnPlayRateEase): eased toward the one wanted, since every bone's speed scales with it and
	 *  a search that picks a new rate would change them all at once (a pop). This update's time, for the easing. */
	float RateShown = 1.0f;
	float RateDt = 0.0f;
	void SetPlayRate(float Want);
};

/**
 * The montage slot over the locomotion: what Epic's slot node does, without the parts that need
 * an Animation Blueprint (its sequencer hook asks the anim class for subsystems a native class
 * doesn't have, and asserts).
 */
/**
 * The hold layer (docs/design/weapon-hold.md): over the locomotion, the held weapon's stance clip on the spine, arms and
 * head (spine_01 turned in mesh space, so a stance that turns its hips still faces the way the clip's chest does over
 * walking legs; the rest blended bone by bone), and a forward bend at the waist for a load on the back.
 */
struct FSimPawnUpperNode : public FAnimNode_Base
{
	FAnimNode_Base* Source = nullptr;
	struct FSimPawnAnimInstanceProxy* Proxy = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
	}
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

private:
	TArray<float> BoneWeights;  // by compact pose index: 0 the legs and hips, 1 the chest, arms and head
	FCompactPoseBoneIndex Pelvis = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Spine = FCompactPoseBoneIndex(INDEX_NONE);
	/** How much the hips are pulled over the sim's position (PawnHipsReach), 0 to 1, eased in Evaluate. */
	float HipsWeight = 0.0f;
	/** The locomotion's pose smoothed (PawnKeySmooth), bone by bone in each bone's own space; this update's time, and
	 *  whether to start again from the pose as it is (the first frame, a teleport). */
	TArray<FTransform> Smoothed;
	float SmoothDt = 0.0f;
	bool bSmoothReset = true;
};

struct FSimPawnSlotNode : public FAnimNode_Base
{
	FAnimNode_Base* Source = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
		SlotNodeWeight = 0.0f;
		SourceWeight = 1.0f;
		TotalNodeWeight = 0.0f;
		if (!Registered.IsSynchronized_Counter(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter()))
		{
			Registered.SynchronizeWith(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter());
			Context.AnimInstanceProxy->RegisterSlotNodeWithAnimInstance(FAnimSlotGroup::DefaultSlotName);
		}
	}

	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override
	{
		Source->CacheBones_AnyThread(Context);
	}

	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override
	{
		Context.AnimInstanceProxy->GetSlotWeight(FAnimSlotGroup::DefaultSlotName, SlotNodeWeight, SourceWeight, TotalNodeWeight);
		Context.AnimInstanceProxy->UpdateSlotNodeWeight(FAnimSlotGroup::DefaultSlotName, SlotNodeWeight, Context.GetFinalBlendWeight());
		// Locomotion always updates, so the pawn walks on under an action.
		Source->Update_AnyThread(Context);
	}

	virtual void Evaluate_AnyThread(FPoseContext& Output) override
	{
		if (SlotNodeWeight <= ZERO_ANIMWEIGHT_THRESH)
		{
			Source->Evaluate_AnyThread(Output);
			return;
		}
		FPoseContext SourceContext(Output);
		Source->Evaluate_AnyThread(SourceContext);
		const FAnimationPoseData SourcePose(SourceContext);
		FAnimationPoseData OutputPose(Output);
		Output.AnimInstanceProxy->SlotEvaluatePose(FAnimSlotGroup::DefaultSlotName, SourcePose, SourceWeight, OutputPose, SlotNodeWeight, TotalNodeWeight);
	}

private:
	FGraphTraversalCounter Registered;
	float SlotNodeWeight = 0.0f;
	float SourceWeight = 1.0f;
	float TotalNodeWeight = 0.0f;
};

/**
 * The upper-body slot (USimPawnAnimInstance::UpperBodySlot): a montage there takes the chest, arms and head, and the
 * legs and hips keep what plays under it, so a staggered pawn stumbles on walking legs and the player swings while
 * walking (sneed 2026-11-18; docs/design/body-motion.md "Upper-body clips"). spine_01 is set in mesh space, as the
 * hold layer does, so the clip's chest faces the way it does in the clip over whatever the hips are doing.
 */
struct FSimPawnUpperSlotNode : public FAnimNode_Base
{
	FAnimNode_Base* Source = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
		SlotNodeWeight = 0.0f;
		SourceWeight = 1.0f;
		TotalNodeWeight = 0.0f;
		if (!Registered.IsSynchronized_Counter(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter()))
		{
			Registered.SynchronizeWith(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter());
			Context.AnimInstanceProxy->RegisterSlotNodeWithAnimInstance(USimPawnAnimInstance::UpperBodySlot);
		}
	}

	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;

	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override
	{
		Context.AnimInstanceProxy->GetSlotWeight(USimPawnAnimInstance::UpperBodySlot, SlotNodeWeight, SourceWeight, TotalNodeWeight);
		Context.AnimInstanceProxy->UpdateSlotNodeWeight(USimPawnAnimInstance::UpperBodySlot, SlotNodeWeight, Context.GetFinalBlendWeight());
		Source->Update_AnyThread(Context);
	}

	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

private:
	FGraphTraversalCounter Registered;
	float SlotNodeWeight = 0.0f;
	float SourceWeight = 1.0f;
	float TotalNodeWeight = 0.0f;
	TArray<float> BoneWeights;  // by compact pose index: 0 the legs and hips, 1 under spine_01
	FCompactPoseBoneIndex Pelvis = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Spine = FCompactPoseBoneIndex(INDEX_NONE);
};

/**
 * The hit layer, over everything else (docs/design/body-health.md part E). A blow or a bullet knocks the zone it lands
 * on along its way and the zone springs back with a wobble, whatever animation plays underneath (which is never
 * stopped or restarted); a hurt leg makes the body lurch over it while that foot is down; a pawn patching a wound
 * bends over it with a hand on the place. Runs on animation worker threads; it reads only the proxy.
 */
struct FSimPawnHitLayerNode : public FAnimNode_Base
{
	FAnimNode_Base* Source = nullptr;
	struct FSimPawnAnimInstanceProxy* Proxy = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
		Forget();
	}

	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

	int32 Kicks = 0;
	float WobbleDegrees = 0.0f;
	float MaxWobbleDegrees = 0.0f;
	float LimpNow = 0.0f;
	float MaxLimp = 0.0f;
	/** The standing turn: how far the upper body is twisted over the planted feet now (degrees), and the most so far;
	 *  how much the planted feet show (0 to 1), whether a foot is stepping, and steps taken. */
	float TwistDegrees = 0.0f;
	float MaxTwistDegrees = 0.0f;
	float FootHold = 0.0f;
	bool bStepping = false;
	int32 FootSteps = 0;
	/** The walking foot lock: how much it shows (0 to 1), how many feet it holds now and which (bit 0 left, 1 right). */
	float WalkLockWeight = 0.0f;
	int32 WalkFeetHeld = 0;
	int32 WalkHeldMask = 0;

private:
	struct FSpring
	{
		/** How far the zone is turned off its animation (a rotation vector in the mesh's space, radians) and how fast it turns. */
		FVector Angle = FVector::ZeroVector;
		FVector Rate = FVector::ZeroVector;
	};

	void Forget();
	void Kick(const FSimPawnKick& Hit);

	FSpring Springs[static_cast<int32>(ESimPawnZone::Count)];
	bool bOn = true;
	/** 0 to 1: walking (the limp shows only then), and how much of the weight each hurt leg carries now. */
	float Walking = 0.0f;
	float WeightLeft = 0.5f;
	float WeightRight = 0.5f;
	float LimpLeft = 0.0f;
	float LimpRight = 0.0f;
	float Patch = 0.0f;
	ESimPawnZone PatchZone = ESimPawnZone::Torso;
	/** Standing feet, all worked out in Evaluate: wanted now (set in the update), held, how much the hold and the twist
	 *  show (0 to 1, fading out on leaving), where the body stood when it took hold (world), the way the feet face
	 *  (actor yaw, degrees), each foot's place (world), and the step under way (the foot, its time, where from, the
	 *  feet's yaw then, and whether it is the first of the two). */
	bool bWantFootLock = false;
	bool bFootLocked = false;
	float FootLockWeight = 0.0f;
	FVector FootLockCentre = FVector::ZeroVector;
	float FeetYaw = 0.0f;
	FVector FootPlant[2] = {FVector::ZeroVector, FVector::ZeroVector};
	int32 StepFoot = INDEX_NONE;
	float StepTime = 0.0f;
	FVector StepFrom = FVector::ZeroVector;
	float StepYawFrom = 0.0f;
	bool bFirstStep = true;
	bool bSecondStepDue = false;
	/** Walking feet (PawnWalkLock*): wanted (set in the update); per foot whether it is held, where (world), the gap
	 *  left when it let go (world, closing as WalkFade goes to nought) and the lowest it has been lately (mesh space). */
	bool bWantWalkLock = false;
	bool bWalkHeld[2] = {false, false};
	FVector WalkPlant[2] = {FVector::ZeroVector, FVector::ZeroVector};
	/** A held foot's speed left from landing (world, cm/s, dying away) and its following speed last frame. */
	FVector WalkVel[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector WalkFollow[2] = {FVector::ZeroVector, FVector::ZeroVector};
	/** A held ankle's speed over the ground last frame (world, cm/s), all told. */
	FVector WalkHeldVel[2] = {FVector::ZeroVector, FVector::ZeroVector};
	/** The let-go foot's offset when it let go and how fast that offset was changing then. */
	FVector WalkGap[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector WalkGapVel[2] = {FVector::ZeroVector, FVector::ZeroVector};
	/** The animation's feet last frame (world), for their speed. */
	FVector WalkLastWanted[2] = {FVector::ZeroVector, FVector::ZeroVector};
	float WalkFade[2] = {0.0f, 0.0f};

	float WalkGround[2] = {0.0f, 0.0f};
	bool bWalkGroundKnown = false;
	/** The toes: the ball's lowest lately (mesh space); a foot is down while its heel or its ball is. On the ball alone
	 *  (the heel lifting to push off) the ball stays put and the ankle swings over it: the ball's place from the ankle
	 *  last frame (world, with the held facing) is what moves the held ankle. */
	float WalkBallGround[2] = {0.0f, 0.0f};
	FVector WalkToeLast[2] = {FVector::ZeroVector, FVector::ZeroVector};
	/** The held facing (world yaw, degrees) and the offset from the animation's facing drawn now; a let-go foot's
	 *  facing offset and its rate when it let go; the animation's facing last frame. */
	float WalkYawHeld[2] = {0.0f, 0.0f};
	float WalkYaw[2] = {0.0f, 0.0f};
	float WalkYawGap[2] = {0.0f, 0.0f};
	float WalkYawGapRate[2] = {0.0f, 0.0f};
	float WalkLastYaw[2] = {0.0f, 0.0f};
	/** The standing feet's facing (world yaw): held from when the hold took them (or a step landed), the turn put on
	 *  last frame, whether it is to be taken this frame, and the facing a stepping foot left from. */
	float StandYawHeld[2] = {0.0f, 0.0f};
	float StandYawLast[2] = {0.0f, 0.0f};
	bool bStandYawTake[2] = {false, false};
	float StandYawFrom[2] = {0.0f, 0.0f};
	/** Each foot's turn drawn (degrees) and how fast it turns: a spring after the holds' wanted turn. */
	float FootTurn[2] = {0.0f, 0.0f};
	float FootTurnRate[2] = {0.0f, 0.0f};
	float FootTurnWanted[2] = {0.0f, 0.0f};
	FCompactPoseBoneIndex Calf[2] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};

	FCompactPoseBoneIndex Pelvis = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex SpineLow = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex SpineMid = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex SpineHigh = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Head = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex UpperArm[2] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};
	FCompactPoseBoneIndex LowerArm[2] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};
	FCompactPoseBoneIndex Thigh[2] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};
	FCompactPoseBoneIndex Foot[2] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};
	FCompactPoseBoneIndex Ball[2] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};
};

struct FSimPawnAnimInstanceProxy : public FAnimInstanceProxy
{
	explicit FSimPawnAnimInstanceProxy(USimPawnAnimInstance* InInstance)
		: FAnimInstanceProxy(InInstance)
	{
	}

	FSimPawnAnimInputs Inputs;
	TArray<TObjectPtr<const UPoseSearchDatabase>> Databases;
	UAnimSequence* IdleLoop = nullptr;
	TArray<UAnimSequence*, TInlineAllocator<8>> IdleLoops;
	TArray<int32, TInlineAllocator<8>> IdleFeet;
	/** Set by the locomotion each update: a standing idle plays (the hit layer then keeps the feet planted on turns). */
	bool bStandingIdleNow = false;
	UAnimSequence* WalkLoop = nullptr;
	UAnimSequence* RunLoop = nullptr;
	float WalkLoopSpeed = 150.0f;
	float RunLoopSpeed = 400.0f;

protected:
	virtual void Initialize(UAnimInstance* InAnimInstance) override
	{
		// The graph is five native nodes: the hit layer over the upper-body slot (staggers, the player's swings) over
		// the montage slot (actions, whole-body clips) over the hold layer over the locomotion.
		HitLayer.Source = &UpperSlot;
		HitLayer.Proxy = this;
		UpperSlot.Source = &Slot;
		Slot.Source = &Upper;
		Upper.Source = &Locomotion;
		Upper.Proxy = this;
		Locomotion.Proxy = this;
		FAnimInstanceProxy::Initialize(InAnimInstance);
	}

	virtual FAnimNode_Base* GetCustomRootNode() override { return &HitLayer; }

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override
	{
		FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
		USimPawnAnimInstance* Instance = CastChecked<USimPawnAnimInstance>(InAnimInstance);
		// Hits wait here until the hit layer takes them: an update can come without the graph running.
		for (const FSimPawnKick& Hit : Instance->Inputs.Kicks)
		{
			if (PendingKicks.Num() < 8)
			{
				PendingKicks.Add(Hit);
			}
		}
		Instance->Inputs.Kicks.Reset();
		Inputs = Instance->Inputs;
		Databases = Instance->Databases;
		IdleLoop = Instance->IdleLoop;
		if (IdleLoops.Num() != Instance->IdleLoops.Num())
		{
			IdleLoops.Reset();
			IdleFeet.Reset();
			for (int32 i = 0; i < Instance->IdleLoops.Num(); ++i)
			{
				IdleLoops.Add(Instance->IdleLoops[i].Get());
				IdleFeet.Add(Instance->IdleFeet.IsValidIndex(i) ? Instance->IdleFeet[i] : 0);
			}
		}
		WalkLoop = Instance->WalkLoop;
		RunLoop = Instance->RunLoop;
		WalkLoopSpeed = Instance->WalkLoopSpeed;
		RunLoopSpeed = Instance->RunLoopSpeed;
		USimPawnAnimInstance* Mutable = CastChecked<USimPawnAnimInstance>(InAnimInstance);
		Mutable->AdvanceUpper(DeltaSeconds);
		UpperClip = Mutable->UpperClip;
		UpperOldClip = Mutable->UpperBlend < 1.0f ? Mutable->UpperOldClip.Get() : nullptr;
		UpperTime = Mutable->UpperTime;
		UpperOldTime = Mutable->UpperOldTime;
		UpperBlend = Mutable->UpperBlend;
		UpperWeight = Mutable->UpperWeight;
		SpineBend = Mutable->SpineBend;
	}

public:
	// The hold layer, copied before each update (game thread).
	UAnimSequence* UpperClip = nullptr;
	UAnimSequence* UpperOldClip = nullptr;
	float UpperTime = 0.0f;
	float UpperOldTime = 0.0f;
	float UpperBlend = 1.0f;
	float UpperWeight = 0.0f;
	float SpineBend = 0.0f;

protected:

	virtual void PostUpdate(UAnimInstance* InAnimInstance) const override
	{
		FAnimInstanceProxy::PostUpdate(InAnimInstance);
		FSimPawnAnimStats& Out = CastChecked<USimPawnAnimInstance>(InAnimInstance)->Stats;
		Out = Locomotion.Stats;
		Out.Kicks = HitLayer.Kicks;
		Out.WobbleDegrees = HitLayer.WobbleDegrees;
		Out.MaxWobbleDegrees = HitLayer.MaxWobbleDegrees;
		Out.LimpNow = HitLayer.LimpNow;
		Out.MaxLimp = HitLayer.MaxLimp;
		Out.StandTwistDegrees = HitLayer.TwistDegrees;
		Out.MaxStandTwistDegrees = HitLayer.MaxTwistDegrees;
		Out.FootHold = HitLayer.FootHold;
		Out.bFootStepping = HitLayer.bStepping;
		Out.FootSteps = HitLayer.FootSteps;
		Out.WalkLock = HitLayer.WalkLockWeight;
		Out.WalkFeetHeld = HitLayer.WalkFeetHeld;
		Out.WalkHeldMask = HitLayer.WalkLockWeight >= 0.999f ? HitLayer.WalkHeldMask : 0;
	}

public:
	TArray<FSimPawnKick, TInlineAllocator<8>> PendingKicks;

private:
	FSimPawnHitLayerNode HitLayer;
	FSimPawnUpperSlotNode UpperSlot;
	FSimPawnSlotNode Slot;
	FSimPawnUpperNode Upper;
	FSimPawnLocomotionNode Locomotion;
};

// ---------------------------------------------------------------------------
// The hold layer.

namespace
{
	/** A looping clip's time, wrapped. */
	float PawnUpperWrap(const UAnimSequence* Clip, float Time)
	{
		const float Length = Clip ? FMath::Max(Clip->GetPlayLength(), 0.001f) : 1.0f;
		return FMath::Fmod(FMath::Max(Time, 0.0f), Length);
	}

	/** A bone's transform in component space, from a local pose: its own and each parent's, multiplied up. */
	FTransform PawnUpperComponent(const FCompactPose& Pose, FCompactPoseBoneIndex Bone)
	{
		FTransform Out = Pose[Bone];
		for (FCompactPoseBoneIndex At = Pose.GetParentBoneIndex(Bone); At.IsValid(); At = Pose.GetParentBoneIndex(At))
		{
			Out = Out * Pose[At];
		}
		return Out;
	}
}

void FSimPawnUpperNode::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source->CacheBones_AnyThread(Context);
	const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
	const FReferenceSkeleton& Ref = Bones.GetReferenceSkeleton();
	const int32 Count = Bones.GetCompactPoseNumBones();
	BoneWeights.SetNumZeroed(Count);
	const int32 PelvisRef = Ref.FindBoneIndex(TEXT("pelvis"));
	const int32 SpineRef = Ref.FindBoneIndex(TEXT("spine_01"));
	Pelvis = PelvisRef != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(PelvisRef)) : FCompactPoseBoneIndex(INDEX_NONE);
	Spine = SpineRef != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(SpineRef)) : FCompactPoseBoneIndex(INDEX_NONE);
	if (SpineRef == INDEX_NONE)
	{
		return;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 RefIndex = Bones.MakeMeshPoseIndex(FCompactPoseBoneIndex(Index)).GetInt();
		// Everything under spine_01 takes the stance; spine_01 itself is set in mesh space below.
		for (int32 At = Ref.GetParentIndex(RefIndex); At != INDEX_NONE; At = Ref.GetParentIndex(At))
		{
			if (At == SpineRef)
			{
				BoneWeights[Index] = 1.0f;
				break;
			}
		}
	}
}

void FSimPawnUpperNode::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	Source->Update_AnyThread(Context);
	// The hips are pulled while the body stands on its own legs (walking, standing, or a clip over the walk), not while
	// it jumps, crawls, lies downed or tumbles in a dodge roll.
	const FSimPawnAnimInputs* In = Proxy ? &Proxy->Inputs : nullptr;
	// Not over a standing idle either: its hips stand where they stand over its feet (pulling them shifted the whole
	// body, and all it carries, off its pose); the pull eases out and in over the idle's own blend.
	const float Want = In && In->bHitLayer && (In->bStandTwist || In->bLegsFromClip) && !Proxy->bStandingIdleNow ? 1.0f : 0.0f;
	const float Dt = FMath::Min(Context.GetDeltaTime(), 0.25f);
	HipsWeight = In && In->bTeleported ? Want : PawnApproach(HipsWeight, Want, Dt / PawnStandBlendTime);
	SmoothDt = FMath::Max(0.0f, Context.GetDeltaTime());
	bSmoothReset = bSmoothReset || (In && In->bTeleported);
}

void FSimPawnUpperNode::Evaluate_AnyThread(FPoseContext& Output)
{
	Source->Evaluate_AnyThread(Output);
	const FSimPawnAnimInstanceProxy* P = Proxy;
	if (!P || Spine.GetInt() == INDEX_NONE || Pelvis.GetInt() == INDEX_NONE || BoneWeights.Num() != Output.Pose.GetNumBones())
	{
		return;
	}
	// The locomotion's keys are 1/30 s apart and played straight from one to the next, so every bone's speed changes
	// at each key all at once: at a few hundred frames a second a running foot jolted at every key. Each bone follows
	// its animation with a short exponential ease (PawnKeySmooth), which rounds those corners off; to the eye it is the
	// same motion 1/30 s later. Under the montage slot, so the sim's clips (rolls, staggers, swings) play unchanged.
	{
		const int32 Count = Output.Pose.GetNumBones();
		if (bSmoothReset || Smoothed.Num() != Count || SmoothDt > 0.2f)
		{
			Smoothed.SetNum(Count);
			for (FCompactPoseBoneIndex Bone : Output.Pose.ForEachBoneIndex())
			{
				Smoothed[Bone.GetInt()] = Output.Pose[Bone];
			}
			bSmoothReset = false;
		}
		else if (SmoothDt > 0.0f)
		{
			const float Follow = 1.0f - FMath::Exp(-SmoothDt / PawnKeySmooth);
			for (FCompactPoseBoneIndex Bone : Output.Pose.ForEachBoneIndex())
			{
				FTransform& Eased = Smoothed[Bone.GetInt()];
				const FTransform& Now = Output.Pose[Bone];
				Eased.SetRotation(FQuat::Slerp(Eased.GetRotation(), Now.GetRotation(), Follow).GetNormalized());
				Eased.SetTranslation(FMath::Lerp(Eased.GetTranslation(), Now.GetTranslation(), static_cast<double>(Follow)));
				Eased.SetScale3D(Now.GetScale3D());
				Output.Pose[Bone] = Eased;
			}
		}
	}
	// The hips over the sim's position (PawnHipsReach), on the locomotion's own pose, under the montage slot: a roll or
	// a sidestep then cross-fades from the pulled walk to its own hips, and the clip's set-back (which takes out exactly
	// the clip's travel times its weight) is never fought by the pull while the clip blends in or out.
	const float HipsShow = PawnSmooth(HipsWeight);
	if (HipsShow > 0.0f)
	{
		const FVector At = PawnBonePlace(Output.Pose, Pelvis);
		const double Flat = FVector2D(At.X, At.Y).Size();
		if (Flat > PawnHipsEase)
		{
			const double Span = PawnHipsReach - PawnHipsEase;
			const double Fall = FMath::Exp(-2.0 * (Flat - PawnHipsEase) / Span);
			const double Kept = PawnHipsEase + Span * (1.0 - Fall) / (1.0 + Fall);  // tanh, which starts at slope 1
			const double Pull = (Flat - Kept) / Flat * HipsShow;
			Output.Pose[Pelvis].AddToTranslation(PawnParentRotation(Output.Pose, Pelvis).UnrotateVector(FVector(-At.X * Pull, -At.Y * Pull, 0.0)));
		}
	}
	const float Weight = P->UpperClip ? FMath::Clamp(P->UpperWeight, 0.0f, 1.0f) : 0.0f;
	if (Weight > ZERO_ANIMWEIGHT_THRESH)
	{
		FPoseContext Stance(Output);
		{
			FAnimationPoseData Data(Stance);
			P->UpperClip->GetAnimationPose(Data, FAnimExtractContext(static_cast<double>(PawnUpperWrap(P->UpperClip, P->UpperTime)), false));
		}
		if (P->UpperOldClip && P->UpperBlend < 1.0f)
		{
			FPoseContext Old(Output);
			FAnimationPoseData OldData(Old);
			P->UpperOldClip->GetAnimationPose(OldData, FAnimExtractContext(static_cast<double>(PawnUpperWrap(P->UpperOldClip, P->UpperOldTime)), false));
			for (FCompactPoseBoneIndex Bone : Stance.Pose.ForEachBoneIndex())
			{
				FTransform Mixed = Old.Pose[Bone];
				Mixed.BlendWith(Stance.Pose[Bone], P->UpperBlend);
				Stance.Pose[Bone] = Mixed;
			}
		}
		// spine_01: the clip's chest direction in component space, under the walk's own hips.
		const FQuat LocoPelvis = PawnUpperComponent(Output.Pose, Pelvis).GetRotation();
		const FQuat ClipSpine = PawnUpperComponent(Stance.Pose, Spine).GetRotation();
		const FQuat Wanted = (LocoPelvis.Inverse() * ClipSpine).GetNormalized();
		Output.Pose[Spine].SetRotation(FQuat::Slerp(Output.Pose[Spine].GetRotation(), Wanted, Weight).GetNormalized());
		for (FCompactPoseBoneIndex Bone : Output.Pose.ForEachBoneIndex())
		{
			const float W = BoneWeights[Bone.GetInt()] * Weight;
			if (W > ZERO_ANIMWEIGHT_THRESH)
			{
				FTransform Mixed = Output.Pose[Bone];
				Mixed.BlendWith(Stance.Pose[Bone], W);
				Output.Pose[Bone] = Mixed;
			}
		}
	}
	// The bend: the whole upper body tips forward about spine_01. The mesh faces +Y, so forward is a turn about -X.
	if (FMath::Abs(P->SpineBend) > 0.05f)
	{
		const FQuat PelvisRot = PawnUpperComponent(Output.Pose, Pelvis).GetRotation();
		const FQuat Delta(FVector::XAxisVector, -FMath::DegreesToRadians(P->SpineBend));
		const FQuat Local = Output.Pose[Spine].GetRotation();
		Output.Pose[Spine].SetRotation((PelvisRot.Inverse() * Delta * PelvisRot * Local).GetNormalized());
	}
}

// ---------------------------------------------------------------------------
// The upper-body slot.

const FName USimPawnAnimInstance::UpperBodySlot(TEXT("UpperBody"));

void FSimPawnUpperSlotNode::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source->CacheBones_AnyThread(Context);
	const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
	const FReferenceSkeleton& Ref = Bones.GetReferenceSkeleton();
	const int32 Count = Bones.GetCompactPoseNumBones();
	BoneWeights.SetNumZeroed(Count);
	const int32 PelvisRef = Ref.FindBoneIndex(TEXT("pelvis"));
	const int32 SpineRef = Ref.FindBoneIndex(TEXT("spine_01"));
	Pelvis = PelvisRef != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(PelvisRef)) : FCompactPoseBoneIndex(INDEX_NONE);
	Spine = SpineRef != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(SpineRef)) : FCompactPoseBoneIndex(INDEX_NONE);
	if (SpineRef == INDEX_NONE)
	{
		return;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 RefIndex = Bones.MakeMeshPoseIndex(FCompactPoseBoneIndex(Index)).GetInt();
		for (int32 At = Ref.GetParentIndex(RefIndex); At != INDEX_NONE; At = Ref.GetParentIndex(At))
		{
			if (At == SpineRef)
			{
				BoneWeights[Index] = 1.0f;
				break;
			}
		}
	}
}

void FSimPawnUpperSlotNode::Evaluate_AnyThread(FPoseContext& Output)
{
	Source->Evaluate_AnyThread(Output);
	if (SlotNodeWeight <= ZERO_ANIMWEIGHT_THRESH || Spine.GetInt() == INDEX_NONE || Pelvis.GetInt() == INDEX_NONE ||
		BoneWeights.Num() != Output.Pose.GetNumBones())
	{
		return;
	}
	// The whole-body pose the montage makes over what plays under it (blended in and out by the montage's weight) ...
	FPoseContext Slotted(Output);
	{
		const FAnimationPoseData SourcePose(Output);
		FAnimationPoseData SlottedPose(Slotted);
		Output.AnimInstanceProxy->SlotEvaluatePose(USimPawnAnimInstance::UpperBodySlot, SourcePose, SourceWeight, SlottedPose, SlotNodeWeight, TotalNodeWeight);
	}
	// ... of which the legs and hips are left as they were: spine_01 takes the slotted chest's direction in mesh space
	// under the walk's own hips, and every bone above it the slotted pose.
	const FQuat LocoPelvis = PawnUpperComponent(Output.Pose, Pelvis).GetRotation();
	const FQuat SlottedSpine = PawnUpperComponent(Slotted.Pose, Spine).GetRotation();
	Output.Pose[Spine].SetRotation((LocoPelvis.Inverse() * SlottedSpine).GetNormalized());
	for (FCompactPoseBoneIndex Bone : Output.Pose.ForEachBoneIndex())
	{
		if (BoneWeights[Bone.GetInt()] > 0.0f)
		{
			Output.Pose[Bone] = Slotted.Pose[Bone];
		}
	}
	Output.Curve.Combine(Slotted.Curve);
}

// ---------------------------------------------------------------------------

void FSimPawnLocomotionNode::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	// The default of 4 blends at once was too few for motion matching's jumps: the oldest blend was dropped while it
	// still showed, and the body popped.
	BlendStack.SetMaxActiveBlends(PawnBlendStackMax);  // protected in UE 5.8: set through its setter
	BlendStack.Initialize_AnyThread(Context);
	History.Initialize_AnyThread(PawnHistoryPoses, PawnHistoryPoseInterval);
	Trajectory.Samples.Reset();
	State.Reset();
	bResetHistory = true;
	StandTime = 0.0f;
	StillTime = 0.0f;
	IdleIndex = INDEX_NONE;
	IdleDriftIn = 0.0f;
	Random.Initialize(FMath::Rand());
}

void FSimPawnLocomotionNode::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	BlendStack.CacheBones_AnyThread(Context);
	bCacheBones = true;
}

void FSimPawnLocomotionNode::SetPlayRate(float Want)
{
	// The first clip, or after a teleport, starts at the rate wanted (RateDt is negative then).
	RateShown = RateDt < 0.0f ? Want : RateShown + (Want - RateShown) * (1.0f - FMath::Exp(-RateDt / PawnPlayRateEase));
	BlendStack.UpdatePlayRate(RateShown);
	Stats.PlayRate = RateShown;
}

void FSimPawnLocomotionNode::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	check(Proxy);
	const FSimPawnAnimInputs& In = Proxy->Inputs;
	const float DeltaTime = Context.GetDeltaTime();
	RateDt = In.bTeleported || BlendStack.GetAnimAsset() == nullptr ? -1.0f : FMath::Max(0.0f, DeltaTime);

	if (In.bTeleported)
	{
		Trajectory.Samples.Reset();
		bResetHistory = true;
	}
	if (DeltaTime > 0.0f)
	{
		UpdateTrajectory(In, DeltaTime);
		History.SetTrajectory(Trajectory);
	}

	StandTime = IsStanding(In) ? StandTime + DeltaTime : 0.0f;
	StillTime = In.Velocity.Size2D() < PawnStandSpeed ? StillTime + DeltaTime : 0.0f;
	const bool bStand = StandTime >= PawnStandDelay || StillTime >= PawnStillDelay;
	Stats.bStandingIdle = false;
	if (BlendStack.GetAnimAsset() == nullptr)
	{
		// Something to show from the first frame on, even while paused.
		if (IsStanding(In) && UpdateStandingIdle(Context, 0.0f, 0.0f))
		{
			StandTime = PawnStandDelay;
		}
		else
		{
			UpdateLoops(Context, In, 0.0f);
		}
	}
	else if (DeltaTime > 0.0f)
	{
		if (In.Detail == ESimPawnDetail::Full && !Proxy->Databases.IsEmpty())
		{
			// Standing still, the pawn plays its standing idle with its feet planted. Motion matching over the sample's idles
			// and turn-in-place clips kept picking weight shifts and small steps, so a standing pawn shuffled on the spot
			// (those databases are no longer loaded at all).
			if (bStand && UpdateStandingIdle(Context, PawnStandBlendTime, DeltaTime))
			{
				State.Reset();
				Stats.Database = NAME_None;
			}
			else
			{
				if (IsIdleAsset(BlendStack.GetAnimAsset()))
				{
					State.Reset();  // walking off: search afresh (the idle is in no database)
				}
				UpdateMotionMatching(Context, DeltaTime);
			}
		}
		else
		{
			// Motion matching starts fresh when the pawn comes close again.
			State.Reset();
			Stats.Database = NAME_None;
			UpdateLoops(Context, In, PawnLoopBlendTime);
		}
	}

	BlendStack.Update_AnyThread(Context);
	Proxy->bStandingIdleNow = Stats.bStandingIdle;
	Stats.Detail = In.Detail;
	Stats.PlayingAsset = BlendStack.GetAnimAsset() ? BlendStack.GetAnimAsset()->GetFName() : NAME_None;
}

void FSimPawnLocomotionNode::Evaluate_AnyThread(FPoseContext& Output)
{
	BlendStack.Evaluate_AnyThread(Output);

	// The pose history: what motion matching compares candidate poses against.
	FCSPose<FCompactPose> ComponentSpacePose;
	ComponentSpacePose.InitPose(Output.Pose);
	UE::PoseSearch::FComponentSpacePoseProvider PoseProvider(ComponentSpacePose);
	const TArray<FBoneIndexType> AllBones;
	History.EvaluateComponentSpace_AnyThread(Output.AnimInstanceProxy->GetDeltaSeconds(), PoseProvider, false,
		0.0f, 0.0f, 0.0f, bResetHistory, bCacheBones, AllBones, Output.Curve);
	bResetHistory = false;
	bCacheBones = false;
}

void FSimPawnLocomotionNode::UpdateTrajectory(const FSimPawnAnimInputs& In, float DeltaTime)
{
	FPoseSearchTrajectoryData::FSampling Sampling;
	Sampling.NumHistorySamples = PawnHistorySamples;
	Sampling.SecondsPerHistorySample = PawnHistoryInterval;
	Sampling.NumPredictionSamples = PawnPredictionSamples;
	Sampling.SecondsPerPredictionSample = PawnPredictionInterval;

	const FQuat Facing = PawnMeshFacing(In.Yaw);
	UPoseSearchTrajectoryLibrary::InitTrajectorySamples(Trajectory, In.Location, Facing, Sampling, DeltaTime);

	// The past is where the pawn really was. Its velocity is taken from how far it moved, so the
	// library sees no "moving platform" difference between the two.
	FTransformTrajectorySample& Now = Trajectory.Samples[PawnHistorySamples];
	const FVector Moved = (In.Location - Now.Position) / DeltaTime;
	UPoseSearchTrajectoryLibrary::UpdateHistory_TransformHistory(Trajectory, In.Location, Moved, Sampling, DeltaTime);

	// Sample [history] is now; 0 s is the last frame, as motion matching expects.
	Now.TimeInSeconds = DeltaTime;
	Now.Position = In.Location;
	Now.Facing = Facing;
	PredictAlongRoute(In, DeltaTime);
}

void FSimPawnLocomotionNode::PredictAlongRoute(const FSimPawnAnimInputs& In, float DeltaTime)
{
	// Walk the sim's route the way the sim does: speed up to the cruise speed, brake into the last
	// point, turn to face the way it goes. Without a route the pawn brakes where it is heading.
	const float Accel = FMath::Max(In.CruiseSpeed, 100.0f) * PawnAccelerationPerSpeed;
	FVector Position = In.Location;
	SmoothVelocity = In.bTeleported ? In.Velocity : FMath::Lerp(SmoothVelocity, In.Velocity, FMath::Clamp(DeltaTime / 0.12f, 0.0f, 1.0f));
	float Speed = SmoothVelocity.Size2D();
	// The pace it heads for: the cruise speed while it is still getting going, otherwise the pace it keeps now. Pawns often
	// move slower than their cruise speed (drafted, slow ground, the player's walk), and predicting a speed-up they never
	// make had motion matching play walk-to-run starts over and over while the feet slid.
	const bool bSpeedingUp = Speed < PawnStandSpeed || Speed > RecentSpeed + 0.04f * In.CruiseSpeed;
	const bool bSlowingDown = Speed < RecentSpeed - 0.06f * In.CruiseSpeed;
	const float Pace = bSpeedingUp ? In.CruiseSpeed : FMath::Min(Speed, In.CruiseSpeed * 2.0f);
	RecentSpeed = FMath::Lerp(RecentSpeed, Speed, FMath::Clamp(DeltaTime / 0.25f, 0.0f, 1.0f));
	FVector Heading = Speed > PawnStandSpeed ? SmoothVelocity.GetSafeNormal2D() : FVector::ZeroVector;
	float Yaw = In.Yaw;
	int32 Next = 0;

	float RouteLeft = 0.0f;
	{
		FVector From = Position;
		for (const FVector& Point : In.Route)
		{
			RouteLeft += FVector::Dist2D(From, Point);
			From = Point;
		}
	}

	constexpr int32 SubSteps = 4;
	const float SubDt = PawnPredictionInterval / SubSteps;
	for (int32 Sample = 1; Sample <= PawnPredictionSamples; ++Sample)
	{
		for (int32 Sub = 0; Sub < SubSteps; ++Sub)
		{
			const bool bOnRoute = Next < In.Route.Num();
			// Without a route a moving pawn is the player's (driven by the keys), which keeps going the way it goes; the sim's
			// pawns have braked at their last waypoint by then.
			const float Wanted = bOnRoute ? FMath::Min(Pace, FMath::Sqrt(2.0f * Accel * RouteLeft)) : (In.Route.IsEmpty() && Speed > PawnStandSpeed && !bSlowingDown ? Pace : 0.0f);
			Speed = PawnApproach(Speed, Wanted, Accel * SubDt);
			float Distance = Speed * SubDt;
			if (bOnRoute)
			{
				RouteLeft = FMath::Max(0.0f, RouteLeft - Distance);
				while (Distance > 0.0f && Next < In.Route.Num())
				{
					const FVector ToPoint = In.Route[Next] - Position;
					const float Length = ToPoint.Size2D();
					if (Length > KINDA_SMALL_NUMBER)
					{
						Heading = ToPoint.GetSafeNormal2D();
					}
					if (Length <= Distance)
					{
						Position = In.Route[Next++];
						Distance -= Length;
					}
					else
					{
						Position += ToPoint * (Distance / Length);
						Distance = 0.0f;
					}
				}
				if (Next >= In.Route.Num())
				{
					Speed = 0.0f;
				}
			}
			else
			{
				Position += Heading * Distance;
			}
			if (!Heading.IsNearlyZero() && Speed > PawnStandSpeed && !In.bHoldFacing)
			{
				Yaw = Heading.Rotation().Yaw;
			}
		}
		FTransformTrajectorySample& Out = Trajectory.Samples[PawnHistorySamples + Sample];
		Out.Position = Position;
		Out.Facing = PawnMeshFacing(Yaw);
		Out.TimeInSeconds = DeltaTime + Sample * PawnPredictionInterval;
	}
}

void FSimPawnLocomotionNode::UpdateMotionMatching(const FAnimationUpdateContext& Context, float DeltaTime)
{
	using namespace UE::PoseSearch;

	// Keep the state in step with what the blend stack really plays (its time moves with the play rate).
	State.SearchResult.SelectedAnim = BlendStack.GetAnimAsset();
	State.SearchResult.SelectedTime = BlendStack.GetAccumulatedTime();
	State.SearchResult.bIsMirrored = BlendStack.GetMirror();
	State.SearchResult.BlendParameters = BlendStack.GetBlendParameters();
#if WITH_EDITOR
	// In the editor a database can be re-indexed while running; its old results are not safe then.
	if (State.SearchResult.SelectedDatabase && EAsyncBuildIndexResult::Success != FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(
		State.SearchResult.SelectedDatabase.Get(), ERequestAsyncBuildFlag::ContinueRequest))
	{
		State.Reset();
	}
#endif

	// No new clip within PawnMotionMatchMinGap of the last jump while the clip playing has time left: jumping every few
	// frames stacked blends and twitched the body. The clip just keeps playing, at the rate the speed wants.
	SinceJump += DeltaTime;
	if (SinceJump < PawnMotionMatchMinGap && BlendStack.GetAnimAsset() && !IsIdleAsset(BlendStack.GetAnimAsset()))
	{
		const UAnimSequenceBase* Playing = Cast<UAnimSequenceBase>(BlendStack.GetAnimAsset());
		if (State.SearchResult.bLoop || (Playing && State.SearchResult.SelectedTime < Playing->GetPlayLength() - 0.2f))
		{
			SetPlayRate(State.SearchResult.WantedPlayRate);
			return;
		}
	}
	const FFloatInterval PlayRate(PawnMotionMatchRateMin, PawnMotionMatchRateMax);
	FSearchContext SearchContext(0.0f, FFloatInterval(0.0f, 0.0f), FPoseSearchEvent());
	FChooserEvaluationContext ChooserContext(const_cast<UObject*>(Context.AnimInstanceProxy->GetAnimInstanceObject()));
	SearchContext.AddRole(GetCommonDefaultRole(Proxy->Databases), &ChooserContext, &History);

	TArray<const UObject*, TInlineAllocator<16>> Assets;
	for (const TObjectPtr<const UPoseSearchDatabase>& Database : Proxy->Databases)
	{
		Assets.Add(Database.Get());
	}
	FPoseSearchContinuingProperties Continuing;
	Continuing.InitFrom(State.SearchResult, EPoseSearchInterruptMode::DoNotInterrupt);
	FSearchResults_Single Results;
	// The same call Epic's motion matching node makes; marked experimental in 5.8.
	PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	UPoseSearchLibrary::MotionMatch(SearchContext, MakeArrayView(Assets), Continuing, Results);
	PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
	const FSearchResult Result = Results.GetBestResult();
	const float WantedPlayRate = CalculateWantedPlayRate(Result, SearchContext, PlayRate, History.GetTrajectorySpeedMultiplier(), FPoseSearchEvent());
	const auto Kept = State.SearchResult;
	State.SearchResult.InitFrom(Result, WantedPlayRate);
	++Stats.Searches;

	// Never a turn, pivot or reface clip, nor a run clip at a walking pace (PawnMotionClipBanned): the clip playing goes
	// on if it can, else the plain walk or run loop takes over.
	const FSimPawnAnimInputs& In = Proxy->Inputs;
	const bool bWalkPace = FMath::Max(In.Velocity.Size2D(), In.CruiseSpeed) < 0.5f * (Proxy->WalkLoopSpeed + Proxy->RunLoopSpeed);
	const UObject* Picked = State.SearchResult.SelectedAnim;
	if (PawnMotionClipBanned(Picked, bWalkPace))
	{
		++Stats.BannedPicks;
		State.SearchResult = Kept;
		const UAnimSequenceBase* Playing = Cast<UAnimSequenceBase>(BlendStack.GetAnimAsset());
		if (Playing && !IsIdleAsset(Playing) && !PawnMotionClipBanned(Playing, bWalkPace) &&
			(Kept.bLoop || Kept.SelectedTime < Playing->GetPlayLength() - 0.2f))
		{
			SetPlayRate(Kept.WantedPlayRate);
			return;
		}
		State.Reset();
		Stats.Database = NAME_None;
		UpdateLoops(Context, In, PawnMotionMatchBlendTime);
		SinceJump = 0.0f;
		return;
	}

	const UPoseSearchDatabase* Database = State.SearchResult.SelectedDatabase.Get();
	Stats.Database = Database ? Database->GetFName() : NAME_None;
	if (State.SearchResult.SelectedAnim && !State.SearchResult.bIsContinuingPoseSearch && Database && Database->Schema)
	{
		if (UAnimationAsset* Asset = State.SearchResult.GetAnimationAssetForRole())
		{
			BlendStack.BlendTo(Context, Asset, State.SearchResult.SelectedTime, State.SearchResult.bLoop, State.SearchResult.bIsMirrored,
				Database->Schema->GetMirrorDataTable(State.SearchResult.Role), PawnMotionMatchBlendTime, nullptr,
				EAlphaBlendOption::HermiteCubic, false, NAME_None, State.SearchResult.BlendParameters, WantedPlayRate);
			++Stats.Jumps;
			SinceJump = 0.0f;
		}
	}
	SetPlayRate(State.SearchResult.WantedPlayRate);
}

bool FSimPawnLocomotionNode::IsStanding(const FSimPawnAnimInputs& In)
{
	if (In.Velocity.Size2D() >= PawnStandSpeed)
	{
		return false;
	}
	float RouteLeft = 0.0f;
	FVector From = In.Location;
	for (const FVector& Point : In.Route)
	{
		RouteLeft += FVector::Dist2D(From, Point);
		From = Point;
	}
	return RouteLeft < PawnStandRoute;
}

bool FSimPawnLocomotionNode::IsIdleAsset(const UAnimationAsset* Asset) const
{
	if (!Asset)
	{
		return false;
	}
	for (const UAnimSequence* Idle : Proxy->IdleLoops)
	{
		if (Idle == Asset)
		{
			return true;
		}
	}
	return Asset == Proxy->IdleLoop;
}

int32 FSimPawnLocomotionNode::PickIdle(int32 Feet)
{
	TArray<int32, TInlineAllocator<8>> Choices;
	for (int32 i = 0; i < Proxy->IdleLoops.Num(); ++i)
	{
		if (Proxy->IdleLoops[i] && i != IdleIndex && (Feet < 0 || (Proxy->IdleFeet.IsValidIndex(i) && Proxy->IdleFeet[i] == Feet)))
		{
			Choices.Add(i);
		}
	}
	if (Choices.IsEmpty())
	{
		return Proxy->IdleLoops.IsValidIndex(IdleIndex) && Proxy->IdleLoops[IdleIndex] ? IdleIndex : (Proxy->IdleLoops.IsEmpty() ? INDEX_NONE : 0);
	}
	return Choices[Random.RandRange(0, Choices.Num() - 1)];
}

bool FSimPawnLocomotionNode::UpdateStandingIdle(const FAnimationUpdateContext& Context, float BlendTime, float DeltaTime)
{
	if (Proxy->IdleLoops.IsEmpty())
	{
		return false;
	}
	UAnimationAsset* Playing = BlendStack.GetAnimAsset();
	if (!IsIdleAsset(Playing))
	{
		// Starting to stand: an idle at random (each stop may bring another), from a random point in it so a crowd does
		// not breathe in step.
		IdleIndex = PickIdle(-1);
		UAnimSequence* Idle = Proxy->IdleLoops.IsValidIndex(IdleIndex) ? Proxy->IdleLoops[IdleIndex] : nullptr;
		if (!Idle)
		{
			return false;
		}
		// Started early enough that its blend in is over before its seam blend begins.
		const float Latest = FMath::Max(0.0f, Idle->GetPlayLength() - PawnIdleSeamBlend - BlendTime - 0.1f);
		BlendStack.BlendTo(Context, Idle, Random.FRand() * Latest, true, false, nullptr, BlendTime, nullptr,
			EAlphaBlendOption::HermiteCubic, false, NAME_None, FVector::ZeroVector, 1.0f);
		IdleDriftIn = Random.FRandRange(PawnIdleDriftMin, PawnIdleDriftMax);
		++Stats.IdlesStarted;
	}
	else
	{
		// Standing on: now and then it drifts to another idle (any of them: the feet are held where they stand, so the
		// slow blend moves only the body above them; a foot the new idle wants far off takes a small step).
		const UAnimSequenceBase* Seq = Cast<UAnimSequenceBase>(Playing);
		const float Length = Seq ? Seq->GetPlayLength() : 0.0f;
		const float Left = Length - BlendStack.GetAccumulatedTime();
		if (Length > 2.0f * PawnIdleSeamBlend && Left <= PawnIdleSeamBlend + 0.05f)
		{
			// The seam: into itself from the start; the old copy reaches its end as its weight reaches nought.
			BlendStack.BlendTo(Context, Playing, 0.0f, true, false, nullptr, PawnIdleSeamBlend, nullptr,
				EAlphaBlendOption::HermiteCubic, false, NAME_None, FVector::ZeroVector, 1.0f);
			++Stats.IdleSeams;
		}
		IdleDriftIn -= DeltaTime;
		// A drift waits until the idle playing has its whole blend out left (it must not wrap while it still shows).
		if (IdleDriftIn <= 0.0f && Left > PawnIdleDriftBlendTime + PawnIdleSeamBlend + 0.1f)
		{
			IdleDriftIn = Random.FRandRange(PawnIdleDriftMin, PawnIdleDriftMax);
			const int32 Next = PickIdle(-1);
			if (Next != IdleIndex && Proxy->IdleLoops.IsValidIndex(Next) && Proxy->IdleLoops[Next])
			{
				IdleIndex = Next;
				UAnimSequence* Idle = Proxy->IdleLoops[Next];
				const float Latest = FMath::Max(0.0f, Idle->GetPlayLength() - PawnIdleSeamBlend - PawnIdleDriftBlendTime - 0.1f);
				BlendStack.BlendTo(Context, Idle, Random.FRand() * Latest, true, false, nullptr, PawnIdleDriftBlendTime, nullptr,
					EAlphaBlendOption::HermiteCubic, false, NAME_None, FVector::ZeroVector, 1.0f);
				++Stats.IdleDrifts;
			}
		}
	}
	SetPlayRate(1.0f);
	Stats.bStandingIdle = true;
	return true;
}

void FSimPawnLocomotionNode::UpdateLoops(const FAnimationUpdateContext& Context, const FSimPawnAnimInputs& In, float BlendTime)
{
	const float Speed = In.Velocity.Size2D();
	UAnimSequence* Loop = Proxy->IdleLoops.IsValidIndex(IdleIndex) ? Proxy->IdleLoops[IdleIndex] : Proxy->IdleLoop;
	float Rate = 1.0f;
	if (Speed >= PawnStandSpeed)
	{
		const bool bRun = Speed > 0.5f * (Proxy->WalkLoopSpeed + Proxy->RunLoopSpeed);
		Loop = bRun ? Proxy->RunLoop : Proxy->WalkLoop;
		Rate = FMath::Clamp(Speed / (bRun ? Proxy->RunLoopSpeed : Proxy->WalkLoopSpeed), 0.6f, 1.5f);
	}
	if (!Loop)
	{
		return;
	}
	if (BlendStack.GetAnimAsset() != Loop)
	{
		const bool bIdle = IsIdleAsset(Loop);
		if (bIdle && !Proxy->IdleLoops.IsValidIndex(IdleIndex))
		{
			IdleIndex = Proxy->IdleLoops.IndexOfByKey(Loop);
		}
		BlendStack.BlendTo(Context, Loop, bIdle ? Random.FRand() * Loop->GetPlayLength() : 0.0f, true, false, nullptr, BlendTime, nullptr, EAlphaBlendOption::HermiteCubic,
			false, NAME_None, FVector::ZeroVector, Rate);
	}
	SetPlayRate(Rate);
}

// ---------------------------------------------------------------------------

void FSimPawnHitLayerNode::Forget()
{
	for (FSpring& Spring : Springs)
	{
		Spring = FSpring();
	}
	Walking = 0.0f;
	WeightLeft = 0.5f;
	WeightRight = 0.5f;
	WobbleDegrees = 0.0f;
	LimpNow = 0.0f;
	TwistDegrees = 0.0f;
	WalkLockWeight = 0.0f;
	WalkFeetHeld = 0;
	WalkHeldMask = 0;
	bWantWalkLock = false;
	bWalkHeld[0] = bWalkHeld[1] = false;
	WalkFade[0] = WalkFade[1] = 0.0f;
	WalkYaw[0] = WalkYaw[1] = 0.0f;
	WalkYawGap[0] = WalkYawGap[1] = 0.0f;
	WalkYawGapRate[0] = WalkYawGapRate[1] = 0.0f;
	StandYawLast[0] = StandYawLast[1] = 0.0f;
	bStandYawTake[0] = bStandYawTake[1] = false;
	FootTurn[0] = FootTurn[1] = 0.0f;
	FootTurnRate[0] = FootTurnRate[1] = 0.0f;
	FootTurnWanted[0] = FootTurnWanted[1] = 0.0f;
	bWalkGroundKnown = false;
	bWantFootLock = false;
	bFootLocked = false;
	FootLockWeight = 0.0f;
	StepFoot = INDEX_NONE;
	bSecondStepDue = false;
	FootHold = 0.0f;
	bStepping = false;
}

void FSimPawnHitLayerNode::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source->CacheBones_AnyThread(Context);
	const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
	const auto Find = [&Bones](const TCHAR* Name)
	{
		const int32 MeshBone = Bones.GetPoseBoneIndexForBoneName(FName(Name));
		return MeshBone == INDEX_NONE ? FCompactPoseBoneIndex(INDEX_NONE) : Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBone));
	};
	Pelvis = Find(TEXT("pelvis"));
	SpineLow = Find(TEXT("spine_01"));
	SpineMid = Find(TEXT("spine_02"));
	SpineHigh = Find(TEXT("spine_04"));
	Head = Find(TEXT("head"));
	UpperArm[0] = Find(TEXT("upperarm_l"));
	UpperArm[1] = Find(TEXT("upperarm_r"));
	LowerArm[0] = Find(TEXT("lowerarm_l"));
	LowerArm[1] = Find(TEXT("lowerarm_r"));
	Thigh[0] = Find(TEXT("thigh_l"));
	Thigh[1] = Find(TEXT("thigh_r"));
	Calf[0] = Find(TEXT("calf_l"));
	Calf[1] = Find(TEXT("calf_r"));
	Foot[0] = Find(TEXT("foot_l"));
	Foot[1] = Find(TEXT("foot_r"));
	Ball[0] = Find(TEXT("ball_l"));
	Ball[1] = Find(TEXT("ball_r"));
}

void FSimPawnHitLayerNode::Kick(const FSimPawnKick& Hit)
{
	const int32 Zone = FMath::Clamp(static_cast<int32>(Hit.Zone), 0, static_cast<int32>(ESimPawnZone::Count) - 1);
	// The way the struck place is pushed, flat: a blow straight down bows the body forward.
	FVector Along(Hit.Direction.X, Hit.Direction.Y, 0.0);
	if (!Along.Normalize(0.2))
	{
		Along = FVector::YAxisVector;
	}
	const float Strength = FMath::Clamp(Hit.Strength, 0.0f, 1.0f);
	const auto Push = [this](int32 At, const FVector& Axis, float Peak)
	{
		Springs[At].Rate += Axis * (Peak * PawnKickFrequency / PawnKickPeakShare);
	};
	// Head and torso stand on their joint, so their top goes with the blow; arms and legs hang from theirs, so their
	// far end does.
	const FVector Standing = FVector::CrossProduct(FVector::ZAxisVector, Along);
	const bool bHangs = Zone >= static_cast<int32>(ESimPawnZone::ArmLeft);
	Push(Zone, bHangs ? -Standing : Standing, PawnKickPeak[Zone] * Strength);
	// What is struck drags its neighbours: the head lags behind a struck torso, a struck arm turns the shoulders, a
	// struck leg tips the body over it.
	const int32 TorsoZone = static_cast<int32>(ESimPawnZone::Torso);
	if (Zone == TorsoZone)
	{
		Push(static_cast<int32>(ESimPawnZone::Head), -Standing, 0.4f * PawnKickPeak[0] * Strength);
	}
	else if (Zone == static_cast<int32>(ESimPawnZone::Head))
	{
		Push(TorsoZone, Standing, 0.25f * PawnKickPeak[TorsoZone] * Strength);
	}
	else if (Zone == static_cast<int32>(ESimPawnZone::ArmLeft) || Zone == static_cast<int32>(ESimPawnZone::ArmRight))
	{
		// Turned about the spine, away from the blow: the left shoulder is at +X in the mesh's space.
		const double Side = Zone == static_cast<int32>(ESimPawnZone::ArmLeft) ? 1.0 : -1.0;
		Push(TorsoZone, FVector::ZAxisVector * (Side * FVector::DotProduct(Along, FVector::YAxisVector)), 0.6f * PawnKickPeak[TorsoZone] * Strength);
		Push(TorsoZone, Standing, 0.3f * PawnKickPeak[TorsoZone] * Strength);
	}
	else
	{
		Push(TorsoZone, -Standing, 0.35f * PawnKickPeak[TorsoZone] * Strength);
	}
	++Kicks;
}

void FSimPawnHitLayerNode::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	check(Proxy);
	Source->Update_AnyThread(Context);
	const FSimPawnAnimInputs& In = Proxy->Inputs;
	bOn = In.bHitLayer;
	if (!bOn || In.bTeleported)
	{
		Forget();
	}
	if (bOn)
	{
		for (const FSimPawnKick& Hit : Proxy->PendingKicks)
		{
			Kick(Hit);
		}
	}
	Proxy->PendingKicks.Reset();
	LimpLeft = FMath::Clamp(In.LimpLeft, 0.0f, 1.0f);
	LimpRight = FMath::Clamp(In.LimpRight, 0.0f, 1.0f);
	PatchZone = In.PatchZone;

	const float DeltaTime = FMath::Min(Context.GetDeltaTime(), 0.25f);
	if (DeltaTime <= 0.0f || !bOn)
	{
		return;
	}
	Patch = PawnApproach(Patch, FMath::Clamp(In.Patch, 0.0f, 1.0f), DeltaTime / 0.3f);
	Walking = PawnApproach(Walking, In.Velocity.Size2D() > PawnStandSpeed ? 1.0f : 0.0f, DeltaTime / 0.25f);

	// Standing feet and turns are worked out in Evaluate, where the feet are known.
	bWantFootLock = Proxy->bStandingIdleNow && In.bStandTwist && !In.bLegsFromClip;
	bWantWalkLock = In.bStandTwist && !In.bLegsFromClip;

	// The springs, in steps short enough to stay steady on a slow frame.
	const int32 Steps = FMath::Clamp(FMath::CeilToInt32(DeltaTime / PawnKickStep), 1, 30);
	const float Step = DeltaTime / Steps;
	float Most = 0.0f;
	for (FSpring& Spring : Springs)
	{
		if (Spring.Angle.IsNearlyZero(1.0e-4) && Spring.Rate.IsNearlyZero(1.0e-3))
		{
			Spring = FSpring();
			continue;
		}
		for (int32 i = 0; i < Steps; ++i)
		{
			Spring.Rate += (Spring.Angle * (-PawnKickFrequency * PawnKickFrequency) - Spring.Rate * (2.0f * PawnKickDamping * PawnKickFrequency)) * Step;
			Spring.Angle += Spring.Rate * Step;
		}
		const double Size = Spring.Angle.Size();
		if (Size > PawnKickMaxAngle)
		{
			Spring.Angle *= PawnKickMaxAngle / Size;
			// Stopped at the end of its reach: no speed left outwards.
			const FVector Out = Spring.Angle / PawnKickMaxAngle;
			Spring.Rate -= Out * FMath::Max(0.0, FVector::DotProduct(Spring.Rate, Out));
		}
		Most = FMath::Max(Most, static_cast<float>(Spring.Angle.Size()));
	}
	WobbleDegrees = FMath::RadiansToDegrees(Most);
	MaxWobbleDegrees = FMath::Max(MaxWobbleDegrees, WobbleDegrees);
}

void FSimPawnHitLayerNode::Evaluate_AnyThread(FPoseContext& Output)
{
	Source->Evaluate_AnyThread(Output);
	if (!bOn)
	{
		return;
	}
	FCompactPose& Pose = Output.Pose;
	const FVector Forward = FVector::YAxisVector;
	// The left of the body is at +X in the mesh's space.
	const FVector SideOf[2] = {FVector::XAxisVector, -FVector::XAxisVector};

	// The limp: how much of the weight each hurt leg carries, from which foot is lower in the animation underneath.
	FQuat LimpBody = FQuat::Identity;
	FQuat LimpHead = FQuat::Identity;
	float Dip = 0.0f;
	LimpNow = 0.0f;
	if (Walking > 0.01f && (LimpLeft > 0.01f || LimpRight > 0.01f) && PawnBoneValid(Pose, Foot[0]) && PawnBoneValid(Pose, Foot[1]))
	{
		const float Left = static_cast<float>(PawnBonePlace(Pose, Foot[0]).Z);
		const float Right = static_cast<float>(PawnBonePlace(Pose, Foot[1]).Z);
		const float Blend = FMath::Clamp(Output.AnimInstanceProxy->GetDeltaSeconds() * 14.0f, 0.0f, 1.0f);
		WeightLeft = FMath::Lerp(WeightLeft, FMath::Clamp(0.5f + (Right - Left) / PawnLimpFootSpan, 0.0f, 1.0f), Blend);
		WeightRight = FMath::Lerp(WeightRight, FMath::Clamp(0.5f + (Left - Right) / PawnLimpFootSpan, 0.0f, 1.0f), Blend);
		const float Amount[2] = {LimpLeft * WeightLeft * Walking, LimpRight * WeightRight * Walking};
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Amount[Side] <= 0.001f)
			{
				continue;
			}
			LimpBody = PawnLean(SideOf[Side], PawnLimpSideDegrees * Amount[Side]) * PawnLean(Forward, PawnLimpForwardDegrees * Amount[Side]) * LimpBody;
			LimpHead = PawnLean(-SideOf[Side], PawnLimpHeadDegrees * Amount[Side]) * LimpHead;
			Dip += PawnLimpDipCm * Amount[Side];
			LimpNow = FMath::Max(LimpNow, Amount[Side]);
		}
		MaxLimp = FMath::Max(MaxLimp, LimpNow);
	}

	// Patching: bent over the wound with a hand on it (the left hand for the right arm, else the right).
	FQuat PatchBody = FQuat::Identity;
	FQuat PatchHead = FQuat::Identity;
	FQuat PatchUpper = FQuat::Identity;
	FQuat PatchLower = FQuat::Identity;
	int32 PatchArm = 1;
	if (Patch > 0.01f)
	{
		// Smooth in and out.
		const float P = Patch * Patch * (3.0f - 2.0f * Patch);
		float Bend = 12.0f, Nod = 16.0f, Raise = 35.0f, Inward = 25.0f, Fold = 80.0f;
		switch (PatchZone)
		{
			case ESimPawnZone::Head:
				Bend = 5.0f; Nod = 8.0f; Raise = 95.0f; Inward = 15.0f; Fold = 115.0f;
				break;
			case ESimPawnZone::ArmLeft:
			case ESimPawnZone::ArmRight:
				Bend = 10.0f; Nod = 18.0f; Raise = 30.0f; Inward = 45.0f; Fold = 90.0f;
				PatchArm = PatchZone == ESimPawnZone::ArmRight ? 0 : 1;
				break;
			case ESimPawnZone::LegLeft:
			case ESimPawnZone::LegRight:
				Bend = 32.0f; Nod = 14.0f; Raise = 22.0f; Inward = PatchZone == ESimPawnZone::LegRight ? 0.0f : 22.0f; Fold = 15.0f;
				break;
			default:
				break;
		}
		PatchBody = PawnLean(Forward, Bend * P);
		PatchHead = PawnLean(Forward, Nod * P);
		// The arm hangs, so its far end goes the way opposite to a lean: forward and across the body.
		const FVector Across = -SideOf[PatchArm];
		PatchUpper = PawnLean(-Across, Inward * P) * PawnLean(-Forward, Raise * P);
		PatchLower = PawnLean(-(Across + Forward * 0.35).GetSafeNormal(), Fold * P);
	}

	// Standing feet (docs/design/pawns.md "Standing still"), before anything else bends the legs.
	const float Dt = FMath::Min(Output.AnimInstanceProxy->GetDeltaSeconds(), 0.25f);
	const FTransform& ToWorld = Output.AnimInstanceProxy->GetComponentTransform();
	const bool bLegs = PawnBoneValid(Pose, Pelvis) && PawnBoneValid(Pose, Thigh[0]) && PawnBoneValid(Pose, Thigh[1]) && PawnBoneValid(Pose, Calf[0]) &&
		PawnBoneValid(Pose, Calf[1]) && PawnBoneValid(Pose, Foot[0]) && PawnBoneValid(Pose, Foot[1]);
	const float BodyYaw = Proxy->Inputs.Yaw;
	WalkLockWeight = PawnApproach(WalkLockWeight, bWantWalkLock && bLegs ? 1.0f : 0.0f, Dt / PawnWalkLockBlend);
	const float WalkShow = PawnSmooth(WalkLockWeight);
	// (The hips were pulled over the sim's position already, on the locomotion's pose in the hold layer.)
	// Where the animation puts each foot for the body's facing now (world): where a step goes. Its ball too, and the
	// way the foot faces (ankle to ball).
	const bool bToes = bLegs && PawnBoneValid(Pose, Ball[0]) && PawnBoneValid(Pose, Ball[1]);
	FVector Wanted[2] = {FVector::ZeroVector, FVector::ZeroVector};
	FVector WantedBall[2] = {FVector::ZeroVector, FVector::ZeroVector};
	float WantedYaw[2] = {0.0f, 0.0f};
	if (bLegs)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			Wanted[Side] = ToWorld.TransformPosition(PawnBonePlace(Pose, Foot[Side]));
			if (bToes)
			{
				WantedBall[Side] = ToWorld.TransformPosition(PawnBonePlace(Pose, Ball[Side]));
				WantedYaw[Side] = PawnFootYaw(Wanted[Side], WantedBall[Side]);
			}
		}
	}
	// Walking feet: each foot down in the animation (its heel or its ball) is held where it came down, facing the way
	// it came down (PawnWalkLock*, PawnWalkYawMax). WalkTarget is where each ankle goes (world), Shown where it is
	// drawn with the lock's weight.
	FVector WalkTarget[2] = {Wanted[0], Wanted[1]};
	FVector Shown[2] = {Wanted[0], Wanted[1]};
	WalkFeetHeld = 0;
	WalkHeldMask = 0;
	if (!bLegs || WalkLockWeight <= 0.0f)
	{
		bWalkHeld[0] = bWalkHeld[1] = false;
		WalkFade[0] = WalkFade[1] = 0.0f;
		WalkYaw[0] = WalkYaw[1] = 0.0f;
		WalkYawGap[0] = WalkYawGap[1] = 0.0f;
		WalkYawGapRate[0] = WalkYawGapRate[1] = 0.0f;
		bWalkGroundKnown = false;
	}
	else
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Z = static_cast<float>(PawnBonePlace(Pose, Foot[Side]).Z);
			WalkGround[Side] = bWalkGroundKnown ? FMath::Min(WalkGround[Side] + PawnWalkLockGroundRise * Dt, Z) : Z;
			const bool bHeelDown = Z <= WalkGround[Side] + PawnWalkLockContact;
			bool bToeDown = false;
			if (bToes)
			{
				const float BallZ = static_cast<float>(PawnBonePlace(Pose, Ball[Side]).Z);
				WalkBallGround[Side] = bWalkGroundKnown ? FMath::Min(WalkBallGround[Side] + PawnWalkLockGroundRise * Dt, BallZ) : BallZ;
				bToeDown = BallZ <= WalkBallGround[Side] + PawnWalkLockContact;
			}
			FVector WantedVel = bWalkGroundKnown && Dt > 0.0f ? (Wanted[Side] - WalkLastWanted[Side]) / static_cast<double>(Dt) : FVector::ZeroVector;
			WantedVel.Z = 0.0;
			const bool bDown = (bHeelDown || bToeDown) && (bWalkHeld[Side] || WantedVel.Size2D() < PawnWalkLockLandSpeed);
			WalkLastWanted[Side] = Wanted[Side];
			const float WantedYawRate = bToes && bWalkGroundKnown && Dt > 0.0f ? FMath::FindDeltaAngleDegrees(WalkLastYaw[Side], WantedYaw[Side]) / Dt : 0.0f;
			WalkLastYaw[Side] = WantedYaw[Side];
			FVector Offset, Rate;
			PawnWalkRelease(WalkGap[Side], WalkGapVel[Side], WalkFade[Side], Offset, Rate);
			float YawOffset = 0.0f, YawRate = 0.0f;
			PawnWalkYawRelease(WalkYawGap[Side], WalkYawGapRate[Side], WalkFade[Side], YawOffset, YawRate);
			bool bLanded = false;
			if (bWalkHeld[Side] && !bDown)
			{
				// Lifted: let go from where it is, at the speed it has, the offset closing to nought; the facing the same.
				FVector Gap = WalkPlant[Side] - Wanted[Side];
				Gap.Z = 0.0;
				WalkGap[Side] = Gap;
				WalkGapVel[Side] = (WalkHeldVel[Side] - WantedVel).GetClampedToMaxSize(PawnWalkLockMaxSpeed);
				WalkYawGap[Side] = WalkYaw[Side];
				WalkYawGapRate[Side] = FMath::Clamp(-WantedYawRate, -PawnWalkYawMaxRate, PawnWalkYawMaxRate);
				WalkFade[Side] = 1.0f;
				bWalkHeld[Side] = false;
				PawnWalkRelease(WalkGap[Side], WalkGapVel[Side], WalkFade[Side], Offset, Rate);
				PawnWalkYawRelease(WalkYawGap[Side], WalkYawGapRate[Side], WalkFade[Side], YawOffset, YawRate);
			}
			else if (!bWalkHeld[Side] && bDown)
			{
				// Down: held from where it is drawn now, keeping the speed it came down at for a moment, facing the way
				// it is drawn now.
				WalkPlant[Side] = Wanted[Side] + Offset;
				WalkVel[Side] = (WantedVel + Rate).GetClampedToMaxSize(PawnWalkLockMaxSpeed);
				WalkVel[Side].Z = 0.0;
				WalkFollow[Side] = FVector::ZeroVector;
				WalkYawHeld[Side] = FRotator::NormalizeAxis(WantedYaw[Side] + YawOffset);
				WalkFade[Side] = 0.0f;
				bWalkHeld[Side] = true;
				bLanded = true;
			}
			if (bWalkHeld[Side])
			{
				const FVector PlantBefore = WalkPlant[Side];
				if (bToes)
				{
					// The facing held, within its reach (past it the held facing comes round with the animation's).
					const float Off = FMath::FindDeltaAngleDegrees(WantedYaw[Side], WalkYawHeld[Side]);
					WalkYaw[Side] = FMath::Clamp(Off, -PawnWalkYawMax, PawnWalkYawMax);
					if (WalkYaw[Side] != Off)
					{
						WalkYawHeld[Side] = FRotator::NormalizeAxis(WantedYaw[Side] + WalkYaw[Side]);
					}
					// On the ball alone the ball stays put: the ankle moves as the ball's place from it turns.
					FVector Toe = FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(WalkYaw[Side])).RotateVector(WantedBall[Side] - Wanted[Side]);
					Toe.Z = 0.0;
					if (!bLanded && bToeDown && !bHeelDown)
					{
						WalkPlant[Side] -= Toe - WalkToeLast[Side];
					}
					WalkToeLast[Side] = Toe;
				}
				WalkVel[Side] *= static_cast<double>(FMath::Exp(-Dt / PawnWalkLockSettle));
				FVector Gap = WalkPlant[Side] - Wanted[Side];
				Gap.Z = 0.0;
				const double Size = Gap.Size2D();
				WalkFollow[Side] = Size > PawnWalkLockSoftOff ? Gap * (-(Size - PawnWalkLockSoftOff) * PawnWalkLockFollowRate / Size) : FVector::ZeroVector;
				WalkPlant[Side] += (WalkVel[Side] + WalkFollow[Side]) * static_cast<double>(Dt);
				Gap = WalkPlant[Side] - Wanted[Side];
				Gap.Z = 0.0;
				if (Gap.Size2D() > PawnWalkLockMaxOff)
				{
					WalkPlant[Side] = Wanted[Side] + Gap * (PawnWalkLockMaxOff / Gap.Size2D());
				}
				WalkTarget[Side] = FVector(WalkPlant[Side].X, WalkPlant[Side].Y, Wanted[Side].Z);
				// How fast the held ankle really moves (the landing's speed dying away, a follow, the swing over the
				// ball): a lift lets go at exactly that speed.
				WalkHeldVel[Side] = bLanded || Dt <= 0.0f ? WalkVel[Side] : (WalkPlant[Side] - PlantBefore) / static_cast<double>(Dt);
				WalkHeldVel[Side].Z = 0.0;
			}
			else
			{
				WalkFade[Side] = FMath::Max(0.0f, WalkFade[Side] - Dt / PawnWalkLockRelease);
				PawnWalkRelease(WalkGap[Side], WalkGapVel[Side], WalkFade[Side], Offset, Rate);
				PawnWalkYawRelease(WalkYawGap[Side], WalkYawGapRate[Side], WalkFade[Side], YawOffset, YawRate);
				WalkTarget[Side] = Wanted[Side] + Offset;
				WalkYaw[Side] = bToes ? YawOffset : 0.0f;
			}
			Shown[Side] = Wanted[Side] + (WalkTarget[Side] - Wanted[Side]) * WalkShow;
			WalkFeetHeld += bWalkHeld[Side] ? 1 : 0;
			WalkHeldMask |= bWalkHeld[Side] ? (1 << Side) : 0;
		}
		bWalkGroundKnown = true;
	}

	// How much the standing hold shows: eased, so neither taking hold nor letting go starts or stops a foot at once.
	float HoldShow = bFootLocked ? PawnSmooth(FootLockWeight) : 0.0f;
	bool bWant = bWantFootLock && bLegs;
	if (bFootLocked && FVector::Dist2D(ToWorld.GetLocation(), FootLockCentre) > PawnFootLockDrift)
	{
		bWant = false;  // the body has crept off its spot without the walk taking over: let go, take hold again after
	}
	if (bWant && (!bFootLocked || FootLockWeight < 1.0f))
	{
		// Take hold where the feet are drawn now (part way out of a hold that was letting go, the blend of the two),
		// facing the body's way: nothing moves this frame.
		for (int32 Side = 0; Side < 2; ++Side)
		{
			FootPlant[Side] = bFootLocked ? FMath::Lerp(Shown[Side], FootPlant[Side], static_cast<double>(HoldShow)) : Shown[Side];
		}
		FeetYaw = bFootLocked ? FRotator::NormalizeAxis(BodyYaw - FMath::Clamp(FMath::FindDeltaAngleDegrees(FeetYaw, BodyYaw), -PawnStandTwistMax,
			PawnStandTwistMax) * HoldShow) : BodyYaw;
		bFootLocked = true;
		FootLockWeight = 1.0f;
		FootLockCentre = ToWorld.GetLocation();
		StepFoot = INDEX_NONE;
		bStandYawTake[0] = bStandYawTake[1] = true;  // the facing each foot is drawn with now, in the legs below
		bSecondStepDue = false;
	}
	else if (!bWant && bFootLocked)
	{
		// Leaving: the twist and the hold fade out together, the steps stop where they are.
		FootLockWeight = FMath::Max(0.0f, FootLockWeight - Dt / PawnStandRelease);
		StepFoot = INDEX_NONE;
		if (FootLockWeight <= 0.0f)
		{
			bFootLocked = false;
		}
	}
	if (bFootLocked && bWant)
	{
		float Off = FMath::FindDeltaAngleDegrees(FeetYaw, BodyYaw);
		if (StepFoot == INDEX_NONE)
		{
			double Far0 = FVector::Dist2D(FootPlant[0], Wanted[0]);
			double Far1 = FVector::Dist2D(FootPlant[1], Wanted[1]);
			// A held foot the leg can hardly reach any more (a lunge, a push, the hips swaying away) steps before the
			// stretched leg would drag it: it goes first.
			bool bOverReach = false;
			for (int32 Side = 0; Side < 2; ++Side)
			{
				const FVector Hip = PawnBonePlace(Pose, Thigh[Side]);
				const FVector Knee = PawnBonePlace(Pose, Calf[Side]);
				const FVector Ankle = PawnBonePlace(Pose, Foot[Side]);
				const double Leg = (Knee - Hip).Size() + (Ankle - Knee).Size();
				// Not past the animation's own reach (a standing idle's legs are nearly straight), but before the leg is.
				const double Reach = FMath::Max(PawnStandReachShare * Leg, FMath::Min((Ankle - Hip).Size() + 2.0, Leg - 0.5));
				if (FVector::Dist(ToWorld.InverseTransformPosition(FootPlant[Side]), Hip) > Reach)
				{
					bOverReach = true;
					(Side == 0 ? Far0 : Far1) += 1000.0;
				}
			}
			const bool bTurn = FMath::Abs(Off) > PawnStandStepAt || (bSecondStepDue && FMath::Abs(Off) > PawnStandStepAgain);
			if (bTurn || bOverReach || FMath::Max(Far0, Far1) > PawnStandStepFar || (bSecondStepDue && FMath::Max(Far0, Far1) > 8.0))
			{
				// The foot furthest from where it should be goes first; the other follows.
				StepFoot = Far1 > Far0 ? 1 : 0;
				StepTime = 0.0f;
				StepFrom = FootPlant[StepFoot];
				StandYawFrom[StepFoot] = StandYawHeld[StepFoot];
				StepYawFrom = FeetYaw;
				bFirstStep = !bSecondStepDue;
				bSecondStepDue = bFirstStep;
				++FootSteps;
			}
			else
			{
				bSecondStepDue = false;
			}
		}
		if (StepFoot != INDEX_NONE)
		{
			StepTime += Dt;
			const float T = FMath::Clamp(StepTime / PawnStandStepTime, 0.0f, 1.0f);
			const float Ease = T * T * (3.0f - 2.0f * T);
			FootPlant[StepFoot] = FMath::Lerp(StepFrom, Wanted[StepFoot], static_cast<double>(Ease)) +
				FVector(0.0, 0.0, PawnStandStepLift * FMath::Sin(UE_PI * T));
			// The feet's facing comes round with the steps: half with the first, the rest with the second.
			const float Share = bFirstStep ? 0.5f : 1.0f;
			FeetYaw = FRotator::NormalizeAxis(StepYawFrom + FMath::FindDeltaAngleDegrees(StepYawFrom, BodyYaw) * Share * Ease);
			if (T >= 1.0f)
			{
				FootPlant[StepFoot] = Wanted[StepFoot];
				bStandYawTake[StepFoot] = true;  // landed: held facing the way it is drawn
				StepFoot = INDEX_NONE;
			}
		}
		// Never more twist than the waist has.
		Off = FMath::FindDeltaAngleDegrees(FeetYaw, BodyYaw);
		if (FMath::Abs(Off) > PawnStandTwistMax)
		{
			FeetYaw = FRotator::NormalizeAxis(BodyYaw - FMath::Sign(Off) * PawnStandTwistMax);
		}
	}
	// Leaving, the body may turn away fast while the hold fades: the twist stays within the waist's reach then too.
	HoldShow = bFootLocked ? PawnSmooth(FootLockWeight) : 0.0f;
	const float Twist = bFootLocked ? FMath::Clamp(FMath::FindDeltaAngleDegrees(FeetYaw, BodyYaw), -PawnStandTwistMax, PawnStandTwistMax) * HoldShow : 0.0f;
	TwistDegrees = Twist;
	MaxTwistDegrees = FMath::Max(MaxTwistDegrees, FMath::Abs(Twist));
	FootHold = bFootLocked ? FootLockWeight : 0.0f;
	bStepping = StepFoot != INDEX_NONE;

	// The twist: the hips turn back by it, legs and all (the legs face the feet's way; the actor, and with it the mesh,
	// faces the body's), and spine_01, spine_02, spine_04 and the head turn it forward again bit by bit, so the chest and
	// head face the body's way. The pelvis, not the root, since the bodies' retargeters carry the pelvis over and not
	// the root. In the mesh's space a turn about +Z goes the way a growing actor yaw does.
	FQuat TwistAt[4] = {FQuat::Identity, FQuat::Identity, FQuat::Identity, FQuat::Identity};
	if (FMath::Abs(Twist) > 0.05f && PawnBoneValid(Pose, Pelvis))
	{
		const float Radians = FMath::DegreesToRadians(Twist);
		PawnTurnBone(Pose, Pelvis, FQuat(FVector::ZAxisVector, -Radians));
		for (int32 i = 0; i < 4; ++i)
		{
			TwistAt[i] = FQuat(FVector::ZAxisVector, Radians * PawnTwistShare[i]);
		}
	}

	// Parents before children: each turn is about the bone's own joint, on top of the turns above it.
	if (Dip > 0.0f && PawnBoneValid(Pose, Pelvis))
	{
		Pose[Pelvis].AddToTranslation(PawnParentRotation(Pose, Pelvis).UnrotateVector(FVector(0.0, 0.0, -Dip)));
	}
	PawnTurnBone(Pose, SpineLow, TwistAt[0] * PatchBody * LimpBody);
	const FQuat Torso = PawnRotationOf(Springs[static_cast<int32>(ESimPawnZone::Torso)].Angle * 0.5);
	PawnTurnBone(Pose, SpineMid, TwistAt[1] * Torso);
	PawnTurnBone(Pose, PawnBoneValid(Pose, SpineHigh) ? SpineHigh : SpineMid, TwistAt[2] * Torso);
	PawnTurnBone(Pose, Head, TwistAt[3] * PawnRotationOf(Springs[static_cast<int32>(ESimPawnZone::Head)].Angle) * PatchHead * LimpHead);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const bool bPatching = Patch > 0.01f && Side == PatchArm;
		PawnTurnBone(Pose, UpperArm[Side], PawnRotationOf(Springs[static_cast<int32>(ESimPawnZone::ArmLeft) + Side].Angle) * (bPatching ? PatchUpper : FQuat::Identity));
		if (bPatching)
		{
			PawnTurnBone(Pose, LowerArm[Side], PatchLower);
		}
		PawnTurnBone(Pose, Thigh[Side], PawnRotationOf(Springs[static_cast<int32>(ESimPawnZone::LegLeft) + Side].Angle));
	}

	// Planted feet, last, over everything above: each foot held at its place (or on its way in a step), the leg bent to
	// reach it, blended with the animation's foot by how much the hold shows.
	// The walking hold goes on as a gap from where the animation had the foot (so the twist and the springs above
	// still show), the standing hold over it.
	if (bLegs && (WalkShow > 0.0f || HoldShow > 0.0f))
	{
		const float StepEase = StepFoot != INDEX_NONE ? PawnSmooth(StepTime / PawnStandStepTime) : 0.0f;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FVector Animated = PawnBonePlace(Pose, Foot[Side]);
			FVector Target = Animated;
			if (WalkShow > 0.0f)
			{
				FVector Gap = ToWorld.InverseTransformPosition(WalkTarget[Side]) - ToWorld.InverseTransformPosition(Wanted[Side]);
				Gap.Z = 0.0;
				Target += Gap * static_cast<double>(WalkShow);
			}
			if (HoldShow > 0.0f)
			{
				Target = FMath::Lerp(Target, ToWorld.InverseTransformPosition(FootPlant[Side]), static_cast<double>(HoldShow));
			}
			if (!Target.Equals(Animated, 0.01))
			{
				PawnReachLeg(Pose, Thigh[Side], Calf[Side], Foot[Side], Target);
			}
			// The facing: the walking hold's turn off the animation, and the standing hold's facing over it; the foot
			// turns about its ankle, so the planted ball stays put too.
			if (!bToes)
			{
				continue;
			}
			const FVector Way = ToWorld.TransformVectorNoScale(PawnBonePlace(Pose, Ball[Side]) - PawnBonePlace(Pose, Foot[Side]));
			const float WalkTurn = WalkYaw[Side] * WalkShow;
			const float Drawn = PawnFootYaw(FVector::ZeroVector, Way) + WalkTurn;
			if (bStandYawTake[Side])
			{
				StandYawHeld[Side] = FRotator::NormalizeAxis(Drawn + StandYawLast[Side]);
				bStandYawTake[Side] = false;
				if (StepFoot == Side)
				{
					StandYawFrom[Side] = StandYawHeld[Side];  // a step that began this very frame starts from it too
				}
			}
			float StandTurn = 0.0f;
			if (bFootLocked && HoldShow > 0.0f)
			{
				float Want = StandYawHeld[Side];
				if (StepFoot == Side)
				{
					Want = StandYawFrom[Side] + FMath::FindDeltaAngleDegrees(StandYawFrom[Side], Drawn) * StepEase;
				}
				StandTurn = FMath::Clamp(FMath::FindDeltaAngleDegrees(Drawn, Want), -PawnStandYawMax, PawnStandYawMax) * HoldShow;
			}
			StandYawLast[Side] = StandTurn;
			// The turn put on follows the one wanted as a stiff damped spring (PawnFootTurnRate): the holds change it
			// smoothly anyway, but no hand-over between them (a hold taken, a step, a lock fading) can then spin the
			// toes round in a frame.
			// The wanted turn's own speed is fed forward (so a body turning steadily over a held foot is followed without
			// lag: a lag slid the toes), up to PawnFootTurnFeed: a jump in it is still eased.
			const float Want = FMath::Clamp(WalkTurn + StandTurn, -PawnStandYawMax, PawnStandYawMax);
			const float WantRate = Dt > 0.0f ? FMath::Clamp((Want - FootTurnWanted[Side]) / Dt, -PawnFootTurnFeed, PawnFootTurnFeed) : 0.0f;
			FootTurnWanted[Side] = Want;
			const int32 TurnSteps = FMath::Clamp(FMath::CeilToInt32(Dt * 240.0f), 1, 60);
			for (int32 i = 0; i < TurnSteps; ++i)
			{
				const float H = Dt / TurnSteps;
				FootTurnRate[Side] += (PawnFootTurnRate * PawnFootTurnRate * (Want - FootTurn[Side]) + 2.0f * PawnFootTurnRate * (WantRate - FootTurnRate[Side])) * H;
				FootTurn[Side] += FootTurnRate[Side] * H;
			}
			if (FMath::Abs(FootTurn[Side]) > 0.01f)
			{
				PawnTurnBone(Pose, Foot[Side], FQuat(FVector::ZAxisVector, FMath::DegreesToRadians(FootTurn[Side])));
			}
		}
	}
	else
	{
		StandYawLast[0] = StandYawLast[1] = 0.0f;
		bStandYawTake[0] = bStandYawTake[1] = false;
		FootTurn[0] = FootTurn[1] = 0.0f;
		FootTurnRate[0] = FootTurnRate[1] = 0.0f;
		FootTurnWanted[0] = FootTurnWanted[1] = 0.0f;
	}
}

// ---------------------------------------------------------------------------

USimPawnAnimInstance::USimPawnAnimInstance()
{
	// The sim moves the pawn: animations play in place (root motion is taken out and dropped).
	RootMotionMode = ERootMotionMode::IgnoreRootMotion;
}

void USimPawnAnimInstance::SetAssets(const TArray<TObjectPtr<const UPoseSearchDatabase>>& InDatabases, const TArray<TObjectPtr<UAnimSequence>>& InIdles,
	const TArray<int32>& InIdleFeet, UAnimSequence* InWalk, float InWalkSpeed, UAnimSequence* InRun, float InRunSpeed)
{
	Databases = InDatabases;
	IdleLoops = InIdles;
	IdleFeet = InIdleFeet;
	IdleLoop = IdleLoops.IsEmpty() ? nullptr : IdleLoops[0].Get();
	WalkLoop = InWalk;
	RunLoop = InRun;
	WalkLoopSpeed = FMath::Max(InWalkSpeed, 50.0f);
	RunLoopSpeed = FMath::Max(InRunSpeed, WalkLoopSpeed + 50.0f);
}

FAnimInstanceProxy* USimPawnAnimInstance::CreateAnimInstanceProxy()
{
	return new FSimPawnAnimInstanceProxy(this);
}

void USimPawnAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FSimPawnAnimInstanceProxy*>(InProxy);
}

void USimPawnAnimInstance::SetUpperStance(UAnimSequence* Clip, float Weight, float BlendSeconds, float WeightRate)
{
	UpperWeightTarget = FMath::Clamp(Weight, 0.0f, 1.0f);
	UpperWeightRate = FMath::Max(WeightRate, 0.1f);
	if (Clip == UpperClip || !Clip)
	{
		// No clip: keep the last one while the weight fades out.
		if (!Clip)
		{
			UpperWeightTarget = 0.0f;
		}
		return;
	}
	if (!UpperClip || UpperWeight <= 0.01f)
	{
		// Nothing showing: start straight on the new clip.
		UpperClip = Clip;
		UpperTime = 0.0f;
		UpperBlend = 1.0f;
		UpperOldClip = nullptr;
		return;
	}
	UpperOldClip = UpperClip;
	UpperOldTime = UpperTime;
	UpperClip = Clip;
	UpperTime = 0.0f;
	UpperBlend = 0.0f;
	UpperBlendRate = BlendSeconds > 0.0f ? 1.0f / BlendSeconds : 1000.0f;
}

void USimPawnAnimInstance::AdvanceUpper(float DeltaSeconds)
{
	const float Dt = FMath::Max(DeltaSeconds, 0.0f);
	UpperTime += Dt;
	UpperOldTime += Dt;
	UpperBlend = FMath::Min(1.0f, UpperBlend + Dt * UpperBlendRate);
	if (UpperBlend >= 1.0f)
	{
		UpperOldClip = nullptr;
	}
	UpperWeight = FMath::FInterpConstantTo(UpperWeight, UpperWeightTarget, Dt, UpperWeightRate);
	SpineBend = FMath::FInterpConstantTo(SpineBend, SpineBendTarget, Dt, 60.0f);
}
