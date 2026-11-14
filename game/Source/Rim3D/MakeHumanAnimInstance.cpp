#include "MakeHumanAnimInstance.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Retargeter/IKRetargeter.h"

namespace
{
	TAutoConsoleVariable<int32> GMakeHumanPhysics(TEXT("Pawns.MakeHumanPhysics"), 1,
		TEXT("1 runs jiggle (breast bones) and hair sway (ponytail bones) on MakeHuman characters; read when a character's animation starts."), ECVF_Default);

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

void FMakeHumanJiggleNode::UpdateInternal(const FAnimationUpdateContext& Context)
{
	FAnimNode_SkeletalControlBase::UpdateInternal(Context);
	DeltaTime = Context.GetDeltaTime();
}

bool FMakeHumanJiggleNode::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	for (const FBody& Body : Bodies)
	{
		if (Body.Bone.IsValidToEvaluate(RequiredBones))
		{
			return true;
		}
	}
	return false;
}

void FMakeHumanJiggleNode::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	for (FBody& Body : Bodies)
	{
		Body.Bone.Initialize(RequiredBones);
	}
}

void FMakeHumanJiggleNode::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FTransform Component = Output.AnimInstanceProxy->GetComponentTransform();
	const FBoneContainer& Bones = Output.Pose.GetPose().GetBoneContainer();
	// Small steps keep the spring steady at low frame rates.
	const float Step = FMath::Clamp(DeltaTime, 0.0f, 0.1f);
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Step / (1.0f / 120.0f)));
	const float Dt = Step / Steps;
	TArray<FBoneTransform> Out;
	for (FBody& Body : Bodies)
	{
		if (!Body.Bone.IsValidToEvaluate(Bones))
		{
			continue;
		}
		const FCompactPoseBoneIndex Index = Body.Bone.GetCompactPoseIndex(Bones);
		FTransform Pose = Output.Pose.GetComponentSpaceTransform(Index);
		const FVector Goal = Component.TransformPosition(Pose.GetLocation());
		if (!Body.bStarted || FVector::DistSquared(Goal, Body.Position) > 50.0 * 50.0)
		{
			Body.Position = Goal;  // first frame, or a teleport
			Body.Velocity = FVector::ZeroVector;
			Body.bStarted = true;
		}
		for (int32 i = 0; i < Steps; ++i)
		{
			const FVector Accel = (Goal - Body.Position) * Body.Stiffness - Body.Velocity * Body.Damping;
			Body.Velocity += Accel * Dt;
			Body.Position += Body.Velocity * Dt;
		}
		FVector Offset = Body.Position - Goal;
		if (Offset.Size() > Body.MaxOffset)
		{
			Offset = Offset.GetSafeNormal() * Body.MaxOffset;
			Body.Position = Goal + Offset;
			Body.Velocity *= 0.5f;
		}
		LargestOffset = FMath::Max(LargestOffset, static_cast<float>(Offset.Size()));
		Pose.SetLocation(Component.InverseTransformPosition(Goal + Offset));
		Out.Add(FBoneTransform(Index, Pose));
	}
	Out.Sort(FCompareBoneTransformIndex());
	OutBoneTransforms.Append(Out);
}

void FMakeHumanAnimProxy::Initialize(UAnimInstance* Instance)
{
	// The graph is linked up first: the engine's Initialize takes the root node (GetCustomRootNode) there and keeps it, so
	// linking afterwards left the jiggle and hair nodes out of the graph.
	Retarget.RetargetFrom = ERetargetSourceMode::ParentSkeletalMeshComponent;
	Dynamics.Reset();
	const USkeletalMeshComponent* Mesh = Instance ? Instance->GetSkelMeshComponent() : nullptr;
	const USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	if (Asset && GMakeHumanPhysics.GetValueOnGameThread() != 0)
	{
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		// Soft tissue: breasts (MPFB's breast bones) and buttocks (the bones mh_export.py adds), a little softer below.
		Jiggle.Bodies.Reset();
		for (const TCHAR* Bone : {TEXT("breast_l"), TEXT("breast_r"), TEXT("butt_l"), TEXT("butt_r")})
		{
			if (Ref.FindBoneIndex(Bone) != INDEX_NONE)
			{
				FMakeHumanJiggleNode::FBody& Body = Jiggle.Bodies.AddDefaulted_GetRef();
				Body.Bone.BoneName = Bone;
				const bool bButt = FString(Bone).StartsWith(TEXT("butt"));
				Body.Stiffness = bButt ? 130.0f : 160.0f;
				Body.Damping = bButt ? 6.5f : 6.0f;
				Body.MaxOffset = bButt ? 3.0f : 4.0f;
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
	bPhysics = !Dynamics.IsEmpty() || !Jiggle.Bodies.IsEmpty();
	if (bPhysics)
	{
		ToComponent.LocalPose.SetLinkNode(&Retarget);
		FAnimNode_Base* Previous = &ToComponent;
		if (!Jiggle.Bodies.IsEmpty())
		{
			Jiggle.ComponentPose.SetLinkNode(Previous);
			Previous = &Jiggle;
		}
		for (const TSharedPtr<FAnimNode_AnimDynamics>& Node : Dynamics)
		{
			Node->ComponentPose.SetLinkNode(Previous);
			Previous = Node.Get();
		}
		ToLocal.ComponentPose.SetLinkNode(Previous);
	}
	FAnimInstanceProxy::Initialize(Instance);
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
	const FMakeHumanAnimProxy& Proxy = GetProxyOnGameThread<FMakeHumanAnimProxy>();
	return Proxy.Dynamics.Num() + Proxy.Jiggle.Bodies.Num();
}

float UMakeHumanAnimInstance::TakeLargestJiggle()
{
	FMakeHumanAnimProxy& Proxy = GetProxyOnGameThread<FMakeHumanAnimProxy>();
	const float Largest = Proxy.Jiggle.LargestOffset;
	Proxy.Jiggle.LargestOffset = 0.0f;
	return Largest;
}

FAnimInstanceProxy* UMakeHumanAnimInstance::CreateAnimInstanceProxy()
{
	return new FMakeHumanAnimProxy(this);
}

void UMakeHumanAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy)
{
	delete static_cast<FMakeHumanAnimProxy*>(Proxy);
}
