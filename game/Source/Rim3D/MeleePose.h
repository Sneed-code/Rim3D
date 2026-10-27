#pragma once

#include "CoreMinimal.h"

/**
 * The look of a melee swing (docs/design/melee-draw.md): a procedural pose for the weapon and the body,
 * timed to the sim. There are no human melee animations in the project, so a swing is a short track of
 * key poses (draw back, strike, follow through, recover) driven by the sim's MeleeSwing and MeleeHit
 * events. Plain maths, no sim and no Unreal objects, so the smoke test can run it too.
 *
 * Everything is in the pawn actor's space: x forward, y right, z up, centimetres, degrees. Weapon meshes
 * lie along +x with the grip at the origin (the defs' `visual.parts`).
 */

/** How a swing moves. */
enum class EMeleeStyle : uint8
{
	/** Overhead chop: sword, club, mace, axe. */
	Chop,
	/** A thrust straight ahead: spear, knife. */
	Thrust,
	/** Fists, teeth: the body lunges, no weapon in the hand. */
	Punch,
};

/** One pose: where the weapon is held and how the body leans. */
struct FMeleePose
{
	FVector WeaponLocation = FVector(24.0, 20.0, 100.0);
	/** Pitch positive points the weapon's tip up. */
	FRotator WeaponRotation = FRotator(50.0, 0.0, 0.0);
	/** Degrees the body leans forward (negative: back). */
	float Lean = 0.0f;
	/** Degrees the body twists to the right (negative: to the left). */
	float Twist = 0.0f;
	/** Degrees the top of the body tips to the right. */
	float Tilt = 0.0f;
	/** Centimetres the body moves forward. */
	float Lunge = 0.0f;
	/** Centimetres the body drops. */
	float Dip = 0.0f;

	static FMeleePose Lerp(const FMeleePose& A, const FMeleePose& B, float T)
	{
		FMeleePose Out;
		Out.WeaponLocation = FMath::Lerp(A.WeaponLocation, B.WeaponLocation, static_cast<double>(T));
		Out.WeaponRotation = FQuat::Slerp(A.WeaponRotation.Quaternion(), B.WeaponRotation.Quaternion(), T).Rotator();
		Out.Lean = FMath::Lerp(A.Lean, B.Lean, T);
		Out.Twist = FMath::Lerp(A.Twist, B.Twist, T);
		Out.Tilt = FMath::Lerp(A.Tilt, B.Tilt, T);
		Out.Lunge = FMath::Lerp(A.Lunge, B.Lunge, T);
		Out.Dip = FMath::Lerp(A.Dip, B.Dip, T);
		return Out;
	}
};

namespace MeleePose
{
	/** The weapon held at the side, tip up and forward. `Height` is 1 standing, less crouching and prone. */
	inline FMeleePose Rest(float Height)
	{
		FMeleePose Pose;
		Pose.WeaponLocation = FVector(24.0, 20.0, 100.0 * Height);
		Pose.WeaponRotation = FRotator(50.0, 0.0, 0.0);
		return Pose;
	}

	/** The weapon across the body, held up in front of the chest. */
	inline FMeleePose Block(float Height)
	{
		FMeleePose Pose;
		Pose.WeaponLocation = FVector(34.0, 0.0, 118.0 * Height);
		Pose.WeaponRotation = FRotator(18.0, 90.0, 0.0);
		Pose.Lean = 4.0f;
		Pose.Lunge = -3.0f;
		return Pose;
	}

	inline FMeleePose Windup(EMeleeStyle Style, float Height)
	{
		FMeleePose Pose;
		switch (Style)
		{
			case EMeleeStyle::Chop:
				Pose.WeaponLocation = FVector(-4.0, 26.0, 152.0 * Height);
				Pose.WeaponRotation = FRotator(118.0, 8.0, 0.0);
				Pose.Lean = -7.0f;
				Pose.Twist = -12.0f;
				Pose.Lunge = -6.0f;
				break;
			case EMeleeStyle::Thrust:
				Pose.WeaponLocation = FVector(-6.0, 20.0, 108.0 * Height);
				Pose.WeaponRotation = FRotator(2.0, 0.0, 0.0);
				Pose.Lean = -5.0f;
				Pose.Twist = -14.0f;
				Pose.Lunge = -10.0f;
				break;
			default:
				Pose.WeaponLocation = FVector(-4.0, 20.0, 108.0 * Height);
				Pose.WeaponRotation = FRotator(0.0, 0.0, 0.0);
				Pose.Lean = -8.0f;
				Pose.Twist = -18.0f;
				Pose.Lunge = -8.0f;
				break;
		}
		return Pose;
	}

	inline FMeleePose Strike(EMeleeStyle Style, float Height)
	{
		FMeleePose Pose;
		switch (Style)
		{
			case EMeleeStyle::Chop:
				Pose.WeaponLocation = FVector(54.0, 6.0, 90.0 * Height);
				Pose.WeaponRotation = FRotator(-28.0, -4.0, 0.0);
				Pose.Lean = 16.0f;
				Pose.Twist = 12.0f;
				Pose.Lunge = 16.0f;
				break;
			case EMeleeStyle::Thrust:
				Pose.WeaponLocation = FVector(62.0, 12.0, 108.0 * Height);
				Pose.WeaponRotation = FRotator(0.0, -3.0, 0.0);
				Pose.Lean = 14.0f;
				Pose.Twist = 10.0f;
				Pose.Lunge = 22.0f;
				break;
			default:
				Pose.WeaponLocation = FVector(50.0, 14.0, 110.0 * Height);
				Pose.WeaponRotation = FRotator(0.0, 0.0, 0.0);
				Pose.Lean = 14.0f;
				Pose.Twist = 20.0f;
				Pose.Lunge = 20.0f;
				break;
		}
		return Pose;
	}

	inline FMeleePose FollowThrough(EMeleeStyle Style, float Height)
	{
		FMeleePose Pose = Strike(Style, Height);
		Pose.WeaponLocation += FVector(Style == EMeleeStyle::Chop ? -6.0 : 4.0, -2.0, Style == EMeleeStyle::Chop ? -18.0 * Height : 0.0);
		if (Style == EMeleeStyle::Chop)
		{
			Pose.WeaponRotation = FRotator(-55.0, -6.0, 0.0);
		}
		Pose.Lean += 3.0f;
		Pose.Twist += 4.0f;
		Pose.Lunge += 3.0f;
		return Pose;
	}

	/** Smooth start and end. */
	inline float EaseInOut(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return T * T * (3.0f - 2.0f * T);
	}

	/** Slow start, fast end: the blow speeds up into its target. */
	inline float EaseIn(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return T * T * T;
	}

	/** Fast start, slow end. */
	inline float EaseOut(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		return 1.0f - FMath::Cube(1.0f - T);
	}
}

/**
 * One pawn's swing: a clock in sim steps that runs from the MeleeSwing event to the hit moment, then
 * through the follow-through and recovery. The track is
 *
 *  - draw back (until `kStrikeSteps` before the hit): rest to the wound-up pose;
 *  - strike (the sim's commit window, `kStrikeSteps`, ends at the hit): wound-up to the blow, speeding up;
 *  - follow through, then recover to rest.
 *
 * The clock stops at the hit moment until the sim's MeleeHit arrives, so the blow lands on the sim's step
 * and never before it. Hit-stop freezes it for a moment (Advance with 0 steps).
 */
struct FMeleeSwingTrack
{
	/** The sim's `commitSteps`: the strike starts this many steps before the hit. */
	static constexpr float kStrikeSteps = 6.0f;
	static constexpr float kFollowSteps = 5.0f;
	static constexpr float kRecoverSteps = 14.0f;

	enum class EPhase : uint8
	{
		Idle,
		DrawBack,
		Strike,
		WaitForHit,
		Follow,
		Recover,
	};

	EPhase Phase = EPhase::Idle;
	EMeleeStyle Style = EMeleeStyle::Chop;
	float Height = 1.0f;
	/** Steps into the phase and its length. */
	float Clock = 0.0f;
	float Length = 0.0f;
	FMeleePose From;
	FMeleePose Current = MeleePose::Rest(1.0f);
	/** Swings drawn and hit moments reached, for tests. */
	int32 Swings = 0;
	int32 HitMoments = 0;

	bool IsActive() const { return Phase != EPhase::Idle; }
	/** True from the start of the swing until the hit moment: the weapon is going back or coming down. */
	bool IsWinding() const { return Phase == EPhase::DrawBack || Phase == EPhase::Strike || Phase == EPhase::WaitForHit; }

	/** Sim steps until the blow lands on screen (0 once it has, or when there is no swing). */
	float StepsToHit() const
	{
		switch (Phase)
		{
			case EPhase::DrawBack: return (Length - Clock) + StrikeSteps;
			case EPhase::Strike: return Length - Clock;
			default: return 0.0f;
		}
	}

	/** The sim says a swing began and the hit comes `StepsToHit` steps later. */
	void Begin(EMeleeStyle InStyle, float StepsToHit, float InHeight)
	{
		Style = InStyle;
		Height = InHeight;
		From = Current;
		++Swings;
		const float Strike = FMath::Min(kStrikeSteps, StepsToHit * 0.5f);
		StrikeSteps = Strike;
		Phase = EPhase::DrawBack;
		Clock = 0.0f;
		Length = FMath::Max(0.0f, StepsToHit - Strike);
		if (Length <= 0.0f)
		{
			EnterStrike();
		}
	}

	/** The sim's hit moment came: if the blow has not landed on screen yet it does now. */
	void HitMoment()
	{
		if (!IsWinding())
		{
			return;
		}
		++HitMoments;
		Current = MeleePose::Strike(Style, Height);
		EnterFollow();
	}

	/** Runs the clock `Steps` sim steps (real time x game speed x 30; 0 while the hit-stop holds it). */
	void Advance(float Steps)
	{
		if (Phase == EPhase::Idle || Phase == EPhase::WaitForHit)
		{
			return;
		}
		Clock += Steps;
		while (Phase != EPhase::Idle && Phase != EPhase::WaitForHit && Clock >= Length)
		{
			const float Over = Clock - Length;
			Evaluate(1.0f);
			switch (Phase)
			{
				case EPhase::DrawBack: EnterStrike(); break;
				case EPhase::Strike: Phase = EPhase::WaitForHit; Clock = Length = 0.0f; From = Current; return;
				case EPhase::Follow: EnterRecover(); break;
				default: Phase = EPhase::Idle; Current = MeleePose::Rest(Height); return;
			}
			Clock = Over;
		}
		Evaluate(Length > 0.0f ? Clock / Length : 1.0f);
	}

	/** The swing was called off (the attacker went down, lost its weapon): recover from wherever it is. */
	void Abort()
	{
		if (IsWinding())
		{
			From = Current;
			Phase = EPhase::Recover;
			Clock = 0.0f;
			Length = kRecoverSteps;
		}
	}

	/** Back to rest at once. */
	void Reset()
	{
		Phase = EPhase::Idle;
		Current = MeleePose::Rest(Height);
	}

private:
	float StrikeSteps = kStrikeSteps;

	void EnterStrike()
	{
		From = MeleePose::Windup(Style, Height);
		Current = From;
		Phase = EPhase::Strike;
		Clock = 0.0f;
		Length = StrikeSteps;
	}

	void EnterFollow()
	{
		From = Current;
		Phase = EPhase::Follow;
		Clock = 0.0f;
		Length = kFollowSteps;
	}

	void EnterRecover()
	{
		From = MeleePose::FollowThrough(Style, Height);
		Current = From;
		Phase = EPhase::Recover;
		Clock = 0.0f;
		Length = kRecoverSteps;
	}

	void Evaluate(float T)
	{
		switch (Phase)
		{
			case EPhase::DrawBack:
				Current = FMeleePose::Lerp(From, MeleePose::Windup(Style, Height), MeleePose::EaseInOut(T));
				break;
			case EPhase::Strike:
				Current = FMeleePose::Lerp(From, MeleePose::Strike(Style, Height), MeleePose::EaseIn(T));
				break;
			case EPhase::WaitForHit:
				Current = MeleePose::Strike(Style, Height);
				break;
			case EPhase::Follow:
				Current = FMeleePose::Lerp(From, MeleePose::FollowThrough(Style, Height), MeleePose::EaseOut(T));
				break;
			case EPhase::Recover:
				Current = FMeleePose::Lerp(From, MeleePose::Rest(Height), MeleePose::EaseInOut(T));
				break;
			default:
				break;
		}
	}
};
