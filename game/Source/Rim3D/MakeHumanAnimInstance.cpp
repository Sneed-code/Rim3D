#include "MakeHumanAnimInstance.h"

#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Retargeter/IKRetargeter.h"

namespace
{
	TAutoConsoleVariable<int32> GMakeHumanPhysics(TEXT("Pawns.MakeHumanPhysics"), 1,
		TEXT("1 runs jiggle (soft-tissue bones) and long hair on MakeHuman characters; read when a character's animation starts."), ECVF_Default);
	TAutoConsoleVariable<int32> GMakeHumanHair(TEXT("Pawns.HairPhysics"), 2,
		TEXT("Long hair on MakeHuman characters: 2 hangs, swings and rests on the body; 1 only pushed out of the body; 0 as animated (stiff)."), ECVF_Default);
	TAutoConsoleVariable<float> GMakeHumanHairSimFar(TEXT("Pawns.HairSimFar"), 2500.0f,
		TEXT("Long hair simulates fully within this many cm of the camera; further off it is only pushed out of the body."), ECVF_Default);
	TAutoConsoleVariable<float> GMakeHumanHairCollideFar(TEXT("Pawns.HairCollideFar"), 5000.0f,
		TEXT("Beyond this many cm from the camera long hair stays as animated (not even pushed out of the body)."), ECVF_Default);
	TAutoConsoleVariable<int32> GMakeHumanHairDebug(TEXT("Pawns.HairDebug"), 0,
		TEXT("1 draws the body's hair collision capsules (green) and the hair strands (yellow) on MakeHuman characters."), ECVF_Default);
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
	return !Chains.IsEmpty() && Mode > 0;
}

void FMakeHumanHairNode::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	for (TArray<FBoneReference>& Chain : Chains)
	{
		for (FBoneReference& Bone : Chain)
		{
			Bone.Initialize(RequiredBones);
		}
	}
	for (FShape& Shape : Shapes)
	{
		Shape.Bone.Initialize(RequiredBones);
	}
}

namespace
{
	colony::HairVec MhHairVec(const FVector& V) { return {static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z)}; }
	FVector MhHairFVector(const colony::HairVec& V) { return FVector(V.x, V.y, V.z); }
}

void FMakeHumanHairNode::Setup(const FReferenceSkeleton& Ref)
{
	Chains.Reset();
	Shapes.Reset();
	Strands.reset();
	// The reference pose in component space: the pose the hair was styled and weighted in, lying on the body.
	const TArray<FTransform>& Local = Ref.GetRefBonePose();
	TArray<FTransform> RefCS;
	RefCS.SetNum(Ref.GetNum());
	for (int32 i = 0; i < Ref.GetNum(); ++i)
	{
		const int32 Parent = Ref.GetParentIndex(i);
		RefCS[i] = Parent == INDEX_NONE ? Local[i] : Local[i] * RefCS[Parent];
	}
	const auto Find = [&Ref](const FString& Name) { return Ref.FindBoneIndex(FName(*Name)); };
	// Long scalp hair (mh_common.add_hair_bones: chains down the back, back corners and sides, shared by the long, bob and
	// braid styles), then MakeHuman's ponytail sub-rig (ponytail1 ... under the head). Two bones at least each.
	TArray<TArray<int32>> ChainBones;
	for (int32 c = 1; c < 10; ++c)
	{
		TArray<int32> Bones;
		for (int32 i = 1; i < 12 && Find(FString::Printf(TEXT("hair_c%d_%d"), c, i)) != INDEX_NONE; ++i)
		{
			Bones.Add(Find(FString::Printf(TEXT("hair_c%d_%d"), c, i)));
		}
		if (Bones.Num() >= 2)
		{
			ChainBones.Add(Bones);
		}
	}
	const int32 SheetChains = ChainBones.Num();
	{
		TArray<int32> Bones;
		for (int32 i = 1; i < 12 && Find(FString::Printf(TEXT("ponytail%d"), i)) != INDEX_NONE; ++i)
		{
			Bones.Add(Find(FString::Printf(TEXT("ponytail%d"), i)));
		}
		if (Bones.Num() >= 2)
		{
			ChainBones.Add(Bones);
		}
	}
	const int32 HeadIndex = Find(TEXT("head"));
	const int32 PelvisIndex = Find(TEXT("pelvis"));
	if (ChainBones.IsEmpty() || HeadIndex == INDEX_NONE || PelvisIndex == INDEX_NONE)
	{
		return;
	}
	// The rest shape of each strand: its joints and a tip past the last one (as long as the bone before it).
	std::vector<std::vector<colony::HairVec>> Rest;
	std::vector<colony::HairVec> Roots;
	for (const TArray<int32>& Bones : ChainBones)
	{
		TArray<FBoneReference>& Chain = Chains.AddDefaulted_GetRef();
		std::vector<colony::HairVec>& Points = Rest.emplace_back();
		for (const int32 Bone : Bones)
		{
			Chain.Emplace(Ref.GetBoneName(Bone));
			Points.push_back(MhHairVec(RefCS[Bone].GetLocation()));
		}
		const FVector Last = RefCS[Bones.Last()].GetLocation();
		Points.push_back(MhHairVec(Last + (Last - RefCS[Bones[Bones.Num() - 2]].GetLocation())));
		if (static_cast<int32>(Roots.size()) < SheetChains)
		{
			Roots.push_back(Points.front());
		}
	}
	// The body's bones in the reference pose, and the shapes fitted to them and to the hair lying on them.
	colony::HairBodyBones Body;
	Body.pelvis = MhHairVec(RefCS[PelvisIndex].GetLocation());
	Body.head = MhHairVec(RefCS[HeadIndex].GetLocation());
	const int32 NeckIndex = Find(TEXT("neck_01"));
	Body.hasNeck = NeckIndex != INDEX_NONE;
	if (Body.hasNeck)
	{
		Body.neck = MhHairVec(RefCS[NeckIndex].GetLocation());
	}
	TArray<int32> Spine;
	for (int32 i = 1; i <= 5; ++i)
	{
		const int32 Index = Find(FString::Printf(TEXT("spine_%02d"), i));
		if (Index != INDEX_NONE)
		{
			Spine.Add(Index);
			Body.spine.push_back(MhHairVec(RefCS[Index].GetLocation()));
		}
	}
	const TCHAR* const ArmNames[3][2] = {{TEXT("clavicle_l"), TEXT("clavicle_r")}, {TEXT("upperarm_l"), TEXT("upperarm_r")}, {TEXT("lowerarm_l"), TEXT("lowerarm_r")}};
	int32 Arm[3][2];
	Body.hasArms = true;
	for (int32 j = 0; j < 3; ++j)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			Arm[j][Side] = Find(ArmNames[j][Side]);
			Body.hasArms = Body.hasArms && Arm[j][Side] != INDEX_NONE;
		}
	}
	if (Body.hasArms)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			Body.clavicle[Side] = MhHairVec(RefCS[Arm[0][Side]].GetLocation());
			Body.upperArm[Side] = MhHairVec(RefCS[Arm[1][Side]].GetLocation());
			Body.lowerArm[Side] = MhHairVec(RefCS[Arm[2][Side]].GetLocation());
		}
	}
	const std::vector<colony::HairShape> Fitted = colony::buildHairShapes(Body, Rest);
	for (const colony::HairShape& Fit : Fitted)
	{
		int32 Bone = INDEX_NONE;
		switch (Fit.bone)
		{
		case colony::HairBone::Head: Bone = HeadIndex; break;
		case colony::HairBone::Neck: Bone = NeckIndex; break;
		case colony::HairBone::SpineTop: Bone = Spine.IsEmpty() ? INDEX_NONE : Spine.Last(); break;
		case colony::HairBone::SpineMid: Bone = Spine.IsValidIndex(Body.spineMid) ? Spine[Body.spineMid] : INDEX_NONE; break;
		case colony::HairBone::ClavicleL: Bone = Arm[0][0]; break;
		case colony::HairBone::ClavicleR: Bone = Arm[0][1]; break;
		case colony::HairBone::UpperArmL: Bone = Arm[1][0]; break;
		case colony::HairBone::UpperArmR: Bone = Arm[1][1]; break;
		}
		if (Bone == INDEX_NONE)
		{
			continue;
		}
		FShape& Shape = Shapes.AddDefaulted_GetRef();
		Shape.Bone.BoneName = Ref.GetBoneName(Bone);
		Shape.A = RefCS[Bone].InverseTransformPosition(MhHairFVector(Fit.capsule.a));
		Shape.B = RefCS[Bone].InverseTransformPosition(MhHairFVector(Fit.capsule.b));
		Shape.Radius = Fit.capsule.radius;
	}
	Strands.setSheetNeighbours(colony::hairSheetNeighbours(Roots, Body.head, colony::HairVec{0.0f, 0.0f, 1.0f}));
}

void FMakeHumanHairNode::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const FTransform Component = Output.AnimInstanceProxy->GetComponentTransform();
	const FBoneContainer& Container = Output.Pose.GetPose().GetBoneContainer();
	const float Scale = static_cast<float>(Component.GetMaximumAxisScale());
	// Where the animation puts each strand (component space and world), its bones' transforms kept to turn them after.
	TArray<TArray<FCompactPoseBoneIndex, TInlineAllocator<12>>, TInlineAllocator<8>> Index;
	TArray<TArray<FTransform, TInlineAllocator<12>>, TInlineAllocator<8>> Original;
	Index.SetNum(Chains.Num());
	Original.SetNum(Chains.Num());
	Goals.resize(static_cast<size_t>(Chains.Num()));
	for (int32 c = 0; c < Chains.Num(); ++c)
	{
		for (const FBoneReference& Bone : Chains[c])
		{
			if (!Bone.IsValidToEvaluate(Container))
			{
				break;
			}
			Index[c].Add(Bone.GetCompactPoseIndex(Container));
			Original[c].Add(Output.Pose.GetComponentSpaceTransform(Index[c].Last()));
		}
		if (Index[c].Num() < Chains[c].Num())
		{
			Strands.reset();  // a lower detail level without the hair bones: as animated
			return;
		}
		std::vector<colony::HairVec>& Points = Goals[static_cast<size_t>(c)];
		Points.clear();
		for (const FTransform& T : Original[c])
		{
			Points.push_back(MhHairVec(Component.TransformPosition(T.GetLocation())));
		}
		const int32 N = Original[c].Num();
		const FVector Tip = Original[c][N - 1].GetLocation() * 2.0 - Original[c][N - 2].GetLocation();
		Points.push_back(MhHairVec(Component.TransformPosition(Tip)));
	}
	// The body where it is drawn this frame.
	Capsules.clear();
	for (const FShape& Shape : Shapes)
	{
		if (!Shape.Bone.IsValidToEvaluate(Container))
		{
			continue;
		}
		const FTransform BoneCS = Output.Pose.GetComponentSpaceTransform(Shape.Bone.GetCompactPoseIndex(Container));
		Capsules.push_back({MhHairVec(Component.TransformPosition(BoneCS.TransformPosition(Shape.A))),
			MhHairVec(Component.TransformPosition(BoneCS.TransformPosition(Shape.B))), Shape.Radius * Scale});
	}
	if (Mode >= 2)
	{
		Strands.step(FMath::Max(DeltaTime, 0.0f), Goals, Capsules, Settings);
	}
	else
	{
		Strands.pose(Goals, Capsules, Settings);
	}
	const std::vector<std::vector<colony::HairVec>>& Solved = Strands.points();
	if (Solved.size() != Goals.size())
	{
		return;
	}
	LargestPenetration = FMath::Max(LargestPenetration, colony::hairDeepestPenetration(Solved, Capsules));
	LargestAnimatedPenetration = FMath::Max(LargestAnimatedPenetration, colony::hairDeepestPenetration(Goals, Capsules));
	// Each bone turned from where the animation points it to where its strand now runs, from the root down.
	for (int32 c = 0; c < Chains.Num(); ++c)
	{
		const int32 N = Original[c].Num();
		const std::vector<colony::HairVec>& Q = Solved[static_cast<size_t>(c)];
		FVector Joint = Original[c][0].GetLocation();
		for (int32 k = 0; k < N; ++k)
		{
			const FVector From = Original[c][k].GetLocation();
			const FVector To = k + 1 < N ? Original[c][k + 1].GetLocation() : From * 2.0 - Original[c][k - 1].GetLocation();
			const FVector Animated = To - From;
			const double Length = Animated.Size();
			const FVector Now = Component.InverseTransformPosition(MhHairFVector(Q[static_cast<size_t>(k) + 1])) -
				Component.InverseTransformPosition(MhHairFVector(Q[static_cast<size_t>(k)]));
			const FVector AnimatedDir = Animated.GetSafeNormal();
			const FVector NowDir = Now.GetSafeNormal();
			FTransform Moved = Original[c][k];
			Moved.SetLocation(Joint);
			if (!AnimatedDir.IsNearlyZero() && !NowDir.IsNearlyZero())
			{
				Moved.SetRotation((FQuat::FindBetweenNormals(AnimatedDir, NowDir) * Original[c][k].GetRotation()).GetNormalized());
				Joint += NowDir * Length;
			}
			else
			{
				Joint += Animated;
			}
			OutBoneTransforms.Add(FBoneTransform(Index[c][k], Moved));
		}
	}
	OutBoneTransforms.Sort(FCompareBoneTransformIndex());
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

void FMakeHumanAnimProxy::Initialize(UAnimInstance* Instance)
{
	// The graph is linked up first: the engine's Initialize takes the root node (GetCustomRootNode) there and keeps it, so
	// linking afterwards left the jiggle and hair nodes out of the graph.
	Retarget.RetargetFrom = ERetargetSourceMode::ParentSkeletalMeshComponent;
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
		// Long hair and a ponytail: strands with the body's collision shapes fitted to this skeleton.
		Hair.Setup(Ref);
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
	float HairDistance = 0.0f;  // no view drawn yet (a test without a camera): as if near
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
		HairDistance = Closest;
	}
	// Long hair: fully simulated near the camera, only pushed out of the body further off, as animated beyond that.
	int32 HairMode = FMath::Clamp(GMakeHumanHair.GetValueOnGameThread(), 0, 2);
	if (HairMode == 2 && HairDistance > GMakeHumanHairSimFar.GetValueOnGameThread())
	{
		HairMode = 1;
	}
	if (HairDistance > GMakeHumanHairCollideFar.GetValueOnGameThread())
	{
		HairMode = 0;
	}
	if (HairMode == 0)
	{
		Hair.Strands.reset();  // starts again from the animated pose when it comes back
	}
	Hair.Mode = HairMode;
	// Pawns.HairDebug: the last frame's collision capsules and strands (the evaluation has finished by now).
	if (World && GMakeHumanHairDebug.GetValueOnGameThread() != 0 && HairMode > 0)
	{
		for (const colony::HairCapsule& Cap : Hair.Capsules)
		{
			const FVector A(Cap.a.x, Cap.a.y, Cap.a.z);
			const FVector B(Cap.b.x, Cap.b.y, Cap.b.z);
			const FVector Axis = B - A;
			const FQuat Turn = Axis.IsNearlyZero() ? FQuat::Identity : FRotationMatrix::MakeFromZ(Axis.GetSafeNormal()).ToQuat();
			DrawDebugCapsule(World, (A + B) * 0.5, static_cast<float>(Axis.Size()) * 0.5f + Cap.radius, Cap.radius, Turn, FColor::Green, false, -1.0f, 0, 0.3f);
		}
		for (const std::vector<colony::HairVec>& Points : Hair.Strands.points())
		{
			for (size_t k = 1; k < Points.size(); ++k)
			{
				DrawDebugLine(World, FVector(Points[k - 1].x, Points[k - 1].y, Points[k - 1].z), FVector(Points[k].x, Points[k].y, Points[k].z),
					FColor::Yellow, false, -1.0f, 1, 0.5f);
			}
		}
	}
	Retarget.RetargetFrom = ERetargetSourceMode::ParentSkeletalMeshComponent;
	Retarget.PreUpdate(Instance);
}

void UMakeHumanAnimInstance::SetRetargeter(UIKRetargeter* InRetargeter)
{
	Retargeter = InRetargeter;
}

int32 UMakeHumanAnimInstance::GetPhysicsChains() const
{
	const FMakeHumanAnimProxy& Proxy = GetProxyOnGameThread<FMakeHumanAnimProxy>();
	return Proxy.Hair.Chains.Num() + Proxy.Jiggle.Bodies.Num() + (Proxy.Tail.Bones.IsEmpty() ? 0 : 1);
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

int32 UMakeHumanAnimInstance::GetHairStrands() const
{
	return GetProxyOnGameThread<FMakeHumanAnimProxy>().Hair.Chains.Num();
}

float UMakeHumanAnimInstance::TakeLargestHairPenetration(float* OutAnimated)
{
	FMakeHumanAnimProxy& Proxy = GetProxyOnGameThread<FMakeHumanAnimProxy>();
	const float Largest = Proxy.Hair.LargestPenetration;
	Proxy.Hair.LargestPenetration = 0.0f;
	if (OutAnimated)
	{
		*OutAnimated = Proxy.Hair.LargestAnimatedPenetration;
	}
	Proxy.Hair.LargestAnimatedPenetration = 0.0f;
	return Largest;
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
