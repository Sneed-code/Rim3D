#pragma once

#include "CoreMinimal.h"

class USkeletalMeshComponent;

/**
 * The body as it is drawn, as a target (docs/design/aiming.md "Shots go where the crosshair is"): capsules from bone to
 * bone of the animated mesh this frame, each named after the sim hitbox part it stands for ("head", "left arm"...).
 * The player's crosshair is traced against these, so what you aim at is what you see, whatever the clip is doing.
 */
namespace DrawnHit
{
	struct FResult
	{
		double DistanceCm = 0.0;   // along the ray from its start
		FVector Point = FVector::ZeroVector;
		const char* Part = nullptr;  // the sim hitbox label
	};

	/** The nearest capsule of the drawn body the ray from `Start` along unit `Direction` enters within `MaxCm`. `Scale` is
	 *  the body's height factor (radii grow with it). False if it meets none, or the mesh lacks the bones. */
	bool Trace(const USkeletalMeshComponent& Mesh, float Scale, const FVector& Start, const FVector& Direction, double MaxCm, FResult& Out);

	/** Whether the mesh has the bones the capsules need (a humanlike skeleton). */
	bool HasSkeleton(const USkeletalMeshComponent& Mesh);
}
