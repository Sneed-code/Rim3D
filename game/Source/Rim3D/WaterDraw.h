#pragma once

#include "CoreMinimal.h"

#include "ColonySim/Snapshot.h"

/**
 * How bodies in water are drawn (docs/design/water.md, W4). The sim says how deep the water is at a pawn's feet and
 * whether it swims; a swimming body is drawn floating with its shoulders at the surface (its feet 80% of its height
 * below it) instead of walking on the bottom. Only the picture moves: the sim's position is unchanged.
 */
namespace WaterDraw
{
	/** Share of a body's height under the surface while it swims (shoulders at the surface). */
	constexpr float SwimSubmergedShare = 0.8f;

	/** Centimetres a body is drawn above the ground at its feet: 0 unless it swims. */
	inline float SwimLiftTargetCm(const colony::ThingView& Now, float BodyHeightM)
	{
		if (!Now.swimming)
		{
			return 0.0f;
		}
		return FMath::Max(0.0f, (Now.waterDepth - SwimSubmergedShare * BodyHeightM) * 100.0f);
	}

	/** Eases the drawn lift towards its target so a body floats up and settles instead of jumping. */
	inline float SwimLiftEase(float Current, float Target, float DeltaTime, bool bSnap)
	{
		return bSnap ? Target : FMath::FInterpTo(Current, Target, DeltaTime, 3.0f);
	}
}
