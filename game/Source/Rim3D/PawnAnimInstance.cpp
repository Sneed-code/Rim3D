#include "PawnAnimInstance.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/TrajectoryTypes.h"
#include "BlendStack/AnimNode_BlendStack.h"
#include "BonePose.h"
#include "IObjectChooser.h"
#include "PoseSearch/PoseSearchContext.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchEvent.h"
#include "PoseSearch/PoseSearchHistory.h"
#include "PoseSearch/PoseSearchLibrary.h"
#include "PoseSearch/PoseSearchResult.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchTrajectoryLibrary.h"
#if WITH_EDITOR
#include "PoseSearch/PoseSearchDerivedData.h"
#endif

namespace
{
	// Trajectory sampling, as the Game Animation Sample's characters use: 10 past samples 0.04 s
	// apart, 15 future samples 0.1 s apart (1.5 s ahead).
	constexpr int32 PawnHistorySamples = 10;
	constexpr float PawnHistoryInterval = 0.04f;
	constexpr int32 PawnPredictionSamples = 15;
	constexpr float PawnPredictionInterval = 0.1f;
	// Poses kept for the pose features of the search.
	constexpr int32 PawnHistoryPoses = 10;
	constexpr float PawnHistoryPoseInterval = 0.04f;

	// The sim speeds a pawn up and brakes by 20% of its speed per step, 30 steps a second at 1x.
	constexpr float PawnAccelerationPerSpeed = 0.2f * 30.0f;

	constexpr float PawnMotionMatchBlendTime = 0.25f;
	constexpr float PawnLoopBlendTime = 0.3f;
	// Below this a pawn stands (cm/s).
	constexpr float PawnStandSpeed = 30.0f;

	/** The mesh faces +Y in its own space, so it is turned -90 degrees under the actor. */
	FQuat PawnMeshFacing(float ActorYaw)
	{
		return FRotator(0.0, ActorYaw - 90.0, 0.0).Quaternion();
	}

	float PawnApproach(float Value, float Target, float MaxStep)
	{
		return Value < Target ? FMath::Min(Value + MaxStep, Target) : FMath::Max(Value - MaxStep, Target);
	}
}

/**
 * Locomotion: motion matching or loops, both through a blend stack, plus the pose history and
 * trajectory motion matching needs. Runs on animation worker threads; it reads only the proxy.
 */
struct FSimPawnLocomotionNode : public FAnimNode_Base
{
	struct FSimPawnAnimInstanceProxy* Proxy = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

	FSimPawnAnimStats Stats;

private:
	void UpdateTrajectory(const FSimPawnAnimInputs& In, float DeltaTime);
	void PredictAlongRoute(const FSimPawnAnimInputs& In, float DeltaTime);
	void UpdateMotionMatching(const FAnimationUpdateContext& Context, float DeltaTime);
	void UpdateLoops(const FAnimationUpdateContext& Context, const FSimPawnAnimInputs& In, float BlendTime);

	FAnimNode_BlendStack_Standalone BlendStack;
	UE::PoseSearch::FPoseHistory History;
	FTransformTrajectory Trajectory;
	FMotionMatchingState State;
	bool bCacheBones = true;
	bool bResetHistory = true;
	/** The ground speed a moment ago (smoothed over about 0.15 s), cm/s: whether the pawn is speeding up or holding its pace. */
	float RecentSpeed = 0.0f;
	/** The velocity smoothed over about 0.12 s: the player's pawn moves by the steps the keys send, uneven from step to step. */
	FVector SmoothVelocity = FVector::ZeroVector;
};

/**
 * The montage slot over the locomotion: what Epic's slot node does, without the parts that need
 * an Animation Blueprint (its sequencer hook asks the anim class for subsystems a native class
 * doesn't have, and asserts).
 */
struct FSimPawnSlotNode : public FAnimNode_Base
{
	FSimPawnLocomotionNode* Source = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
		SlotNodeWeight = 0.0f;
		SourceWeight = 1.0f;
		TotalNodeWeight = 0.0f;
		if (!Registered.IsSynchronized_Counter(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter()))
		{
			Registered.SynchronizeWith(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter());
			Context.AnimInstanceProxy->RegisterSlotNodeWithAnimInstance(FAnimSlotGroup::DefaultSlotName);
		}
	}

	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override
	{
		Source->CacheBones_AnyThread(Context);
	}

	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override
	{
		Context.AnimInstanceProxy->GetSlotWeight(FAnimSlotGroup::DefaultSlotName, SlotNodeWeight, SourceWeight, TotalNodeWeight);
		Context.AnimInstanceProxy->UpdateSlotNodeWeight(FAnimSlotGroup::DefaultSlotName, SlotNodeWeight, Context.GetFinalBlendWeight());
		// Locomotion always updates, so the pawn walks on under an action.
		Source->Update_AnyThread(Context);
	}

	virtual void Evaluate_AnyThread(FPoseContext& Output) override
	{
		if (SlotNodeWeight <= ZERO_ANIMWEIGHT_THRESH)
		{
			Source->Evaluate_AnyThread(Output);
			return;
		}
		FPoseContext SourceContext(Output);
		Source->Evaluate_AnyThread(SourceContext);
		const FAnimationPoseData SourcePose(SourceContext);
		FAnimationPoseData OutputPose(Output);
		Output.AnimInstanceProxy->SlotEvaluatePose(FAnimSlotGroup::DefaultSlotName, SourcePose, SourceWeight, OutputPose, SlotNodeWeight, TotalNodeWeight);
	}

private:
	FGraphTraversalCounter Registered;
	float SlotNodeWeight = 0.0f;
	float SourceWeight = 1.0f;
	float TotalNodeWeight = 0.0f;
};

/**
 * The free hand (docs/design/free-hand.md) over the slot: when its alpha is above nought, the right arm reaches a point
 * (a two-bone solve on upperarm_r, lowerarm_r, hand_r; the wrist keeps its pose relative to the forearm), the palm turns
 * towards a direction and the fingers curl towards the palm. The curl axis comes from where the knuckles and the
 * fingers point, so no bone axis convention is assumed. At alpha nought it only passes the slot's pose on.
 */
struct FSimPawnAnimInstanceProxy* Proxy = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

	FSimPawnAnimStats Stats;

private:
	void UpdateTrajectory(const FSimPawnAnimInputs& In, float DeltaTime);
	void PredictAlongRoute(const FSimPawnAnimInputs& In, float DeltaTime);
	void UpdateMotionMatching(const FAnimationUpdateContext& Context, float DeltaTime);
	void UpdateLoops(const FAnimationUpdateContext& Context, const FSimPawnAnimInputs& In, float BlendTime);

	FAnimNode_BlendStack_Standalone BlendStack;
	UE::PoseSearch::FPoseHistory History;
	FTransformTrajectory Trajectory;
	FMotionMatchingState State;
	bool bCacheBones = true;
	bool bResetHistory = true;
	/** The ground speed a moment ago (smoothed over about 0.15 s), cm/s: whether the pawn is speeding up or holding its pace. */
	float RecentSpeed = 0.0f;
	/** The velocity smoothed over about 0.12 s: the player's pawn moves by the steps the keys send, uneven from step to step. */
	FVector SmoothVelocity = FVector::ZeroVector;
};

/**
 * The montage slot over the locomotion: what Epic's slot node does, without the parts that need
 * an Animation Blueprint (its sequencer hook asks the anim class for subsystems a native class
 * doesn't have, and asserts).
 */
struct FSimPawnSlotNode : public FAnimNode_Base
{
	FSimPawnLocomotionNode* Source = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
		SlotNodeWeight = 0.0f;
		SourceWeight = 1.0f;
		TotalNodeWeight = 0.0f;
		if (!Registered.IsSynchronized_Counter(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter()))
		{
			Registered.SynchronizeWith(Context.AnimInstanceProxy->GetSlotNodeInitializationCounter());
			Context.AnimInstanceProxy->RegisterSlotNodeWithAnimInstance(FAnimSlotGroup::DefaultSlotName);
		}
	}

	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override
	{
		Source->CacheBones_AnyThread(Context);
	}

	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override
	{
		Context.AnimInstanceProxy->GetSlotWeight(FAnimSlotGroup::DefaultSlotName, SlotNodeWeight, SourceWeight, TotalNodeWeight);
		Context.AnimInstanceProxy->UpdateSlotNodeWeight(FAnimSlotGroup::DefaultSlotName, SlotNodeWeight, Context.GetFinalBlendWeight());
		// Locomotion always updates, so the pawn walks on under an action.
		Source->Update_AnyThread(Context);
	}

	virtual void Evaluate_AnyThread(FPoseContext& Output) override
	{
		if (SlotNodeWeight <= ZERO_ANIMWEIGHT_THRESH)
		{
			Source->Evaluate_AnyThread(Output);
			return;
		}
		FPoseContext SourceContext(Output);
		Source->Evaluate_AnyThread(SourceContext);
		const FAnimationPoseData SourcePose(SourceContext);
		FAnimationPoseData OutputPose(Output);
		Output.AnimInstanceProxy->SlotEvaluatePose(FAnimSlotGroup::DefaultSlotName, SourcePose, SourceWeight, OutputPose, SlotNodeWeight, TotalNodeWeight);
	}

private:
	FGraphTraversalCounter Registered;
	float SlotNodeWeight = 0.0f;
	float SourceWeight = 1.0f;
	float TotalNodeWeight = 0.0f;
};

/**
 * The free hand (docs/design/free-hand.md) over the slot: when its alpha is above nought, the right arm reaches a point
 * (a two-bone solve on upperarm_r, lowerarm_r, hand_r; the wrist keeps its pose relative to the forearm), the palm turns
 * towards a direction and the fingers curl towards the palm. The curl axis comes from where the knuckles and the
 * fingers point, so no bone axis convention is assumed. At alpha nought it only passes the slot's pose on.
 */
struct FSimPawnHandNode : public FAnimNode_Base
{
	FSimPawnSlotNode* Source = nullptr;
	float Alpha = 0.0f;
	FVector Target = FVector::ZeroVector;
	FVector Elbow = FVector::ZeroVector;
	FVector Palm = FVector::ZeroVector;
	float PalmWeight = 0.0f;
	float Curl = 0.0f;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
	}

	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override
	{
		Source->CacheBones_AnyThread(Context);
		const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
		const auto Find = [&Bones](const TCHAR* Name)
		{
			FBoneReference Ref(Name);
			Ref.Initialize(Bones);
			return Ref.IsValidToEvaluate(Bones) ? Ref.GetCompactPoseIndex(Bones) : FCompactPoseBoneIndex(INDEX_NONE);
		};
		Upper = Find(TEXT("upperarm_r"));
		Lower = Find(TEXT("lowerarm_r"));
		Hand = Find(TEXT("hand_r"));
		static const TCHAR* const FingerNames[5] = {TEXT("thumb"), TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky")};
		for (int32 f = 0; f < 5; ++f)
		{
			for (int32 k = 0; k < 3; ++k)
			{
				Fingers[f][k] = Find(*FString::Printf(TEXT("%s_%02d_r"), FingerNames[f], k + 1));
			}
		}
	}

	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override
	{
		Source->Update_AnyThread(Context);
	}

	virtual void Evaluate_AnyThread(FPoseContext& Output) override
	{
		Source->Evaluate_AnyThread(Output);
		if (Alpha > 0.001f && Upper.IsValid() && Lower.IsValid() && Hand.IsValid())
		{
			Reach(Output.Pose);
		}
	}

private:
	FCompactPoseBoneIndex Upper = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Lower = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Hand = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Fingers[5][3] = {
		{FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)},
		{FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)},
		{FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)},
		{FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)},
		{FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE), FCompactPoseBoneIndex(INDEX_NONE)}};

	void Reach(FCompactPose& Pose) const
	{
		FCSPose<FCompactPose> Space;
		Space.InitPose(Pose);
		const FTransform UpperCS = Space.GetComponentSpaceTransform(Upper);
		const FTransform LowerCS = Space.GetComponentSpaceTransform(Lower);
		const FTransform HandCS = Space.GetComponentSpaceTransform(Hand);
		const FVector R = UpperCS.GetLocation();
		const FVector J = LowerCS.GetLocation();
		const FVector E = HandCS.GetLocation();
		const double L1 = FVector::Dist(R, J);
		const double L2 = FVector::Dist(J, E);
		if (L1 < 1.0 || L2 < 1.0)
		{
			return;
		}
		// Blend the goal from the animated wrist, so alpha eases the arm in and out.
		const FVector Goal = FMath::Lerp(E, Target, static_cast<double>(FMath::Clamp(Alpha, 0.0f, 1.0f)));
		FVector ToGoal = Goal - R;
		const double Want = ToGoal.Size();
		if (Want < 1.0)
		{
			return;
		}
		const FVector Dir = ToGoal / Want;
		const double Dist = FMath::Clamp(Want, FMath::Abs(L1 - L2) + 0.5, (L1 + L2) * 0.999);
		// The bend: towards the elbow hint, else the way the elbow already bends.
		FVector Bend = (FMath::Lerp(J, Elbow, static_cast<double>(FMath::Clamp(Alpha, 0.0f, 1.0f))) - R);
		Bend -= Dir * FVector::DotProduct(Bend, Dir);
		if (!Bend.Normalize())
		{
			Bend = (J - R) - Dir * FVector::DotProduct(J - R, Dir);
			if (!Bend.Normalize())
			{
				Bend = FVector::CrossProduct(Dir, FVector::UpVector).GetSafeNormal();
			}
		}
		const double CosA = FMath::Clamp((L1 * L1 + Dist * Dist - L2 * L2) / (2.0 * L1 * Dist), -1.0, 1.0);
		const double SinA = FMath::Sqrt(FMath::Max(0.0, 1.0 - CosA * CosA));
		const FVector NewJ = R + Dir * (L1 * CosA) + Bend * (L1 * SinA);
		const FVector NewE = R + Dir * Dist;

		const FQuat D1 = FQuat::FindBetweenNormals((J - R).GetSafeNormal(), (NewJ - R).GetSafeNormal());
		const FQuat D2 = FQuat::FindBetweenNormals(D1.RotateVector(E - J).GetSafeNormal(), (NewE - NewJ).GetSafeNormal());
		FTransform NewUpper = UpperCS;
		NewUpper.SetRotation((D1 * UpperCS.GetRotation()).GetNormalized());
		FTransform NewLower = LowerCS;
		NewLower.SetLocation(NewJ);
		NewLower.SetRotation((D2 * D1 * LowerCS.GetRotation()).GetNormalized());
		FQuat HandDelta = D2 * D1;

		// The palm (from the knuckles and the fingers, as CreatorWeapons' grip frame) and which side it is.
		FVector PalmNow = FVector::ZeroVector;
		FVector FingersDir = FVector::ZeroVector;
		const bool bHasFingers = Fingers[1][0].IsValid() && Fingers[4][0].IsValid() && Fingers[2][0].IsValid();
		if (bHasFingers)
		{
			const FVector I = Space.GetComponentSpaceTransform(Fingers[1][0]).GetLocation();
			const FVector K = Space.GetComponentSpaceTransform(Fingers[4][0]).GetLocation();
			const FVector M = Space.GetComponentSpaceTransform(Fingers[2][0]).GetLocation();
			const FVector X = (I - K).GetSafeNormal();
			FingersDir = (M - E).GetSafeNormal();
			FingersDir = (FingersDir - X * FVector::DotProduct(FingersDir, X)).GetSafeNormal();
			PalmNow = FVector::CrossProduct(FingersDir, X).GetSafeNormal();
			if (Fingers[1][2].IsValid())
			{
				const double Side = FVector::DotProduct(Space.GetComponentSpaceTransform(Fingers[1][2]).GetLocation() - I, PalmNow);
				if (Side < -0.5)
				{
					PalmNow = -PalmNow;
				}
			}
		}
		// Turn the palm towards the wanted way (about the wrist), by the weight.
		const FVector PalmWant = Palm.GetSafeNormal();
		if (bHasFingers && PalmWeight > 0.001f && !PalmWant.IsNearlyZero())
		{
			const FQuat Turn = FQuat::FindBetweenNormals(HandDelta.RotateVector(PalmNow), PalmWant);
			HandDelta = FQuat::Slerp(FQuat::Identity, Turn, FMath::Clamp(PalmWeight * Alpha, 0.0f, 1.0f)) * HandDelta;
		}
		FTransform NewHand = HandCS;
		NewHand.SetLocation(NewE);
		NewHand.SetRotation((HandDelta * HandCS.GetRotation()).GetNormalized());

		// Write the arm back as local transforms (children keep theirs and follow).
		const FCompactPoseBoneIndex UpperParent = Pose.GetParentBoneIndex(Upper);
		const FTransform ParentCS = UpperParent.IsValid() ? Space.GetComponentSpaceTransform(UpperParent) : FTransform::Identity;
		Pose[Upper] = NewUpper.GetRelativeTransform(ParentCS);
		Pose[Upper].NormalizeRotation();
		if (Pose.GetParentBoneIndex(Lower) == Upper)
		{
			Pose[Lower] = NewLower.GetRelativeTransform(NewUpper);
			Pose[Lower].NormalizeRotation();
		}
		if (Pose.GetParentBoneIndex(Hand) == Lower)
		{
			Pose[Hand] = NewHand.GetRelativeTransform(NewLower);
			Pose[Hand].NormalizeRotation();
		}

		// The curl: each finger bone turns towards the palm about the axis across its finger, more at the base.
		if (!bHasFingers || Curl <= 0.001f)
		{
			return;
		}
		const FVector PalmAfter = HandDelta.RotateVector(PalmNow);
		static const float CurlDegrees[5][3] = {{12.0f, 22.0f, 18.0f}, {48.0f, 62.0f, 38.0f}, {50.0f, 64.0f, 40.0f}, {52.0f, 64.0f, 40.0f}, {55.0f, 62.0f, 38.0f}};
		const float Amount = FMath::Clamp(Curl, 0.0f, 1.0f) * FMath::Clamp(Alpha, 0.0f, 1.0f);
		for (int32 f = 0; f < 5; ++f)
		{
			if (!Fingers[f][0].IsValid())
			{
				continue;
			}
			// The finger's direction after the arm moved: its base to its next joint, turned with the hand.
			const FVector Base = Space.GetComponentSpaceTransform(Fingers[f][0]).GetLocation();
			const FVector Next = Fingers[f][1].IsValid() ? Space.GetComponentSpaceTransform(Fingers[f][1]).GetLocation() : Base + HandCS.GetRotation().RotateVector(FVector::XAxisVector);
			const FVector Along = HandDelta.RotateVector((Next - Base).GetSafeNormal());
			const FVector Axis = FVector::CrossProduct(Along, PalmAfter).GetSafeNormal();
			if (Axis.IsNearlyZero())
			{
				continue;
			}
			// Walk the chain with each bone's new component-space transform: its parent's new one times its local.
			FTransform ParentNew;
			{
				// The finger base's parent (the hand, or a metacarpal under it): its new transform from the hand's.
				const FCompactPoseBoneIndex P = Pose.GetParentBoneIndex(Fingers[f][0]);
				if (P == Hand)
				{
					ParentNew = NewHand;
				}
				else if (P.IsValid() && Pose.GetParentBoneIndex(P) == Hand)
				{
					ParentNew = Pose[P] * NewHand;
				}
				else
				{
					continue;  // not a hand we know
				}
			}
			FQuat Turned = FQuat::Identity;
			for (int32 k = 0; k < 3; ++k)
			{
				const FCompactPoseBoneIndex B = Fingers[f][k];
				if (!B.IsValid() || (k > 0 && Pose.GetParentBoneIndex(B) != Fingers[f][k - 1]))
				{
					break;
				}
				FTransform New = Pose[B] * ParentNew;
				const FQuat D(Turned.RotateVector(Axis), FMath::DegreesToRadians(CurlDegrees[f][k] * Amount));
				Turned = D * Turned;
				New.SetRotation((D * New.GetRotation()).GetNormalized());
				Pose[B] = New.GetRelativeTransform(ParentNew);
				Pose[B].NormalizeRotation();
				ParentNew = New;
			}
		}
	}
};

struct FSimPawnAnimInstanceProxy : public FAnimInstanceProxy
{
	explicit FSimPawnAnimInstanceProxy(USimPawnAnimInstance* InInstance)
		: FAnimInstanceProxy(InInstance)
	{
	}

	FSimPawnAnimInputs Inputs;
	TArray<TObjectPtr<const UPoseSearchDatabase>> Databases;
	UAnimSequence* IdleLoop = nullptr;
	UAnimSequence* WalkLoop = nullptr;
	UAnimSequence* RunLoop = nullptr;
	float WalkLoopSpeed = 150.0f;
	float RunLoopSpeed = 400.0f;

protected:
	virtual void Initialize(UAnimInstance* InAnimInstance) override
	{
		// The graph is two native nodes: the montage slot (actions) over the locomotion.
		Slot.Source = &Locomotion;
		Locomotion.Proxy = this;
		HandNode.Source = &Slot;
		FAnimInstanceProxy::Initialize(InAnimInstance);
	}

	// The free hand over the slot (it only passes the pose on while off).
	virtual FAnimNode_Base* GetCustomRootNode() override { return &HandNode; }

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override
	{
		FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
		const USimPawnAnimInstance* Instance = CastChecked<USimPawnAnimInstance>(InAnimInstance);
		Inputs = Instance->Inputs;
		Databases = Instance->Databases;
		IdleLoop = Instance->IdleLoop;
		WalkLoop = Instance->WalkLoop;
		RunLoop = Instance->RunLoop;
		WalkLoopSpeed = Instance->WalkLoopSpeed;
		RunLoopSpeed = Instance->RunLoopSpeed;
		HandNode.Alpha = Instance->FreeHandAlpha;
		HandNode.Target = Instance->FreeHandTarget;
		HandNode.Elbow = Instance->FreeHandElbow;
		HandNode.Palm = Instance->FreeHandPalm;
		HandNode.PalmWeight = Instance->FreeHandPalmWeight;
		HandNode.Curl = Instance->FreeHandCurl;
	}

	virtual void PostUpdate(UAnimInstance* InAnimInstance) const override
	{
		FAnimInstanceProxy::PostUpdate(InAnimInstance);
		CastChecked<USimPawnAnimInstance>(InAnimInstance)->Stats = Locomotion.Stats;
	}

private:
	FSimPawnHandNode HandNode;
	FSimPawnSlotNode Slot;
	FSimPawnLocomotionNode Locomotion;
};

// ---------------------------------------------------------------------------

void FSimPawnLocomotionNode::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	BlendStack.Initialize_AnyThread(Context);
	History.Initialize_AnyThread(PawnHistoryPoses, PawnHistoryPoseInterval);
	Trajectory.Samples.Reset();
	State.Reset();
	bResetHistory = true;
}

void FSimPawnLocomotionNode::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	BlendStack.CacheBones_AnyThread(Context);
	bCacheBones = true;
}

void FSimPawnLocomotionNode::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	check(Proxy);
	const FSimPawnAnimInputs& In = Proxy->Inputs;
	const float DeltaTime = Context.GetDeltaTime();

	if (In.bTeleported)
	{
		Trajectory.Samples.Reset();
		bResetHistory = true;
	}
	if (DeltaTime > 0.0f)
	{
		UpdateTrajectory(In, DeltaTime);
		History.SetTrajectory(Trajectory);
	}

	if (BlendStack.GetAnimAsset() == nullptr)
	{
		// Something to show from the first frame on, even while paused.
		UpdateLoops(Context, In, 0.0f);
	}
	else if (DeltaTime > 0.0f)
	{
		if (In.Detail == ESimPawnDetail::Full && !Proxy->Databases.IsEmpty())
		{
			UpdateMotionMatching(Context, DeltaTime);
		}
		else
		{
			// Motion matching starts fresh when the pawn comes close again.
			State.Reset();
			Stats.Database = NAME_None;
			UpdateLoops(Context, In, PawnLoopBlendTime);
		}
	}

	BlendStack.Update_AnyThread(Context);
	Stats.Detail = In.Detail;
	Stats.PlayingAsset = BlendStack.GetAnimAsset() ? BlendStack.GetAnimAsset()->GetFName() : NAME_None;
}

void FSimPawnLocomotionNode::Evaluate_AnyThread(FPoseContext& Output)
{
	BlendStack.Evaluate_AnyThread(Output);

	// The pose history: what motion matching compares candidate poses against.
	FCSPose<FCompactPose> ComponentSpacePose;
	ComponentSpacePose.InitPose(Output.Pose);
	UE::PoseSearch::FComponentSpacePoseProvider PoseProvider(ComponentSpacePose);
	const TArray<FBoneIndexType> AllBones;
	History.EvaluateComponentSpace_AnyThread(Output.AnimInstanceProxy->GetDeltaSeconds(), PoseProvider, false,
		0.0f, 0.0f, 0.0f, bResetHistory, bCacheBones, AllBones, Output.Curve);
	bResetHistory = false;
	bCacheBones = false;
}

void FSimPawnLocomotionNode::UpdateTrajectory(const FSimPawnAnimInputs& In, float DeltaTime)
{
	FPoseSearchTrajectoryData::FSampling Sampling;
	Sampling.NumHistorySamples = PawnHistorySamples;
	Sampling.SecondsPerHistorySample = PawnHistoryInterval;
	Sampling.NumPredictionSamples = PawnPredictionSamples;
	Sampling.SecondsPerPredictionSample = PawnPredictionInterval;

	const FQuat Facing = PawnMeshFacing(In.Yaw);
	UPoseSearchTrajectoryLibrary::InitTrajectorySamples(Trajectory, In.Location, Facing, Sampling, DeltaTime);

	// The past is where the pawn really was. Its velocity is taken from how far it moved, so the
	// library sees no "moving platform" difference between the two.
	FTransformTrajectorySample& Now = Trajectory.Samples[PawnHistorySamples];
	const FVector Moved = (In.Location - Now.Position) / DeltaTime;
	UPoseSearchTrajectoryLibrary::UpdateHistory_TransformHistory(Trajectory, In.Location, Moved, Sampling, DeltaTime);

	// Sample [history] is now; 0 s is the last frame, as motion matching expects.
	Now.TimeInSeconds = DeltaTime;
	Now.Position = In.Location;
	Now.Facing = Facing;
	PredictAlongRoute(In, DeltaTime);
}

void FSimPawnLocomotionNode::PredictAlongRoute(const FSimPawnAnimInputs& In, float DeltaTime)
{
	// Walk the sim's route the way the sim does: speed up to the cruise speed, brake into the last
	// point, turn to face the way it goes. Without a route the pawn brakes where it is heading.
	const float Accel = FMath::Max(In.CruiseSpeed, 100.0f) * PawnAccelerationPerSpeed;
	FVector Position = In.Location;
	SmoothVelocity = In.bTeleported ? In.Velocity : FMath::Lerp(SmoothVelocity, In.Velocity, FMath::Clamp(DeltaTime / 0.12f, 0.0f, 1.0f));
	float Speed = SmoothVelocity.Size2D();
	// The pace it heads for: the cruise speed while it is still getting going, otherwise the pace it keeps now. Pawns often
	// move slower than their cruise speed (drafted, slow ground, the player's walk), and predicting a speed-up they never
	// make had motion matching play walk-to-run starts over and over while the feet slid.
	const bool bSpeedingUp = Speed < PawnStandSpeed || Speed > RecentSpeed + 0.04f * In.CruiseSpeed;
	const bool bSlowingDown = Speed < RecentSpeed - 0.06f * In.CruiseSpeed;
	const float Pace = bSpeedingUp ? In.CruiseSpeed : FMath::Min(Speed, In.CruiseSpeed * 2.0f);
	RecentSpeed = FMath::Lerp(RecentSpeed, Speed, FMath::Clamp(DeltaTime / 0.25f, 0.0f, 1.0f));
	FVector Heading = Speed > PawnStandSpeed ? SmoothVelocity.GetSafeNormal2D() : FVector::ZeroVector;
	float Yaw = In.Yaw;
	int32 Next = 0;

	float RouteLeft = 0.0f;
	{
		FVector From = Position;
		for (const FVector& Point : In.Route)
		{
			RouteLeft += FVector::Dist2D(From, Point);
			From = Point;
		}
	}

	constexpr int32 SubSteps = 4;
	const float SubDt = PawnPredictionInterval / SubSteps;
	for (int32 Sample = 1; Sample <= PawnPredictionSamples; ++Sample)
	{
		for (int32 Sub = 0; Sub < SubSteps; ++Sub)
		{
			const bool bOnRoute = Next < In.Route.Num();
			// Without a route a moving pawn is the player's (driven by the keys), which keeps going the way it goes; the sim's
			// pawns have braked at their last waypoint by then.
			const float Wanted = bOnRoute ? FMath::Min(Pace, FMath::Sqrt(2.0f * Accel * RouteLeft)) : (In.Route.IsEmpty() && Speed > PawnStandSpeed && !bSlowingDown ? Pace : 0.0f);
			Speed = PawnApproach(Speed, Wanted, Accel * SubDt);
			float Distance = Speed * SubDt;
			if (bOnRoute)
			{
				RouteLeft = FMath::Max(0.0f, RouteLeft - Distance);
				while (Distance > 0.0f && Next < In.Route.Num())
				{
					const FVector ToPoint = In.Route[Next] - Position;
					const float Length = ToPoint.Size2D();
					if (Length > KINDA_SMALL_NUMBER)
					{
						Heading = ToPoint.GetSafeNormal2D();
					}
					if (Length <= Distance)
					{
						Position = In.Route[Next++];
						Distance -= Length;
					}
					else
					{
						Position += ToPoint * (Distance / Length);
						Distance = 0.0f;
					}
				}
				if (Next >= In.Route.Num())
				{
					Speed = 0.0f;
				}
			}
			else
			{
				Position += Heading * Distance;
			}
			if (!Heading.IsNearlyZero() && Speed > PawnStandSpeed)
			{
				Yaw = Heading.Rotation().Yaw;
			}
		}
		FTransformTrajectorySample& Out = Trajectory.Samples[PawnHistorySamples + Sample];
		Out.Position = Position;
		Out.Facing = PawnMeshFacing(Yaw);
		Out.TimeInSeconds = DeltaTime + Sample * PawnPredictionInterval;
	}
}

void FSimPawnLocomotionNode::UpdateMotionMatching(const FAnimationUpdateContext& Context, float DeltaTime)
{
	using namespace UE::PoseSearch;

	// Keep the state in step with what the blend stack really plays (its time moves with the play rate).
	State.SearchResult.SelectedAnim = BlendStack.GetAnimAsset();
	State.SearchResult.SelectedTime = BlendStack.GetAccumulatedTime();
	State.SearchResult.bIsMirrored = BlendStack.GetMirror();
	State.SearchResult.BlendParameters = BlendStack.GetBlendParameters();
#if WITH_EDITOR
	// In the editor a database can be re-indexed while running; its old results are not safe then.
	if (State.SearchResult.SelectedDatabase && EAsyncBuildIndexResult::Success != FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(
		State.SearchResult.SelectedDatabase.Get(), ERequestAsyncBuildFlag::ContinueRequest))
	{
		State.Reset();
	}
#endif

	const FFloatInterval PlayRate(0.8f, 1.2f);
	FSearchContext SearchContext(0.0f, FFloatInterval(0.0f, 0.0f), FPoseSearchEvent());
	FChooserEvaluationContext ChooserContext(const_cast<UObject*>(Context.AnimInstanceProxy->GetAnimInstanceObject()));
	SearchContext.AddRole(GetCommonDefaultRole(Proxy->Databases), &ChooserContext, &History);

	TArray<const UObject*, TInlineAllocator<16>> Assets;
	for (const TObjectPtr<const UPoseSearchDatabase>& Database : Proxy->Databases)
	{
		Assets.Add(Database.Get());
	}
	FPoseSearchContinuingProperties Continuing;
	Continuing.InitFrom(State.SearchResult, EPoseSearchInterruptMode::DoNotInterrupt);
	FSearchResults_Single Results;
	// The same call Epic's motion matching node makes; marked experimental in 5.8.
	PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	UPoseSearchLibrary::MotionMatch(SearchContext, MakeArrayView(Assets), Continuing, Results);
	PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
	const FSearchResult Result = Results.GetBestResult();
	const float WantedPlayRate = CalculateWantedPlayRate(Result, SearchContext, PlayRate, History.GetTrajectorySpeedMultiplier(), FPoseSearchEvent());
	State.SearchResult.InitFrom(Result, WantedPlayRate);
	++Stats.Searches;

	const UPoseSearchDatabase* Database = State.SearchResult.SelectedDatabase.Get();
	Stats.Database = Database ? Database->GetFName() : NAME_None;
	if (State.SearchResult.SelectedAnim && !State.SearchResult.bIsContinuingPoseSearch && Database && Database->Schema)
	{
		if (UAnimationAsset* Asset = State.SearchResult.GetAnimationAssetForRole())
		{
			BlendStack.BlendTo(Context, Asset, State.SearchResult.SelectedTime, State.SearchResult.bLoop, State.SearchResult.bIsMirrored,
				Database->Schema->GetMirrorDataTable(State.SearchResult.Role), PawnMotionMatchBlendTime, nullptr,
				EAlphaBlendOption::HermiteCubic, false, NAME_None, State.SearchResult.BlendParameters, WantedPlayRate);
			++Stats.Jumps;
		}
	}
	BlendStack.UpdatePlayRate(State.SearchResult.WantedPlayRate);
	Stats.PlayRate = State.SearchResult.WantedPlayRate;
}

void FSimPawnLocomotionNode::UpdateLoops(const FAnimationUpdateContext& Context, const FSimPawnAnimInputs& In, float BlendTime)
{
	const float Speed = In.Velocity.Size2D();
	UAnimSequence* Loop = Proxy->IdleLoop;
	float Rate = 1.0f;
	if (Speed >= PawnStandSpeed)
	{
		const bool bRun = Speed > 0.5f * (Proxy->WalkLoopSpeed + Proxy->RunLoopSpeed);
		Loop = bRun ? Proxy->RunLoop : Proxy->WalkLoop;
		Rate = FMath::Clamp(Speed / (bRun ? Proxy->RunLoopSpeed : Proxy->WalkLoopSpeed), 0.6f, 1.5f);
	}
	if (!Loop)
	{
		return;
	}
	if (BlendStack.GetAnimAsset() != Loop)
	{
		BlendStack.BlendTo(Context, Loop, 0.0f, true, false, nullptr, BlendTime, nullptr, EAlphaBlendOption::HermiteCubic,
			false, NAME_None, FVector::ZeroVector, Rate);
	}
	BlendStack.UpdatePlayRate(Rate);
	Stats.PlayRate = Rate;
}

// ---------------------------------------------------------------------------

USimPawnAnimInstance::USimPawnAnimInstance()
{
	// The sim moves the pawn: animations play in place (root motion is taken out and dropped).
	RootMotionMode = ERootMotionMode::IgnoreRootMotion;
}

void USimPawnAnimInstance::SetAssets(const TArray<TObjectPtr<const UPoseSearchDatabase>>& InDatabases, UAnimSequence* InIdle,
	UAnimSequence* InWalk, float InWalkSpeed, UAnimSequence* InRun, float InRunSpeed)
{
	Databases = InDatabases;
	IdleLoop = InIdle;
	WalkLoop = InWalk;
	RunLoop = InRun;
	WalkLoopSpeed = FMath::Max(InWalkSpeed, 50.0f);
	RunLoopSpeed = FMath::Max(InRunSpeed, WalkLoopSpeed + 50.0f);
}

FAnimInstanceProxy* USimPawnAnimInstance::CreateAnimInstanceProxy()
{
	return new FSimPawnAnimInstanceProxy(this);
}

void USimPawnAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete static_cast<FSimPawnAnimInstanceProxy*>(InProxy);
}
