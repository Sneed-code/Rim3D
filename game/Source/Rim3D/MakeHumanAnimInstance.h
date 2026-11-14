#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeSpaceConversions.h"
#include "AnimNodes/AnimNode_RetargetPoseFromMesh.h"
#include "BoneControllers/AnimNode_AnimDynamics.h"

#include "MakeHumanAnimInstance.generated.h"

class UIKRetargeter;

/**
 * Soft tissue (breasts, buttocks): each listed bone's point follows where the animation puts it through a damped
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
		FVector Position = FVector::ZeroVector;  // world, simulated
		FVector Velocity = FVector::ZeroVector;
		bool bStarted = false;
	};
	TArray<FBody> Bodies;
	float DeltaTime = 0.0f;
	float LargestOffset = 0.0f;  // cm, since the last read (tests)
	bool bEnabled = true;        // off: the soft parts stay as animated (the creator's "keep still")

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

/**
 * The pose of the mesh this one is attached to (the pawn's hidden mannequin), retargeted; then, where the skeleton
 * has them, soft-body jiggle on the breast bones and a swinging chain on a ponytail's bones (AnimDynamics).
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
	TArray<TSharedPtr<FAnimNode_AnimDynamics>> Dynamics;
	FAnimNode_ConvertComponentToLocalSpace ToLocal;
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
	/** How many soft bodies run (jiggle bones and hair chains), for the tests. */
	int32 GetPhysicsChains() const;
	/** The largest jiggle offset (cm) since the last call, for the tests. */
	float TakeLargestJiggle();
	/** The largest tail swing at the tip (degrees) since the last call, for the tests; 0 without a tail. */
	float TakeLargestTailSwing();
	/** Soft-tissue jiggle on or off (on by default). Off, breasts, buttocks and genitals stay as animated. */
	void SetSoftTissueEnabled(bool bEnabled);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override;

private:
	UPROPERTY()
	TObjectPtr<UIKRetargeter> Retargeter;
};
