#include "MakeHumanAnimInstance.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Retargeter/IKRetargeter.h"

namespace
{
	TAutoConsoleVariable<int32> GMakeHumanPhysics(TEXT("Pawns.MakeHumanPhysics"), 1,
		TEXT("1 runs jiggle (breast bones) and hair sway (ponytail bones) on MakeHuman characters; read when a character's animation starts."), ECVF_Default);
	TAutoConsoleVariable<int32> GMakeHumanHandIK(TEXT("Pawns.MakeHumanHandIK"), 1,
		TEXT("1 lets the creator's weapons put the left hand on a two-handed hilt (two-bone IK, off until asked for); read when a character's animation starts."), ECVF_Default);
	TAutoConsoleVariable<int32> GMakeHumanArmLimits(TEXT("Pawns.ArmLimits"), 1,
		TEXT("1 keeps every solved arm inside human joint limits (forearm twist, wrist bend); 0 lets the hand take any turn its goal asks."), ECVF_Default);
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
		FTransform Folded = Output.Pose.GetComponentSpaceTransform(Index);
		Folded.SetScale3D(FVector(0.001));
		OutBoneTransforms.Add(FBoneTransform(Index, Folded));
	}
	OutBoneTransforms.Sort(FCompareBoneTransformIndex());
}

namespace
{
	colony::arm::V3 MhV(const FVector& V)
	{
		return colony::arm::V3{V.X, V.Y, V.Z};
	}

	FVector MhF(const colony::arm::V3& V)
	{
		return FVector(V.x, V.y, V.z);
	}

	// A bone's rest-pose transform in component space (its local rest transforms up the chain).
	FTransform MhRefComponent(const FBoneContainer& Container, FCompactPoseBoneIndex Index)
	{
		FTransform T = Container.GetRefPoseTransform(Index);
		FCompactPoseBoneIndex Parent = Container.GetParentBoneIndex(Index);
		while (Parent.IsValid())
		{
			T = T * Container.GetRefPoseTransform(Parent);
			Parent = Container.GetParentBoneIndex(Parent);
		}
		return T;
	}

	// A hand's finger direction (wrist to the middle knuckle) and palm normal (out of the palm) from its knuckles: the
	// palm side from the knuckles alone (fingers x across: the same rule as HoldDraw::FistFrame).
	bool MhHandFrame(const FVector& Hand, const FVector& Index, const FVector& Pinky, const FVector& Middle, const FVector* IndexTip, bool bRight,
					 FVector& OutFingers, FVector& OutPalm)
	{
		const FVector Across = (Index - Pinky).GetSafeNormal();
		FVector Fingers = (Middle - Hand).GetSafeNormal();
		Fingers = (Fingers - Across * FVector::DotProduct(Fingers, Across)).GetSafeNormal();
		if (Across.IsNearlyZero() || Fingers.IsNearlyZero())
		{
			return false;
		}
		FVector Palm = FVector::CrossProduct(Fingers, Across).GetSafeNormal();
		// The palm's side from the knuckles alone (as HoldDraw::FistFrame): the index finger's curl turned an open hand's
		// palm inside out, and the wrist limits then spun the hand round.
		(void)IndexTip;
		const float Side = bRight ? 1.0f : -1.0f;
		OutFingers = (Middle - Hand).GetSafeNormal();
		OutPalm = Palm * Side;
		// Square to the fingers.
		OutPalm = (OutPalm - OutFingers * FVector::DotProduct(OutPalm, OutFingers)).GetSafeNormal();
		return !OutPalm.IsNearlyZero();
	}
}

bool FMakeHumanArmsNode::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	bool bAny = false;
	for (int32 Side = 0; Side < 2; ++Side)
	{
		bAny = bAny || (Arms[Side].bValid && Goals[Side].Alpha > 0.0f);
	}
	return bAny;
}

void FMakeHumanArmsNode::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	Spine.Initialize(RequiredBones);
	Neck.Initialize(RequiredBones);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		FArm& Arm = Arms[Side];
		Arm.bValid = Arm.Upper.Initialize(RequiredBones) && Arm.Lower.Initialize(RequiredBones) && Arm.Hand.Initialize(RequiredBones) &&
			Arm.Index.Initialize(RequiredBones) && Arm.Pinky.Initialize(RequiredBones) && Arm.Middle.Initialize(RequiredBones);
		Arm.IndexTip.Initialize(RequiredBones);
		Arm.Clavicle.Initialize(RequiredBones);
		Arm.bValid = Arm.bValid && Arm.Upper.IsValidToEvaluate(RequiredBones) && Arm.Lower.IsValidToEvaluate(RequiredBones) &&
			Arm.Hand.IsValidToEvaluate(RequiredBones) && Arm.Index.IsValidToEvaluate(RequiredBones) && Arm.Pinky.IsValidToEvaluate(RequiredBones) &&
			Arm.Middle.IsValidToEvaluate(RequiredBones);
		Arm.NeutralFlex = 0.0;
		Arm.NeutralDeviation = 0.0;
		if (!Arm.bValid)
		{
			continue;
		}
		// The rest pose's wrist readings: its knuckles are seldom exactly on the forearm's line.
		const auto Ref = [&RequiredBones](const FBoneReference& B) { return MhRefComponent(RequiredBones, B.GetCompactPoseIndex(RequiredBones)).GetLocation(); };
		const FVector Upper = Ref(Arm.Upper), Lower = Ref(Arm.Lower), Hand = Ref(Arm.Hand);
		FVector Fingers, Palm;
		if (MhHandFrame(Hand, Ref(Arm.Index), Ref(Arm.Pinky), Ref(Arm.Middle), nullptr, Side == 1, Fingers, Palm))
		{
			const colony::arm::ArmAngles A = colony::arm::measureArm(MhV(Upper), MhV(Lower), MhV(Hand), MhV(Fingers), MhV(Palm), colony::arm::V3{});
			// Only a small offset is the skeleton's own (a rest pose with a bent wrist would hide real bends).
			Arm.NeutralFlex = FMath::Clamp(A.wristFlex, -15.0, 15.0);
			Arm.NeutralDeviation = FMath::Clamp(A.wristDeviation, -15.0, 15.0);
		}
	}
}

void FMakeHumanArmsNode::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	using namespace colony::arm;
	const FBoneContainer& Container = Output.Pose.GetPose().GetBoneContainer();
	const Limits Human;
	FTransform FinalHand[2];
	bool bFinal[2] = {false, false};
	bSolved[0] = bSolved[1] = false;
	// A hand given in the other hand's space goes after that one.
	int32 Order[2] = {1, 0};
	if (Goals[1].Space == EMakeHumanGoalSpace::OtherHand && Goals[0].Space != EMakeHumanGoalSpace::OtherHand)
	{
		Order[0] = 0;
		Order[1] = 1;
	}
	const V3 BodyUp = MhV(Up), BodyForward = MhV(Forward), BodyRight = MhV(Right);

	// The chest turns about the body's up axis (at most 40 degrees either way) when that brings grips into reach: a
	// long gun's fore-end is past the far arm's reach with the shoulders square, and a shooter blades the body (left
	// shoulder forward for a right-handed hold). The neck turns back so the head keeps looking where it did.
	const auto GoalInComponent = [&Output, &Container](const FMakeHumanHandGoal& G, FTransform& OutT)
	{
		OutT = G.Target;
		if (G.Space == EMakeHumanGoalSpace::Anchor)
		{
			FBoneReference AnchorBone;
			AnchorBone.BoneName = G.Anchor;
			if (!AnchorBone.Initialize(Container) || !AnchorBone.IsValidToEvaluate(Container))
			{
				return false;
			}
			OutT.AddToTranslation(Output.Pose.GetComponentSpaceTransform(AnchorBone.GetCompactPoseIndex(Container)).GetLocation());
		}
		return G.Space != EMakeHumanGoalSpace::OtherHand;
	};
	FQuat ChestQuat = FQuat::Identity;
	FVector ChestPivot = FVector::ZeroVector;
	ChestTurn = 0.0f;
	const FVector UpAxis = Up.GetSafeNormal();
	if (Spine.IsValidToEvaluate(Container) && !UpAxis.IsNearlyZero())
	{
		const FCompactPoseBoneIndex SpineI = Spine.GetCompactPoseIndex(Container);
		const FTransform SpineT = Output.Pose.GetComponentSpaceTransform(SpineI);
		ChestPivot = SpineT.GetLocation();
		FVector ChestShoulder[2], ChestGoal[2];
		double ChestReach[2] = {0.0, 0.0}, ChestNear[2] = {0.0, 0.0};
		bool bChestUse[2] = {false, false};
		float ChestAlpha = 0.0f;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FArm& A = Arms[Side];
			FTransform GoalT;
			if (!A.bValid || Goals[Side].Alpha <= 0.0f || !GoalInComponent(Goals[Side], GoalT))
			{
				continue;
			}
			const FVector Sh = Output.Pose.GetComponentSpaceTransform(A.Upper.GetCompactPoseIndex(Container)).GetLocation();
			const FVector El = Output.Pose.GetComponentSpaceTransform(A.Lower.GetCompactPoseIndex(Container)).GetLocation();
			const FVector Wr = Output.Pose.GetComponentSpaceTransform(A.Hand.GetCompactPoseIndex(Container)).GetLocation();
			const double Len = FVector::Dist(Sh, El) + FVector::Dist(El, Wr);
			ChestShoulder[Side] = Sh;
			ChestGoal[Side] = GoalT.GetLocation();
			ChestReach[Side] = Len * Human.reachShare;
			ChestNear[Side] = Len * 0.45;  // closer than this bunches the arm up
			bChestUse[Side] = Len > 1.0;
			ChestAlpha = FMath::Max(ChestAlpha, FMath::Clamp(Goals[Side].Alpha, 0.0f, 1.0f));
		}
		const auto ChestCost = [&](double Radians)
		{
			const FQuat Q(UpAxis, Radians);
			double Cost = 0.01 * FMath::Abs(FMath::RadiansToDegrees(Radians));
			for (int32 Side = 0; Side < 2; ++Side)
			{
				if (bChestUse[Side])
				{
					const double D = FVector::Dist(ChestPivot + Q.RotateVector(ChestShoulder[Side] - ChestPivot), ChestGoal[Side]);
					Cost += FMath::Max(0.0, D - ChestReach[Side]) + FMath::Max(0.0, ChestNear[Side] - D);
				}
			}
			return Cost;
		};
		if (bChestUse[0] || bChestUse[1])
		{
			double Best = 0.0, BestCost = ChestCost(0.0);
			for (int32 Step = -16; Step <= 16; ++Step)
			{
				const double Radians = FMath::DegreesToRadians(2.5 * Step);
				const double Cost = ChestCost(Radians);
				if (Cost < BestCost - 1e-6)
				{
					Best = Radians;
					BestCost = Cost;
				}
			}
			if (FMath::Abs(Best) > 1e-4)
			{
				ChestQuat = FQuat(UpAxis, Best * ChestAlpha);
				ChestTurn = static_cast<float>(FMath::RadiansToDegrees(Best * ChestAlpha));
				OutBoneTransforms.Add(FBoneTransform(SpineI, FTransform((ChestQuat * SpineT.GetRotation()).GetNormalized(), ChestPivot, SpineT.GetScale3D())));
				if (Neck.IsValidToEvaluate(Container))
				{
					const FCompactPoseBoneIndex NeckI = Neck.GetCompactPoseIndex(Container);
					const FTransform NeckT = Output.Pose.GetComponentSpaceTransform(NeckI);
					OutBoneTransforms.Add(FBoneTransform(NeckI, FTransform(NeckT.GetRotation(), ChestPivot + ChestQuat.RotateVector(NeckT.GetLocation() - ChestPivot), NeckT.GetScale3D())));
				}
			}
		}
	}
	// A bone under the chest, carried by its turn.
	const auto ChestCarry = [&ChestQuat, &ChestPivot](FTransform& T)
	{
		T.SetLocation(ChestPivot + ChestQuat.RotateVector(T.GetLocation() - ChestPivot));
		T.SetRotation((ChestQuat * T.GetRotation()).GetNormalized());
	};

	for (const int32 Side : Order)
	{
		const FMakeHumanHandGoal& Goal = Goals[Side];
		FArm& Arm = Arms[Side];
		const float GoalAlpha = FMath::Clamp(Goal.Alpha, 0.0f, 1.0f);
		if (!Arm.bValid || GoalAlpha <= 0.0f)
		{
			continue;
		}
		const bool bRight = Side == 1;
		const FCompactPoseBoneIndex UpperI = Arm.Upper.GetCompactPoseIndex(Container);
		const FCompactPoseBoneIndex LowerI = Arm.Lower.GetCompactPoseIndex(Container);
		const FCompactPoseBoneIndex HandI = Arm.Hand.GetCompactPoseIndex(Container);
		// As animated (the hand's frame below is read from these: its knuckles move rigidly with the hand bone), then
		// carried by the chest's turn.
		const FTransform HandAnimated = Output.Pose.GetComponentSpaceTransform(HandI);
		FTransform UpperT = Output.Pose.GetComponentSpaceTransform(UpperI);
		FTransform LowerT = Output.Pose.GetComponentSpaceTransform(LowerI);
		FTransform HandT = HandAnimated;
		ChestCarry(UpperT);
		ChestCarry(LowerT);
		ChestCarry(HandT);
		const double UpperLen = FVector::Dist(UpperT.GetLocation(), LowerT.GetLocation());
		const double LowerLen = FVector::Dist(LowerT.GetLocation(), HandT.GetLocation());
		if (UpperLen < 1.0 || LowerLen < 1.0)
		{
			continue;
		}

		// The goal in component space.
		FTransform Target = Goal.Target;
		if (Goal.Space == EMakeHumanGoalSpace::Anchor)
		{
			FBoneReference AnchorBone;
			AnchorBone.BoneName = Goal.Anchor;
			if (AnchorBone.Initialize(Container) && AnchorBone.IsValidToEvaluate(Container))
			{
				Target.AddToTranslation(Output.Pose.GetComponentSpaceTransform(AnchorBone.GetCompactPoseIndex(Container)).GetLocation());
			}
		}
		else if (Goal.Space == EMakeHumanGoalSpace::OtherHand)
		{
			const int32 Other = 1 - Side;
			FTransform OtherHand = bFinal[Other] ? FinalHand[Other] : FTransform::Identity;
			if (!bFinal[Other])
			{
				if (!Arms[Other].bValid)
				{
					continue;
				}
				OtherHand = Output.Pose.GetComponentSpaceTransform(Arms[Other].Hand.GetCompactPoseIndex(Container));
				ChestCarry(OtherHand);
			}
			OtherHand.SetScale3D(FVector::OneVector);
			Target = Goal.Target * OtherHand;
		}
		GoalCS[Side] = Target;

		// A grip past the arm's reach brings the shoulder girdle forward towards it (the clavicle turns, as a person
		// reaching out does), at most 25 degrees, before the arm is solved; the arm itself is never stretched.
		ShoulderReach[Side] = 0.0f;
		if (Arm.Clavicle.IsValidToEvaluate(Container))
		{
			const FCompactPoseBoneIndex ClavI = Arm.Clavicle.GetCompactPoseIndex(Container);
			FTransform ClavT = Output.Pose.GetComponentSpaceTransform(ClavI);
			ChestCarry(ClavT);
			const FVector Pivot = ClavT.GetLocation();
			const FVector Shoulder = UpperT.GetLocation();
			const double MaxReach = (UpperLen + LowerLen) * Human.reachShare;
			const double Over = FVector::Dist(Shoulder, Target.GetLocation()) - MaxReach;
			const FVector ToShoulder = Shoulder - Pivot;
			const FVector ToGoal = Target.GetLocation() - Pivot;
			const FVector Axis = FVector::CrossProduct(ToShoulder, ToGoal).GetSafeNormal();
			if (Over > 0.0 && ToShoulder.Size() > 2.0 && !Axis.IsNearlyZero())
			{
				// Turn by as much as closes the gap (found along the shoulder's arc), at most 25 degrees, and never past
				// the line to the goal.
				const double Between = FMath::Acos(FMath::Clamp(FVector::DotProduct(ToShoulder.GetSafeNormal(), ToGoal.GetSafeNormal()), -1.0, 1.0));
				double Lo = 0.0, Hi = FMath::Min(FMath::DegreesToRadians(25.0), Between);
				for (int32 Step = 0; Step < 14; ++Step)
				{
					const double Mid = (Lo + Hi) * 0.5;
					const FVector Moved = Pivot + FQuat(Axis, Mid).RotateVector(ToShoulder);
					(FVector::Dist(Moved, Target.GetLocation()) > MaxReach ? Lo : Hi) = Mid;
				}
				const double Angle = Hi * GoalAlpha;
				const FQuat Turn(Axis, Angle);
				const auto Swing = [&Turn, &Pivot](FTransform& T)
				{
					T.SetLocation(Pivot + Turn.RotateVector(T.GetLocation() - Pivot));
					T.SetRotation((Turn * T.GetRotation()).GetNormalized());
				};
				FTransform ClavNew = ClavT;
				ClavNew.SetRotation((Turn * ClavT.GetRotation()).GetNormalized());
				Swing(UpperT);
				Swing(LowerT);
				Swing(HandT);
				OutBoneTransforms.Add(FBoneTransform(ClavI, FTransform(ClavNew.GetRotation(), Pivot, ClavT.GetScale3D())));
				ShoulderReach[Side] = static_cast<float>(FVector::Dist(Shoulder, UpperT.GetLocation()));
			}
		}
		// Still out of reach with the other hand on the same weapon: the second hand (the support hand) slides back along the weapon towards
		// the other grip (a support hand further back on the fore-end), at most 15 cm.
		{
			const double MaxReach = (UpperLen + LowerLen) * Human.reachShare;
			const double Over = FVector::Dist(UpperT.GetLocation(), Target.GetLocation()) - MaxReach;
			FTransform OtherGoal;
			const int32 Other = 1 - Side;
			if (Side == Order[1] && Over > 0.0 && Arms[Other].bValid && Goals[Other].Alpha > 0.0f && GoalInComponent(Goals[Other], OtherGoal))
			{
				const FVector Along = OtherGoal.GetLocation() - Target.GetLocation();
				const double Slide = FMath::Min3(Over, 15.0, Along.Size() * 0.6);
				if (Slide > 0.0)
				{
					Target.AddToTranslation(Along.GetSafeNormal() * Slide);
					GoalCS[Side] = Target;
				}
			}
		}
		const FVector S = UpperT.GetLocation(), E0 = LowerT.GetLocation(), W0 = HandT.GetLocation();
		const FQuat WantRot = Goal.bRotation ? Target.GetRotation() : HandT.GetRotation();

		// The elbow's pole, then the hinge solve (no stretching, never folded past the limit).
		V3 Pole;
		if (Goal.bElbowPoint)
		{
			Pole = MhV(Goal.ElbowPoint - (S + Target.GetLocation()) * 0.5);
		}
		else if (!Goal.Pole.IsNearlyZero())
		{
			Pole = MhV(Goal.Pole);
		}
		else
		{
			Pole = naturalPole(MhV(S), MhV(Target.GetLocation()), BodyUp, BodyForward, BodyRight, bRight);
		}
		const ArmSolve Solve = solveArm(MhV(S), UpperLen, LowerLen, MhV(Target.GetLocation()), Pole, Human);
		const FVector E1 = MhF(Solve.elbow), W1 = MhF(Solve.wrist);
		const FVector U0 = (E0 - S).GetSafeNormal(), L0 = (W0 - E0).GetSafeNormal();
		const FVector U1 = (E1 - S).GetSafeNormal(), L1 = (W1 - E1).GetSafeNormal();

		// The elbow's hinge as the animation has it (kept from the last bent pose when the clip's arm is straight).
		const V3 Seen = medialHinge(MhV(U0), MhV(L0), BodyUp, BodyForward, BodyRight, bRight);
		if (length(Seen) > 0.5)
		{
			Arm.HingeInUpper = UpperT.GetRotation().UnrotateVector(MhF(Seen));
			Arm.bHinge = true;
		}
		FVector HingeNow;
		if (Arm.bHinge)
		{
			HingeNow = UpperT.GetRotation().RotateVector(Arm.HingeInUpper);
		}
		else
		{
			// Never seen bent: as if the forearm swung forward from the upper arm.
			const FVector Ahead = (Forward - U0 * FVector::DotProduct(Forward, U0)).GetSafeNormal();
			HingeNow = MhF(medialHinge(MhV(U0), MhV(Ahead.IsNearlyZero() ? Up : Ahead), BodyUp, BodyForward, BodyRight, bRight));
		}

		// Upper arm: turned onto its new line, then rolled so its hinge is the new bend's (the elbow bends only as a hinge).
		const FQuat R1 = FQuat::FindBetweenNormals(U0, U1);
		FQuat UpperRot = R1 * UpperT.GetRotation();
		const FVector HingeWant = MhF(medialHinge(MhV(U1), MhV(L1), BodyUp, BodyForward, BodyRight, bRight));
		if (!HingeWant.IsNearlyZero() && !HingeNow.IsNearlyZero())
		{
			const FVector H1 = R1.RotateVector(HingeNow);
			const FVector A = (H1 - U1 * FVector::DotProduct(H1, U1)).GetSafeNormal();
			const FVector B = (HingeWant - U1 * FVector::DotProduct(HingeWant, U1)).GetSafeNormal();
			if (!A.IsNearlyZero() && !B.IsNearlyZero())
			{
				const double Roll = FMath::Atan2(FVector::DotProduct(FVector::CrossProduct(A, B), U1), FVector::DotProduct(A, B));
				UpperRot = FQuat(U1, Roll) * UpperRot;
			}
		}
		// Forearm: carried by the upper arm, then turned about the hinge onto its new line.
		const FQuat UpperDelta = UpperRot * UpperT.GetRotation().Inverse();
		const FQuat LowerCarried = UpperDelta * LowerT.GetRotation();
		const FVector LCarried = UpperDelta.RotateVector(L0).GetSafeNormal();
		const FQuat LowerRot = FQuat::FindBetweenNormals(LCarried, L1) * LowerCarried;

		// Hand: the goal's turn, brought inside the forearm's twist and the wrist's bend.
		FQuat HandRot = WantRot;
		const auto Loc = [&Output, &Container](const FBoneReference& B) { return Output.Pose.GetComponentSpaceTransform(B.GetCompactPoseIndex(Container)).GetLocation(); };
		FVector Tip = FVector::ZeroVector;
		const bool bTip = Arm.IndexTip.IsValidToEvaluate(Container);
		if (bTip)
		{
			Tip = Loc(Arm.IndexTip);
		}
		FVector Fingers0, Palm0;
		if (MhHandFrame(HandAnimated.GetLocation(), Loc(Arm.Index), Loc(Arm.Pinky), Loc(Arm.Middle), bTip ? &Tip : nullptr, bRight, Fingers0, Palm0))
		{
			// The frame as animated, carried to the goal's turn by the hand bone's own rotation (as animated too).
			const FQuat HandInv = HandAnimated.GetRotation().Inverse();
			const FVector FingersWant = WantRot.RotateVector(HandInv.RotateVector(Fingers0));
			const FVector PalmWant = WantRot.RotateVector(HandInv.RotateVector(Palm0));
			if (bLimits && !HingeWant.IsNearlyZero())
			{
				const HandFrame Got = clampHand(MhV(L1), MhV(HingeWant), HandFrame{MhV(FingersWant), MhV(PalmWant)}, Human, Arm.NeutralFlex,
					Arm.NeutralDeviation);
				const FVector FingersGot = MhF(Got.fingers), PalmGot = MhF(Got.palm);
				if (!FingersGot.Equals(FingersWant, 1e-5) || !PalmGot.Equals(PalmWant, 1e-5))
				{
					const FQuat From = FRotationMatrix::MakeFromXY(FingersWant, PalmWant).ToQuat();
					const FQuat To = FRotationMatrix::MakeFromXY(FingersGot, PalmGot).ToQuat();
					HandRot = (To * From.Inverse()) * WantRot;
				}
			}
			Measured[Side] = measureArm(MhV(S), MhV(E1), MhV(W1), MhV(HandRot.RotateVector(HandInv.RotateVector(Fingers0))),
				MhV(HandRot.RotateVector(HandInv.RotateVector(Palm0))), MhV(HingeWant), Arm.NeutralFlex, Arm.NeutralDeviation);
		}
		HandRot.Normalize();

		// Blended in by the goal's alpha, the bones rebuilt down the chain so they keep their lengths.
		const FQuat UpperB = FQuat::Slerp(UpperT.GetRotation(), UpperRot, GoalAlpha).GetNormalized();
		const FQuat LowerB = FQuat::Slerp(LowerT.GetRotation(), LowerRot, GoalAlpha).GetNormalized();
		const FQuat HandB = FQuat::Slerp(HandT.GetRotation(), HandRot, GoalAlpha).GetNormalized();
		const FVector EB = S + UpperB.RotateVector(UpperT.GetRotation().UnrotateVector(E0 - S));
		const FVector WB = EB + LowerB.RotateVector(LowerT.GetRotation().UnrotateVector(W0 - E0));
		OutBoneTransforms.Add(FBoneTransform(UpperI, FTransform(UpperB, S, UpperT.GetScale3D())));
		OutBoneTransforms.Add(FBoneTransform(LowerI, FTransform(LowerB, EB, LowerT.GetScale3D())));
		OutBoneTransforms.Add(FBoneTransform(HandI, FTransform(HandB, WB, HandT.GetScale3D())));
		FinalHand[Side] = FTransform(HandB, WB);
		bFinal[Side] = true;
		ShortBy[Side] = static_cast<float>(Solve.shortBy);
		bSolved[Side] = true;
	}
	OutBoneTransforms.Sort(FCompareBoneTransformIndex());
}

void UMakeHumanAnimInstance::SetLeftHandGrip(float Alpha, const FVector& InRightHandOffset, const FVector& InElbowWorld)
{
	FMakeHumanHandGoal Goal;
	Goal.Alpha = Alpha;
	Goal.Space = EMakeHumanGoalSpace::OtherHand;
	Goal.Target = FTransform(InRightHandOffset);
	if (const USkeletalMeshComponent* Mesh = GetSkelMeshComponent(); Mesh && Alpha > 0.0f)
	{
		Goal.bElbowPoint = true;
		Goal.ElbowPoint = Mesh->GetComponentTransform().InverseTransformPosition(InElbowWorld);
	}
	HandGoals[0] = Goal;
}

void UMakeHumanAnimInstance::SetRightHandReach(float Alpha, const FVector& InTargetComponent, const FVector& InElbowComponent)
{
	FMakeHumanHandGoal Goal;
	Goal.Alpha = Alpha;
	Goal.Target = FTransform(InTargetComponent);
	Goal.bElbowPoint = true;
	Goal.ElbowPoint = InElbowComponent;
	HandGoals[1] = Goal;
}

void UMakeHumanAnimInstance::SetLeftHandReach(float Alpha, const FVector& InTargetComponent, const FVector& InElbowComponent)
{
	FMakeHumanHandGoal Goal;
	Goal.Alpha = Alpha;
	Goal.Target = FTransform(InTargetComponent);
	Goal.bElbowPoint = true;
	Goal.ElbowPoint = InElbowComponent;
	HandGoals[0] = Goal;
}

bool UMakeHumanAnimInstance::GetSolvedArm(bool bRight, colony::arm::ArmAngles& Out, float& OutShortBy) const
{
	const FMakeHumanArmsNode& Node = GetProxyOnGameThread<FMakeHumanAnimProxy>().ArmsIK;
	const int32 Side = bRight ? 1 : 0;
	Out = Node.Measured[Side];
	OutShortBy = Node.ShortBy[Side];
	return Node.bSolved[Side];
}

bool UMakeHumanAnimInstance::GetSolvedGoal(bool bRight, FTransform& OutGoalWorld, float& OutShoulderReach) const
{
	const FMakeHumanArmsNode& Node = GetProxyOnGameThread<FMakeHumanAnimProxy>().ArmsIK;
	const int32 Side = bRight ? 1 : 0;
	const USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	if (!Node.bSolved[Side] || !Mesh)
	{
		return false;
	}
	OutGoalWorld = Node.GoalCS[Side] * Mesh->GetComponentTransform();
	OutGoalWorld.SetScale3D(FVector::OneVector);
	OutShoulderReach = Node.ShoulderReach[Side];
	return true;
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
	// Both arms' solver (FMakeHumanArmsNode): weapons' grips, two-handed hilts, reaches; off until a hand gets a goal.
	const bool bHandIK = Asset && GMakeHumanHandIK.GetValueOnGameThread() != 0 && Asset->GetRefSkeleton().FindBoneIndex(TEXT("hand_l")) != INDEX_NONE &&
		Asset->GetRefSkeleton().FindBoneIndex(TEXT("hand_r")) != INDEX_NONE;
	if (bHandIK)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const TCHAR* S = Side == 1 ? TEXT("r") : TEXT("l");
			FMakeHumanArmsNode::FArm& Arm = ArmsIK.Arms[Side];
			Arm.Upper.BoneName = FName(*FString::Printf(TEXT("upperarm_%s"), S));
			Arm.Lower.BoneName = FName(*FString::Printf(TEXT("lowerarm_%s"), S));
			Arm.Hand.BoneName = FName(*FString::Printf(TEXT("hand_%s"), S));
			Arm.Index.BoneName = FName(*FString::Printf(TEXT("index_01_%s"), S));
			Arm.Pinky.BoneName = FName(*FString::Printf(TEXT("pinky_01_%s"), S));
			Arm.Middle.BoneName = FName(*FString::Printf(TEXT("middle_01_%s"), S));
			Arm.IndexTip.BoneName = FName(*FString::Printf(TEXT("index_03_%s"), S));
			Arm.Clavicle.BoneName = FName(*FString::Printf(TEXT("clavicle_%s"), S));
		}
		ArmsIK.Spine.BoneName = FName(TEXT("spine_05"));
		ArmsIK.Neck.BoneName = FName(TEXT("neck_01"));
		ArmsIK.Alpha = 1.0f;
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
			ArmsIK.ComponentPose.SetLinkNode(Previous);
			Previous = &ArmsIK;
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
		ArmsIK.Goals[0] = MakeHuman->HandGoals[0];
		ArmsIK.Goals[1] = MakeHuman->HandGoals[1];
		ArmsIK.bLimits = GMakeHumanArmLimits.GetValueOnGameThread() != 0;
		// The body's axes in component space: the owner actor's (or the override's, the first-person arms' view).
		if (const USkeletalMeshComponent* Body = MakeHuman->GetSkelMeshComponent())
		{
			const AActor* Owner = Body->GetOwner();
			if (MakeHuman->bAxesOverride || Owner)
			{
				const FTransform& C = Body->GetComponentTransform();
				const FVector WUp = MakeHuman->bAxesOverride ? MakeHuman->AxesUp : Owner->GetActorUpVector();
				const FVector WForward = MakeHuman->bAxesOverride ? MakeHuman->AxesForward : Owner->GetActorForwardVector();
				const FVector WRight = MakeHuman->bAxesOverride ? MakeHuman->AxesRight : Owner->GetActorRightVector();
				ArmsIK.Up = C.InverseTransformVectorNoScale(WUp).GetSafeNormal();
				ArmsIK.Forward = C.InverseTransformVectorNoScale(WForward).GetSafeNormal();
				ArmsIK.Right = C.InverseTransformVectorNoScale(WRight).GetSafeNormal();
			}
		}
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
