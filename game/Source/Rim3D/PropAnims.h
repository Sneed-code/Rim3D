#pragma once

#include "CoreMinimal.h"

/**
 * The moving parts of the Neon models (docs/design/neon-assets.md, "Moving parts"): each model's glTF actions, sampled by
 * tools/codex_prep.py into Content/Rim3D/Generated/PropAnims/<Name>.json (not in Git). A track is a part's motion away
 * from the pose its mesh was baked in, in the model's own frame (cm): put it before the model's placement
 * (Delta * Placed). Cosmetic only: the sim never knows a dish turns.
 */
namespace PropAnims
{
	struct FTrack
	{
		TArray<float> Times;
		TArray<FVector> Locations;
		TArray<FQuat> Rotations;
		TArray<FVector> Scales;
	};

	struct FClip
	{
		float Seconds = 0.0f;
		TMap<FString, FTrack> Parts;
	};

	struct FSet
	{
		TMap<FString, FClip> Clips;
		/** Each part's pose when nothing plays (identity, or scale 0 for a part hidden at rest, like a spark). */
		TMap<FString, FTransform> Rest;
	};

	/** The set of a model by name, read once; null when the file is missing (the codex-models art step has not run). */
	const FSet* Find(const FString& Name);

	/** A track's pose at `Time` seconds (clamped to its ends): straight blends of location and scale, slerped rotation. */
	FTransform Sample(const FTrack& Track, float Time);

	/** The part's pose for a clip at `Time` (wrapped when `bLoop`, else held at the end); false when the clip doesn't move it. */
	bool Evaluate(const FSet& Set, const FString& Clip, const FString& Part, float Time, bool bLoop, FTransform& Out);

	/** The part's rest pose (identity if the set says nothing). */
	FTransform RestOf(const FSet& Set, const FString& Part);

	/** Drops what was read, so the next Find reads the files again (a new sim, after an art step). */
	void Reset();
}
