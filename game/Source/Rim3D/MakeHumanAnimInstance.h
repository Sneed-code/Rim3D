#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeSpaceConversions.h"
#include "AnimNodes/AnimNode_RetargetPoseFromMesh.h"
#include "BoneControllers/AnimNode_AnimDynamics.h"
#include "BoneControllers/AnimNode_TwoBoneIK.h"
#include "ColonySim/HairStrands.h"

#include "ColonySim/ArmPose.h"

#include "MakeHumanAnimInstance.generated.h"

class UIKRetargeter;

/** A push on a body's soft parts (docs/design/soft-body.md): a hit (a velocity at a point, fading out over Radius) or a
 *  blast (bRadial: Speed away from At, fading over Radius). Parts nearer the point get more of it. */
struct FMakeHumanKick
{
	FVector At = FVector::ZeroVector;        // world
	FVector Velocity = FVector::ZeroVector;  // cm/s, a hit's
	float Speed = 0.0f;                      // cm/s, a blast's (away from At)
	float Radius = 40.0f;                    // cm
	bool bRadial = false;
};

/**
 * Soft tissue (breasts, buttocks, belly, thighs, upper arms): each listed bone's point follows where the animation puts it through a damped
 * spring in world space, so it lags behind starts, stops, steps and turns and bounces back, within MaxOffset.
 */
USTRUCT()
struct FMakeHumanJiggleNode : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	struct FBody
	{
		FBoneReference Bone;
		float Stiffness = 160.0f;  // 1/s^2
		float Damping = 7.0f;      // 1/s
		float MaxOffset = 2.5f;    // cm
		// As set for the part (and the breast slider); Stiffness and MaxOffset are these scaled by the body's softness.
		float BaseStiffness = 160.0f;
		float BaseMaxOffset = 2.5f;
		bool bBodySoft = false;    // scaled by the body's softness (belly, thighs, arms, buttocks; not breasts or genitals)
		// Squash and stretch: pushed in, the part flattens along the line out from its parent bone and spreads sideways;
		// pulled out, it stretches and narrows (volume kept roughly). 0 none; the share of the scale change at full room.
		float Squash = 0.0f;
		FVector Position = FVector::ZeroVector;  // world, simulated
		FVector Velocity = FVector::ZeroVector;
		bool bStarted = false;
	};
	TArray<FBody> Bodies;
	/** Kicks waiting for the next evaluation (a hit, a blast), handed over from the game thread in PreUpdate. */
	TArray<FMakeHumanKick> Kicks;
	float DeltaTime = 0.0f;
	float LargestOffset = 0.0f;  // cm, since the last read (tests)
	bool bEnabled = true;        // off: the soft parts stay as animated (the creator's "keep still")
	float Fade = 1.0f;           // 1 near the camera .. 0 beyond Pawns.SoftBodyFar (off: as animated)

	virtual void UpdateInternal(const FAnimationUpdateContext& Context) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
};

/**
 * A tail's motion (the tail_01 ... chain): an idle swish from side to side, quicker and wider when moving, and a swing
 * the other way when the body turns, bent a little more at each bone towards the tip. No physics: a tail is muscle.
 */
USTRUCT()
struct FMakeHumanTailNode : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	TArray<FBoneReference> Bones;
	float DeltaTime = 0.0f;
	float Time = 0.0f;
	float TurnSwing = 0.0f;          // degrees, smoothed
	float Speed = 0.0f;              // cm/s, smoothed
	FTransform LastComponent;
	bool bStarted = false;
	float LargestAngle = 0.0f;       // degrees at the tip since the last read (tests)

	virtual void UpdateInternal(const FAnimationUpdateContext& Context) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
};

/** A gun held up in both hands, for long hair (docs/design/hair-physics.md, "Aiming"): the hair rests on the gun and
 *  the forearms instead of passing through them, and while it is aimed the hair goes over the other shoulder. */
struct FMakeHumanHeldGun
{
	bool bOn = false;
	bool bRightHand = true;                     // the hand on the grip (its shoulder takes the stock)
	FTransform GunInHand;                       // the gun, with its size, in the main hand bone's space (world cm)
	FVector Rear = FVector(-25.0, 0.0, 2.0);    // the butt and the muzzle in the gun's own frame (cm, before its size)
	FVector Front = FVector(45.0, 0.0, 3.0);
	float Radius = 3.5f;                        // cm, before its size
	float Aim = 0.0f;                           // 0 at the ready .. 1 aimed
};

/**
 * Long hair (docs/design/hair-physics.md): the long, bob and braid styles' chains (mh_common.add_hair_bones: hair_c1_1 ...
 * under the head) and a ponytail's (ponytail1 ...), as strands of colony::HairStrands. Each point falls under gravity,
 * keeps its length, lags behind the head and is pulled softly back towards the style's shape; capsules round the skull,
 * neck, chest and upper back, shoulders and upper arms (fitted to each skeleton so the hair at rest just touches them)
 * push it out of the body, so it rests on the back and shoulders instead of passing through them. Full simulation near
 * the camera (Pawns.HairSimFar), only pushed out of the body further off (Pawns.HairCollideFar), as animated beyond.
 */
USTRUCT()
struct FMakeHumanHairNode : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	/** A collision shape riding on a bone: a capsule between two points in the bone's space. */
	struct FShape
	{
		FBoneReference Bone;
		FVector A = FVector::ZeroVector;
		FVector B = FVector::ZeroVector;
		float Radius = 0.0f;  // cm, component space
	};

	TArray<TArray<FBoneReference>> Chains;  // the hair_c chains (one sheet) first, then a ponytail's
	TArray<FShape> Shapes;
	/** A held gun and the forearms holding it (capsules only while a gun is held), and the sweep off its stock. */
	FMakeHumanHeldGun Gun;
	FBoneReference HeadBone;
	FBoneReference Hands[2];                // 0 left, 1 right
	FBoneReference LowerArms[2];
	float ForearmRadius = 4.0f;             // component space (the skeleton's units)
	FVector Right = FVector(-1.0, 0.0, 0.0);   // the body's right and forward in component space (the arm solver's)
	FVector Forward = FVector(0.0, 1.0, 0.0);
	float SweepDegrees = 60.0f;             // Pawns.HairAimSweep
	/** What the last evaluation did with a held gun (tests' notes): whether it was on, its aim, the capsules it added,
	 *  how far the sweep moved the goals' tips and how far the solved tips ended from the hair as animated (cm). */
	bool bLastGun = false;
	float LastAim = 0.0f;
	int32 LastGunCapsules = 0;
	int32 BodyCapsules = 0;                 // the first this many of Capsules are the body's (Pawns.HairDebug: green; the rest cyan)
	float LastSweepShift = 0.0f;
	float LastTipsMoved = 0.0f;
	float DeltaTime = 0.0f;
	int32 Mode = 2;                         // 2 full simulation, 1 pushed out of the body only, 0 as animated
	colony::HairStrands Strands;
	colony::HairSettings Settings;
	std::vector<std::vector<colony::HairVec>> Goals;  // this frame's animated points, world
	std::vector<colony::HairCapsule> Capsules;        // this frame's body, world (also drawn by Pawns.HairDebug)
	float LargestPenetration = 0.0f;                  // cm into the body, since the last read (tests)
	float LargestAnimatedPenetration = 0.0f;          // cm the animated hair alone would have gone in (tests)

	virtual void UpdateInternal(const FAnimationUpdateContext& Context) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
	/** Chains and shapes for a skeleton (game thread, when the animation starts). */
	void Setup(const FReferenceSkeleton& Ref);
};

/**
 * Severed limbs (a corpse cut apart): each listed bone, and with it all below it, shrinks to a point at its joint, so
 * the limb is gone and the skin closes there. (HideBone is not used on this mesh: it drops the bones from the pose,
 * and the nodes above that still use them then read bone -1.)
 */
USTRUCT()
struct FMakeHumanSeverNode : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	TArray<FName> Names;

	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override {}
};

/** Where a hand's goal is given (FMakeHumanHandGoal). */
enum class EMakeHumanGoalSpace : uint8
{
	Component,  // the mesh component's space
	Anchor,     // offset from a bone's place in component space this very pose (no lag behind the body), rotation in component space
	OtherHand,  // in the other hand's bone space after that hand is solved (a second hand on the same weapon)
};

/** A hand's goal for the arm solver (FMakeHumanArmsNode). */
struct FMakeHumanHandGoal
{
	float Alpha = 0.0f;                 // 0 off .. 1 fully on the goal
	EMakeHumanGoalSpace Space = EMakeHumanGoalSpace::Component;
	FName Anchor;                       // Space Anchor: the bone
	FTransform Target;                  // the hand bone's goal (its location and, with bRotation, its rotation)
	bool bRotation = false;             // false: the hand keeps its animated rotation (still within the wrist's range)
	bool bCurl = false;                 // close the fingers round a grip (only adds curl the animation lacks)
	float CurlWrap = 1.0f;              // how far round: 1 a pistol or hilt grip, more for a hand wrapped round a fore-end
	float RollFree = 0.0f;              // with bRotation: degrees the hand may turn about a round shaft either way to fit the wrist
	FVector RollAxis = FVector::ZeroVector;   // the shaft's axis in the hand goal's own space
	FVector RollPoint = FVector::ZeroVector;  // a point on the shaft in the hand goal's own space
	FVector Pole = FVector::ZeroVector; // component-space direction the elbow points; zero: naturally down and out
	bool bElbowPoint = false;           // instead, a component-space point the elbow bends towards
	FVector ElbowPoint = FVector::ZeroVector;
};

/**
 * Both arms onto their hands' goals without a joint a person couldn't make (docs/design/weapon-hold.md, "Arms that
 * can't bend wrong"): the elbow is a hinge about the axis the animation gave it (the upper arm rolls to keep it so),
 * never past 145 degrees nor straighter than the arm's reach allows (no stretching), the forearm's twist and the
 * wrist's bend are clamped to human range (colony::arm, tested in the sim's ArmPoseTests). A goal the arm can't
 * reach leaves the hand short of it, never a stretched or snapped arm.
 */
USTRUCT()
struct FMakeHumanArmsNode : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	struct FArm
	{
		FBoneReference Upper, Lower, Hand, Index, Pinky, Middle, IndexTip;
		FBoneReference Clavicle;                     // optional: the shoulder girdle, brought forward for a far grip
		FBoneReference Fingers[4][3];                // optional: index, middle, ring, pinky joints 01..03 (curled round a grip)
		FVector HingeInUpper = FVector::ZeroVector;  // the elbow's medial hinge in the upper arm's space, last seen bent
		bool bHinge = false;
		double NeutralFlex = 0.0;       // the rest pose's wrist readings (its knuckles off the forearm's line)
		double NeutralDeviation = 0.0;
		double LastSwivel = 0.0;        // last solve's elbow swing and turn round a shaft (degrees): kept unless clearly better
		float LastRoll = 0.0f;
		bool bValid = false;
	};
	FArm Arms[2];                        // 0 left, 1 right
	FBoneReference Spine, Neck;          // optional: the chest turns about the body's up axis to reach a long gun (the neck turns back)
	FMakeHumanHandGoal Goals[2];
	/** The body's axes in component space (up, forward, right), from the owner each update unless overridden. */
	FVector Up = FVector::UpVector;
	FVector Forward = FVector(0.0, 1.0, 0.0);
	FVector Right = FVector(-1.0, 0.0, 0.0);
	bool bLimits = true;                 // Pawns.ArmLimits
	/** What each arm read after the last solve (tests): angles, and how far the goal was out of reach (cm). */
	colony::arm::ArmAngles Measured[2];
	colony::arm::ArmAngles Wanted[2];    // the goal's own hand turn read the same way, before the wrist's limits
	float Swivel[2] = {0.0f, 0.0f};      // degrees the elbow was swung round from its natural way so the wrist fits the grip
	float CurlAdded[2] = {0.0f, 0.0f};   // mean degrees of curl added per finger last solve (-1 no hand frame, -2 no finger bones)
	float ShortBy[2] = {0.0f, 0.0f};
	bool bSolved[2] = {false, false};
	/** The goal each hand was solved to (component space) and how far the shoulder was brought forward for it (cm). */
	FTransform GoalCS[2];
	float ShoulderReach[2] = {0.0f, 0.0f};
	float ChestTurn = 0.0f;              // degrees the chest turned for the last solve (+ brings the left shoulder forward)

	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
};

/**
 * The pose of the mesh this one is attached to (the pawn's hidden mannequin), retargeted; then, where the skeleton
 * has them, soft-body jiggle on the soft-tissue bones, a tail's swish and long hair's strands.
 */
USTRUCT()
struct FMakeHumanAnimProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FMakeHumanAnimProxy() = default;
	explicit FMakeHumanAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	FAnimNode_RetargetPoseFromMesh Retarget;
	FAnimNode_ConvertLocalToComponentSpace ToComponent;
	FMakeHumanJiggleNode Jiggle;
	FMakeHumanTailNode Tail;
	FMakeHumanHairNode Hair;
	FMakeHumanSeverNode Sever;       // last: what was cut off folds away after everything else has moved
	FAnimNode_ConvertComponentToLocalSpace ToLocal;
	FMakeHumanArmsNode ArmsIK;       // both hands on weapons and reaches, within human joint limits (Pawns.MakeHumanHandIK)
	bool bPhysics = false;

	virtual void Initialize(UAnimInstance* Instance) override;
	virtual FAnimNode_Base* GetCustomRootNode() override { return bPhysics ? static_cast<FAnimNode_Base*>(&ToLocal) : static_cast<FAnimNode_Base*>(&Retarget); }
	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override;
};

/**
 * The animation of a MakeHuman character (docs/design/makehuman.md): the game's mannequin animation, retargeted
 * with the character's own retargeter (the Game Animation Sample's generic retarget animation picks retargeters by
 * its own list and ignored ours), plus jiggle and hair physics (Pawns.MakeHumanPhysics, on by default).
 */
UCLASS(Transient, NotBlueprintable)
class UMakeHumanAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	void SetRetargeter(UIKRetargeter* InRetargeter);
	UIKRetargeter* GetRetargeter() const { return Retargeter; }
	/** How many soft bodies run (jiggle bones, hair strands and a tail), for the tests. */
	int32 GetPhysicsChains() const;
	/** The largest jiggle offset (cm) since the last call, for the tests. */
	float TakeLargestJiggle();
	/** The largest tail swing at the tip (degrees) since the last call, for the tests; 0 without a tail. */
	float TakeLargestTailSwing();
	/** How many hair strands simulate (0 without long hair or a ponytail), for the tests. */
	int32 GetHairStrands() const;
	/** The deepest any hair went into the body (cm) since the last call, for the tests; OutAnimated: how deep the hair
	 *  as animated (no physics) would have gone in the same frames. */
	float TakeLargestHairPenetration(float* OutAnimated = nullptr);
	/** Soft-tissue jiggle on or off (on by default). Off, breasts, buttocks and genitals stay as animated. */
	void SetSoftTissueEnabled(bool bEnabled);
	/** Whether the soft parts run (the creator's "Jiggle" box); skin squish on contact follows it too. */
	bool IsSoftTissueEnabled() const { return GetProxyOnGameThread<FMakeHumanAnimProxy>().Jiggle.bEnabled; }
	/** Breast jiggle, 0 (the default: firm, a small lag) to 1 (softer, looser and bouncier, still damped). */
	void SetBreastJiggle(float Amount);
	/** How soft the body's flesh is, from the creator's sliders (Weight -1 thin .. 1 heavy, Muscle -1 soft .. 1
	 *  muscular; older is a little looser): a heavy soft body's belly, thighs, arms and buttocks wobble more and
	 *  slower, a lean muscular one's hardly at all. */
	void SetBodySoftness(float Weight, float Muscle, float Age = 0.0f);
	/** A hit on this body: the soft parts near At get Velocity (cm/s), fading out over Radius (cm); the whole body
	 *  gets a tenth of it. */
	void AddHitKick(const FVector& At, const FVector& Velocity, float Radius = 40.0f);
	/** A blast at At: every soft part is pushed away from it, Speed (cm/s) at the centre fading to 0 at Radius (cm). */
	void AddBlastKick(const FVector& At, float Speed, float Radius = 800.0f);
	/** The softness last set (0 firm .. 1 very soft), for the tests and the creator's readout. */
	float GetBodySoftness() const { return BodySoftness; }
	/** Bones cut off (each with all below it): folded to a point from the next animation update on. */
	void SetSeveredBones(const TArray<FName>& Bones) { SeveredBones = Bones; }
	const TArray<FName>& GetSeveredBones() const { return SeveredBones; }
	/** Kicks queued since the last animation update (moved to the spring node in PreUpdate). */
	TArray<FMakeHumanKick> PendingKicks;
	/** The deepest skin dent (cm) since the last call, for the tests. */
	float TakeDeepestDent();
	/** The left hand's wrist pulled to a point given in the right hand bone's space (a two-handed grip), the elbow bent
	 *  towards ElbowWorld; Alpha 0 is off. Its rotation stays as animated (within the wrist's range). */
	void SetLeftHandGrip(float Alpha, const FVector& InRightHandOffset, const FVector& InElbowWorld);
	/** The right hand's wrist moved to a point in this component's space (its rotation kept as animated), the elbow
	 *  bent towards a point in the same space; Alpha 0 is off. */
	void SetRightHandReach(float Alpha, const FVector& InTargetComponent, const FVector& InElbowComponent);
	/** The left hand's wrist moved to a point in this component's space instead (a guard, a shield raised, hands under a
	 *  carried body's legs), the elbow bent towards a point in the same space; Alpha 0 is off. Replaces the grip. */
	void SetLeftHandReach(float Alpha, const FVector& InTargetComponent, const FVector& InElbowComponent);
	/** A hand's full goal (location and rotation, in any space): a weapon's grip (docs/design/weapon-hold.md). Replaces
	 *  whatever the hand was given before; Alpha 0 lets it go. */
	void SetHandGoal(bool bRight, const FMakeHumanHandGoal& Goal) { HandGoals[bRight ? 1 : 0] = Goal; }
	const FMakeHumanHandGoal& GetHandGoal(bool bRight) const { return HandGoals[bRight ? 1 : 0]; }
	/** The gun held up in both hands (bOn false: none), for long hair: it rests on the gun and the forearms, and goes over
	 *  the other shoulder while aimed (docs/design/hair-physics.md, "Aiming"). */
	void SetHeldGun(const FMakeHumanHeldGun& InGun) { HeldGun = InGun; }
	const FMakeHumanHeldGun& GetHeldGun() const { return HeldGun; }
	/** The hair's last frame with a held gun, for the tests' notes (see FMakeHumanHairNode::bLastGun). */
	FString DescribeHairAim() const;
	FMakeHumanHeldGun HeldGun;
	/** The body's up, forward and right in world space, used instead of the owner actor's (the first-person arms face
	 *  the view, not their holder); off with bOn false. */
	void SetBodyAxesOverride(bool bOn, const FVector& InUp = FVector::UpVector, const FVector& InForward = FVector::ForwardVector,
							 const FVector& InRight = FVector::RightVector)
	{
		bAxesOverride = bOn;
		AxesUp = InUp;
		AxesForward = InForward;
		AxesRight = InRight;
	}
	/** What an arm read after the solver's last pass (degrees; tests), false if it was not solved. */
	bool GetSolvedArm(bool bRight, colony::arm::ArmAngles& Out, float& OutShortBy) const;
	/** What the goal's hand turn asked of the arm before the wrist's limits (degrees; tests' notes), and the fingers' added curl. */
	bool GetWantedArm(bool bRight, colony::arm::ArmAngles& Out, float& OutCurl, float& OutSwivel) const;
	/** The goal the arm was last solved to, in world space (tests: compared with where the hand is drawn), and how far
	 *  the shoulder came forward for it (cm, component units). */
	bool GetSolvedGoal(bool bRight, FTransform& OutGoalWorld, float& OutShoulderReach) const;
	FMakeHumanHandGoal HandGoals[2];  // 0 left, 1 right
	bool bAxesOverride = false;
	FVector AxesUp = FVector::UpVector;
	FVector AxesForward = FVector::ForwardVector;
	FVector AxesRight = FVector::RightVector;

protected:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override;

private:
	void ApplySoftness();

	UPROPERTY()
	TObjectPtr<UIKRetargeter> Retargeter;
	TArray<FName> SeveredBones;
	float BodySoftness = 0.5f;

	/** A dent in the skin where a hit landed (M_MH_SkinDetail's SkinDent, custom primitive data 16 to 25): pushed in along
	 *  the skin's normal round a point that rides on the nearest bone, then springing back past flat and settling. */
	struct FDent
	{
		FName Bone;
		FVector Local = FVector::ZeroVector;  // the point in the bone's space
		float Depth = 0.0f;                   // cm at the deepest
		float Radius = 10.0f;                 // cm
		float Age = 0.0f;                     // s
	};
	FDent Dents[2];
	int32 NextDent = 0;
	float DeepestDent = 0.0f;
	bool bDentsShown = false;
};
