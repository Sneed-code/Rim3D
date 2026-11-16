#include "GunViewAnim.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"

namespace
{
	float GunViewClipTime(const UAnimSequence* Clip, float Time, float Hold)
	{
		if (!Clip)
		{
			return 0.0f;
		}
		const float Length = FMath::Max(Clip->GetPlayLength(), 0.001f);
		return Hold >= 0.0f ? FMath::Clamp(Hold, 0.0f, Length) : FMath::Fmod(FMath::Max(Time, 0.0f), Length);
	}
}

void UGunViewAnimInstance::SetStance(UAnimSequence* Clip, float Hold, float BlendSeconds, float Rate)
{
	if (Clip == Stance && Hold == StanceHold)
	{
		StanceRate = Rate;
		return;
	}
	OldStance = Stance;
	OldStanceTime = StanceTime;
	OldStanceHold = StanceHold;
	Stance = Clip;
	StanceTime = 0.0f;
	StanceHold = Hold;
	StanceRate = Rate;
	StanceBlend = OldStance ? 0.0f : 1.0f;
	StanceBlendRate = BlendSeconds > 0.0f ? 1.0f / BlendSeconds : 1000.0f;
}

void UGunViewAnimInstance::PlayAction(UAnimSequence* Clip, float From, float To, float Rate, float BlendIn, float BlendOut)
{
	if (!Clip)
	{
		return;
	}
	Action = Clip;
	ActionFrom = FMath::Clamp(From, 0.0f, Clip->GetPlayLength());
	ActionTo = To > ActionFrom ? FMath::Min(To, Clip->GetPlayLength()) : Clip->GetPlayLength();
	ActionTime = ActionFrom;
	ActionRate = FMath::Max(Rate, 0.01f);
	ActionBlendIn = FMath::Max(BlendIn, 0.01f);
	ActionBlendOut = FMath::Max(BlendOut, 0.01f);
	bActionStopping = false;
}

void UGunViewAnimInstance::StopAction(float BlendOut)
{
	if (Action)
	{
		bActionStopping = true;
		ActionBlendOut = FMath::Max(BlendOut, 0.01f);
	}
}

float UGunViewAnimInstance::GetActionFraction() const
{
	if (!Action || ActionTo <= ActionFrom)
	{
		return -1.0f;
	}
	return FMath::Clamp((ActionTime - ActionFrom) / (ActionTo - ActionFrom), 0.0f, 1.0f);
}

void UGunViewAnimInstance::Advance(float DeltaSeconds)
{
	StanceTime += DeltaSeconds * StanceRate;
	OldStanceTime += DeltaSeconds * StanceRate;
	StanceBlend = FMath::Min(1.0f, StanceBlend + DeltaSeconds * StanceBlendRate);
	if (StanceBlend >= 1.0f)
	{
		OldStance = nullptr;
	}
	if (!Action)
	{
		ActionWeight = 0.0f;
		return;
	}
	ActionTime += DeltaSeconds * ActionRate;
	// In over BlendIn (real seconds), out over the last BlendOut of the played span, or at once when stopped.
	const float RealLeft = (ActionTo - ActionTime) / ActionRate;
	const float RealIn = (ActionTime - ActionFrom) / ActionRate;
	float Target = FMath::Min(1.0f, FMath::Min(RealIn / ActionBlendIn, RealLeft / ActionBlendOut));
	if (bActionStopping)
	{
		Target = FMath::Max(0.0f, ActionWeight - DeltaSeconds / ActionBlendOut);
	}
	ActionWeight = FMath::Clamp(Target, 0.0f, 1.0f);
	if (ActionTime >= ActionTo || (bActionStopping && ActionWeight <= 0.0f))
	{
		Action = nullptr;
		ActionWeight = 0.0f;
		bActionStopping = false;
	}
}

void FGunViewAnimProxy::PreUpdate(UAnimInstance* Instance, float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
	UGunViewAnimInstance* Gun = Cast<UGunViewAnimInstance>(Instance);
	if (!Gun)
	{
		return;
	}
	Gun->Advance(DeltaSeconds);
	Stance = Gun->Stance;
	OldStance = Gun->OldStance;
	Action = Gun->Action;
	StanceTime = GunViewClipTime(Gun->Stance, Gun->StanceTime, Gun->StanceHold);
	OldStanceTime = GunViewClipTime(Gun->OldStance, Gun->OldStanceTime, Gun->OldStanceHold);
	StanceWeight = Gun->StanceBlend;
	ActionTime = Gun->ActionTime;
	ActionWeight = Gun->ActionWeight;
}

bool FGunViewAnimProxy::Evaluate(FPoseContext& Output)
{
	UAnimSequence* Main = Stance.Get();
	if (!Main)
	{
		Output.ResetToRefPose();
		return true;
	}
	{
		FAnimationPoseData Data(Output);
		Main->GetAnimationPose(Data, FAnimExtractContext(static_cast<double>(StanceTime), false));
	}
	if (UAnimSequence* Old = OldStance.Get(); Old && StanceWeight < 1.0f)
	{
		FPoseContext OldPose(Output);
		FAnimationPoseData OldData(OldPose);
		Old->GetAnimationPose(OldData, FAnimExtractContext(static_cast<double>(OldStanceTime), false));
		FPoseContext Mixed(Output);
		FAnimationPoseData MainData(Output);
		FAnimationPoseData MixedData(Mixed);
		FAnimationRuntime::BlendTwoPosesTogether(MainData, OldData, StanceWeight, MixedData);
		Output.Pose.CopyBonesFrom(Mixed.Pose);
		Output.Curve.CopyFrom(Mixed.Curve);
	}
	if (UAnimSequence* Act = Action.Get(); Act && ActionWeight > 0.0f)
	{
		FPoseContext ActPose(Output);
		FAnimationPoseData ActData(ActPose);
		Act->GetAnimationPose(ActData, FAnimExtractContext(static_cast<double>(ActionTime), false));
		FPoseContext Mixed(Output);
		FAnimationPoseData MainData(Output);
		FAnimationPoseData MixedData(Mixed);
		FAnimationRuntime::BlendTwoPosesTogether(ActData, MainData, ActionWeight, MixedData);
		Output.Pose.CopyBonesFrom(Mixed.Pose);
		Output.Curve.CopyFrom(Mixed.Curve);
	}
	return true;
}

FAnimInstanceProxy* UGunViewAnimInstance::CreateAnimInstanceProxy()
{
	return new FGunViewAnimProxy(this);
}

void UGunViewAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy)
{
	delete static_cast<FGunViewAnimProxy*>(Proxy);
}
