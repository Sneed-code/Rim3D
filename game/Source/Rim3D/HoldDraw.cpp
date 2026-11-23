#include "HoldDraw.h"

#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
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
	const FVector Grip = Main.GetLocation();
	// The shaft always points where the body faces (2026-11-19: with one hand on it, or both hands close together, it
	// used to follow the single fist and stood upright along the leg). The hands' line, when there are two hands apart
	// on the shaft, only tilts it: at most 10 degrees up (the rifle idle's raised front hand held it like a pike, 35 degrees
	// up, in the 2026-11-19 side picture: a low guard points at the chest) and 20 down.
	double Pitch = 0.0;
	if (BladeFrame(Mesh, !bRight, Other))
	{
		const FVector Line = Other.GetLocation() - Grip;
		if (Line.Size() >= 8.0)
		{
			Pitch = FMath::Clamp(FMath::Asin(FMath::Clamp(Line.GetSafeNormal().Z, -1.0, 1.0)), FMath::DegreesToRadians(-20.0), FMath::DegreesToRadians(10.0));
		}
	}
	FVector Ahead = FVector::ZeroVector;
	if (const AActor* Holder = Mesh->GetOwner())
	{
		Ahead = Holder->GetActorForwardVector().GetSafeNormal2D();
	}
	if (Ahead.IsNearlyZero())
	{
		Out = Main;
		return true;
	}
	const FVector X = (Ahead * FMath::Cos(Pitch) + FVector::UpVector * FMath::Sin(Pitch)).GetSafeNormal();
	// Up across the shaft is the world's up (the shaft is never steep).
	const FVector Z = (FVector::UpVector - X * FVector::DotProduct(FVector::UpVector, X)).GetSafeNormal();
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
	UAnimSequence* Clip = Rel.empty() ? nullptr : LoadClip(UTF8_TO_TCHAR(Rel.c_str()));
	// The fallback when the key is empty, or names a clip not imported yet (a new download still to come in).
	if (!Clip && Fallback != nullptr)
	{
		Rel = Style.text(Fallback);
		Clip = Rel.empty() ? nullptr : LoadClip(UTF8_TO_TCHAR(Rel.c_str()));
	}
	return Clip;
}

// ---- Weapon first, then the hands -------------------------------------------------------------------------------------

HoldDraw::FPoints HoldDraw::PointsOf(const colony::WeaponHold& Hold, bool bShieldOnArm)
{
	FPoints P;
	P.Main = HdPoint(Hold.main);
	if (Hold.hasOff)
	{
		P.Off = HdPoint(Hold.off);
		P.bOff = true;
	}
	else if (Hold.hasSupport && !bShieldOnArm && Hold.hand == colony::HoldHand::Right)
	{
		// Given from the main grip (a pistol's grip point may sit anywhere on its model): the support hand cups the grip hand.
		P.Off = P.Main + HdPoint(Hold.support);
		P.bOff = true;
	}
	// No model: the sight a little above the grip's line, the butt a forearm behind it.
	P.Sight = P.Main + FVector(Hold.hands >= 2 ? -4.0 : 4.0, 0.0, 8.0);
	P.Stock = P.Main + FVector(-24.0, 0.0, 3.0);
	return P;
}

FVector HoldDraw::StockFromBounds(const FBox& BoxInGunFrame, const FVector& Sight)
{
	if (!BoxInGunFrame.IsValid)
	{
		return FVector(-25.0, 0.0, Sight.Z - 6.0);
	}
	const double Z = FMath::Clamp(Sight.Z - 6.0, BoxInGunFrame.Min.Z + 2.0, BoxInGunFrame.Max.Z - 2.0);
	return FVector(BoxInGunFrame.Min.X + 1.5, 0.0, Z);
}

FTransform HoldDraw::PlaceInAnchor(const colony::HoldPlace& Place, const FPoints& Points, float Size, float BodyPlace)
{
	const FQuat Turn = FRotator(Place.pitch, Place.yaw, Place.roll).Quaternion();
	FVector Point = Points.Main;
	if (Place.point == colony::HoldPlacePoint::Sight)
	{
		Point = Points.Sight;
	}
	else if (Place.point == colony::HoldPlacePoint::Stock)
	{
		Point = Points.Stock;
	}
	const FVector At = HdPoint(Place.at) * BodyPlace;
	return FTransform(Turn, At - Turn.RotateVector(Point * Size));
}

bool HoldDraw::FistFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out)
{
	if (!HasHand(Mesh, bRight))
	{
		return false;
	}
	const FVector H = HdBone(Mesh, bRight ? TEXT("hand_r") : TEXT("hand_l"));
	const FVector I = HdBone(Mesh, bRight ? TEXT("index_01_r") : TEXT("index_01_l"));
	const FVector K = HdBone(Mesh, bRight ? TEXT("pinky_01_r") : TEXT("pinky_01_l"));
	const FVector M = HdBone(Mesh, bRight ? TEXT("middle_01_r") : TEXT("middle_01_l"));
	FVector X = (I - K).GetSafeNormal();
	FVector Fingers = (M - H).GetSafeNormal();
	Fingers = (Fingers - X * FVector::DotProduct(Fingers, X)).GetSafeNormal();
	if (X.IsNearlyZero() || Fingers.IsNearlyZero())
	{
		return false;
	}
	FVector Palm = FVector::CrossProduct(Fingers, X).GetSafeNormal();
	// The palm's side from the knuckles alone (fingers x across, index side to pinky side): the same for every pose.
	// It used to follow the index finger's curl, which an open or overextended hand turned inside out (PC 2026-11-19:
	// the pistol's hand read as turned round, so the wrist limits spun it, fingers up, and the pistol hid behind it).
	const float Side = bRight ? 1.0f : -1.0f;
	Palm *= Side;
	const FVector Grip = (I + K) * 0.5f + Palm * 2.5f + Fingers * 0.5f;
	// A hilt sits across the fist leaning a little the way the fingers point (about 15 degrees).
	X = (X + Fingers * 0.27f).GetSafeNormal();
	Fingers = (Fingers - X * FVector::DotProduct(Fingers, X)).GetSafeNormal();
	Out = FTransform(FRotationMatrix::MakeFromXZ(X, Fingers).ToQuat(), Grip);
	return true;
}

bool HoldDraw::HandInGrip(const USkeletalMeshComponent* Mesh, bool bRight, bool bPistolGrip, float RakeDegrees, FTransform& Out)
{
	FTransform Grip;
	if (!(bPistolGrip ? GunFrame(Mesh, bRight, RakeDegrees, Grip) : FistFrame(Mesh, bRight, Grip)))
	{
		return false;
	}
	FTransform Hand = Mesh->GetSocketTransform(bRight ? TEXT("hand_r") : TEXT("hand_l"), RTS_World);
	Hand.SetScale3D(FVector::OneVector);
	Grip.SetScale3D(FVector::OneVector);
	Out = Hand.GetRelativeTransform(Grip);
	return true;
}

bool HoldDraw::PinHands(const USkeletalMeshComponent* Mesh, const colony::WeaponHold& Hold, const FPoints& Points, float Size, const FTransform& Weapon,
						FPinned& Out)
{
	const bool bRight = Hold.hand != colony::HoldHand::Left;
	FTransform MainInGrip;
	if (!HandInGrip(Mesh, bRight, Hold.grip == colony::HoldGrip::Gun, Hold.rake, MainInGrip))
	{
		return false;
	}
	FTransform W = Weapon;
	W.SetScale3D(FVector::OneVector);
	const FTransform MainGrip = FTransform(FQuat::Identity, Points.Main * Size) * W;
	Out.MainHand = MainInGrip * MainGrip;
	FTransform WeaponScaled = W;
	WeaponScaled.SetScale3D(FVector(Size));
	Out.WeaponInHand = WeaponScaled.GetRelativeTransform(Out.MainHand);
	Out.bOff = false;
	FTransform OffInGrip;
	if (Points.bOff && HandInGrip(Mesh, !bRight, false, 0.0f, OffInGrip))
	{
		const FQuat Turn = FRotator(Hold.offPitch, Hold.offYaw, Hold.offRoll).Quaternion();
		const FTransform OffGrip = FTransform(Turn, Points.Off * Size) * W;
		Out.OffInMain = (OffInGrip * OffGrip).GetRelativeTransform(Out.MainHand);
		Out.bOff = true;
		// The weapon's long axis through the off grip, in the off hand's own space: the hand may turn round it.
		const FTransform GripInHand = OffInGrip.Inverse();
		Out.OffRollPoint = GripInHand.GetLocation();
		Out.OffRollAxis = GripInHand.TransformVectorNoScale(Turn.UnrotateVector(FVector::ForwardVector)).GetSafeNormal();
		// A support hand cupping a pistol's grip hand has no round shaft to turn about.
		Out.OffRollFree = !Hold.hasOff ? 0.0f : (Hold.grip == colony::HoldGrip::Pole ? 180.0f : (Hold.grip == colony::HoldGrip::Gun ? 90.0f : 45.0f));
	}
	return true;
}

void HoldDraw::ApplyPinned(USkeletalMeshComponent* Mesh, const colony::WeaponHold& Hold, const FPinned& Pinned, uint8 Space, FName Anchor, float MainAlpha,
						   float OffAlpha)
{
	UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
	if (!Anim)
	{
		return;
	}
	// The goals in the mesh's component space (a body's height scales its mesh: component units are world cm over it).
	const FTransform Comp = Mesh->GetComponentTransform();
	const double Scale = FMath::Max(static_cast<double>(Comp.GetScale3D().GetAbsMax()), 0.01);
	FTransform MainCS = Pinned.MainHand.GetRelativeTransform(Comp);
	MainCS.SetScale3D(FVector::OneVector);
	if (static_cast<EMakeHumanGoalSpace>(Space) == EMakeHumanGoalSpace::Anchor && HdHas(Mesh, *Anchor.ToString()))
	{
		MainCS.AddToTranslation(-Comp.InverseTransformPosition(Mesh->GetSocketLocation(Anchor)));
	}
	FTransform OffCS = Pinned.OffInMain;
	OffCS.SetTranslation(OffCS.GetTranslation() / Scale);
	OffCS.SetScale3D(FVector::OneVector);
	const bool bRight = Hold.hand != colony::HoldHand::Left;
	FMakeHumanHandGoal Main;
	Main.Alpha = FMath::Clamp(MainAlpha, 0.0f, 1.0f);
	Main.Space = static_cast<EMakeHumanGoalSpace>(Space);
	Main.Anchor = Anchor;
	Main.Target = MainCS;
	Main.bRotation = true;
	Main.bCurl = true;
	Anim->SetHandGoal(bRight, Main);
	FMakeHumanHandGoal Off;
	Off.Alpha = Pinned.bOff ? FMath::Clamp(OffAlpha, 0.0f, 1.0f) : 0.0f;
	Off.bRotation = true;
	Off.bCurl = true;
	// Wrapped round the fore-end or shaft (a front hand left open under it read as resting on an open palm, PC 2026-11-20).
	Off.CurlWrap = 1.3f;
	if (Main.Alpha >= 0.5f)
	{
		// The weapon is at its place: the other hand goes to its own grip there, not after the main hand (whose turn
		// the wrist limits may have held back), so it stays on the fore-end.
		FTransform OffWorldCS = (Pinned.OffInMain * Pinned.MainHand).GetRelativeTransform(Comp);
		OffWorldCS.SetScale3D(FVector::OneVector);
		if (Main.Space == EMakeHumanGoalSpace::Anchor && HdHas(Mesh, *Anchor.ToString()))
		{
			OffWorldCS.AddToTranslation(-Comp.InverseTransformPosition(Mesh->GetSocketLocation(Anchor)));
		}
		Off.Space = Main.Space;
		Off.Anchor = Anchor;
		Off.Target = OffWorldCS;
		Off.RollFree = Pinned.OffRollFree;
		Off.RollAxis = Pinned.OffRollAxis;
		Off.RollPoint = Pinned.OffRollPoint / Scale;
	}
	else
	{
		// The main hand follows a clip (a swing): the other hand stays on the weapon in it.
		Off.Space = EMakeHumanGoalSpace::OtherHand;
		Off.Target = OffCS;
	}
	Anim->SetHandGoal(!bRight, Off);
}

void HoldDraw::ReleaseHands(USkeletalMeshComponent* Mesh)
{
	if (UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr)
	{
		Anim->SetHandGoal(true, FMakeHumanHandGoal());
		Anim->SetHandGoal(false, FMakeHumanHandGoal());
	}
}

bool HoldDraw::MeasureDrawnArm(const USkeletalMeshComponent* Mesh, bool bRight, const FVector& Up, const FVector& Forward, const FVector& Right,
							   colony::arm::ArmAngles& Out)
{
	if (!HasHand(Mesh, bRight) || !HdHas(Mesh, bRight ? TEXT("upperarm_r") : TEXT("upperarm_l")) || !HdHas(Mesh, bRight ? TEXT("lowerarm_r") : TEXT("lowerarm_l")))
	{
		return false;
	}
	const auto V = [](const FVector& F) { return colony::arm::V3{F.X, F.Y, F.Z}; };
	const FVector S = HdBone(Mesh, bRight ? TEXT("upperarm_r") : TEXT("upperarm_l"));
	const FVector E = HdBone(Mesh, bRight ? TEXT("lowerarm_r") : TEXT("lowerarm_l"));
	const FVector W = HdBone(Mesh, bRight ? TEXT("hand_r") : TEXT("hand_l"));
	const FVector M = HdBone(Mesh, bRight ? TEXT("middle_01_r") : TEXT("middle_01_l"));
	FTransform Fist;
	if (!FistFrame(Mesh, bRight, Fist))
	{
		return false;
	}
	// The palm's normal: from the fist's frame (x across the knuckles, z the fingers' way), out of the palm towards
	// the grip point it sets inside the curled fingers.
	const FVector Fingers = (M - W).GetSafeNormal();
	FVector Palm = FVector::CrossProduct(Fist.GetUnitAxis(EAxis::Z), Fist.GetUnitAxis(EAxis::X)).GetSafeNormal();
	const FVector Knuckles = (HdBone(Mesh, bRight ? TEXT("index_01_r") : TEXT("index_01_l")) + HdBone(Mesh, bRight ? TEXT("pinky_01_r") : TEXT("pinky_01_l"))) * 0.5;
	if (FVector::DotProduct(Fist.GetLocation() - Knuckles, Palm) < 0.0)
	{
		Palm = -Palm;
	}
	Palm = (Palm - Fingers * FVector::DotProduct(Palm, Fingers)).GetSafeNormal();
	const colony::arm::V3 Hinge = colony::arm::medialHinge(V(E - S), V(W - E), V(Up), V(Forward), V(Right), bRight);
	Out = colony::arm::measureArm(V(S), V(E), V(W), V(Fingers), V(Palm), Hinge);
	return true;
}
