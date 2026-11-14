#include "MakeHumanAnimInstance.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Retargeter/IKRetargeter.h"

namespace
{
	TAutoConsoleVariable<int32> GMakeHumanPhysics(TEXT("Pawns.MakeHumanPhysics"), 1,
		TEXT("1 runs jiggle (breast bones) and hair sway (ponytail bones) on MakeHuman characters; read when a character's animation starts."), ECVF_Default);

	// A soft body on one bone: it lags behind the body's motion and springs back.
	TSharedPtr<FAnimNode_AnimDynamics> MakeJiggle(FName Bone)
	{
		TSharedPtr<FAnimNode_AnimDynamics> Node = MakeShared<FAnimNode_AnimDynamics>();
		Node->BoundBone.BoneName = Bone;
		Node->bChain = false;
		Node->SimulationSpace = AnimPhysSimSpaceType::World;
		Node->GravityScale = 0.25f;
		Node->bLinearSpring = true;
		Node->LinearSpringConstant = 650.0f;
		Node->bAngularSpring = true;
		Node->AngularSpringConstant = 900.0f;
		Node->bOverrideLinearDamping = true;
		Node->LinearDampingOverride = 0.65f;
		Node->bOverrideAngularDamping = true;
		Node->AngularDampingOverride = 0.6f;
		Node->PhysicsBodyDefinitions.SetNum(1);
		FAnimPhysBodyDefinition& Body = Node->PhysicsBodyDefinitions[0];
		Body.BoundBone.BoneName = Bone;
		Body.BoxExtents = FVector(6.0f, 6.0f, 6.0f);
		Body.LocalJointOffset = FVector::ZeroVector;
		FAnimPhysConstraintSetup& C = Body.ConstraintSetup;
		C.LinearXLimitType = AnimPhysLinearConstraintType::Limited;
		C.LinearYLimitType = AnimPhysLinearConstraintType::Limited;
		C.LinearZLimitType = AnimPhysLinearConstraintType::Limited;
		C.LinearAxesMin = FVector(-1.2f, -1.2f, -1.6f);
		C.LinearAxesMax = FVector(1.2f, 1.2f, 1.0f);
		C.AngularConstraintType = AnimPhysAngularConstraintType::Angular;
		C.AngularLimitsMin = FVector(-8.0f, -8.0f, -8.0f);
		C.AngularLimitsMax = FVector(8.0f, 8.0f, 8.0f);
		return Node;
	}

	// A swinging chain (hair): each bone a body hanging from the last, limited to a cone.
	TSharedPtr<FAnimNode_AnimDynamics> MakeChain(FName First, FName Last, const FReferenceSkeleton& Ref)
	{
		TSharedPtr<FAnimNode_AnimDynamics> Node = MakeShared<FAnimNode_AnimDynamics>();
		Node->bChain = true;
		Node->BoundBone.BoneName = First;
		Node->ChainEnd.BoneName = Last;
		Node->SimulationSpace = AnimPhysSimSpaceType::World;
		Node->GravityScale = 1.0f;
		Node->bAngularSpring = true;
		Node->AngularSpringConstant = 120.0f;
		Node->bOverrideAngularDamping = true;
		Node->AngularDampingOverride = 0.5f;
		Node->UpdateChainPhysicsBodyDefinitions(Ref);
		for (FAnimPhysBodyDefinition& Body : Node->PhysicsBodyDefinitions)
		{
			Body.BoxExtents = FVector(3.0f, 3.0f, 6.0f);
			Body.ConstraintSetup.AngularConstraintType = AnimPhysAngularConstraintType::Cone;
			Body.ConstraintSetup.ConeAngle = 25.0f;
		}
		return Node;
	}
}

void FMakeHumanAnimProxy::Initialize(UAnimInstance* Instance)
{
	FAnimInstanceProxy::Initialize(Instance);
	Retarget.RetargetFrom = ERetargetSourceMode::ParentSkeletalMeshComponent;
	Dynamics.Reset();
	const USkeletalMeshComponent* Mesh = Instance ? Instance->GetSkelMeshComponent() : nullptr;
	const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (Asset && GMakeHumanPhysics.GetValueOnGameThread() != 0)
	{
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		for (const TCHAR* Breast : {TEXT("breast_l"), TEXT("breast_r")})
		{
			if (Ref.FindBoneIndex(Breast) != INDEX_NONE)
			{
				Dynamics.Add(MakeJiggle(Breast));
			}
		}
		// MakeHuman's ponytail sub-rig: ponytail, ponytail1 ... ponytail4 under the head.
		if (Ref.FindBoneIndex(TEXT("ponytail1")) != INDEX_NONE)
		{
			FName Last = TEXT("ponytail1");
			for (int32 i = 2; i < 12 && Ref.FindBoneIndex(FName(*FString::Printf(TEXT("ponytail%d"), i))) != INDEX_NONE; ++i)
			{
				Last = FName(*FString::Printf(TEXT("ponytail%d"), i));
			}
			Dynamics.Add(MakeChain(TEXT("ponytail1"), Last, Ref));
		}
	}
	bPhysics = !Dynamics.IsEmpty();
	if (bPhysics)
	{
		ToComponent.LocalPose.SetLinkNode(&Retarget);
		FAnimNode_Base* Previous = &ToComponent;
		for (const TSharedPtr<FAnimNode_AnimDynamics>& Node : Dynamics)
		{
			Node->ComponentPose.SetLinkNode(Previous);
			Previous = Node.Get();
		}
		ToLocal.ComponentPose.SetLinkNode(Previous);
	}
}

void FMakeHumanAnimProxy::PreUpdate(UAnimInstance* Instance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
	if (const UMakeHumanAnimInstance* MakeHuman = Cast<UMakeHumanAnimInstance>(Instance))
	{
		Retarget.IKRetargeterAsset = MakeHuman->GetRetargeter();
	}
	Retarget.RetargetFrom = ERetargetSourceMode::ParentSkeletalMeshComponent;
	Retarget.PreUpdate(Instance);
	for (const TSharedPtr<FAnimNode_AnimDynamics>& Node : Dynamics)
	{
		Node->PreUpdate(Instance);
	}
}

void UMakeHumanAnimInstance::SetRetargeter(UIKRetargeter* InRetargeter)
{
	Retargeter = InRetargeter;
}

int32 UMakeHumanAnimInstance::GetPhysicsChains() const
{
	return GetProxyOnGameThread<FMakeHumanAnimProxy>().Dynamics.Num();
}

FAnimInstanceProxy* UMakeHumanAnimInstance::CreateAnimInstanceProxy()
{
	return new FMakeHumanAnimProxy(this);
}

void UMakeHumanAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy)
{
	delete static_cast<FMakeHumanAnimProxy*>(Proxy);
}
