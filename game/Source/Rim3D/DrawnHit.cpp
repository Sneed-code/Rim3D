#include "DrawnHit.h"

#include "Components/SkeletalMeshComponent.h"

namespace
{
	// Where the ray first enters a capsule (A to B, radius R): the distance along it, or -1. Same maths as the sim's
	// combatSegmentCapsule, in doubles.
	double DrawnHitRaySphere(const FVector& P, const FVector& U, const FVector& C, double R, double MaxT)
	{
		const FVector Oc = P - C;
		const double B = FVector::DotProduct(Oc, U);
		const double Cc = FVector::DotProduct(Oc, Oc) - R * R;
		if (Cc <= 0.0)
		{
			return 0.0;
		}
		const double H = B * B - Cc;
		if (H < 0.0)
		{
			return -1.0;
		}
		const double T = -B - FMath::Sqrt(H);
		return T >= 0.0 && T <= MaxT ? T : -1.0;
	}

	double DrawnHitRayCapsule(const FVector& P, const FVector& U, const FVector& A, const FVector& B, double R, double MaxT)
	{
		double Best = -1.0;
		auto Keep = [&Best](double T)
		{
			if (T >= 0.0 && (Best < 0.0 || T < Best))
			{
				Best = T;
			}
		};
		Keep(DrawnHitRaySphere(P, U, A, R, MaxT));
		Keep(DrawnHitRaySphere(P, U, B, R, MaxT));
		const FVector Ba = B - A;
		const double Baba = FVector::DotProduct(Ba, Ba);
		if (Baba > 1e-6)
		{
			const FVector Oa = P - A;
			const double Bard = FVector::DotProduct(Ba, U);
			const double Baoa = FVector::DotProduct(Ba, Oa);
			const double Rdoa = FVector::DotProduct(U, Oa);
			const double Oaoa = FVector::DotProduct(Oa, Oa);
			const double Qa = Baba - Bard * Bard;
			const double Qb = Baba * Rdoa - Baoa * Bard;
			const double Qc = Baba * Oaoa - Baoa * Baoa - R * R * Baba;
			if (Qc <= 0.0 && Baoa >= 0.0 && Baoa <= Baba)
			{
				Keep(0.0);
			}
			else if (Qa > 1e-9)
			{
				const double H = Qb * Qb - Qa * Qc;
				if (H >= 0.0)
				{
					const double T = (-Qb - FMath::Sqrt(H)) / Qa;
					const double Y = Baoa + T * Bard;
					if (T >= 0.0 && T <= MaxT && Y > 0.0 && Y < Baba)
					{
						Keep(T);
					}
				}
			}
		}
		return Best;
	}

	bool DrawnHitBone(const USkeletalMeshComponent& Mesh, const TCHAR* Name, FVector& Out)
	{
		if (Mesh.GetBoneIndex(FName(Name)) == INDEX_NONE)
		{
			return false;
		}
		Out = Mesh.GetBoneLocation(FName(Name));
		return true;
	}

	struct FDrawnHitCapsule
	{
		FVector A;
		FVector B;
		double R;
		const char* Part;
	};

	// The capsules (cm, for a 1.80 m body; radii times the body's scale). Bone names are the UE mannequin's, which the
	// MakeHuman game_engine skeleton and the MetaHuman body share.
	void DrawnHitCapsules(const USkeletalMeshComponent& Mesh, float Scale, TArray<FDrawnHitCapsule, TInlineAllocator<24>>& Out)
	{
		const double K = FMath::Clamp(Scale, 0.5f, 1.5f);
		FVector Pelvis, Spine, Neck, Head;
		if (!DrawnHitBone(Mesh, TEXT("pelvis"), Pelvis) || !DrawnHitBone(Mesh, TEXT("spine_02"), Spine) ||
			!DrawnHitBone(Mesh, TEXT("neck_01"), Neck) || !DrawnHitBone(Mesh, TEXT("head"), Head))
		{
			return;
		}
		const FVector Up = (Head - Neck).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		const FVector TorsoUp = (Neck - Spine).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		// Head: from just over the skull's base to the crown (the bone sits where the neck meets the skull).
		Out.Add({Head + Up * (4.0 * K), Head + Up * (13.0 * K), 9.5 * K, "head"});
		Out.Add({Neck, Head, 5.5 * K, "neck"});
		// Torso: hips to the base of the neck, its round top kept under the chin.
		Out.Add({Pelvis, Spine, 14.0 * K, "torso"});
		Out.Add({Spine, Neck - TorsoUp * (12.0 * K), 14.5 * K, "torso"});
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const bool bLeft = Side == 0;
			auto Name = [bLeft](const TCHAR* Base) { return FString::Printf(TEXT("%s_%s"), Base, bLeft ? TEXT("l") : TEXT("r")); };
			FVector Clavicle, Upper, Lower, Hand, Thigh, Calf, Foot, Ball;
			const bool bArm = DrawnHitBone(Mesh, *Name(TEXT("upperarm")), Upper) && DrawnHitBone(Mesh, *Name(TEXT("lowerarm")), Lower) &&
				DrawnHitBone(Mesh, *Name(TEXT("hand")), Hand);
			if (bArm)
			{
				if (DrawnHitBone(Mesh, *Name(TEXT("clavicle")), Clavicle))
				{
					Out.Add({Clavicle, Upper, 6.5 * K, bLeft ? "left shoulder" : "right shoulder"});
				}
				Out.Add({Upper, Lower, 5.5 * K, bLeft ? "left arm" : "right arm"});
				Out.Add({Lower, Hand, 4.5 * K, bLeft ? "left arm" : "right arm"});
				const FVector Along = (Hand - Lower).GetSafeNormal();
				Out.Add({Hand, Hand + Along * (9.0 * K), 4.5 * K, bLeft ? "left hand" : "right hand"});
			}
			if (DrawnHitBone(Mesh, *Name(TEXT("thigh")), Thigh) && DrawnHitBone(Mesh, *Name(TEXT("calf")), Calf) &&
				DrawnHitBone(Mesh, *Name(TEXT("foot")), Foot))
			{
				Out.Add({Thigh, Calf, 8.0 * K, bLeft ? "left leg" : "right leg"});
				Out.Add({Calf, Foot, 6.0 * K, bLeft ? "left leg" : "right leg"});
				if (DrawnHitBone(Mesh, *Name(TEXT("ball")), Ball))
				{
					Out.Add({Foot, Ball, 5.0 * K, bLeft ? "left foot" : "right foot"});
				}
				else
				{
					Out.Add({Foot, Foot, 6.0 * K, bLeft ? "left foot" : "right foot"});
				}
			}
		}
	}
}

bool DrawnHit::HasSkeleton(const USkeletalMeshComponent& Mesh)
{
	return Mesh.GetBoneIndex(TEXT("pelvis")) != INDEX_NONE && Mesh.GetBoneIndex(TEXT("spine_02")) != INDEX_NONE &&
		Mesh.GetBoneIndex(TEXT("neck_01")) != INDEX_NONE && Mesh.GetBoneIndex(TEXT("head")) != INDEX_NONE;
}

bool DrawnHit::Trace(const USkeletalMeshComponent& Mesh, float Scale, const FVector& Start, const FVector& Direction, double MaxCm, FResult& Out)
{
	TArray<FDrawnHitCapsule, TInlineAllocator<24>> Capsules;
	DrawnHitCapsules(Mesh, Scale, Capsules);
	double Best = -1.0;
	const char* BestPart = nullptr;
	for (const FDrawnHitCapsule& C : Capsules)
	{
		const double T = DrawnHitRayCapsule(Start, Direction, C.A, C.B, C.R, MaxCm);
		if (T >= 0.0 && (Best < 0.0 || T < Best))
		{
			Best = T;
			BestPart = C.Part;
		}
	}
	if (Best < 0.0)
	{
		return false;
	}
	Out.DistanceCm = Best;
	Out.Point = Start + Direction * Best;
	Out.Part = BestPart;
	return true;
}
