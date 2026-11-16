#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"

#include "GunViewAnim.generated.h"

class UAnimSequence;

/**
 * The gun range's first-person arms (docs/design/gun-range.md): a native animator for the hidden mannequin the arms
 * are retargeted from. No animation graph: it samples clips itself and blends them, so a gun's stance can cross-fade
 * into another gun's and a reload clip can blend in over the stance and back out.
 *
 * - The stance: one looping clip, at a fixed time or playing (StanceTime < 0 plays it). A new stance cross-fades from
 *   the old one over BlendSeconds.
 * - The action: a clip played once over the stance (a reload), from From to To seconds of it, at a rate, with a blend in
 *   and out.
 */
USTRUCT()
struct FGunViewAnimProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FGunViewAnimProxy() = default;
	explicit FGunViewAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

	// Copied from the instance before each update (game thread).
	TWeakObjectPtr<UAnimSequence> Stance;
	TWeakObjectPtr<UAnimSequence> OldStance;
	TWeakObjectPtr<UAnimSequence> Action;
	float StanceTime = 0.0f;
	float OldStanceTime = 0.0f;
	float StanceWeight = 1.0f;  // of the new stance against the old one
	float ActionTime = 0.0f;
	float ActionWeight = 0.0f;

	virtual void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;
};

UCLASS(Transient, NotBlueprintable)
class UGunViewAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** Loops Clip as the stance (cross-fading from the one before); Hold >= 0 holds that second of it instead. */
	void SetStance(UAnimSequence* Clip, float Hold = -1.0f, float BlendSeconds = 0.25f, float Rate = 1.0f);
	/** Plays seconds From..To of Clip once over the stance at Rate, blending in and out. To <= 0: to its end. */
	void PlayAction(UAnimSequence* Clip, float From, float To, float Rate, float BlendIn = 0.2f, float BlendOut = 0.3f);
	void StopAction(float BlendOut = 0.2f);
	bool IsActionPlaying() const { return Action != nullptr && ActionTime < ActionTo; }
	/** 0..1 through the action's From..To, or -1 when none plays. */
	float GetActionFraction() const;
	float GetActionWeight() const { return ActionWeight; }
	UAnimSequence* GetStance() const { return Stance; }

	/** Advances the clocks; called by the proxy's PreUpdate on the game thread. */
	void Advance(float DeltaSeconds);

	UPROPERTY()
	TObjectPtr<UAnimSequence> Stance;
	UPROPERTY()
	TObjectPtr<UAnimSequence> OldStance;
	UPROPERTY()
	TObjectPtr<UAnimSequence> Action;
	float StanceTime = 0.0f;
	float OldStanceTime = 0.0f;
	float StanceHold = -1.0f;
	float OldStanceHold = -1.0f;
	float StanceRate = 1.0f;
	float StanceBlend = 1.0f;     // 0..1, the new stance's share
	float StanceBlendRate = 4.0f; // per second
	float ActionTime = 0.0f;
	float ActionFrom = 0.0f;
	float ActionTo = 0.0f;
	float ActionRate = 1.0f;
	float ActionBlendIn = 0.2f;
	float ActionBlendOut = 0.3f;
	float ActionWeight = 0.0f;
	bool bActionStopping = false;

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override;
};
