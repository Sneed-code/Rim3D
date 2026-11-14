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
	/** How many soft bodies run (breasts and hair chains), for the tests. */
	int32 GetPhysicsChains() const;

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override;

private:
	UPROPERTY()
	TObjectPtr<UIKRetargeter> Retargeter;
};
