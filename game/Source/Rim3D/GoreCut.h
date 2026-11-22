#pragma once

#include "CoreMinimal.h"
#include "AnimationRuntime.h"
#include "ReferenceSkeleton.h"

// Clean cuts for dismemberment (docs/design/body-health.md "Gore"). Shared by the MakeHuman sever node (the body's
// side), the severed piece (the other side) and the stump caps, so all three agree on where the cut plane lies.
//
// A cut limb is flattened rather than shrunk to a point: the cut bone is straightened onto its parent (as in the
// mesh's reference pose) and squashed to nothing along the limb, at the joint. Skin on it keeps its width and ends
// in a flat face across the limb, which the stump cap (skin rim, fat, muscle, bone) covers. Bones further down fold
// into the middle of that face.

namespace GoreCut
{
	/** How flat a flattened bone is along the limb. */
	inline constexpr float Flat = 0.001f;
	/** A bone whose limb direction is further than this (cosine) from all of its own axes can't be flattened cleanly:
	 *  it folds to a point instead. */
	inline constexpr float MinAlign = 0.85f;

	/** The way a cut limb runs, in the cut bone's own frame (unit): towards its longest child (the hand from the forearm,
	 *  the calf from the thigh); for the head, or a bone without children, the way its parent ran into it. */
	inline FVector AxisLocal(const FReferenceSkeleton& Ref, int32 Bone)
	{
		if (Bone == INDEX_NONE || Bone >= Ref.GetNum())
		{
			return FVector::XAxisVector;
		}
		const TArray<FTransform>& Pose = Ref.GetRefBonePose();
		const bool bHead = Ref.GetBoneName(Bone).ToString().StartsWith(TEXT("head"));
		FVector Best = FVector::ZeroVector;
		if (!bHead)
		{
			for (int32 Child = Bone + 1; Child < Ref.GetNum(); ++Child)
			{
				if (Ref.GetParentIndex(Child) == Bone && Pose[Child].GetTranslation().SizeSquared() > Best.SizeSquared())
				{
					Best = Pose[Child].GetTranslation();
				}
			}
		}
		if (Best.SizeSquared() < 1.0)
		{
			// Along the parent's line, seen from this bone.
			Best = Pose[Bone].GetRotation().Inverse().RotateVector(Pose[Bone].GetTranslation());
		}
		return Best.GetSafeNormal(UE_SMALL_NUMBER, FVector::XAxisVector);
	}

	/** The way the face looks, in `Bone`'s own frame (unit), from the face bones under it (jaw, eyes, nose, lips) in the
	 *  reference pose; zero if it has none. Puts the windpipe at the front of a neck stump. */
	inline FVector FrontLocal(const FReferenceSkeleton& Ref, int32 Bone)
	{
		if (Bone == INDEX_NONE || Bone >= Ref.GetNum())
		{
			return FVector::ZeroVector;
		}
		const FTransform BoneCS = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bone);
		FVector Sum = FVector::ZeroVector;
		for (int32 i = Bone + 1; i < Ref.GetNum(); ++i)
		{
			const FString Name = Ref.GetBoneName(i).ToString().ToLower();
			if (Ref.BoneIsChildOf(i, Bone) && (Name.Contains(TEXT("jaw")) || Name.Contains(TEXT("eye")) || Name.Contains(TEXT("nose")) ||
				Name.Contains(TEXT("lip")) || Name.Contains(TEXT("chin"))))
			{
				Sum += BoneCS.InverseTransformPositionNoScale(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, i).GetLocation());
			}
		}
		return Sum.GetSafeNormal();
	}

	/** The normal of the flat face FlattenCut leaves, in the cut bone's frame: the axis it squashes, signed down the
	 *  limb. Just the limb's line where the bone can't be flattened. */
	inline FVector FaceNormalLocal(const FVector& Axis)
	{
		const FVector A = Axis.GetAbs();
		const int32 Along = A.X >= A.Y && A.X >= A.Z ? 0 : (A.Y >= A.Z ? 1 : 2);
		if (A[Along] < MinAlign)
		{
			return Axis;
		}
		FVector Normal = FVector::ZeroVector;
		Normal[Along] = Axis[Along] >= 0.0 ? 1.0 : -1.0;
		return Normal;
	}

	/** Which of a frame's axes (0 X, 1 Y, 2 Z) a unit vector lies along, and how well (|cosine|). */
	inline int32 Dominant(const FVector& Unit, float& OutAlign)
	{
		const FVector A = Unit.GetAbs();
		const int32 Axis = A.X >= A.Y && A.X >= A.Z ? 0 : (A.Y >= A.Z ? 1 : 2);
		OutAlign = static_cast<float>(A[Axis]);
		return Axis;
	}

	/** A scale that squashes one axis flat. */
	inline FVector Squash(int32 Axis)
	{
		FVector Scale(1.0);
		Scale[Axis] = Flat;
		return Scale;
	}

	/**
	 * The body's side: the cut bone straightened onto its parent and flattened along the limb at the joint. False if it
	 * can't be done cleanly (then fold it to a point as before). `ParentCS` is the parent's transform now, `RefLocal` the
	 * cut bone's reference transform under it, `Axis` its AxisLocal.
	 */
	inline bool FlattenCut(const FTransform& ParentCS, const FTransform& RefLocal, const FVector& Axis, FTransform& Out)
	{
		float Align = 0.0f;
		const int32 Along = Dominant(Axis, Align);
		Out = RefLocal * ParentCS;
		Out.SetScale3D(FVector::OneVector);
		if (Align < MinAlign)
		{
			return false;
		}
		Out.SetScale3D(Squash(Along));
		return true;
	}

	/**
	 * The piece's side: the parent bone of the cut placed as it would be with the piece in its reference pose under it,
	 * and flattened onto the joint across the piece's line, so whatever skin it has on the piece ends in a flat face at
	 * the cut. `CutCS` is where the piece's cut bone is. False if it can't be done cleanly (then fold the parent to a
	 * point at the cut).
	 */
	inline bool FlattenParent(const FTransform& CutCS, const FTransform& RefLocal, const FVector& Axis, FTransform& Out)
	{
		FTransform Cut = CutCS;
		Cut.SetScale3D(FVector::OneVector);
		Out = RefLocal.Inverse() * Cut;
		Out.SetScale3D(FVector::OneVector);
		float Align = 0.0f;
		const int32 Along = Dominant(RefLocal.GetRotation().RotateVector(Axis), Align);
		if (Align < MinAlign)
		{
			return false;
		}
		// Squashed about the joint (the cut bone's place in the parent's frame), not about the parent's own origin.
		FVector Shift = FVector::ZeroVector;
		Shift[Along] = RefLocal.GetTranslation()[Along] * (1.0 - Flat);
		Out.SetTranslation(Out.GetTranslation() + Out.GetRotation().RotateVector(Shift));
		Out.SetScale3D(Squash(Along));
		return true;
	}
}
