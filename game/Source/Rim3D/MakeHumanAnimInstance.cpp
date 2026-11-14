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
	TSharedPtr<FAnimNode_AnimDynamics> MakeChain(FName First, FName Last, const FReferenceSkeleton& Ref, float Gravity = 1.0f, float Spring = 120.0f,
		float Cone = 25.0f)
	{
		TSharedPtr<FAnimNode_AnimDynamics> Node = MakeShared<FAnimNode_AnimDynamics>();
		Node->bChain = true;
		Node->BoundBone.BoneName = First;
		Node->ChainEnd.BoneName = Last;
		Node->SimulationSpace = AnimPhysSimSpaceType::World;
		Node->GravityScale = Gravity;
		Node->bAngularSpring = true;
		Node->AngularSpringConstant = Spring;
		Node->bOverrideAngularDamping = true;
		Node->AngularDampingOverride = 0.5f;
		Node->UpdateChainPhysicsBodyDefinitions(Ref);
		for (FAnimPhysBodyDefinition& Body : Node->PhysicsBodyDefinitions)
		{
			Body.BoxExtents = FVector(3.0f, 3.0f, 6.0f);
			Body.ConstraintSetup.AngularConstraintType = AnimPhysAngularConstraintType::Cone;
			Body.ConstraintSetup.ConeAngle = Cone;
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

void FMakeHumanTailNode::UpdateInternal(const FAnimationUpdateContext& Context)
{
	FAnimNode_SkeletalControlBase::UpdateInternal(Context);
	DeltaTime = Context.GetDeltaTime();
}

bool FMakeHumanTailNode::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return !Bones.IsEmpty() && Bones[0].IsValidToEvaluate(RequiredBones);
}

void FMakeHumanTailNode::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	for (FBoneReference& Bone : Bones)
	{
		Bone.Initialize(RequiredBones);
	}
}

void FMakeHumanTailNode::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FTransform Component = Output.AnimInstanceProxy->GetComponentTransform();
	const FBoneContainer& Container = Output.Pose.GetPose().GetBoneContainer();
	const float Dt = FMath::Clamp(DeltaTime, 0.0f, 0.1f);
	// How the body moves: ground speed and turn rate (degrees a second, about up), from frame to frame.
	float TurnRate = 0.0f;
	if (bStarted && Dt > 0.0f)
	{
		const float Moved = static_cast<float>(FVector::Dist2D(Component.GetLocation(), LastComponent.GetLocation())) / Dt;
		if (Moved < 3000.0f)  // not a teleport
		{
			Speed = FMath::FInterpTo(Speed, Moved, Dt, 4.0f);
			const float YawNow = Component.GetRotation().Rotator().Yaw;
			const float YawBefore = LastComponent.GetRotation().Rotator().Yaw;
			TurnRate = FMath::FindDeltaAngleDegrees(YawBefore, YawNow) / Dt;
		}
	}
	LastComponent = Component;
	bStarted = true;
	// Swings out against a turn and settles back.
	TurnSwing = FMath::FInterpTo(TurnSwing, FMath::Clamp(-TurnRate * 0.12f, -35.0f, 35.0f), Dt, 3.0f);
	const float Pace = FMath::Clamp(Speed / 450.0f, 0.0f, 1.5f);
	Time += Dt * (0.7f + 1.1f * Pace);  // swish cycles per second

	const int32 Count = Bones.Num();
	TArray<FCompactPoseBoneIndex, TInlineAllocator<16>> Index;
	TArray<FTransform, TInlineAllocator<16>> Original;
	for (const FBoneReference& Bone : Bones)
	{
		if (!Bone.IsValidToEvaluate(Container))
		{
			break;
		}
		Index.Add(Bone.GetCompactPoseIndex(Container));
		Original.Add(Output.Pose.GetComponentSpaceTransform(Index.Last()));
	}
	// Each joint turns about the component's up axis: a wave running down the tail, plus the turn's swing.
	FQuat Total = FQuat::Identity;
	FVector Joint = Original.IsEmpty() ? FVector::ZeroVector : Original[0].GetLocation();
	float TipAngle = 0.0f;
	for (int32 i = 0; i < Index.Num(); ++i)
	{
		if (i > 0)
		{
			Joint += Total.RotateVector(Original[i].GetLocation() - Original[i - 1].GetLocation());
		}
		const float Along = (i + 1.0f) / FMath::Max(Count, 1);
		const float Wave = (5.0f + 4.0f * Pace) * FMath::Sin(2.0f * PI * Time - 0.9f * i);
		const float Angle = (Wave + TurnSwing / FMath::Max(Count, 1) * 2.0f * Along) * (0.5f + Along);
		TipAngle += Angle;
		Total = FQuat(FVector::UpVector, FMath::DegreesToRadians(Angle)) * Total;
		FTransform Moved = Original[i];
		Moved.SetLocation(Joint);
		Moved.SetRotation((Total * Original[i].GetRotation()).GetNormalized());
		OutBoneTransforms.Add(FBoneTransform(Index[i], Moved));
	}
	LargestAngle = FMath::Max(LargestAngle, FMath::Abs(TipAngle));
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
		for (const TCHAR* Bone : {TEXT("breast_l"), TEXT("breast_r"), TEXT("butt_l"), TEXT("butt_r"), TEXT("genital")})
		{
			if (Ref.FindBoneIndex(Bone) != INDEX_NONE)
			{
				FMakeHumanJiggleNode::FBody& Body = Jiggle.Bodies.AddDefaulted_GetRef();
				Body.Bone.BoneName = Bone;
				const bool bButt = FString(Bone).StartsWith(TEXT("butt"));
				const bool bGenital = FString(Bone) == TEXT("genital");
				// Butt: softer and bouncier (about 1.7 Hz, a quarter of critical damping), so walking shows it too.
				// Male genitals (mh_common.add_genital_bone): loose, they hang and swing the most.
				Body.Stiffness = bGenital ? 70.0f : (bButt ? 110.0f : 150.0f);
				Body.Damping = bGenital ? 3.5f : (bButt ? 5.0f : 5.5f);
				Body.MaxOffset = bGenital ? 3.5f : 4.0f;
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
		// A tail (mh_export.py's add_tail: tail_01 ... under the pelvis): its own swish (FMakeHumanTailNode).
		Tail.Bones.Reset();
		for (int32 i = 1; i < 30 && Ref.FindBoneIndex(FName(*FString::Printf(TEXT("tail_%02d"), i))) != INDEX_NONE; ++i)
		{
			Tail.Bones.Emplace(FName(*FString::Printf(TEXT("tail_%02d"), i)));
		}
	}
	bPhysics = !Dynamics.IsEmpty() || !Jiggle.Bodies.IsEmpty() || !Tail.Bones.IsEmpty();
	if (bPhysics)
	{
		ToComponent.LocalPose.SetLinkNode(&Retarget);
		FAnimNode_Base* Previous = &ToComponent;
		if (!Jiggle.Bodies.IsEmpty())
		{
			Jiggle.ComponentPose.SetLinkNode(Previous);
			Previous = &Jiggle;
		}
		if (!Tail.Bones.IsEmpty())
		{
			Tail.ComponentPose.SetLinkNode(Previous);
			Previous = &Tail;
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
	return Proxy.Dynamics.Num() + Proxy.Jiggle.Bodies.Num() + (Proxy.Tail.Bones.IsEmpty() ? 0 : 1);
}

float UMakeHumanAnimInstance::TakeLargestTailSwing()
{
	FMakeHumanAnimProxy& Proxy = GetProxyOnGameThread<FMakeHumanAnimProxy>();
	const float Largest = Proxy.Tail.LargestAngle;
	Proxy.Tail.LargestAngle = 0.0f;
	return Largest;
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
