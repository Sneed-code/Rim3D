#include "MakeHumanAnimInstance.h"

#include "GoreCut.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Retargeter/IKRetargeter.h"

namespace
{
	TAutoConsoleVariable<int32> GMakeHumanPhysics(TEXT("Pawns.MakeHumanPhysics"), 1,
		TEXT("1 runs jiggle (breast bones) and hair sway (ponytail bones) on MakeHuman characters; read when a character's animation starts."), ECVF_Default);
	TAutoConsoleVariable<int32> GMakeHumanHandIK(TEXT("Pawns.MakeHumanHandIK"), 1,
		TEXT("1 lets the creator's weapons put the left hand on a two-handed hilt (two-bone IK, off until asked for); read when a character's animation starts."), ECVF_Default);
	TAutoConsoleVariable<float> GMakeHumanSoftNear(TEXT("Pawns.SoftBodyNear"), 1300.0f,
		TEXT("Soft-body springs run fully on MakeHuman characters within this many cm of the camera, fading out to Pawns.SoftBodyFar."), ECVF_Default);
	TAutoConsoleVariable<float> GMakeHumanSoftFar(TEXT("Pawns.SoftBodyFar"), 1500.0f,
		TEXT("Beyond this many cm from the camera a MakeHuman character's soft parts stay as animated (no springs)."), ECVF_Default);
	TAutoConsoleVariable<float> GMakeHumanDentScale(TEXT("Pawns.SkinDentScale"), 1.0f,
		TEXT("How deep hits dent the skin of MakeHuman characters (1 as tuned, 0 no dents)."), ECVF_Default);
	TAutoConsoleVariable<float> GMakeHumanSoftScale(TEXT("Pawns.SoftBodyScale"), 1.0f,
		TEXT("How hard hits and blasts kick the soft parts (1 as tuned, 0 none)."), ECVF_Default);

	// One soft part's spring (docs/design/soft-body.md): stiffness 1/s^2, damping 1/s, the farthest it lags (cm), and
	// whether the body's softness (weight, muscle, age) scales it.
	struct FMhSoftPart
	{
		const TCHAR* Bone;
		float Stiffness;
		float Damping;
		float MaxOffset;
		bool bBodySoft;
		float Squash;
	};
	// Breasts about 2 Hz and well damped (their own slider); buttocks softer and bouncier (about 1.7 Hz, a quarter of
	// critical damping) so walking shows it; male genitals loose (they hang and swing the most); the belly a slow heavy
	// wobble; thighs and upper arms quicker and small, so the limbs' flesh shivers on a landing or a hit, not swings.
	// Rooms and kicks were first a third of these: measured up to 6 cm, but nothing showed on screen (2026-11-16 clips).
	const FMhSoftPart MhSoftParts[] = {
		{TEXT("breast_l"), 150.0f, 5.5f, 4.0f, false, 0.0f},
		{TEXT("breast_r"), 150.0f, 5.5f, 4.0f, false, 0.0f},
		{TEXT("butt_l"), 110.0f, 5.0f, 5.0f, true, 0.35f},
		{TEXT("butt_r"), 110.0f, 5.0f, 5.0f, true, 0.35f},
		{TEXT("genital"), 70.0f, 3.5f, 3.5f, false, 0.0f},
		{TEXT("belly"), 110.0f, 4.5f, 6.0f, true, 0.45f},
		{TEXT("thigh_soft_l"), 200.0f, 6.0f, 3.0f, true, 0.0f},
		{TEXT("thigh_soft_r"), 200.0f, 6.0f, 3.0f, true, 0.0f},
		{TEXT("arm_soft_l"), 220.0f, 6.0f, 2.5f, true, 0.0f},
		{TEXT("arm_soft_r"), 220.0f, 6.0f, 2.5f, true, 0.0f},
	};


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
	if (!bEnabled || Fade <= 0.0f)
	{
		for (FBody& Body : Bodies)
		{
			Body.bStarted = false;  // starts again at rest when turned back on (or back within reach of the camera)
		}
		Kicks.Reset();
		return;
	}
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
		// Hits and blasts: a kick to the part's velocity, more the nearer it is (a blast's falls off with the square).
		for (const FMakeHumanKick& Kick : Kicks)
		{
			const float Distance = static_cast<float>(FVector::Dist(Kick.At, Goal));
			if (Distance >= Kick.Radius)
			{
				continue;
			}
			const float Near = 1.0f - Distance / FMath::Max(Kick.Radius, 1.0f);
			Body.Velocity += Kick.bRadial ? (Goal - Kick.At).GetSafeNormal() * Kick.Speed * Near * Near : Kick.Velocity * Near;
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
		// Fading out towards Pawns.SoftBodyFar, so a pawn walking away never visibly switches off.
		Pose.SetLocation(Component.InverseTransformPosition(Goal + Offset * Fade));
		// Squash and stretch along the line out from the parent bone (the belly's points forwards, a buttock's back):
		// scaled on whichever of the bone's own axes lies closest to it.
		if (Body.Squash > 0.0f)
		{
			const FCompactPoseBoneIndex ParentIndex = Bones.GetParentBoneIndex(Index);
			if (ParentIndex.IsValid())
			{
				const FVector OutDir = (Pose.GetLocation() - Output.Pose.GetComponentSpaceTransform(ParentIndex).GetLocation()).GetSafeNormal();
				const FVector OffsetC = Component.InverseTransformVectorNoScale(Offset);
				const float Along = FMath::Clamp(static_cast<float>(FVector::DotProduct(OffsetC, OutDir)) / FMath::Max(Body.MaxOffset, 0.1f), -1.0f, 1.0f) * Fade;
				const FVector Local = Pose.GetRotation().UnrotateVector(OutDir).GetAbs();
				const int32 Axis = Local.X >= Local.Y && Local.X >= Local.Z ? 0 : (Local.Y >= Local.Z ? 1 : 2);
				FVector Scale(static_cast<double>(1.0f - 0.5f * Body.Squash * Along));
				Scale[Axis] = 1.0f + Body.Squash * Along;
				Pose.SetScale3D(Pose.GetScale3D() * Scale);
			}
		}
		Out.Add(FBoneTransform(Index, Pose));
	}
	Kicks.Reset();
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

void FMakeHumanHairNode::UpdateInternal(const FAnimationUpdateContext& Context)
{
	FAnimNode_SkeletalControlBase::UpdateInternal(Context);
	DeltaTime = Context.GetDeltaTime();
}

bool FMakeHumanHairNode::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return !Chains.IsEmpty() && Head.IsValidToEvaluate(RequiredBones);
}

void FMakeHumanHairNode::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	Head.Initialize(RequiredBones);
	for (TArray<FBoneReference>& Chain : Chains)
	{
		for (FBoneReference& Bone : Chain)
		{
			Bone.Initialize(RequiredBones);
		}
	}
}

void FMakeHumanHairNode::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FTransform Component = Output.AnimInstanceProxy->GetComponentTransform();
	const FBoneContainer& Container = Output.Pose.GetPose().GetBoneContainer();
	const float Dt = FMath::Clamp(DeltaTime, 0.0f, 0.05f);
	// The head's motion in the world: its acceleration swings the hair the other way.
	const FVector HeadWorld = Component.TransformPosition(Output.Pose.GetComponentSpaceTransform(Head.GetCompactPoseIndex(Container)).GetLocation());
	FVector Accel = FVector::ZeroVector;
	if (bStarted && Dt > 1.0e-4f)
	{
		const FVector Velocity = (HeadWorld - LastHead) / Dt;
		if (Velocity.Size() < 1500.0f)  // not a teleport or a jump of the mesh
		{
			Accel = (Velocity - HeadVelocity) / Dt;
			HeadVelocity = Velocity;
		}
		else
		{
			HeadVelocity = FVector::ZeroVector;
		}
	}
	LastHead = HeadWorld;
	bStarted = true;
	FVector Target = -Accel * 0.012f;  // degrees per cm/s^2
	Target.Z = 0.0;
	Target = Target.GetClampedToMaxSize(22.0f);
	// A damped spring (a little under critical: one soft overshoot), small steps.
	for (float Left = Dt; Left > 1.0e-5f;)
	{
		const float Step = FMath::Min(Left, 1.0f / 120.0f);
		SwingVelocity += (55.0f * (Target - Swing) - 11.0f * SwingVelocity) * Step;
		Swing += SwingVelocity * Step;
		Swing = Swing.GetClampedToMaxSize(25.0f);
		Left -= Step;
	}
	const FVector DownC = Component.InverseTransformVectorNoScale(FVector::DownVector);
	const FVector SwingAxisC = Component.InverseTransformVectorNoScale(FVector::CrossProduct(FVector::UpVector, Swing.GetSafeNormal()));
	const float SwingAngle = static_cast<float>(Swing.Size());
	for (const TArray<FBoneReference>& Chain : Chains)
	{
		TArray<FCompactPoseBoneIndex, TInlineAllocator<8>> Index;
		TArray<FTransform, TInlineAllocator<8>> Original;
		for (const FBoneReference& Bone : Chain)
		{
			if (!Bone.IsValidToEvaluate(Container))
			{
				break;
			}
			Index.Add(Bone.GetCompactPoseIndex(Container));
			Original.Add(Output.Pose.GetComponentSpaceTransform(Index.Last()));
		}
		const int32 N = Index.Num();
		if (N < 2)
		{
			continue;
		}
		// Hanging: the chain's line (root to tip) turned most of the way towards the ground, at most 70 degrees.
		const FVector Root = Original[0].GetLocation();
		const FVector Tip = Original[N - 1].GetLocation() + (Original[N - 1].GetLocation() - Original[N - 2].GetLocation());
		const FVector Line = (Tip - Root).GetSafeNormal();
		FQuat Hang = FQuat::FindBetweenNormals(Line, FMath::Lerp(Line, DownC, 0.75f).GetSafeNormal());
		FVector HangAxis;
		float HangAngle;
		Hang.ToAxisAndAngle(HangAxis, HangAngle);
		HangAngle = FMath::Min(HangAngle, FMath::DegreesToRadians(70.0f));
		// Both spread down the chain, more of it at the root for the hang, more at the tip for the swing.
		FQuat Total = FQuat::Identity;
		FVector Joint = Root;
		const float Sum = N * (N + 1) * 0.5f;
		for (int32 i = 0; i < N; ++i)
		{
			if (i > 0)
			{
				Joint += Total.RotateVector(Original[i].GetLocation() - Original[i - 1].GetLocation());
			}
			const float HangShare = (N - i) / Sum;
			const float SwingShare = (i + 1) / Sum;
			FQuat Turn = FQuat(HangAxis, HangAngle * HangShare);
			if (SwingAngle > 0.01f && !SwingAxisC.IsNearlyZero())
			{
				Turn = FQuat(SwingAxisC.GetSafeNormal(), FMath::DegreesToRadians(SwingAngle) * SwingShare) * Turn;
			}
			Total = (Turn * Total).GetNormalized();
			FTransform Moved = Original[i];
			Moved.SetLocation(Joint);
			Moved.SetRotation((Total * Original[i].GetRotation()).GetNormalized());
			OutBoneTransforms.Add(FBoneTransform(Index[i], Moved));
		}
	}
}

bool FMakeHumanSeverNode::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return !Names.IsEmpty();
}

void FMakeHumanSeverNode::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FBoneContainer& Container = Output.Pose.GetPose().GetBoneContainer();
	TArray<FCompactPoseBoneIndex, TInlineAllocator<12>> Cut;
	for (const FName& Name : Names)
	{
		FBoneReference Bone;
		Bone.BoneName = Name;
		if (Bone.Initialize(Container) && Bone.IsValidToEvaluate(Container))
		{
			Cut.Add(Bone.GetCompactPoseIndex(Container));
		}
	}
	for (const FCompactPoseBoneIndex Index : Cut)
	{
		// A bone under another cut is folded with it already.
		bool bUnderAnother = false;
		for (const FCompactPoseBoneIndex Other : Cut)
		{
			bUnderAnother = bUnderAnother || (Other != Index && Container.BoneIsChildOf(Index, Other));
		}
		if (bUnderAnother)
		{
			continue;
		}
		// A clean cut (GoreCut.h): the bone straightened onto its parent and flattened across the limb at the joint, so
		// the stump keeps its width and ends flat under its cap; everything below it folds into the middle of the cut.
		// Where that can't be done the bone folds to a point as before.
		const FCompactPoseBoneIndex Parent = Container.GetParentBoneIndex(Index);
		const FMeshPoseBoneIndex MeshIndex = Container.MakeMeshPoseIndex(Index);
		FTransform Cut = Output.Pose.GetComponentSpaceTransform(Index);
		bool bFlat = false;
		if (Parent.IsValid() && MeshIndex.IsValid() && MeshIndex.GetInt() < Container.GetReferenceSkeleton().GetNum())
		{
			const FReferenceSkeleton& Ref = Container.GetReferenceSkeleton();
			bFlat = GoreCut::FlattenCut(Output.Pose.GetComponentSpaceTransform(Parent), Ref.GetRefBonePose()[MeshIndex.GetInt()],
				GoreCut::AxisLocal(Ref, MeshIndex.GetInt()), Cut);
		}
		if (!bFlat)
		{
			Cut.SetScale3D(FVector(GoreCut::Flat));
		}
		OutBoneTransforms.Add(FBoneTransform(Index, Cut));
		const FTransform Gone(Cut.GetRotation(), Cut.GetTranslation(), FVector(GoreCut::Flat));
		for (FCompactPoseBoneIndex Below(Index.GetInt() + 1); Below.GetInt() < Container.GetCompactPoseNumBones(); ++Below)
		{
			if (Container.BoneIsChildOf(Below, Index))
			{
				OutBoneTransforms.Add(FBoneTransform(Below, Gone));
			}
		}
	}
	OutBoneTransforms.Sort(FCompareBoneTransformIndex());
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
		Hair.Chains.Reset();
		// Belly, thighs and upper arms: mh_common.add_body_soft_bones (bodies exported before it have none).
		for (const FMhSoftPart& Part : MhSoftParts)
		{
			if (Ref.FindBoneIndex(Part.Bone) != INDEX_NONE)
			{
				FMakeHumanJiggleNode::FBody& Body = Jiggle.Bodies.AddDefaulted_GetRef();
				Body.Bone.BoneName = Part.Bone;
				Body.Stiffness = Body.BaseStiffness = Part.Stiffness;
				Body.Damping = Part.Damping;
				Body.MaxOffset = Body.BaseMaxOffset = Part.MaxOffset;
				Body.bBodySoft = Part.bBodySoft;
				Body.Squash = Part.Squash;
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
		// Long scalp hair (mh_common.add_hair_bones: hair_c1_1 ... chains down the back, back corners and sides, shared by
		// the long, bob and braid styles): stiffer and in a narrower cone than the ponytail, so the sheet sways and does
		// not fly up into the body.
		for (int32 c = 1; c < 10; ++c)
		{
			const FName First(*FString::Printf(TEXT("hair_c%d_1"), c));
			if (Ref.FindBoneIndex(First) == INDEX_NONE)
			{
				continue;
			}
			FName Last = First;
			for (int32 i = 2; i < 12 && Ref.FindBoneIndex(FName(*FString::Printf(TEXT("hair_c%d_%d"), c, i))) != INDEX_NONE; ++i)
			{
				Last = FName(*FString::Printf(TEXT("hair_c%d_%d"), c, i));
			}
			// Our own spring (FMakeHumanHairNode), not AnimDynamics: those chains flung the hair into a sheet now and then.
			TArray<FBoneReference>& Chain = Hair.Chains.AddDefaulted_GetRef();
			for (int32 i = 1; i < 12 && Ref.FindBoneIndex(FName(*FString::Printf(TEXT("hair_c%d_%d"), c, i))) != INDEX_NONE; ++i)
			{
				Chain.Emplace(FName(*FString::Printf(TEXT("hair_c%d_%d"), c, i)));
			}
			(void)Last;
		}
		Hair.Head.BoneName = TEXT("head");
		// A tail (mh_export.py's add_tail: tail_01 ... under the pelvis): its own swish (FMakeHumanTailNode).
		Tail.Bones.Reset();
		for (int32 i = 1; i < 30 && Ref.FindBoneIndex(FName(*FString::Printf(TEXT("tail_%02d"), i))) != INDEX_NONE; ++i)
		{
			Tail.Bones.Emplace(FName(*FString::Printf(TEXT("tail_%02d"), i)));
		}
	}
	// Creator weapons' two-handed grip: hand_l to a point in hand_r's space, the elbow towards a world point (alpha 0 = off).
	const bool bHandIK = Asset && GMakeHumanHandIK.GetValueOnGameThread() != 0 && Asset->GetRefSkeleton().FindBoneIndex(TEXT("hand_l")) != INDEX_NONE &&
		Asset->GetRefSkeleton().FindBoneIndex(TEXT("hand_r")) != INDEX_NONE;
	if (bHandIK)
	{
		LeftHandIK.IKBone.BoneName = TEXT("hand_l");
		LeftHandIK.EffectorTarget = FBoneSocketTarget(TEXT("hand_r"));
		LeftHandIK.EffectorLocationSpace = BCS_BoneSpace;
		LeftHandIK.JointTargetLocationSpace = BCS_WorldSpace;
		LeftHandIK.Alpha = 0.0f;
		RightHandIK.IKBone.BoneName = TEXT("hand_r");
		RightHandIK.EffectorLocationSpace = BCS_ComponentSpace;
		RightHandIK.JointTargetLocationSpace = BCS_ComponentSpace;
		RightHandIK.bAllowStretching = false;
		RightHandIK.Alpha = 0.0f;
	}
	// Always through component space: the sever node at the end must be in the graph before anything is cut off (the
	// engine keeps the root node it is given here).
	bPhysics = true;
	if (bPhysics)
	{
		ToComponent.LocalPose.SetLinkNode(&Retarget);
		FAnimNode_Base* Previous = &ToComponent;
		if (bHandIK)
		{
			RightHandIK.ComponentPose.SetLinkNode(Previous);
			Previous = &RightHandIK;
			LeftHandIK.ComponentPose.SetLinkNode(Previous);
			Previous = &LeftHandIK;
		}
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
		if (!Hair.Chains.IsEmpty())
		{
			Hair.ComponentPose.SetLinkNode(Previous);
			Previous = &Hair;
		}
		for (const TSharedPtr<FAnimNode_AnimDynamics>& Node : Dynamics)
		{
			Node->ComponentPose.SetLinkNode(Previous);
			Previous = Node.Get();
		}
		Sever.ComponentPose.SetLinkNode(Previous);
		Previous = &Sever;
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
		Sever.Names = MakeHuman->GetSeveredBones();
		LeftHandIK.EffectorLocationSpace = MakeHuman->bLeftReach ? BCS_ComponentSpace : BCS_BoneSpace;
		LeftHandIK.JointTargetLocationSpace = MakeHuman->bLeftReach ? BCS_ComponentSpace : BCS_WorldSpace;
		LeftHandIK.Alpha = MakeHuman->LeftGripAlpha;
		LeftHandIK.EffectorLocation = MakeHuman->LeftGripOffset;
		LeftHandIK.JointTargetLocation = MakeHuman->LeftElbowWorld;
		RightHandIK.Alpha = MakeHuman->RightReachAlpha;
		RightHandIK.EffectorLocation = MakeHuman->RightReachTarget;
		RightHandIK.JointTargetLocation = MakeHuman->RightElbowComponent;
		// Hits and blasts since the last update; the node uses them on its next evaluation.
		if (UMakeHumanAnimInstance* Kicked = Cast<UMakeHumanAnimInstance>(Instance))
		{
			Jiggle.Kicks = MoveTemp(Kicked->PendingKicks);
			Kicked->PendingKicks.Reset();
		}
		const float Scale = FMath::Max(0.0f, GMakeHumanSoftScale.GetValueOnGameThread());
		for (FMakeHumanKick& Kick : Jiggle.Kicks)
		{
			Kick.Velocity *= Scale;
			Kick.Speed *= Scale;
		}
	}
	// The soft parts fade out between Pawns.SoftBodyNear and Pawns.SoftBodyFar from the camera (the view last drawn).
	Jiggle.Fade = 1.0f;
	const UWorld* World = Instance ? Instance->GetWorld() : nullptr;
	const USkeletalMeshComponent* Mesh = Instance ? Instance->GetSkelMeshComponent() : nullptr;
	if (World && Mesh && !World->ViewLocationsRenderedLastFrame.IsEmpty())
	{
		float Closest = TNumericLimits<float>::Max();
		for (const FVector& View : World->ViewLocationsRenderedLastFrame)
		{
			Closest = FMath::Min(Closest, static_cast<float>(FVector::Dist(View, Mesh->GetComponentLocation())));
		}
		const float Near = GMakeHumanSoftNear.GetValueOnGameThread();
		const float Far = FMath::Max(GMakeHumanSoftFar.GetValueOnGameThread(), Near + 1.0f);
		Jiggle.Fade = 1.0f - FMath::Clamp((Closest - Near) / (Far - Near), 0.0f, 1.0f);
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

void UMakeHumanAnimInstance::SetSoftTissueEnabled(bool bEnabled)
{
	GetProxyOnGameThread<FMakeHumanAnimProxy>().Jiggle.bEnabled = bEnabled;
}

void UMakeHumanAnimInstance::SetBreastJiggle(float Amount)
{
	const float A = FMath::Clamp(Amount, 0.0f, 1.0f);
	for (FMakeHumanJiggleNode::FBody& Body : GetProxyOnGameThread<FMakeHumanAnimProxy>().Jiggle.Bodies)
	{
		if (Body.Bone.BoneName.ToString().StartsWith(TEXT("breast")))
		{
			// About 2 Hz and well damped at 0; about 1.3 Hz, a third of critical damping, and more room at 1.
			Body.Stiffness = Body.BaseStiffness = FMath::Lerp(150.0f, 65.0f, A);
			Body.Damping = FMath::Lerp(5.5f, 3.2f, A);
			Body.MaxOffset = Body.BaseMaxOffset = FMath::Lerp(4.0f, 7.5f, A);
		}
	}
}

void UMakeHumanAnimInstance::SetBodySoftness(float Weight, float Muscle, float Age)
{
	// 0.5 for the default body; heavy and soft towards 1, lean and muscular towards 0; age loosens a little.
	BodySoftness = FMath::Clamp(0.5f + 0.4f * Weight - 0.3f * Muscle + 0.1f * Age, 0.0f, 1.0f);
	ApplySoftness();
}

void UMakeHumanAnimInstance::ApplySoftness()
{
	// Softer: a lower, slower spring with more room (about 0.65 times the frequency and 1.8 times the room at 1); firmer:
	// quicker and tighter (1.2 times and 0.45 times at 0).
	const float S = BodySoftness;
	const float StiffnessScale = S < 0.5f ? FMath::Lerp(1.45f, 1.0f, S * 2.0f) : FMath::Lerp(1.0f, 0.42f, (S - 0.5f) * 2.0f);
	const float RoomScale = S < 0.5f ? FMath::Lerp(0.45f, 1.0f, S * 2.0f) : FMath::Lerp(1.0f, 1.8f, (S - 0.5f) * 2.0f);
	for (FMakeHumanJiggleNode::FBody& Body : GetProxyOnGameThread<FMakeHumanAnimProxy>().Jiggle.Bodies)
	{
		Body.Stiffness = Body.bBodySoft ? Body.BaseStiffness * StiffnessScale : Body.BaseStiffness;
		Body.MaxOffset = Body.bBodySoft ? Body.BaseMaxOffset * RoomScale : Body.BaseMaxOffset;
	}
}

void UMakeHumanAnimInstance::AddHitKick(const FVector& At, const FVector& Velocity, float Radius)
{
	if (PendingKicks.Num() > 32)
	{
		return;  // nobody is drawing this body: don't let kicks pile up
	}
	PendingKicks.Add({At, Velocity, 0.0f, Radius, false});
	PendingKicks.Add({At, Velocity * 0.1f, 0.0f, 250.0f, false});  // the whole body feels a tenth of it
	// A dent in the skin at the hit, riding on the nearest bone: deeper for a harder hit (up to 5 cm), about half as
	// wide as the kick's reach.
	const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	const float Depth = FMath::Min(static_cast<float>(Velocity.Size()) * 0.03f, 5.0f) * FMath::Max(0.0f, GMakeHumanDentScale.GetValueOnGameThread());
	if (Mesh && Depth > 0.2f)
	{
		const FName Bone = Mesh->FindClosestBone(At);
		if (Bone != NAME_None)
		{
			FDent& Dent = Dents[NextDent];
			NextDent = (NextDent + 1) % UE_ARRAY_COUNT(Dents);
			Dent.Bone = Bone;
			Dent.Local = Mesh->GetSocketTransform(Bone).InverseTransformPosition(At);
			Dent.Depth = Depth;
			Dent.Radius = FMath::Clamp(Radius * 0.45f, 8.0f, 18.0f);
			Dent.Age = 0.0f;
		}
	}
}

void UMakeHumanAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	// Skin dents: pushed in over 40 ms and held 0.3 s, then springing back past flat (a small bulge) and settling,
	// fading out with the soft body's distance fade. The skin material reads them from custom primitive data 16 to 25.
	USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	if (!Mesh)
	{
		return;
	}
	const float Fade = GetProxyOnGameThread<FMakeHumanAnimProxy>().Jiggle.Fade;
	bool bAny = false;
	for (int32 d = 0; d < static_cast<int32>(UE_ARRAY_COUNT(Dents)); ++d)
	{
		FDent& Dent = Dents[d];
		float Now = 0.0f;
		FVector At = FVector::ZeroVector;
		if (Dent.Depth > 0.0f)
		{
			Dent.Age += DeltaSeconds;
			const float T = Dent.Age;
			// Held 0.3 s before it springs back (it lasted a tenth of a second and read as a shadow flicker), then slower.
			const float Back = FMath::Max(T - 0.3f, 0.0f);
			Now = T < 0.04f ? Dent.Depth * T / 0.04f : Dent.Depth * FMath::Exp(-Back * 3.5f) * FMath::Cos(Back * 9.0f);
			Now *= Fade;
			if (T > 1.8f)
			{
				Dent.Depth = 0.0f;
				Now = 0.0f;
			}
			At = Mesh->GetSocketTransform(Dent.Bone).TransformPosition(Dent.Local);
			DeepestDent = FMath::Max(DeepestDent, Now);
		}
		bAny |= Now != 0.0f;
		const int32 First = 16 + d * 5;  // x, y, z, depth, radius
		if (Now != 0.0f || bDentsShown)
		{
			Mesh->SetCustomPrimitiveDataVector3(First, At);
			Mesh->SetCustomPrimitiveDataFloat(First + 3, Now);
			Mesh->SetCustomPrimitiveDataFloat(First + 4, Dent.Radius);
		}
	}
	bDentsShown = bAny;
}

float UMakeHumanAnimInstance::TakeDeepestDent()
{
	const float Deepest = DeepestDent;
	DeepestDent = 0.0f;
	return Deepest;
}

void UMakeHumanAnimInstance::AddBlastKick(const FVector& At, float Speed, float Radius)
{
	if (PendingKicks.Num() > 32)
	{
		return;
	}
	PendingKicks.Add({At, FVector::ZeroVector, Speed, Radius, true});
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
