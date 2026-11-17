#include "PawnAnimInstance.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
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
	// Standing this long (s) with no route ahead, a full-detail pawn leaves motion matching for the breathing idle, blended
	// in over PawnStandBlendTime; the short wait lets a walk's stop clip plant the feet first.
	constexpr float PawnStandDelay = 0.3f;
	constexpr float PawnStandBlendTime = 0.4f;
	// A route shorter than this (cm) is no walk: the pawn is where it wants to be.
	constexpr float PawnStandRoute = 25.0f;

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
	/** Whether the pawn stands where it wants to be: hardly moving and no route ahead. */
	static bool IsStanding(const FSimPawnAnimInputs& In);
	/** Plays the breathing idle (blending in once); false without one. */
	bool UpdateStandingIdle(const FAnimationUpdateContext& Context, float BlendTime);

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
	/** How long the pawn has stood still (s); 0 while it moves. */
	float StandTime = 0.0f;
	/** Where in the breathing idle this pawn starts (0..1), so a crowd does not breathe in step. Cosmetic, not the sim's. */
	float IdlePhase = 0.0f;
};

/**
 * The montage slot over the locomotion: what Epic's slot node does, without the parts that need
 * an Animation Blueprint (its sequencer hook asks the anim class for subsystems a native class
 * doesn't have, and asserts).
 */
/**
 * The hold layer (docs/design/weapon-hold.md): over the locomotion, the held weapon's stance clip on the spine, arms and
 * head (spine_01 turned in mesh space, so a stance that turns its hips still faces the way the clip's chest does over
 * walking legs; the rest blended bone by bone), and a forward bend at the waist for a load on the back.
 */
struct FSimPawnUpperNode : public FAnimNode_Base
{
	FAnimNode_Base* Source = nullptr;
	struct FSimPawnAnimInstanceProxy* Proxy = nullptr;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override
	{
		FAnimNode_Base::Initialize_AnyThread(Context);
		Source->Initialize_AnyThread(Context);
	}
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override { Source->Update_AnyThread(Context); }
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;

private:
	TArray<float> BoneWeights;  // by compact pose index: 0 the legs and hips, 1 the chest, arms and head
	FCompactPoseBoneIndex Pelvis = FCompactPoseBoneIndex(INDEX_NONE);
	FCompactPoseBoneIndex Spine = FCompactPoseBoneIndex(INDEX_NONE);
};

struct FSimPawnSlotNode : public FAnimNode_Base
{
	FAnimNode_Base* Source = nullptr;

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
		// The graph is three native nodes: the montage slot (actions) over the hold layer over the locomotion.
		Slot.Source = &Upper;
		Upper.Source = &Locomotion;
		Upper.Proxy = this;
		Locomotion.Proxy = this;
		FAnimInstanceProxy::Initialize(InAnimInstance);
	}

	virtual FAnimNode_Base* GetCustomRootNode() override { return &Slot; }

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
		USimPawnAnimInstance* Mutable = CastChecked<USimPawnAnimInstance>(InAnimInstance);
		Mutable->AdvanceUpper(DeltaSeconds);
		UpperClip = Mutable->UpperClip;
		UpperOldClip = Mutable->UpperBlend < 1.0f ? Mutable->UpperOldClip.Get() : nullptr;
		UpperTime = Mutable->UpperTime;
		UpperOldTime = Mutable->UpperOldTime;
		UpperBlend = Mutable->UpperBlend;
		UpperWeight = Mutable->UpperWeight;
		SpineBend = Mutable->SpineBend;
	}

public:
	// The hold layer, copied before each update (game thread).
	UAnimSequence* UpperClip = nullptr;
	UAnimSequence* UpperOldClip = nullptr;
	float UpperTime = 0.0f;
	float UpperOldTime = 0.0f;
	float UpperBlend = 1.0f;
	float UpperWeight = 0.0f;
	float SpineBend = 0.0f;

protected:

	virtual void PostUpdate(UAnimInstance* InAnimInstance) const override
	{
		FAnimInstanceProxy::PostUpdate(InAnimInstance);
		CastChecked<USimPawnAnimInstance>(InAnimInstance)->Stats = Locomotion.Stats;
	}

private:
	FSimPawnSlotNode Slot;
	FSimPawnUpperNode Upper;
	FSimPawnLocomotionNode Locomotion;
};

// ---------------------------------------------------------------------------
// The hold layer.

namespace
{
	/** A looping clip's time, wrapped. */
	float PawnUpperWrap(const UAnimSequence* Clip, float Time)
	{
		const float Length = Clip ? FMath::Max(Clip->GetPlayLength(), 0.001f) : 1.0f;
		return FMath::Fmod(FMath::Max(Time, 0.0f), Length);
	}

	/** A bone's transform in component space, from a local pose: its own and each parent's, multiplied up. */
	FTransform PawnUpperComponent(const FCompactPose& Pose, FCompactPoseBoneIndex Bone)
	{
		FTransform Out = Pose[Bone];
		for (FCompactPoseBoneIndex At = Pose.GetParentBoneIndex(Bone); At.IsValid(); At = Pose.GetParentBoneIndex(At))
		{
			Out = Out * Pose[At];
		}
		return Out;
	}
}

void FSimPawnUpperNode::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source->CacheBones_AnyThread(Context);
	const FBoneContainer& Bones = Context.AnimInstanceProxy->GetRequiredBones();
	const FReferenceSkeleton& Ref = Bones.GetReferenceSkeleton();
	const int32 Count = Bones.GetCompactPoseNumBones();
	BoneWeights.SetNumZeroed(Count);
	const int32 PelvisRef = Ref.FindBoneIndex(TEXT("pelvis"));
	const int32 SpineRef = Ref.FindBoneIndex(TEXT("spine_01"));
	Pelvis = PelvisRef != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(PelvisRef)) : FCompactPoseBoneIndex(INDEX_NONE);
	Spine = SpineRef != INDEX_NONE ? Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(SpineRef)) : FCompactPoseBoneIndex(INDEX_NONE);
	if (SpineRef == INDEX_NONE)
	{
		return;
	}
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 RefIndex = Bones.MakeMeshPoseIndex(FCompactPoseBoneIndex(Index)).GetInt();
		// Everything under spine_01 takes the stance; spine_01 itself is set in mesh space below.
		for (int32 At = Ref.GetParentIndex(RefIndex); At != INDEX_NONE; At = Ref.GetParentIndex(At))
		{
			if (At == SpineRef)
			{
				BoneWeights[Index] = 1.0f;
				break;
			}
		}
	}
}

void FSimPawnUpperNode::Evaluate_AnyThread(FPoseContext& Output)
{
	Source->Evaluate_AnyThread(Output);
	const FSimPawnAnimInstanceProxy* P = Proxy;
	if (!P || Spine.GetInt() == INDEX_NONE || Pelvis.GetInt() == INDEX_NONE || BoneWeights.Num() != Output.Pose.GetNumBones())
	{
		return;
	}
	const float Weight = P->UpperClip ? FMath::Clamp(P->UpperWeight, 0.0f, 1.0f) : 0.0f;
	if (Weight > ZERO_ANIMWEIGHT_THRESH)
	{
		FPoseContext Stance(Output);
		{
			FAnimationPoseData Data(Stance);
			P->UpperClip->GetAnimationPose(Data, FAnimExtractContext(static_cast<double>(PawnUpperWrap(P->UpperClip, P->UpperTime)), false));
		}
		if (P->UpperOldClip && P->UpperBlend < 1.0f)
		{
			FPoseContext Old(Output);
			FAnimationPoseData OldData(Old);
			P->UpperOldClip->GetAnimationPose(OldData, FAnimExtractContext(static_cast<double>(PawnUpperWrap(P->UpperOldClip, P->UpperOldTime)), false));
			for (FCompactPoseBoneIndex Bone : Stance.Pose.ForEachBoneIndex())
			{
				FTransform Mixed = Old.Pose[Bone];
				Mixed.BlendWith(Stance.Pose[Bone], P->UpperBlend);
				Stance.Pose[Bone] = Mixed;
			}
		}
		// spine_01: the clip's chest direction in component space, under the walk's own hips.
		const FQuat LocoPelvis = PawnUpperComponent(Output.Pose, Pelvis).GetRotation();
		const FQuat ClipSpine = PawnUpperComponent(Stance.Pose, Spine).GetRotation();
		const FQuat Wanted = (LocoPelvis.Inverse() * ClipSpine).GetNormalized();
		Output.Pose[Spine].SetRotation(FQuat::Slerp(Output.Pose[Spine].GetRotation(), Wanted, Weight).GetNormalized());
		for (FCompactPoseBoneIndex Bone : Output.Pose.ForEachBoneIndex())
		{
			const float W = BoneWeights[Bone.GetInt()] * Weight;
			if (W > ZERO_ANIMWEIGHT_THRESH)
			{
				FTransform Mixed = Output.Pose[Bone];
				Mixed.BlendWith(Stance.Pose[Bone], W);
				Output.Pose[Bone] = Mixed;
			}
		}
	}
	// The bend: the whole upper body tips forward about spine_01. The mesh faces +Y, so forward is a turn about -X.
	if (FMath::Abs(P->SpineBend) > 0.05f)
	{
		const FQuat PelvisRot = PawnUpperComponent(Output.Pose, Pelvis).GetRotation();
		const FQuat Delta(FVector::XAxisVector, -FMath::DegreesToRadians(P->SpineBend));
		const FQuat Local = Output.Pose[Spine].GetRotation();
		Output.Pose[Spine].SetRotation((PelvisRot.Inverse() * Delta * PelvisRot * Local).GetNormalized());
	}
}

// ---------------------------------------------------------------------------

void FSimPawnLocomotionNode::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	BlendStack.Initialize_AnyThread(Context);
	History.Initialize_AnyThread(PawnHistoryPoses, PawnHistoryPoseInterval);
	Trajectory.Samples.Reset();
	State.Reset();
	bResetHistory = true;
	StandTime = 0.0f;
	IdlePhase = FMath::FRand();
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

	StandTime = IsStanding(In) ? StandTime + DeltaTime : 0.0f;
	Stats.bStandingIdle = false;
	if (BlendStack.GetAnimAsset() == nullptr)
	{
		// Something to show from the first frame on, even while paused.
		if (IsStanding(In) && UpdateStandingIdle(Context, 0.0f))
		{
			StandTime = PawnStandDelay;
		}
		else
		{
			UpdateLoops(Context, In, 0.0f);
		}
	}
	else if (DeltaTime > 0.0f)
	{
		if (In.Detail == ESimPawnDetail::Full && !Proxy->Databases.IsEmpty())
		{
			// Standing still, the pawn breathes in the idle with its feet planted. Motion matching over the sample's idles
			// and turn-in-place clips kept picking weight shifts and small steps, so a standing pawn shuffled on the spot.
			if (StandTime >= PawnStandDelay && UpdateStandingIdle(Context, PawnStandBlendTime))
			{
				State.Reset();
				Stats.Database = NAME_None;
			}
			else
			{
				if (BlendStack.GetAnimAsset() == Proxy->IdleLoop)
				{
					State.Reset();  // walking off: search afresh (the idle is in no database)
				}
				UpdateMotionMatching(Context, DeltaTime);
			}
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

bool FSimPawnLocomotionNode::IsStanding(const FSimPawnAnimInputs& In)
{
	if (In.Velocity.Size2D() >= PawnStandSpeed)
	{
		return false;
	}
	float RouteLeft = 0.0f;
	FVector From = In.Location;
	for (const FVector& Point : In.Route)
	{
		RouteLeft += FVector::Dist2D(From, Point);
		From = Point;
	}
	return RouteLeft < PawnStandRoute;
}

bool FSimPawnLocomotionNode::UpdateStandingIdle(const FAnimationUpdateContext& Context, float BlendTime)
{
	UAnimSequence* Idle = Proxy->IdleLoop;
	if (!Idle)
	{
		return false;
	}
	if (BlendStack.GetAnimAsset() != Idle)
	{
		BlendStack.BlendTo(Context, Idle, IdlePhase * Idle->GetPlayLength(), true, false, nullptr, BlendTime, nullptr,
			EAlphaBlendOption::HermiteCubic, false, NAME_None, FVector::ZeroVector, 1.0f);
	}
	BlendStack.UpdatePlayRate(1.0f);
	Stats.PlayRate = 1.0f;
	Stats.bStandingIdle = true;
	return true;
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
		BlendStack.BlendTo(Context, Loop, Loop == Proxy->IdleLoop ? IdlePhase * Loop->GetPlayLength() : 0.0f, true, false, nullptr, BlendTime, nullptr, EAlphaBlendOption::HermiteCubic,
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

void USimPawnAnimInstance::SetUpperStance(UAnimSequence* Clip, float Weight, float BlendSeconds, float WeightRate)
{
	UpperWeightTarget = FMath::Clamp(Weight, 0.0f, 1.0f);
	UpperWeightRate = FMath::Max(WeightRate, 0.1f);
	if (Clip == UpperClip || !Clip)
	{
		// No clip: keep the last one while the weight fades out.
		if (!Clip)
		{
			UpperWeightTarget = 0.0f;
		}
		return;
	}
	if (!UpperClip || UpperWeight <= 0.01f)
	{
		// Nothing showing: start straight on the new clip.
		UpperClip = Clip;
		UpperTime = 0.0f;
		UpperBlend = 1.0f;
		UpperOldClip = nullptr;
		return;
	}
	UpperOldClip = UpperClip;
	UpperOldTime = UpperTime;
	UpperClip = Clip;
	UpperTime = 0.0f;
	UpperBlend = 0.0f;
	UpperBlendRate = BlendSeconds > 0.0f ? 1.0f / BlendSeconds : 1000.0f;
}

void USimPawnAnimInstance::AdvanceUpper(float DeltaSeconds)
{
	const float Dt = FMath::Max(DeltaSeconds, 0.0f);
	UpperTime += Dt;
	UpperOldTime += Dt;
	UpperBlend = FMath::Min(1.0f, UpperBlend + Dt * UpperBlendRate);
	if (UpperBlend >= 1.0f)
	{
		UpperOldClip = nullptr;
	}
	UpperWeight = FMath::FInterpConstantTo(UpperWeight, UpperWeightTarget, Dt, UpperWeightRate);
	SpineBend = FMath::FInterpConstantTo(SpineBend, SpineBendTarget, Dt, 60.0f);
}
