#include "MakeHumanAnimInstance.h"

#include "GoreCut.h"

#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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
	TAutoConsoleVariable<float> GMakeHumanHairAimSweep(TEXT("Pawns.HairAimSweep"), 45.0f,
		TEXT("Degrees long hair turns over the other shoulder while a gun is aimed, so it never hides the stock or the front hand (0: off)."), ECVF_Default);
	TAutoConsoleVariable<int32> GMakeHumanHairDebug(TEXT("Pawns.HairDebug"), 0,
		TEXT("1 draws the body's hair collision capsules (green) and the hair strands (yellow) on MakeHuman characters."), ECVF_Default);
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
	HeadBone.Initialize(RequiredBones);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		Hands[Side].Initialize(RequiredBones);
		LowerArms[Side].Initialize(RequiredBones);
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
		case colony::HairBone::SpineLow: Bone = Spine.IsEmpty() ? INDEX_NONE : Spine[0]; break;
		case colony::HairBone::Pelvis: Bone = PelvisIndex; break;
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
	// A held gun's forearms (capsules only while a gun is held up): a little thinner than the upper arms' shape.
	HeadBone.BoneName = Ref.GetBoneName(HeadIndex);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const int32 HandIndex = Find(Side == 1 ? TEXT("hand_r") : TEXT("hand_l"));
		const bool bForearm = Body.hasArms && HandIndex != INDEX_NONE;
		Hands[Side].BoneName = bForearm ? Ref.GetBoneName(HandIndex) : NAME_None;
		LowerArms[Side].BoneName = bForearm ? Ref.GetBoneName(Arm[2][Side]) : NAME_None;
	}
	// In the skeleton's own units (a mesh may be in metres under a scaled component): 4 cm on a body 60 cm from pelvis to head.
	ForearmRadius = static_cast<float>(FVector::Dist(RefCS[HeadIndex].GetLocation(), RefCS[PelvisIndex].GetLocation())) * (4.0f / 60.0f);
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
	// A gun held up in both hands (docs/design/hair-physics.md, "Aiming"): the hair rests on it and on the forearms.
	const bool bGun = Gun.bOn && Hands[Gun.bRightHand ? 1 : 0].IsValidToEvaluate(Container);
	if (bGun)
	{
		FTransform HandWorld = Output.Pose.GetComponentSpaceTransform(Hands[Gun.bRightHand ? 1 : 0].GetCompactPoseIndex(Container)) * Component;
		HandWorld.SetScale3D(FVector::OneVector);
		const FTransform GunWorld = Gun.GunInHand * HandWorld;
		const float GunSize = static_cast<float>(Gun.GunInHand.GetMaximumAxisScale());
		Capsules.push_back({MhHairVec(GunWorld.TransformPosition(Gun.Rear)), MhHairVec(GunWorld.TransformPosition(Gun.Front)), Gun.Radius * GunSize});
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Hands[Side].IsValidToEvaluate(Container) && LowerArms[Side].IsValidToEvaluate(Container))
			{
				const FVector Elbow = Output.Pose.GetComponentSpaceTransform(LowerArms[Side].GetCompactPoseIndex(Container)).GetLocation();
				const FVector Wrist = Output.Pose.GetComponentSpaceTransform(Hands[Side].GetCompactPoseIndex(Container)).GetLocation();
				Capsules.push_back({MhHairVec(Component.TransformPosition(Elbow)), MhHairVec(Component.TransformPosition(Wrist)), ForearmRadius * Scale});
			}
		}
	}
	LargestAnimatedPenetration = FMath::Max(LargestAnimatedPenetration, colony::hairDeepestPenetration(Goals, Capsules));
	// Aimed, the hair goes over the other shoulder, as a shooter with long hair puts it, off the stock and the front hand
	// (sneed's PC pictures 2026-11-20: it hid both in the over-the-shoulder view).
	if (bGun && Gun.Aim > 0.0f && SweepDegrees > 0.0f && HeadBone.IsValidToEvaluate(Container))
	{
		const FVector HeadWorld = Component.TransformPosition(Output.Pose.GetComponentSpaceTransform(HeadBone.GetCompactPoseIndex(Container)).GetLocation());
		const FVector StockSide = Component.TransformVectorNoScale(Right).GetSafeNormal() * (Gun.bRightHand ? 1.0 : -1.0);
		const FVector Back = -Component.TransformVectorNoScale(Forward).GetSafeNormal();
		const FVector Away = (-StockSide + Back * 0.35).GetSafeNormal();
		colony::sweepHairGoals(Goals, MhHairVec(HeadWorld), MhHairVec(StockSide), MhHairVec(Away), FMath::Clamp(Gun.Aim, 0.0f, 1.0f), SweepDegrees);
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
		// A clean cut (GoreCut.h): the bone straightened onto its parent and flattened across the limb at the joint, so
		// the stump keeps its width and ends flat under its cap; everything below it folds into the middle of the cut.
		// Where that can't be done the bone folds to a point as before.
		const FCompactPoseBoneIndex Parent = Container.GetParentBoneIndex(Index);
		const FMeshPoseBoneIndex MeshIndex = Container.MakeMeshPoseIndex(Index);
		FTransform CutPose = Output.Pose.GetComponentSpaceTransform(Index);
		bool bFlat = false;
		if (Parent.IsValid() && MeshIndex.IsValid() && MeshIndex.GetInt() < Container.GetReferenceSkeleton().GetNum())
		{
			const FReferenceSkeleton& Ref = Container.GetReferenceSkeleton();
			bFlat = GoreCut::FlattenCut(Output.Pose.GetComponentSpaceTransform(Parent), Ref.GetRefBonePose()[MeshIndex.GetInt()],
				GoreCut::AxisLocal(Ref, MeshIndex.GetInt()), CutPose);
		}
		if (!bFlat)
		{
			CutPose.SetScale3D(FVector(GoreCut::Flat));
		}
		OutBoneTransforms.Add(FBoneTransform(Index, CutPose));
		const FTransform Gone(CutPose.GetRotation(), CutPose.GetTranslation(), FVector(GoreCut::Flat));
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

	// Closes a pinned hand's fingers round its grip: each finger is carried rigidly with the solved hand, then each
	// joint gets only the curl the animation lacks (an open-handed clip held air beside the gun), towards the palm and
	// never past a natural fist (index 50/60 at its first two joints, the little finger 65/65), the last joint a third
	// as much. The thumb is left as animated.
	float MhCurlFingers(FComponentSpacePoseContext& Output, const FBoneContainer& Container, const FMakeHumanArmsNode::FArm& Arm,
					   const FTransform& HandAnimated, const FTransform& HandSolved, const FVector& Fingers0, const FVector& Palm0, float CurlAlpha, float CurlWrap,
					   TArray<FBoneTransform>& OutBoneTransforms)
	{
		static const double MhCurlWant[4][2] = {{50.0, 60.0}, {55.0, 65.0}, {60.0, 65.0}, {65.0, 65.0}};
		const FQuat HandDelta = HandSolved.GetRotation() * HandAnimated.GetRotation().Inverse();
		const FVector FingersF = HandDelta.RotateVector(Fingers0).GetSafeNormal();
		const FVector PalmF = HandDelta.RotateVector(Palm0).GetSafeNormal();
		// A turn about this axis by a positive angle brings the fingers towards the palm.
		const FVector CurlAxis = FVector::CrossProduct(FingersF, PalmF).GetSafeNormal();
		if (CurlAxis.IsNearlyZero())
		{
			return -1.0f;
		}
		double AllAdded = 0.0;
		int32 Chains = 0;
		for (int32 F = 0; F < 4; ++F)
		{
			const FBoneReference (&Chain)[3] = Arm.Fingers[F];
			if (!Chain[0].IsValidToEvaluate(Container) || !Chain[1].IsValidToEvaluate(Container) || !Chain[2].IsValidToEvaluate(Container))
			{
				continue;
			}
			++Chains;
			FTransform Joint[3];
			FCompactPoseBoneIndex JointI[3] = {FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)};
			for (int32 J = 0; J < 3; ++J)
			{
				JointI[J] = Chain[J].GetCompactPoseIndex(Container);
				Joint[J] = Output.Pose.GetComponentSpaceTransform(JointI[J]).GetRelativeTransform(HandAnimated) * HandSolved;
			}
			double Added = 0.0;
			for (int32 J = 0; J < 3; ++J)
			{
				double Turn = 0.0;
				if (J < 2)
				{
					const FVector From = J == 0 ? FingersF : (Joint[1].GetLocation() - Joint[0].GetLocation()).GetSafeNormal();
					const FVector Seg = (J == 0 ? Joint[1].GetLocation() - Joint[0].GetLocation() : Joint[2].GetLocation() - Joint[1].GetLocation()).GetSafeNormal();
					if (From.IsNearlyZero() || Seg.IsNearlyZero())
					{
						continue;
					}
					const double Now = FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(FVector::CrossProduct(From, Seg), CurlAxis), FVector::DotProduct(From, Seg)));
					Turn = FMath::Max(0.0, FMath::Min(MhCurlWant[F][J] * CurlWrap, 85.0) - Now);
				}
				else
				{
					Turn = FMath::Min(Added / 3.0, 30.0);
				}
				Turn *= CurlAlpha;
				if (Turn <= 0.01)
				{
					continue;
				}
				Added += Turn;
				const FQuat Q(CurlAxis, FMath::DegreesToRadians(Turn));
				const FVector Pivot = Joint[J].GetLocation();
				for (int32 K = J; K < 3; ++K)
				{
					Joint[K].SetRotation((Q * Joint[K].GetRotation()).GetNormalized());
					Joint[K].SetLocation(Pivot + Q.RotateVector(Joint[K].GetLocation() - Pivot));
				}
			}
			AllAdded += Added;
			if (Added <= 0.0)
			{
				continue;
			}
			for (int32 J = 0; J < 3; ++J)
			{
				OutBoneTransforms.Add(FBoneTransform(JointI[J], Joint[J]));
			}
		}
		// The mean curl added per finger (degrees), -2 if the fingers' bones weren't found.
		return Chains > 0 ? static_cast<float>(AllAdded / Chains) : -2.0f;
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
		for (FBoneReference (&Chain)[3] : Arm.Fingers)
		{
			for (FBoneReference& Joint : Chain)
			{
				Joint.Initialize(RequiredBones);
			}
		}
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
		FQuat WantRot = Goal.bRotation ? Target.GetRotation() : HandT.GetRotation();

		// The hand's frame as animated (finger direction and palm normal from its knuckles), carried to the goal's turn by
		// the hand bone's own rotation (as animated too).
		const auto Loc = [&Output, &Container](const FBoneReference& B) { return Output.Pose.GetComponentSpaceTransform(B.GetCompactPoseIndex(Container)).GetLocation(); };
		FVector Tip = FVector::ZeroVector;
		const bool bTip = Arm.IndexTip.IsValidToEvaluate(Container);
		if (bTip)
		{
			Tip = Loc(Arm.IndexTip);
		}
		FVector Fingers0, Palm0;
		const bool bHandFrame = MhHandFrame(HandAnimated.GetLocation(), Loc(Arm.Index), Loc(Arm.Pinky), Loc(Arm.Middle), bTip ? &Tip : nullptr, bRight, Fingers0, Palm0);
		FVector FingersWant = FVector::ZeroVector, PalmWant = FVector::ZeroVector;
		if (bHandFrame)
		{
			const FQuat AnimatedInv = HandAnimated.GetRotation().Inverse();
			FingersWant = WantRot.RotateVector(AnimatedInv.RotateVector(Fingers0));
			PalmWant = WantRot.RotateVector(AnimatedInv.RotateVector(Palm0));
		}

		// The elbow's pole, then the hinge solve (no stretching, never folded past the limit).
		Swivel[Side] = 0.0f;
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
			// A grip that sets the hand's turn swings the elbow round to where the wrist can take it (a rifle's grip
			// hand lifts its elbow out to the side) rather than bending the wrist past its range.
			// A hand round a round shaft (a support hand under a fore-end, on a spear's shaft) may also turn about it, up
			// to RollFree degrees either way: the turn and the elbow's swing are chosen together.
			if (Goal.bRotation && bHandFrame && bLimits)
			{
				const FQuat AnimatedInv = HandAnimated.GetRotation().Inverse();
				const FVector RollAxis = Target.TransformVectorNoScale(Goal.RollAxis).GetSafeNormal();
				const FVector RollPoint = Target.TransformPosition(Goal.RollPoint);
				const float RollSpan = RollAxis.IsNearlyZero() ? 0.0f : FMath::Clamp(Goal.RollFree, 0.0f, 180.0f);
				const int32 RollSteps = FMath::CeilToInt(RollSpan / 22.5f);
				double BestCost = 1e18;
				FTransform BestTarget = Target;
				float BestRoll = 0.0f;
				SwivelChoice Best;
				Best.pole = Pole;
				for (int32 Step = -RollSteps; Step <= RollSteps; ++Step)
				{
					const float Roll = RollSteps > 0 ? RollSpan * static_cast<float>(Step) / static_cast<float>(RollSteps) : 0.0f;
					FTransform Rolled = Target;
					if (FMath::Abs(Roll) > KINDA_SMALL_NUMBER)
					{
						const FQuat RollQ(RollAxis, FMath::DegreesToRadians(Roll));
						Rolled.SetRotation((RollQ * Target.GetRotation()).GetNormalized());
						Rolled.SetLocation(RollPoint + RollQ.RotateVector(Target.GetLocation() - RollPoint));
					}
					const FQuat RolledRot = Rolled.GetRotation();
					const SwivelChoice Swing = chooseSwivel(MhV(S), UpperLen, LowerLen, MhV(Rolled.GetLocation()),
						naturalPole(MhV(S), MhV(Rolled.GetLocation()), BodyUp, BodyForward, BodyRight, bRight),
						HandFrame{MhV(RolledRot.RotateVector(AnimatedInv.RotateVector(Fingers0))), MhV(RolledRot.RotateVector(AnimatedInv.RotateVector(Palm0)))},
						BodyUp, BodyForward, BodyRight, bRight, Human, Arm.NeutralFlex, Arm.NeutralDeviation, 100.0, Arm.LastSwivel);
					const double Cost = Swing.excess * 4.0 + FMath::Abs(Swing.swivel) * 0.1 + FMath::Abs(Swing.swivel - Arm.LastSwivel) * 0.06 +
						FMath::Abs(Roll) * 0.05 + FMath::Abs(Roll - Arm.LastRoll) * 0.04;
					if (Cost < BestCost - 1e-9)
					{
						BestCost = Cost;
						BestTarget = Rolled;
						BestRoll = Roll;
						Best = Swing;
					}
				}
				if (!BestTarget.Equals(Target, 1e-4))
				{
					Target = BestTarget;
					WantRot = Target.GetRotation();
					FingersWant = WantRot.RotateVector(AnimatedInv.RotateVector(Fingers0));
					PalmWant = WantRot.RotateVector(AnimatedInv.RotateVector(Palm0));
					GoalCS[Side] = Target;
				}
				Pole = Best.pole;
				Swivel[Side] = static_cast<float>(Best.swivel);
				Arm.LastSwivel = Best.swivel;
				Arm.LastRoll = BestRoll;
			}
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
		if (bHandFrame)
		{
			const FQuat HandInv = HandAnimated.GetRotation().Inverse();
			Wanted[Side] = measureArm(MhV(S), MhV(E1), MhV(W1), MhV(FingersWant), MhV(PalmWant), MhV(HingeWant), Arm.NeutralFlex, Arm.NeutralDeviation);
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
		CurlAdded[Side] = 0.0f;
		if (Goal.bCurl && bHandFrame)
		{
			CurlAdded[Side] = MhCurlFingers(Output, Container, Arm, HandAnimated, FTransform(HandB, WB, HandT.GetScale3D()), Fingers0, Palm0, GoalAlpha, Goal.CurlWrap, OutBoneTransforms);
		}
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

bool UMakeHumanAnimInstance::GetWantedArm(bool bRight, colony::arm::ArmAngles& Out, float& OutCurl, float& OutSwivel) const
{
	const FMakeHumanArmsNode& Node = GetProxyOnGameThread<FMakeHumanAnimProxy>().ArmsIK;
	const int32 Side = bRight ? 1 : 0;
	Out = Node.Wanted[Side];
	OutCurl = Node.CurlAdded[Side];
	OutSwivel = Node.Swivel[Side];
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
			static const TCHAR* const MhFingerNames[4] = {TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky")};
			for (int32 F = 0; F < 4; ++F)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					Arm.Fingers[F][J].BoneName = FName(*FString::Printf(TEXT("%s_%02d_%s"), MhFingerNames[F], J + 1, S));
				}
			}
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
		// A held gun for the hair, in the same body axes.
		Hair.Gun = MakeHuman->GetHeldGun();
		Hair.Right = ArmsIK.Right;
		Hair.Forward = ArmsIK.Forward;
		Hair.SweepDegrees = FMath::Max(0.0f, GMakeHumanHairAimSweep.GetValueOnGameThread());
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
