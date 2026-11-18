#include "HoldDraw.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Misc/Paths.h"

#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"

#include "ColonySim/Defs.h"

namespace
{
	bool HdHas(const USkeletalMeshComponent* Mesh, const TCHAR* Bone)
	{
		return Mesh && Mesh->GetBoneIndex(FName(Bone)) != INDEX_NONE;
	}

	FVector HdBone(const USkeletalMeshComponent* Mesh, const TCHAR* Bone)
	{
		return Mesh->GetBoneLocation(FName(Bone), EBoneSpaces::WorldSpace);
	}

	FVector HdPoint(const colony::HoldPoint& P)
	{
		return FVector(P.x, P.y, P.z);
	}

	struct FHdClipEntry
	{
		TWeakObjectPtr<UAnimSequence> Clip;
		double MissingSince = -1.0;
	};
	TMap<FString, FHdClipEntry>& HdClipCache()
	{
		static TMap<FString, FHdClipEntry> Cache;
		return Cache;
	}
}

USkeletalMeshComponent* HoldDraw::VisibleMesh(const ASimPawnActor& Pawn)
{
	return Pawn.GetMakeHumanMesh() ? Pawn.GetMakeHumanMesh() : Pawn.GetBody();
}

bool HoldDraw::HasHand(const USkeletalMeshComponent* Mesh, bool bRight)
{
	return bRight ? (HdHas(Mesh, TEXT("hand_r")) && HdHas(Mesh, TEXT("index_01_r")) && HdHas(Mesh, TEXT("pinky_01_r")) && HdHas(Mesh, TEXT("middle_01_r")))
				  : (HdHas(Mesh, TEXT("hand_l")) && HdHas(Mesh, TEXT("index_01_l")) && HdHas(Mesh, TEXT("pinky_01_l")) && HdHas(Mesh, TEXT("middle_01_l")));
}

bool HoldDraw::BladeFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out)
{
	if (!HasHand(Mesh, bRight))
	{
		return false;
	}
	const FVector H = HdBone(Mesh, bRight ? TEXT("hand_r") : TEXT("hand_l"));
	const FVector I = HdBone(Mesh, bRight ? TEXT("index_01_r") : TEXT("index_01_l"));
	const FVector K = HdBone(Mesh, bRight ? TEXT("pinky_01_r") : TEXT("pinky_01_l"));
	const FVector M = HdBone(Mesh, bRight ? TEXT("middle_01_r") : TEXT("middle_01_l"));
	const TCHAR* IndexTip = bRight ? TEXT("index_03_r") : TEXT("index_03_l");
	FVector X = (I - K).GetSafeNormal();
	FVector Fingers = (M - H).GetSafeNormal();
	Fingers = (Fingers - X * FVector::DotProduct(Fingers, X)).GetSafeNormal();
	if (X.IsNearlyZero() || Fingers.IsNearlyZero())
	{
		return false;
	}
	FVector Palm = FVector::CrossProduct(Fingers, X).GetSafeNormal();
	// Which side is the palm: the curled index finger's tip lies there; else by the hand's side.
	float Side = bRight ? 1.0f : -1.0f;
	if (HdHas(Mesh, IndexTip))
	{
		const float Curl = FVector::DotProduct(HdBone(Mesh, IndexTip) - I, Palm);
		if (FMath::Abs(Curl) > 0.5f)
		{
			Side = FMath::Sign(Curl);
		}
	}
	Palm *= Side;
	const FVector Grip = (I + K) * 0.5f + Palm * 2.5f + Fingers * 0.5f;
	// A held blade leans a little the way the fingers point (about 15 degrees), and never runs back along the forearm.
	X = (X + Fingers * 0.27f).GetSafeNormal();
	const TCHAR* Elbow = bRight ? TEXT("lowerarm_r") : TEXT("lowerarm_l");
	if (HdHas(Mesh, Elbow))
	{
		const FVector Forearm = (H - HdBone(Mesh, Elbow)).GetSafeNormal();
		const double Back = FVector::DotProduct(X, Forearm);
		if (Back < -0.2)
		{
			X = (X - Forearm * (Back + 0.2)).GetSafeNormal();
		}
	}
	// Nor through its holder's own chest: if the line runs into the torso and the other way is clear, it goes the other way.
	if (HdHas(Mesh, TEXT("pelvis")) && HdHas(Mesh, TEXT("neck_01")))
	{
		const FVector Pelvis = HdBone(Mesh, TEXT("pelvis"));
		const FVector Neck = HdBone(Mesh, TEXT("neck_01"));
		const auto IntoBody = [&](const FVector& Dir)
		{
			FVector P1, P2;
			FMath::SegmentDistToSegmentSafe(Grip + Dir * 12.0, Grip + Dir * 55.0, Pelvis, Neck, P1, P2);
			return FVector::Dist(P1, P2) < 13.0;
		};
		if (IntoBody(X) && !IntoBody(-X))
		{
			X = -X;
		}
	}
	Fingers = (Fingers - X * FVector::DotProduct(Fingers, X)).GetSafeNormal();
	Out = FTransform(FRotationMatrix::MakeFromXZ(X, Fingers).ToQuat(), Grip);
	return true;
}

bool HoldDraw::GunFrame(const USkeletalMeshComponent* Mesh, bool bRight, float RakeDegrees, FTransform& Out)
{
	if (!HasHand(Mesh, bRight))
	{
		return false;
	}
	const FVector Hand = HdBone(Mesh, bRight ? TEXT("hand_r") : TEXT("hand_l"));
	const FVector Index = HdBone(Mesh, bRight ? TEXT("index_01_r") : TEXT("index_01_l"));
	const FVector Pinky = HdBone(Mesh, bRight ? TEXT("pinky_01_r") : TEXT("pinky_01_l"));
	const FVector Middle = HdBone(Mesh, bRight ? TEXT("middle_01_r") : TEXT("middle_01_l"));
	const FVector GripAxis = (Index - Pinky).GetSafeNormal();
	const FVector Along = (Middle - Hand).GetSafeNormal();
	const FVector Forward = (Along - FVector::DotProduct(Along, GripAxis) * GripAxis).GetSafeNormal();
	if (GripAxis.IsNearlyZero() || Forward.IsNearlyZero())
	{
		return false;
	}
	const float Rake = FMath::DegreesToRadians(RakeDegrees);
	const FVector GunX = FMath::Sin(Rake) * GripAxis + FMath::Cos(Rake) * Forward;
	const FVector GunZ = FMath::Cos(Rake) * GripAxis - FMath::Sin(Rake) * Forward;
	const FVector Palm = (Index + Pinky) * 0.5f + (Hand - (Index + Pinky) * 0.5f) * 0.35f;
	Out = FTransform(FRotationMatrix::MakeFromXZ(GunX, GunZ).ToQuat(), Palm);
	return true;
}

bool HoldDraw::PoleFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out)
{
	FTransform Main, Other;
	if (!BladeFrame(Mesh, bRight, Main))
	{
		return false;
	}
	if (!BladeFrame(Mesh, !bRight, Other))
	{
		Out = Main;
		return true;
	}
	const FVector Grip = Main.GetLocation();
	FVector X = Other.GetLocation() - Grip;
	if (X.Size() < 8.0)  // hands together on the shaft: no line to follow
	{
		Out = Main;
		return true;
	}
	X.Normalize();
	// Up across the shaft: the world's up, or the holding hand's fingers when the shaft stands upright.
	FVector Z = FVector::UpVector - X * FVector::DotProduct(FVector::UpVector, X);
	if (Z.SizeSquared() < 0.05)
	{
		Z = Main.GetUnitAxis(EAxis::Z) - X * FVector::DotProduct(Main.GetUnitAxis(EAxis::Z), X);
	}
	Z.Normalize();
	Out = FTransform(FRotationMatrix::MakeFromXZ(X, Z).ToQuat(), Grip);
	return true;
}

bool HoldDraw::ShieldFrame(const USkeletalMeshComponent* Mesh, FTransform& Out)
{
	if (!HdHas(Mesh, TEXT("lowerarm_l")) || !HdHas(Mesh, TEXT("hand_l")))
	{
		return false;
	}
	const FVector A = HdBone(Mesh, TEXT("lowerarm_l"));
	const FVector B = HdBone(Mesh, TEXT("hand_l"));
	const FVector Along = (B - A).GetSafeNormal();
	// The back of the hand (away from the palm) is the way the shield faces.
	FVector Face = FVector::UpVector;
	FTransform Hand;
	if (BladeFrame(Mesh, false, Hand))
	{
		const FVector Fingers = Hand.GetUnitAxis(EAxis::Z);
		const FVector Across = Hand.GetUnitAxis(EAxis::X);
		FVector Palm = FVector::CrossProduct(Fingers, Across).GetSafeNormal();
		if (FVector::DotProduct(Hand.GetLocation() - B, Palm) < 0.0f)
		{
			Palm = -Palm;
		}
		Face = -Palm;
	}
	Face = (Face - Along * FVector::DotProduct(Face, Along)).GetSafeNormal();
	if (Face.IsNearlyZero() || Along.IsNearlyZero())
	{
		return false;
	}
	// The shield's up: across the forearm, the way that is more up in the world (a tower shield stands upright).
	FVector Up = FVector::CrossProduct(Face, Along).GetSafeNormal();
	if (FVector::DotProduct(Up, FVector::UpVector) < 0.0)
	{
		Up = -Up;
	}
	const FVector Strap = FMath::Lerp(A, B, 0.55f) + Face * 5.5f;
	Out = FTransform(FRotationMatrix::MakeFromXZ(Face, Up).ToQuat(), Strap);
	return true;
}

bool HoldDraw::HandFrame(const USkeletalMeshComponent* Mesh, const colony::WeaponHold& Hold, FTransform& Out)
{
	const bool bRight = Hold.hand == colony::HoldHand::Right;
	switch (Hold.grip)
	{
		case colony::HoldGrip::Gun: return GunFrame(Mesh, bRight, Hold.rake, Out);
		case colony::HoldGrip::Shield: return ShieldFrame(Mesh, Out);
		case colony::HoldGrip::Pole: return PoleFrame(Mesh, bRight, Out);
		case colony::HoldGrip::None: return false;
		case colony::HoldGrip::Bow:
		case colony::HoldGrip::Blade:
		default: return BladeFrame(Mesh, bRight, Out);
	}
}

FName HoldDraw::HandBone(const colony::WeaponHold& Hold)
{
	if (Hold.grip == colony::HoldGrip::Shield)
	{
		return FName(TEXT("lowerarm_l"));
	}
	return FName(Hold.hand == colony::HoldHand::Left ? TEXT("hand_l") : TEXT("hand_r"));
}

FTransform HoldDraw::WeaponWorld(const FTransform& Frame, const colony::WeaponHold& Hold, float Size)
{
	const FTransform Local(FQuat::Identity, -HdPoint(Hold.main) * Size, FVector(Size));
	FTransform World = Local * FTransform(Frame.GetRotation(), Frame.GetLocation());
	World.SetScale3D(FVector(Size));
	return World;
}

void HoldDraw::SetOffHand(USkeletalMeshComponent* Mesh, const FTransform& Weapon, const colony::WeaponHold& Hold, float Size, float Alpha,
						  const FVector& ElbowOut)
{
	UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Anim)
	{
		return;
	}
	const bool bWant = Alpha > 0.0f && Hold.hasOff && Hold.hand == colony::HoldHand::Right && HasHand(Mesh, false) && HdHas(Mesh, TEXT("upperarm_l")) &&
		HdHas(Mesh, TEXT("upperarm_r")) && HdHas(Mesh, TEXT("pelvis")) && HdHas(Mesh, TEXT("neck_01"));
	FTransform Left;
	const bool bGun = Hold.grip == colony::HoldGrip::Gun;
	if (!bWant || !(bGun ? GunFrame(Mesh, false, Hold.rake, Left) : BladeFrame(Mesh, false, Left)))
	{
		Anim->SetLeftHandGrip(0.0f, FVector::ZeroVector, FVector::ZeroVector);
		return;
	}
	// The weapon's off point in the world (Weapon already carries the size); the wrist goes where it puts the left
	// fist's grip point there, given in the right hand's space so it follows the right hand exactly in the next pose.
	const FVector OnWeapon = Weapon.TransformPosition(HdPoint(Hold.off));
	const FVector Wrist = OnWeapon + (HdBone(Mesh, TEXT("hand_l")) - Left.GetLocation());
	const FVector Offset = Mesh->GetSocketTransform(TEXT("hand_r"), RTS_World).InverseTransformPosition(Wrist);
	const FVector Shoulder = HdBone(Mesh, TEXT("upperarm_l"));
	FVector Out = ElbowOut;
	if (Out.IsNearlyZero())
	{
		const FVector Across = (Shoulder - HdBone(Mesh, TEXT("upperarm_r"))).GetSafeNormal();
		const FVector Up = (HdBone(Mesh, TEXT("neck_01")) - HdBone(Mesh, TEXT("pelvis"))).GetSafeNormal();
		Out = (Across - Up).GetSafeNormal();
	}
	const FVector Elbow = (Shoulder + Wrist) * 0.5 + Out.GetSafeNormal() * 30.0;
	Anim->SetLeftHandGrip(FMath::SmoothStep(0.0f, 1.0f, FMath::Clamp(Alpha, 0.0f, 1.0f)), Offset, Elbow);
	(void)Size;
}

UAnimSequence* HoldDraw::LoadClip(const FString& Rel)
{
	if (Rel.IsEmpty())
	{
		return nullptr;
	}
	FHdClipEntry& Entry = HdClipCache().FindOrAdd(Rel);
	if (Entry.Clip.IsValid())
	{
		return Entry.Clip.Get();
	}
	const double Now = FPlatformTime::Seconds();
	if (Entry.MissingSince >= 0.0 && Now - Entry.MissingSince < 30.0)
	{
		return nullptr;
	}
	const FString Path = FString::Printf(TEXT("/Game/Rim3D/Generated/Animations/%s.%s"), *Rel, *FPaths::GetBaseFilename(Rel));
	UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	Entry.Clip = Clip;
	Entry.MissingSince = Clip ? -1.0 : Now;
	if (!Clip)
	{
		UE_LOG(LogTemp, Warning, TEXT("Hold: no clip %s"), *Path);
	}
	return Clip;
}

UAnimSequence* HoldDraw::StyleClip(const colony::DefDatabase& Defs, const colony::WeaponHold& Hold, const char* Key, const char* Fallback)
{
	if (!Hold.style.valid() || Hold.style.value >= Defs.size())
	{
		return nullptr;
	}
	const colony::Def& Style = Defs.get(Hold.style);
	std::string Rel = Style.text(Key);
	if (Rel.empty() && Fallback != nullptr)
	{
		Rel = Style.text(Fallback);
	}
	return Rel.empty() ? nullptr : LoadClip(UTF8_TO_TCHAR(Rel.c_str()));
}
