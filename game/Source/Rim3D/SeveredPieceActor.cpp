#include "SeveredPieceActor.h"

#include "AnimationRuntime.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ProceduralMeshComponent.h"
#include "GroomAsset.h"
#include "GroomComponent.h"

#include "BloodSubsystem.h"
#include "GoreCut.h"
#include "GoreGibActor.h"
#include "GoreOrgans.h"
#include "GoreStump.h"
#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "SimSubsystem.h"
#include "StrandHair.h"

#include "ColonySim/Filth.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

namespace
{
	constexpr float SeveredGravity = 980.0f;
	constexpr float SeveredBounce = 0.28f;
	constexpr float SeveredGrip = 0.55f;      // the share of its sideways speed a piece keeps at each bounce
	constexpr float SeveredRestSpeed = 35.0f;
	constexpr float SeveredFold = 0.001f;     // bones outside the piece shrink to this, at the cut

	TAutoConsoleVariable<float> GSeveredDripSeconds(TEXT("Gore.DripSeconds"), 120.0f,
		TEXT("How long blood keeps dripping from a fresh stump, seconds at 1x (a severed piece drips for 40% of it)."), ECVF_Default);
	TAutoConsoleVariable<float> GSeveredClothRim(TEXT("Gore.ClothRim"), 1.12f,
		TEXT("On a clothed limb, how wide the stump is against the bare limb (the sleeve or trouser leg is thicker), its rim the torn, soaked cloth (limb radii)."), ECVF_Default);
	TAutoConsoleVariable<float> GSeveredStumpScale(TEXT("Gore.StumpScale"), 1.08f,
		TEXT("Stump caps' size against the limb's usual thickness at the cut (bigger hides more of the flattened skin's edge)."), ECVF_Default);

	/** The bone a piece hangs from, and a bone further down it (None: none) to find its middle. */
	bool SeveredBones(uint16 Bit, FName& OutBone, FName& OutEnd)
	{
		switch (Bit)
		{
		case colony::kSeveredLeftForearm: OutBone = TEXT("lowerarm_l"); OutEnd = TEXT("hand_l"); return true;
		case colony::kSeveredLeftArm: OutBone = TEXT("upperarm_l"); OutEnd = TEXT("lowerarm_l"); return true;
		case colony::kSeveredRightForearm: OutBone = TEXT("lowerarm_r"); OutEnd = TEXT("hand_r"); return true;
		case colony::kSeveredRightArm: OutBone = TEXT("upperarm_r"); OutEnd = TEXT("lowerarm_r"); return true;
		case colony::kSeveredLeftLowerLeg: OutBone = TEXT("calf_l"); OutEnd = TEXT("foot_l"); return true;
		case colony::kSeveredLeftLeg: OutBone = TEXT("thigh_l"); OutEnd = TEXT("calf_l"); return true;
		case colony::kSeveredRightLowerLeg: OutBone = TEXT("calf_r"); OutEnd = TEXT("foot_r"); return true;
		case colony::kSeveredRightLeg: OutBone = TEXT("thigh_r"); OutEnd = TEXT("calf_r"); return true;
		// The head comes off with its neck (sneed, 2026-11-23: "the neck looks weird; make it so the neck is destroyed when
		// the head comes off"): cut at the neck's root on the shoulders, not under the skull.
		case colony::kSeveredHead: OutBone = TEXT("neck_01"); OutEnd = TEXT("head"); return true;
		default: return false;
		}
	}

	/** How thick the limb is at the cut, cm (a grown body; scaled by the pawn's height). */
	float SeveredCapRadius(uint16 Bit)
	{
		switch (Bit)
		{
		case colony::kSeveredLeftForearm:
		case colony::kSeveredRightForearm: return 3.6f;
		case colony::kSeveredLeftArm:
		case colony::kSeveredRightArm: return 4.8f;
		case colony::kSeveredLeftLowerLeg:
		case colony::kSeveredRightLowerLeg: return 5.4f;
		case colony::kSeveredLeftLeg:
		case colony::kSeveredRightLeg: return 7.8f;
		default: return 6.4f;  // the root of the neck
		}
	}

	/** What the cut goes through. */
	EGoreStumpKind SeveredStumpKind(uint16 Bit)
	{
		switch (Bit)
		{
		case colony::kSeveredLeftForearm:
		case colony::kSeveredRightForearm:
		case colony::kSeveredLeftLowerLeg:
		case colony::kSeveredRightLowerLeg: return EGoreStumpKind::TwoBones;
		case colony::kSeveredHead: return EGoreStumpKind::Neck;
		default: return EGoreStumpKind::OneBone;
		}
	}

	/**
	 * Where a cut lies on a mesh as drawn now (GoreCut.h): the joint, the way the lost part went (the flat face's
	 * normal: the cut bone straightened onto its parent, as the sever node draws it) and, for the neck, the way the face
	 * looked. World space. False if the mesh lacks the bone or its parent.
	 */
	bool SeveredCutFrame(const USkinnedMeshComponent& Skin, FName Bone, FVector& OutAt, FVector& OutAlong, FVector& OutFront)
	{
		const USkinnedAsset* Asset = Skin.GetSkinnedAsset();
		const int32 Index = Skin.GetBoneIndex(Bone);
		const FName ParentName = Index != INDEX_NONE ? Skin.GetParentBone(Bone) : NAME_None;
		const int32 Parent = ParentName.IsNone() ? INDEX_NONE : Skin.GetBoneIndex(ParentName);
		if (!Asset || Parent == INDEX_NONE)
		{
			return false;
		}
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		const int32 RefIndex = Ref.FindBoneIndex(Bone);
		if (RefIndex == INDEX_NONE)
		{
			return false;
		}
		const FTransform Straight = Ref.GetRefBonePose()[RefIndex] * Skin.GetBoneTransform(Parent);
		OutAt = Straight.GetLocation();
		OutAlong = Straight.GetRotation().RotateVector(GoreCut::FaceNormalLocal(GoreCut::AxisLocal(Ref, RefIndex))).GetSafeNormal();
		OutFront = Straight.GetRotation().RotateVector(GoreCut::FrontLocal(Ref, RefIndex));
		return !OutAlong.IsNearlyZero();
	}

	/**
	 * The same for the piece's side: where its cut bone is now (straightened onto the parent if the body has flattened it
	 * already) and the normal of the flat face its flattened parent leaves (GoreCut::FlattenParent), pointing down the
	 * piece.
	 */
	bool SeveredPieceCutFrame(const USkinnedMeshComponent& Skin, FName Bone, FVector& OutAt, FVector& OutAlong, FVector& OutFront)
	{
		const USkinnedAsset* Asset = Skin.GetSkinnedAsset();
		const int32 Index = Skin.GetBoneIndex(Bone);
		const FName ParentName = Index != INDEX_NONE ? Skin.GetParentBone(Bone) : NAME_None;
		const int32 Parent = ParentName.IsNone() ? INDEX_NONE : Skin.GetBoneIndex(ParentName);
		if (!Asset || Parent == INDEX_NONE)
		{
			return false;
		}
		const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
		const int32 RefIndex = Ref.FindBoneIndex(Bone);
		if (RefIndex == INDEX_NONE)
		{
			return false;
		}
		const FTransform& RefLocal = Ref.GetRefBonePose()[RefIndex];
		FTransform CutWorld = Skin.GetBoneTransform(Index);
		if (CutWorld.GetScale3D().GetAbsMin() < 0.1)
		{
			CutWorld = RefLocal * Skin.GetBoneTransform(Parent);
		}
		const FVector Axis = GoreCut::AxisLocal(Ref, RefIndex);
		const FQuat ParentRot = CutWorld.GetRotation() * RefLocal.GetRotation().Inverse();
		OutAt = CutWorld.GetLocation();
		OutAlong = ParentRot.RotateVector(GoreCut::FaceNormalLocal(RefLocal.GetRotation().RotateVector(Axis))).GetSafeNormal();
		OutFront = CutWorld.GetRotation().RotateVector(GoreCut::FrontLocal(Ref, RefIndex));
		return !OutAlong.IsNearlyZero();
	}

	/** The pawn's skin colour for the torn rim of a cut. */
	FLinearColor SeveredSkinColour(const ASimPawnActor& Pawn)
	{
		return Pawn.HasLooks() ? Pawn.GetLooks().Skin : FLinearColor(0.45f, 0.26f, 0.2f);
	}

	/** Blood that keeps dripping from an open cut for a while (a hit of its own that bleeds without the sim). */
	void SeveredDrip(UWorld* World, const FVector& At, const FVector& Outward, USkeletalMeshComponent* Mesh, float Seconds)
	{
		UBloodSubsystem* Blood = World ? World->GetSubsystem<UBloodSubsystem>() : nullptr;
		if (!Blood || !Blood->IsReady())
		{
			return;
		}
		FBloodHit Hit;
		Hit.At = At;
		Hit.Direction = Outward + FVector(0.0, 0.0, -0.6);
		Hit.Kind = EBloodWound::Cut;
		Hit.Damage = 3.0f;
		Hit.Force = 0.15f;
		Hit.Mesh = Mesh;
		Hit.BleedSecondsWithoutSim = Seconds;
		Blood->AddHit(Hit);
	}

	/** Every skinned mesh of the pawn: the mannequin, the MetaHuman or MakeHuman body, its clothes, its face. */
	void SeveredSkinned(const ASimPawnActor& Pawn, TArray<USkinnedMeshComponent*>& Out)
	{
		TInlineComponentArray<USkinnedMeshComponent*> Found;
		Pawn.GetComponents(Found);
		Out.Append(Found);
		if (const AActor* MetaHuman = Pawn.GetMetaHuman())
		{
			Found.Reset();
			MetaHuman->GetComponents(Found);
			Out.Append(Found);
		}
	}

	/** Whether a mesh is really drawn: its own flags and its actor's (a hidden second body still has "visible" set). */
	bool SeveredDrawn(const USkinnedMeshComponent& Mesh)
	{
		const AActor* Owner = Mesh.GetOwner();
		return Mesh.IsVisible() && !Mesh.bHiddenInGame && !(Owner && Owner->IsHidden());
	}

	/** How much of a mesh's skin hangs on a bone or under it: the share of the bones its skin uses, 0 to 1. */
	float SeveredSkinnedTo(const USkeletalMesh& Asset, int32 Bone)
	{
		const FSkeletalMeshRenderData* Data = Asset.GetResourceForRendering();
		if (!Data || Data->LODRenderData.Num() == 0)
		{
			return 1.0f;
		}
		const FReferenceSkeleton& Skeleton = Asset.GetRefSkeleton();
		TSet<int32> Used;
		int32 OnPiece = 0;
		for (const FSkelMeshRenderSection& Section : Data->LODRenderData[0].RenderSections)
		{
			for (const FBoneIndexType Index : Section.BoneMap)
			{
				bool bSeen = false;
				Used.Add(Index, &bSeen);
				OnPiece += !bSeen && (Index == Bone || Skeleton.BoneIsChildOf(Index, Bone)) ? 1 : 0;
			}
		}
		return Used.IsEmpty() ? 0.0f : static_cast<float>(OnPiece) / static_cast<float>(Used.Num());
	}

	/** How far a cut's torn cloth reaches (radii): Gore.ClothRim if another drawn mesh than the skin (a sleeve, a
	 *  trouser leg) covers the cut limb, else 1. Not for the neck. */
	float SeveredClothOuter(const TArray<USkinnedMeshComponent*>& Skinned, const USkinnedMeshComponent* SkinMesh, FName Bone, uint16 Bit)
	{
		if (Bit == colony::kSeveredHead)
		{
			return 1.0f;
		}
		for (const USkinnedMeshComponent* Mesh : Skinned)
		{
			const USkeletalMesh* Asset = Mesh ? Cast<USkeletalMesh>(Mesh->GetSkinnedAsset()) : nullptr;
			if (!Asset || Mesh == SkinMesh || !SeveredDrawn(*Mesh))
			{
				continue;
			}
			const int32 Index = Asset->GetRefSkeleton().FindBoneIndex(Bone);
			if (Index != INDEX_NONE && SeveredSkinnedTo(*Asset, Index) >= 0.01f)
			{
				return GSeveredClothRim.GetValueOnGameThread();
			}
		}
		return 1.0f;
	}

	/**
	 * How far (cm, in the mesh's own units) a mesh still bulges out past the flat face once cut, from its skin weights
	 * in the reference pose. Skin weighted partly to bones on both sides of the cut lands between the face and where it
	 * was: a loose sleeve with soft weights round the elbow ends in a rounded dome a few cm long that hid the stump.
	 * `bPieceSide`: on the severed piece (bulging back towards the body), else on the body (bulging out down the limb).
	 * Only skin over the face (within 1.1 `RadiusCm` of the limb's line) counts: the shoulder's sides bulged 8 cm and
	 * lifted the cap off the body. -1 if the mesh keeps no copy of its vertices to read.
	 */
	float SeveredDomeCm(const USkeletalMesh& Asset, int32 CutIndex, bool bPieceSide, float RadiusCm, float& OutOpening, bool bWhole = false)
	{
		OutOpening = -1.0f;
		FSkeletalMeshRenderData* Data = Asset.GetResourceForRendering();
		const FReferenceSkeleton& Ref = Asset.GetRefSkeleton();
		if (!Data || Data->LODRenderData.Num() == 0 || CutIndex == INDEX_NONE || CutIndex >= Ref.GetNum() || FPlatformProperties::RequiresCookedData())
		{
			return -1.0f;
		}
		FSkeletalMeshLODRenderData& Lod = Data->LODRenderData[0];
		FPositionVertexBuffer& Positions = Lod.StaticVertexBuffers.PositionVertexBuffer;
		const FSkinWeightVertexBuffer& Weights = Lod.SkinWeightVertexBuffer;
		// (No CPU copy kept: the vertex data pointer is null and there is nothing to read.)
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData() || Weights.GetNumVertices() != Positions.GetNumVertices())
		{
			return -1.0f;
		}
		TArray<bool> OnPiece;
		OnPiece.SetNumZeroed(Ref.GetNum());
		for (int32 i = CutIndex; i < Ref.GetNum(); ++i)
		{
			OnPiece[i] = i == CutIndex || Ref.BoneIsChildOf(i, CutIndex);
		}
		const FTransform Joint = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, CutIndex);
		const FVector Down = Joint.GetRotation().RotateVector(GoreCut::AxisLocal(Ref, CutIndex));
		const uint32 Influences = Weights.GetMaxBoneInfluences();
		float Dome = 0.0f;
		// How wide the mesh is where it's cut (the open end of a sleeve is wider than the limb): the largest distance
		// from the limb's line in each of 16 directions round it, of skin on the piece's bones (MakeHuman weights its
		// limbs to twist bones under the cut bone, not the cut bone itself) within a quarter radius of the joint; the median
		// of the 16 (the mean let a loose hem or the shoulder's bulge make it twice too wide).
		FVector Across1;
		FVector Across2;
		Down.FindBestAxisVectors(Across1, Across2);
		float Widest[16] = {};
		const auto OnCutBone = [&](const FSkelMeshRenderSection& Section, uint32 V)
		{
			for (uint32 K = 0; K < Influences; ++K)
			{
				const uint32 Local = Weights.GetBoneIndex(V, K);
				if (Weights.GetBoneWeight(V, K) > 0 && static_cast<int32>(Local) < Section.BoneMap.Num() && OnPiece.IsValidIndex(Section.BoneMap[Local]) && OnPiece[Section.BoneMap[Local]])
				{
					return true;
				}
			}
			return false;
		};
		for (const FSkelMeshRenderSection& Section : Lod.RenderSections)
		{
			for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices && V < Positions.GetNumVertices(); ++V)
			{
				const FVector Rel = FVector(Positions.VertexPosition(V)) - Joint.GetLocation();
				const double Along = FVector::DotProduct(Rel, Down);
				const double Radial = (Rel - Along * Down).Size();
				if (FMath::Abs(Along) < 0.25 * RadiusCm && Radial < 2.0 * RadiusCm && OnCutBone(Section, V))
				{
					const double Angle = FMath::Atan2(FVector::DotProduct(Rel, Across2), FVector::DotProduct(Rel, Across1));
					const int32 Bin = FMath::Clamp(FMath::FloorToInt32((Angle + PI) / (2.0 * PI) * 16.0), 0, 15);
					Widest[Bin] = FMath::Max(Widest[Bin], static_cast<float>(Radial));
				}
				if ((bPieceSide ? -Along : Along) <= 0.0 || Radial > 1.1 * RadiusCm)
				{
					continue;
				}
				double Total = 0.0;
				double Piece = 0.0;
				for (uint32 K = 0; K < Influences; ++K)
				{
					const double W = Weights.GetBoneWeight(V, K);
					const uint32 Local = Weights.GetBoneIndex(V, K);
					if (W <= 0.0 || static_cast<int32>(Local) >= Section.BoneMap.Num())
					{
						continue;
					}
					const int32 Bone = Section.BoneMap[Local];
					Total += W;
					Piece += Bone < OnPiece.Num() && OnPiece[Bone] ? W : 0.0;
				}
				if (Total <= 0.0)
				{
					continue;
				}
				// On the body the piece's bones squash onto the face, so what stays out is the share on the body's bones;
				// on the piece the other way round. Skin wholly on one side's bones is left out: on the body that was hip and
				// crotch skin past the hip joint, which lifted a thigh's cap 8 cm off the body.
				const double Stays = bPieceSide ? Piece / Total : 1.0 - Piece / Total;
				if (Stays < 0.999 || bWhole)
				{
					Dome = FMath::Max(Dome, static_cast<float>(Stays * FMath::Abs(Along)));
				}
			}
		}
		TArray<float, TInlineAllocator<16>> Found;
		for (const float W : Widest)
		{
			if (W > 0.0f)
			{
				Found.Add(W);
			}
		}
		Found.Sort();
		OutOpening = Found.Num() >= 8 ? Found[Found.Num() / 2] : -1.0f;
		return FMath::Min(Dome, RadiusCm);
	}

	float SeveredUnreadDome()
	{
		const IConsoleVariable* Lift = IConsoleManager::Get().FindConsoleVariable(TEXT("Gore.ClothLift"));
		return Lift ? FMath::Max(Lift->GetFloat(), 0.0f) : 0.25f;
	}

	/** The most any drawn mesh of the pawn bulges past a cut at `Bone` (cm, world), or -1 if none could be measured;
	 *  `OutOpening`: how wide the widest of them is there (cm, world; -1 unknown). */
	float SeveredDome(const TArray<USkinnedMeshComponent*>& Skinned, FName Bone, bool bPieceSide, float RadiusCm, float& OutOpening, float& OutSkinOpening, bool bWhole = false)
	{
		float Most = -1.0f;
		OutOpening = -1.0f;
		OutSkinOpening = -1.0f;
		for (const USkinnedMeshComponent* Mesh : Skinned)
		{
			const USkeletalMesh* Asset = Mesh ? Cast<USkeletalMesh>(Mesh->GetSkinnedAsset()) : nullptr;
			if (!Asset || !SeveredDrawn(*Mesh))
			{
				continue;
			}
			const int32 Index = Asset->GetRefSkeleton().FindBoneIndex(Bone);
			const float Scale = static_cast<float>(Mesh->GetComponentScale().X);
			if (Index == INDEX_NONE || Scale <= 0.0f || SeveredSkinnedTo(*Asset, Index) < 0.01f)
			{
				continue;
			}
			float Opening = -1.0f;
			const float Dome = SeveredDomeCm(*Asset, Index, bPieceSide, RadiusCm / Scale, Opening, bWhole);
			UE_LOG(LogTemp, Log, TEXT("  %s bulge on %s (%s): %.1f cm, %.1f cm wide (radius)"), bPieceSide ? TEXT("piece") : TEXT("body"), *Asset->GetName(), *Bone.ToString(),
				Dome < 0.0f ? -1.0f : Dome * Scale, Opening < 0.0f ? -1.0f : Opening * Scale);
			if (Opening > 0.0f)
			{
				OutOpening = FMath::Max(OutOpening, Opening * Scale);
				// The skin is the innermost layer: the narrowest mesh there.
				OutSkinOpening = OutSkinOpening < 0.0f ? Opening * Scale : FMath::Min(OutSkinOpening, Opening * Scale);
			}
			// A mesh whose vertices can't be read (no CPU copy) is taken to stick out Gore.ClothLift radii.
			Most = FMath::Max(Most, Dome >= 0.0f ? Dome * Scale : SeveredUnreadDome() * RadiusCm);
		}
		return Most;
	}

	/** The face covers the skin's own measured width where it's cut (the limb is thicker at the shoulder or hip than the
	 *  thickness table says, and a smaller face left the flattened skin's hole showing round it), at most half again the
	 *  table's radius. */
	float SeveredFitFace(float TableRadiusCm, float SkinOpeningCm)
	{
		// Measured at the joint the skin takes in the shoulder's or buttock's bulge, and a face that wide stood off a
		// thigh piece as a ring twice the leg's width (PC round 14). Only a little over the table, then.
		return SkinOpeningCm > 0.0f ? FMath::Clamp(SkinOpeningCm, TableRadiusCm, 1.1f * TableRadiusCm) : TableRadiusCm;
	}

	/** A clothed cut's face reaches out to the sleeve's measured width (radii, 1 to 1.6), so no white inside of the
	 *  sleeve shows round the meat; bare or unmeasured, as given. At most a quarter wider than the face. */
	float SeveredFitCloth(float ClothOuter, float OpeningCm, float RadiusCm)
	{
		if (ClothOuter <= 1.01f || RadiusCm <= 0.0f)
		{
			return ClothOuter;
		}
		if (OpeningCm <= 0.0f)
		{
			return 1.02f;  // unmeasured: a guessed width stood off thinner sleeves as a hoop
		}
		return FMath::Clamp(OpeningCm / RadiusCm, 1.02f, 1.08f);
	}

	/** Whether a material slot is scalp hair (it may hang long below the head, so the head's length leaves it out). */
	bool SeveredIsHairSlot(const FName& Slot)
	{
		static const TCHAR* const Keys[] = {TEXT("hair"), TEXT("long0"), TEXT("bob0"), TEXT("short0"), TEXT("ponytail"), TEXT("braid"),
			TEXT("afro"), TEXT("twintail"), TEXT("messy")};
		const FString Lower = Slot.ToString().ToLower();
		for (const TCHAR* Key : Keys)
		{
			if (Lower.Contains(Key))
			{
				return true;
			}
		}
		return false;
	}

	/** The component whose pose a mesh shows: itself, or the top of its leader chain. */
	const USkinnedMeshComponent* SeveredPoseOf(const USkinnedMeshComponent* Mesh)
	{
		for (int32 Guard = 0; Guard < 8; ++Guard)
		{
			const USkinnedMeshComponent* Leader = Mesh->LeaderPoseComponent.Get();
			if (!Leader)
			{
				break;
			}
			Mesh = Leader;
		}
		return Mesh;
	}
}

ASeveredPieceActor::ASeveredPieceActor()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void ASeveredPieceActor::Throw(const FVector& InVelocity, const FRotator& InSpin, float InRadius)
{
	Velocity = InVelocity;
	Spin = InSpin;
	Radius = InRadius;
	bAtRest = false;
}

void ASeveredPieceActor::Push(const FVector& DeltaVelocity, const FVector& At)
{
	if (DeltaVelocity.IsNearlyZero())
	{
		return;
	}
	Velocity += DeltaVelocity;
	// An off-centre knock spins it: the turn axis is across the knock and the arm from the middle to where it was struck.
	const FVector Arm = At - GetActorLocation();
	const FVector Axis = FVector::CrossProduct(Arm, DeltaVelocity);
	const float Scale = FMath::Clamp(static_cast<float>(DeltaVelocity.Size()) / 300.0f, 0.2f, 2.0f);
	const FVector Local = GetActorQuat().UnrotateVector(Axis.GetSafeNormal()) * 540.0f * Scale;
	Spin += FRotator(Local.Y, Local.Z, Local.X);
	bAtRest = false;
	++Pushes;
}

float ASeveredPieceActor::MeasureSkinLengthCm(int32& OutVertices) const
{
	OutVertices = 0;
	FBox Box(ForceInit);
	for (const TObjectPtr<UPoseableMeshComponent>& Held : Meshes)
	{
		UPoseableMeshComponent* Mesh = Held.Get();
		const USkeletalMesh* Asset = Mesh ? Cast<USkeletalMesh>(Mesh->GetSkinnedAsset()) : nullptr;
		FSkeletalMeshRenderData* Data = Asset ? Asset->GetResourceForRendering() : nullptr;
		if (!Data || Data->LODRenderData.Num() == 0 || FPlatformProperties::RequiresCookedData())
		{
			continue;
		}
		FSkeletalMeshLODRenderData& Lod = Data->LODRenderData[0];
		FPositionVertexBuffer& Positions = Lod.StaticVertexBuffers.PositionVertexBuffer;
		const FSkinWeightVertexBuffer& Weights = Lod.SkinWeightVertexBuffer;
		const TArray<FMatrix44f>& InvRef = Asset->GetRefBasesInvMatrix();
		const TArray<FTransform>& Bones = Mesh->GetComponentSpaceTransforms();
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData() || Weights.GetNumVertices() != Positions.GetNumVertices() ||
			InvRef.Num() != Bones.Num())
		{
			continue;
		}
		// Each bone's skinning matrix as the piece is posed now (reference pose to component space).
		TArray<FMatrix44f> Skin;
		Skin.SetNum(Bones.Num());
		for (int32 b = 0; b < Bones.Num(); ++b)
		{
			Skin[b] = InvRef[b] * FMatrix44f(Bones[b].ToMatrixWithScale());
		}
		const FTransform ToWorld = Mesh->GetComponentTransform();
		const TArray<FSkeletalMaterial>& Slots = Asset->GetMaterials();
		const uint32 Influences = Weights.GetMaxBoneInfluences();
		for (const FSkelMeshRenderSection& Section : Lod.RenderSections)
		{
			const int32 Material = Section.MaterialIndex;
			if (!Mesh->IsMaterialSectionShown(Material, 0) || (Slots.IsValidIndex(Material) && SeveredIsHairSlot(Slots[Material].MaterialSlotName)))
			{
				continue;
			}
			for (uint32 V = Section.BaseVertexIndex; V < Section.BaseVertexIndex + Section.NumVertices && V < Positions.GetNumVertices(); ++V)
			{
				const FVector3f Ref = Positions.VertexPosition(V);
				FVector3f Sum = FVector3f::ZeroVector;
				float Total = 0.0f;
				for (uint32 K = 0; K < Influences; ++K)
				{
					const float W = static_cast<float>(Weights.GetBoneWeight(V, K));
					const uint32 Local = Weights.GetBoneIndex(V, K);
					if (W <= 0.0f || static_cast<int32>(Local) >= Section.BoneMap.Num() || !Skin.IsValidIndex(Section.BoneMap[Local]))
					{
						continue;
					}
					Sum += FVector3f(Skin[Section.BoneMap[Local]].TransformPosition(Ref)) * W;
					Total += W;
				}
				if (Total > 0.0f)
				{
					Box += ToWorld.TransformPosition(FVector(Sum / Total));
					++OutVertices;
				}
			}
		}
	}
	return OutVertices > 0 ? static_cast<float>(Box.GetSize().GetMax()) : -1.0f;
}

void ASeveredPieceActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bAtRest)
	{
		return;
	}
	const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr;
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const float Step = FMath::Min(DeltaSeconds, 0.05f);
	Velocity.Z -= SeveredGravity * Step;
	FVector At = GetActorLocation() + Velocity * Step;
	const FVector Here = (At - Bridge->GetWorldOrigin()) / 100.0;
	const float Floor = static_cast<float>(Bridge->SimToWorld(static_cast<float>(Here.X), static_cast<float>(Here.Y), 0).Z) + Radius;
	if (At.Z <= Floor)
	{
		At.Z = Floor;
		if (!bLanded)
		{
			// It leaves blood where it first comes down, and drips from its open end for a while.
			bLanded = true;
			if (CutEnd)
			{
				SeveredDrip(GetWorld(), CutEnd->GetComponentLocation(), CutEnd->GetUpVector(), nullptr, GSeveredDripSeconds.GetValueOnGameThread() * 0.4f);
			}
			if (UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
			{
				FBloodHit Hit;
				Hit.At = At;
				Hit.Direction = FVector(Velocity.X * 0.3, Velocity.Y * 0.3, -200.0);
				Hit.Kind = EBloodWound::Blunt;
				Hit.Damage = 8.0f;
				Hit.Force = 0.4f;
				Blood->AddHit(Hit);
			}
		}
		if (Velocity.Z < 0.0f)
		{
			Velocity.Z = -Velocity.Z * SeveredBounce;
		}
		Velocity.X *= SeveredGrip;
		Velocity.Y *= SeveredGrip;
		Spin = Spin * SeveredGrip;
		if (Velocity.Size() < SeveredRestSpeed)
		{
			bAtRest = true;
			Velocity = FVector::ZeroVector;
		}
	}
	FQuat Turned = (GetActorQuat() * (Spin * Step).Quaternion()).GetNormalized();
	if (bLanded && !LimbAxis.IsNearlyZero())
	{
		// Once it has touched the ground a limb falls over towards lying flat (quickly; flat when it stops).
		const FVector Now = Turned.RotateVector(LimbAxis);
		const FVector Flat = FVector(Now.X, Now.Y, 0.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const FQuat Down = FQuat::FindBetweenNormals(Now, Flat);
		Turned = (FQuat::Slerp(FQuat::Identity, Down, bAtRest ? 1.0f : FMath::Clamp(Step * 8.0f, 0.0f, 1.0f)) * Turned).GetNormalized();
	}
	SetActorLocationAndRotation(At, Turned);
}

// ---------------------------------------------------------------------------
// The pawn's side (declared in PawnActor.h).

void ASimPawnActor::ApplySevered(uint16 Bits)
{
	// The trunk burst (sneed, 2026-11-22: overkill carries until the corpse is fully gibbed): the body goes a couple of
	// frames on, after the limbs and head that came off with it have been copied into their pieces from the drawn body.
	if ((Bits & colony::kSeveredTrunk) != 0 && !bGibHidden && !bGibPending)
	{
		bGibPending = true;
		GibHideFrames = 3;
	}
	if (bGibPending && !bGibHidden && --GibHideFrames <= 0)
	{
		HideGibbedBody();
	}
	const uint16 Fresh = static_cast<uint16>(Bits & ~SeveredShown);
	if (Fresh == 0)
	{
		return;
	}
	SeveredShown = static_cast<uint16>(SeveredShown | Fresh);
	TArray<USkinnedMeshComponent*> Skinned;
	SeveredSkinned(*this, Skinned);
	TInlineComponentArray<USceneComponent*> Parts;
	GetComponents(Parts);
	if (MetaHuman)
	{
		TInlineComponentArray<USceneComponent*> More;
		MetaHuman->GetComponents(More);
		Parts.Append(More);
	}
	for (uint16 Bit = 1; Bit != 0 && Bit <= colony::kSeveredHead; Bit = static_cast<uint16>(Bit << 1))
	{
		FName Bone;
		FName End;
		if ((Fresh & Bit) == 0 || !SeveredBones(Bit, Bone, End))
		{
			continue;
		}
		// Each mesh folds the bone and all below it into the cut: the limb is gone and the hole closes on a point.
		// Only a mesh that shows another's pose (clothes, a MetaHuman body) or the mannequin can hide a bone: hiding
		// drops the bone from the pose, and a mesh with an animation of its own that still uses it then reads bone -1
		// (the crash on dressed bodies). The MakeHuman body folds the bone in its own animation; any other such mesh
		// (a MetaHuman face) goes whole with the head and otherwise stays.
		for (USkinnedMeshComponent* Mesh : Skinned)
		{
			if (!Mesh || Mesh->GetBoneIndex(Bone) == INDEX_NONE)
			{
				continue;
			}
			if (Mesh == Body.Get() || Mesh->LeaderPoseComponent.IsValid())
			{
				Mesh->HideBoneByName(Bone, PBO_None);
			}
			else if (Mesh == MakeHumanMesh.Get())
			{
				if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(MakeHumanMesh->GetAnimInstance()))
				{
					TArray<FName> Gone = Anim->GetSeveredBones();
					Gone.AddUnique(Bone);
					Anim->SetSeveredBones(Gone);
				}
			}
			else if (Bit == colony::kSeveredHead && SeveredDrawn(*Mesh))
			{
				// Only what is really drawn (SeveredDrawn: the actor too). The MetaHuman under a MakeHuman body is hidden as a
				// whole actor while its face keeps its own visible flag: that face, posed where the body once stood, went
				// into the head's piece and was stretched from the stump up to it as a long pink and white tube.
				SeveredHidden.Add(Mesh);
				Mesh->SetVisibility(false, true);
			}
		}
		// The open stump (skin rim, fat, muscle, bone) over the flat end, on the bone above the cut, and blood that keeps
		// dripping from it for a couple of minutes.
		USkeletalMeshComponent* Skin = GetDecalMesh();
		if (!Skin || Skin->GetBoneIndex(Bone) == INDEX_NONE)
		{
			Skin = Body;
		}
		FVector At;
		FVector Along;
		FVector Front;
		float BodyOpening = -1.0f;
		float BodySkin = -1.0f;
		const float BodyDome = SeveredDome(Skinned, Bone, false, SeveredCapRadius(Bit) * GetBodyScale() * GSeveredStumpScale.GetValueOnGameThread(), BodyOpening, BodySkin);
		const float CapRadius = SeveredFitFace(SeveredCapRadius(Bit) * GetBodyScale() * GSeveredStumpScale.GetValueOnGameThread(), BodySkin);
		UE_LOG(LogTemp, Log, TEXT("Severed 0x%x: body side bulges %.1f cm past the cut (cap radius %.1f cm)"), Bit, BodyDome, CapRadius);
		if (Skin && Skin->GetBoneIndex(Bone) != INDEX_NONE && SeveredCutFrame(*Skin, Bone, At, Along, Front) &&
			GoreMakeStump(this, Skin, Skin->GetParentBone(Bone), At, Along, Front, CapRadius, SeveredStumpKind(Bit),
				SeveredSkinColour(*this), static_cast<int32>(static_cast<uint32>(entt::to_integral(Thing)) * 31u + Bit), SeveredFitCloth(SeveredClothOuter(Skinned, Skin, Bone, Bit), BodyOpening, CapRadius), BodyDome))
		{
			++SeveredCaps;
			SeveredDrip(GetWorld(), At, Along, Skin, GSeveredDripSeconds.GetValueOnGameThread());
		}
		// What hangs on a socket of it (a hat, horns, a held thing) or grows from it (hair) goes with it.
		for (USceneComponent* Part : Parts)
		{
			if (!Part)
			{
				continue;
			}
			if (Bit == colony::kSeveredHead && Part->IsA<UGroomComponent>())
			{
				Part->SetVisibility(false, true);  // (and UStrandHairSubsystem gives a headless body no strands again)
				continue;
			}
			const USkinnedMeshComponent* On = Cast<USkinnedMeshComponent>(Part->GetAttachParent());
			const FName Socket = Part->GetAttachSocketName();
			if (!On || Socket.IsNone())
			{
				continue;
			}
			const FName Held = On->GetSocketBoneName(Socket);
			if (!Held.IsNone() && (Held == Bone || (On->GetBoneIndex(Bone) != INDEX_NONE && On->BoneIsChildOf(Held, Bone))))
			{
				if (Part->IsVisible())
				{
					SeveredHiddenParts.Add(Part);  // it still goes into the piece
				}
				Part->SetVisibility(false, true);
			}
		}
	}
}

void ASimPawnActor::HideGibbedBody()
{
	bGibPending = false;
	bGibHidden = true;
	// Nothing of it is left to draw, push or stand on: every mesh stops simulating and colliding, and the actor (and its
	// MetaHuman) is hidden through SetBodyHidden, which keeps it hidden whatever first person or cut-away do later.
	TInlineComponentArray<UPrimitiveComponent*> Prims;
	GetComponents(Prims);
	if (MetaHuman)
	{
		TInlineComponentArray<UPrimitiveComponent*> More;
		MetaHuman->GetComponents(More);
		Prims.Append(More);
	}
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (!Prim)
		{
			continue;
		}
		if (Prim->IsSimulatingPhysics())
		{
			Prim->SetSimulatePhysics(false);
		}
		Prim->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	SetBodyHidden(bBodyHidden);
	UE_LOG(LogTemp, Log, TEXT("Gibs: body %u hidden (trunk burst)"), static_cast<uint32>(entt::to_integral(Thing)));
}

int32 ASimPawnActor::BurstIntoGibs(float Overkill)
{
	if (bGibsBurst || !GetWorld())
	{
		return 0;
	}
	bGibsBurst = true;
	// Where the trunk lies now: the ragdoll's pelvis, else the middle of the spine, else a little over the actor.
	FVector Centre = GetActorLocation() + FVector(0.0, 0.0, 20.0);
	FVector Pelvis;
	if (GetRagdollPelvis(Pelvis))
	{
		Centre = Pelvis;
	}
	const USkeletalMeshComponent* Skin = GetDecalMesh();
	if (!Skin || Skin->GetBoneIndex(TEXT("spine_02")) == INDEX_NONE)
	{
		Skin = Body.Get();
	}
	if (Skin && Skin->GetBoneIndex(TEXT("spine_02")) != INDEX_NONE)
	{
		Centre = Skin->GetBoneLocation(TEXT("spine_02"), EBoneSpaces::WorldSpace);
	}
	// 25 past 0 bursts it: that is strength 1; a rocket's far more throws harder.
	const float Strength = 0.8f + FMath::Max(Overkill, 0.0f) / 60.0f;
	// The body's up (organs from the chest, guts from the belly): along its spine as it lies now.
	FVector Up = FVector::UpVector;
	if (Skin && Skin->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE && Skin->GetBoneIndex(TEXT("spine_05")) != INDEX_NONE)
	{
		Up = (Skin->GetBoneLocation(TEXT("spine_05"), EBoneSpaces::WorldSpace) - Skin->GetBoneLocation(TEXT("pelvis"), EBoneSpaces::WorldSpace))
			.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	}
	// Organs only in a body that bleeds red (not a machine's fluid or an insect's).
	bool bOrgans = true;
	if (const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>(); Bridge && Bridge->GetSim())
	{
		if (const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Thing))
		{
			const colony::Sim& Sim = *Bridge->GetSim();
			const int Kind = Sim.filth().bloodKindOf(View->def);
			bOrgans = Kind >= 0 && Kind < static_cast<int>(Sim.filth().kinds().size()) && Sim.filth().kinds()[Kind].name == "Blood";
		}
	}
	// The drawn meshes (the bare body and its clothes) for the torso's slabs.
	TArray<USkinnedMeshComponent*> All;
	SeveredSkinned(*this, All);
	TArray<USkinnedMeshComponent*> Drawn;
	for (USkinnedMeshComponent* Mesh : All)
	{
		const AActor* MeshOwner = Mesh ? Mesh->GetOwner() : nullptr;
		if (Mesh && SeveredDrawn(*Mesh) && !(MeshOwner && MeshOwner->IsHidden()) && Mesh->GetBoneIndex(TEXT("pelvis")) != INDEX_NONE)
		{
			Drawn.Add(Mesh);
		}
	}
	GibsThrown = GoreBurstTrunk(GetWorld(), Drawn, Skin, Centre, Up, GetBodyScale(), Strength, SeveredSkinColour(*this),
		static_cast<int32>(static_cast<uint32>(entt::to_integral(Thing)) * 977u + 13u), Thing, bOrgans);
	if (!bGibHidden && !bGibPending)
	{
		bGibPending = true;
		GibHideFrames = 3;
	}
	return GibsThrown;
}

ASeveredPieceActor* ASimPawnActor::SpawnSeveredPiece(uint16 Bit)
{
	FName Bone;
	FName End;
	if (!GetWorld() || !Body || !SeveredBones(Bit, Bone, End) || Body->GetBoneIndex(Bone) == INDEX_NONE)
	{
		return nullptr;
	}
	const FVector Cut = Body->GetBoneLocation(Bone, EBoneSpaces::WorldSpace);
	FVector Middle = Cut;
	if (!End.IsNone() && Body->GetBoneIndex(End) != INDEX_NONE)
	{
		Middle = (Cut + Body->GetBoneLocation(End, EBoneSpaces::WorldSpace)) * 0.5;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASeveredPieceActor* Piece = GetWorld()->SpawnActor<ASeveredPieceActor>(ASeveredPieceActor::StaticClass(), FTransform(Middle), Params);
	if (!Piece)
	{
		return nullptr;
	}
	Piece->SetBit(Bit);
	// Where the drawn body has the joint now (the MakeHuman body's own, which a short body has well below the hidden
	// mannequin's): a mesh whose pose has its cut bone far from here is not posed as the body is drawn, and is left out.
	FVector Joint = Cut;
	if (const USkeletalMeshComponent* Drawn = GetDecalMesh(); Drawn && Drawn->GetBoneIndex(Bone) != INDEX_NONE)
	{
		Joint = Drawn->GetBoneLocation(Bone, EBoneSpaces::WorldSpace);
	}
	const float StaleCm = 30.0f * GetBodyScale();
	TArray<USkinnedMeshComponent*> Skinned;
	SeveredSkinned(*this, Skinned);
	for (USkinnedMeshComponent* Source : Skinned)
	{
		USkeletalMesh* Asset = Source ? Cast<USkeletalMesh>(Source->GetSkinnedAsset()) : nullptr;
		if (!Asset || (!SeveredDrawn(*Source) && !SeveredHidden.Contains(Source)))
		{
			continue;  // (a face hidden because the head is off still goes into the head's piece)
		}
		if (const AActor* SourceOwner = Source->GetOwner(); SourceOwner && SourceOwner->IsHidden())
		{
			continue;  // a whole hidden actor (the MetaHuman under a MakeHuman body) is never part of what is drawn
		}
		const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
		const int32 CutIndex = Skeleton.FindBoneIndex(Bone);
		// Nothing of this mesh on the piece (a face has arm bones, but no arm): left out. A head only takes what is
		// mostly head (the face, a hat): a shirt's collar or the body's neck skin, a little of it hanging on the head
		// bone, was drawn out into a jagged shard under the head. The MakeHuman body is the exception: its one mesh is
		// the whole person, head and face included, so it always goes (everything below the neck folds into the cut).
		const float Share = CutIndex == INDEX_NONE ? 0.0f : SeveredSkinnedTo(*Asset, CutIndex);
		if (Share < 0.01f || (Bit == colony::kSeveredHead && Share < 0.5f && Source != MakeHumanMesh.Get()))
		{
			continue;
		}
		// The piece's bones stand where the body has them now. Where the cut bone is, in the mesh's own space: as posed on
		// the body, or straightened onto its parent if the body has folded or flattened it already (a piece made after the
		// cut was shown).
		const USkinnedMeshComponent* Pose = SeveredPoseOf(Source);
		const FTransform ToCopy = Source->GetComponentTransform();
		FTransform CutNow = FTransform::Identity;
		bool bCutNow = false;
		if (const int32 From = Pose->GetBoneIndex(Skeleton.GetBoneName(CutIndex)); From != INDEX_NONE)
		{
			FTransform Now = Pose->GetBoneTransform(From);
			const FName PoseParent = Pose->GetParentBone(Skeleton.GetBoneName(CutIndex));
			if (Now.GetScale3D().GetAbsMin() < 0.1 && !PoseParent.IsNone() && Pose->GetBoneIndex(PoseParent) != INDEX_NONE)
			{
				Now = Skeleton.GetRefBonePose()[CutIndex] * Pose->GetBoneTransform(Pose->GetBoneIndex(PoseParent));
			}
			Now.SetScale3D(ToCopy.GetScale3D());
			CutNow = Now.GetRelativeTransform(ToCopy);
			CutNow.SetScale3D(FVector::OneVector);
			bCutNow = true;
			const double Off = FVector::Dist(ToCopy.TransformPosition(CutNow.GetLocation()), Joint);
			if (Off > StaleCm)
			{
				// Its pose is not the drawn body's (a mesh that stopped animating while hidden): copied, its skin would be
				// pulled from where its cut bone is to the real cut, a long stretched tube.
				UE_LOG(LogTemp, Warning, TEXT("Severed piece 0x%x: %s left out, its %s is %.0f cm from the body's"), Bit, *Source->GetName(),
					*Bone.ToString(), Off);
				continue;
			}
		}
		UPoseableMeshComponent* Copy = NewObject<UPoseableMeshComponent>(Piece);
		Copy->SetSkinnedAssetAndUpdate(Asset);
		Copy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Copy->SetCanEverAffectNavigation(false);
		Copy->bReceivesDecals = false;  // the pools it lands in would paint it red all over
		for (int32 i = 0; i < Source->GetNumMaterials(); ++i)
		{
			Copy->SetMaterial(i, Source->GetMaterial(i));
		}
		const TArray<float>& Custom = Source->GetCustomPrimitiveData().Data;
		for (int32 i = 0; i < Custom.Num(); ++i)
		{
			Copy->SetCustomPrimitiveDataFloat(i, Custom[i]);
		}
		Copy->SetupAttachment(Piece->GetRootComponent());
		Copy->RegisterComponent();
		Copy->SetWorldTransform(ToCopy);
		// The same sections drawn as on the body: one hair style of the several the MakeHuman body carries, no skin cut
		// away under clothes (a copy shows them all). A head whose wig the strands replaced shows the wig: strands can't
		// go with the piece.
		for (int32 Material = 0; Material < Source->GetNumMaterials(); ++Material)
		{
			if (!Source->IsMaterialSectionShown(Material, 0))
			{
				for (int32 Lod = 0; Lod < Copy->GetNumLODs(); ++Lod)
				{
					Copy->ShowMaterialSection(Material, 0, false, Lod);
				}
			}
		}
		if (Bit == colony::kSeveredHead && Source == MakeHumanMesh.Get())
		{
			TArray<int32> Wig;
			const UStrandHairSubsystem* Strands = GetWorld()->GetSubsystem<UStrandHairSubsystem>();
			if (Strands && Strands->GetHiddenWig(MakeHumanMesh.Get(), Wig))
			{
				for (const int32 Section : Wig)
				{
					for (int32 Lod = 0; Lod < Copy->GetNumLODs(); ++Lod)
					{
						Copy->ShowMaterialSection(Section, 0, true, Lod);
					}
				}
			}
		}

		// The bone above the cut is flattened onto the joint across the piece's line (GoreCut.h), so the piece ends in a
		// flat face under its cap; every other bone is folded into the middle of the cut (and the parent too where it
		// can't be flattened cleanly). The middle of the cut is the piece's own joint (its cut bone as posed), never a
		// point of another mesh: the hidden mannequin's joint stands centimetres off a smaller MakeHuman body's, and skin
		// half on the head and half on the neck was drawn out between the two.
		const FTransform Folded(FQuat::Identity, bCutNow ? CutNow.GetLocation() : ToCopy.InverseTransformPosition(Cut), FVector(SeveredFold));
		if (!bCutNow)
		{
			CutNow = Folded;
		}
		const int32 ParentIndex = Skeleton.GetParentIndex(CutIndex);
		for (int32 i = 0; i < Skeleton.GetNum(); ++i)
		{
			const FName Name = Skeleton.GetBoneName(i);
			const bool bInPiece = i == CutIndex || Skeleton.BoneIsChildOf(i, CutIndex);
			if (!bInPiece)
			{
				FTransform Flat;
				if (i == ParentIndex && bCutNow &&
					GoreCut::FlattenParent(CutNow, Skeleton.GetRefBonePose()[CutIndex], GoreCut::AxisLocal(Skeleton, CutIndex), Flat))
				{
					Copy->SetBoneTransformByName(Name, Flat, EBoneSpaces::ComponentSpace);
				}
				else
				{
					Copy->SetBoneTransformByName(Name, Folded, EBoneSpaces::ComponentSpace);
				}
				continue;
			}
			if (i != CutIndex)
			{
				// Below the cut the piece lies as the mesh was made (its reference pose, straight): a limb cut off while
				// the falling body had it bent double was a stubby tube with its hand or shoe tucked out of sight.
				continue;
			}
			if (bCutNow)
			{
				Copy->SetBoneTransformByName(Name, CutNow, EBoneSpaces::ComponentSpace);
			}
		}
		Piece->AddMesh(Copy);
		UE_LOG(LogTemp, Log, TEXT("Severed piece 0x%x: %s (%s), material %s, %.0f cm across"), Bit, *Source->GetName(), *Asset->GetName(),
			Copy->GetMaterial(0) ? *Copy->GetMaterial(0)->GetName() : TEXT("none"), Copy->Bounds.SphereRadius * 2.0f);
	}
	// What sat on the bone as a plain mesh (hair cards, a hat, horns) goes with the piece. Hair strands (grooms) cannot
	// be copied, so the hair cards stand in for them on a severed head even up close.
	// Hair strands (grooms) do not go with a head: an unbound copy did not draw. The hair cards stand in, up close too.
	TInlineComponentArray<UStaticMeshComponent*> Worn;
	GetComponents(Worn);
	if (MetaHuman)
	{
		TInlineComponentArray<UStaticMeshComponent*> More;
		MetaHuman->GetComponents(More);
		Worn.Append(More);
	}
	for (UStaticMeshComponent* Part : Worn)
	{
		const USkinnedMeshComponent* On = Part ? Cast<USkinnedMeshComponent>(Part->GetAttachParent()) : nullptr;
		const FName Socket = Part ? Part->GetAttachSocketName() : NAME_None;
		if (!On || Socket.IsNone() || !Part->GetStaticMesh())
		{
			continue;
		}
		const FName Held = On->GetSocketBoneName(Socket);
		if (Held.IsNone() || !(Held == Bone || (On->GetBoneIndex(Bone) != INDEX_NONE && On->BoneIsChildOf(Held, Bone))))
		{
			continue;
		}
		// Only what was drawn on this body: not what sits on the hidden MetaHuman under a MakeHuman body (its face's hair
		// cards, posed where that face was left), nor anything hidden for its own reasons. The hair cards stand in for
		// strands even when the strands were showing; never under a helmet.
		const AActor* OnOwner = On->GetOwner();
		const bool bCards = Part == HairCards.Get();
		if ((OnOwner && OnOwner->IsHidden()) || (bCards && bHairHidden) ||
			(!bCards && !Part->IsVisible() && !SeveredHiddenParts.Contains(Part)))
		{
			continue;
		}
		UStaticMeshComponent* Copy = NewObject<UStaticMeshComponent>(Piece);
		Copy->SetStaticMesh(Part->GetStaticMesh());
		for (int32 i = 0; i < Part->GetNumMaterials(); ++i)
		{
			Copy->SetMaterial(i, Part->GetMaterial(i));
		}
		const TArray<float>& WornData = Part->GetCustomPrimitiveData().Data;
		for (int32 i = 0; i < WornData.Num(); ++i)
		{
			Copy->SetCustomPrimitiveDataFloat(i, WornData[i]);
		}
		Copy->bReceivesDecals = false;
		Copy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Copy->SetCanEverAffectNavigation(false);
		Copy->SetupAttachment(Piece->GetRootComponent());
		Copy->RegisterComponent();
		Copy->SetWorldTransform(Part->GetComponentTransform());
		UE_LOG(LogTemp, Log, TEXT("Severed piece 0x%x: worn %s (%s)"), Bit, *Part->GetName(), *Part->GetStaticMesh()->GetName());
	}
	if (Piece->GetMeshCount() == 0)
	{
		Piece->Destroy();
		return nullptr;
	}
	// Blood gushes out of the stump, along the limb that left.
	if (UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
	{
		FVector Out = Middle - Cut;
		Out.Z += 20.0;
		FBloodHit Hit;
		Hit.At = Cut;
		Hit.Direction = Out;
		Hit.Kind = EBloodWound::Cut;
		Hit.Damage = 35.0f;
		Hit.Force = 1.2f;
		Hit.Victim = Thing;
		Hit.Mesh = GetDecalMesh();
		Blood->AddHit(Hit);
	}
	// Its open end, capped the same way, facing back where the body was; it drips once it has come down.
	{
		FVector At = Cut;
		FVector Along = Middle - Cut;
		FVector Front = GetActorForwardVector();
		USkeletalMeshComponent* Skin = GetDecalMesh();
		if (!Skin || !SeveredPieceCutFrame(*Skin, Bone, At, Along, Front))
		{
			At = Cut;
			Along = Bit == colony::kSeveredHead ? FVector::UpVector : (Middle - Cut).GetSafeNormal();
		}
		const float PieceRadiusTable = SeveredCapRadius(Bit) * GetBodyScale() * GSeveredStumpScale.GetValueOnGameThread();
		float PieceOpening = -1.0f;
		float PieceSkin = -1.0f;
		// A head (with its neck) is cut at the neck's root: skin wholly on the piece closes over its cap, which stands below
		// all of it.
		const float PieceDome = SeveredDome(Skinned, Bone, true, PieceRadiusTable, PieceOpening, PieceSkin, Bit == colony::kSeveredHead);
		const float PieceRadius = SeveredFitFace(PieceRadiusTable, PieceSkin);
		UE_LOG(LogTemp, Log, TEXT("Severed piece 0x%x: bulges %.1f cm past the cut (cap radius %.1f cm)"), Bit, PieceDome, PieceRadius);
		if (USceneComponent* Stump = GoreMakeStump(Piece, Piece->GetRootComponent(), NAME_None, At, -Along, Front, PieceRadius,
				SeveredStumpKind(Bit), SeveredSkinColour(*this), static_cast<int32>(static_cast<uint32>(entt::to_integral(Thing)) * 17u + Bit * 7u),
				SeveredFitCloth(SeveredClothOuter(Skinned, Skin, Bone, Bit), PieceOpening, PieceRadius), PieceDome))
		{
			Piece->SetCutEnd(Stump);
		}
	}
	// Thrown up and away from the body's middle, tumbling.
	FVector Away = Middle - GetActorLocation();
	Away.Z = 0.0;
	Away = Away.GetSafeNormal();
	if (Away.IsNearlyZero())
	{
		Away = FVector(FMath::FRandRange(-1.0f, 1.0f), FMath::FRandRange(-1.0f, 1.0f), 0.0f).GetSafeNormal();
	}
	const FVector Velocity = Away * FMath::FRandRange(120.0f, 260.0f) + FVector(FMath::FRandRange(-60.0f, 60.0f), FMath::FRandRange(-60.0f, 60.0f), FMath::FRandRange(220.0f, 380.0f));
	const FRotator Spin(FMath::FRandRange(-360.0f, 360.0f), FMath::FRandRange(-360.0f, 360.0f), FMath::FRandRange(-360.0f, 360.0f));
	if (Bit != colony::kSeveredHead)
	{
		Piece->SetLimbAxis(Middle - Cut);
	}
	Piece->Throw(Velocity, Spin, Bit == colony::kSeveredHead ? 10.0f : 6.0f);
	return Piece;
}
