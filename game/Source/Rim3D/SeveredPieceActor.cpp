#include "SeveredPieceActor.h"

#include "AnimationRuntime.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GroomAsset.h"
#include "GroomComponent.h"

#include "BloodSubsystem.h"
#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "SimSubsystem.h"

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
		case colony::kSeveredHead: OutBone = TEXT("head"); OutEnd = NAME_None; return true;
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
		default: return 5.6f;  // the neck
		}
	}

	/** Wet flesh over a cut: a flat disc of the blood drops' glossy dark red across the limb (Along: the way the limb
	 *  runs), where the folded mesh closes on a point. A full ball there read as a pom-pom stuck on the stump. */
	UStaticMeshComponent* SeveredCap(AActor* Owner, USceneComponent* Parent, FName Socket, const FVector& At, const FVector& Along, float RadiusCm)
	{
		UStaticMesh* Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		if (!Owner || !Parent || !Ball)
		{
			return nullptr;
		}
		UStaticMeshComponent* Cap = NewObject<UStaticMeshComponent>(Owner);
		Cap->SetStaticMesh(Ball);
		if (UMaterialInterface* Flesh = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Rim3D/Generated/Blood/M_BloodDrop.M_BloodDrop")))
		{
			Cap->SetMaterial(0, Flesh);
		}
		Cap->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Cap->SetCanEverAffectNavigation(false);
		Cap->SetCastShadow(false);
		Cap->bReceivesDecals = false;
		Cap->SetupAttachment(Parent, Socket);
		Cap->RegisterComponent();
		const FVector Axis = Along.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		Cap->SetWorldLocationAndRotation(At, FRotationMatrix::MakeFromZ(Axis).ToQuat());
		const float Across = RadiusCm * 0.9f / 50.0f;
		Cap->SetWorldScale3D(FVector(Across, Across, Across * 0.3f));
		return Cap;
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
			// It leaves blood where it first comes down.
			bLanded = true;
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
			else if (Bit == colony::kSeveredHead && Mesh->IsVisible())
			{
				SeveredHidden.Add(Mesh);
				Mesh->SetVisibility(false, true);
			}
		}
		// Wet flesh over the stump, on the bone above the cut.
		USkeletalMeshComponent* Skin = GetDecalMesh();
		if (!Skin || Skin->GetBoneIndex(Bone) == INDEX_NONE)
		{
			Skin = Body;
		}
		if (Skin && Skin->GetBoneIndex(Bone) != INDEX_NONE &&
			SeveredCap(this, Skin, Skin->GetParentBone(Bone), Skin->GetBoneLocation(Bone, EBoneSpaces::WorldSpace),
				Skin->GetBoneLocation(Bone, EBoneSpaces::WorldSpace) - Skin->GetBoneLocation(Skin->GetParentBone(Bone), EBoneSpaces::WorldSpace), SeveredCapRadius(Bit) * GetBodyScale()))
		{
			++SeveredCaps;
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
				Part->SetVisibility(false, true);
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
				Part->SetVisibility(false, true);
			}
		}
	}
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
	TArray<USkinnedMeshComponent*> Skinned;
	SeveredSkinned(*this, Skinned);
	for (USkinnedMeshComponent* Source : Skinned)
	{
		USkeletalMesh* Asset = Source ? Cast<USkeletalMesh>(Source->GetSkinnedAsset()) : nullptr;
		if (!Asset || (!SeveredDrawn(*Source) && !SeveredHidden.Contains(Source)))
		{
			continue;  // (a face hidden because the head is off still goes into the head's piece)
		}
		const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
		const int32 CutIndex = Skeleton.FindBoneIndex(Bone);
		// Nothing of this mesh on the piece (a face has arm bones, but no arm): left out. A head only takes what is
		// mostly head (the face, a hat): a shirt's collar or the body's neck skin, a little of it hanging on the head
		// bone, was drawn out into a jagged shard under the head.
		const float Share = CutIndex == INDEX_NONE ? 0.0f : SeveredSkinnedTo(*Asset, CutIndex);
		if (Share < 0.01f || (Bit == colony::kSeveredHead && Share < 0.5f))
		{
			continue;
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
		Copy->SetWorldTransform(Source->GetComponentTransform());

		// The piece's bones stand where the body has them now; every other bone is folded into the cut, so the rest of
		// the mesh shrinks to a point there (which also closes the open end).
		const USkinnedMeshComponent* Pose = SeveredPoseOf(Source);
		const FTransform ToCopy = Copy->GetComponentTransform();
		const FTransform Folded(FQuat::Identity, ToCopy.InverseTransformPosition(Cut), FVector(SeveredFold));
		for (int32 i = 0; i < Skeleton.GetNum(); ++i)
		{
			const FName Name = Skeleton.GetBoneName(i);
			const bool bInPiece = i == CutIndex || Skeleton.BoneIsChildOf(i, CutIndex);
			if (!bInPiece)
			{
				Copy->SetBoneTransformByName(Name, Folded, EBoneSpaces::ComponentSpace);
				continue;
			}
			const int32 From = Pose->GetBoneIndex(Name);
			if (From == INDEX_NONE)
			{
				continue;  // a bone only this mesh has: it keeps its place under its parent
			}
			FTransform Now = Pose->GetBoneTransform(From);
			if (Now.GetScale3D().GetAbsMax() < 0.1)
			{
				// Already folded away on the body (a piece made after the cut was shown): the mannequin's bone instead.
				const int32 Under = Pose != Body.Get() ? Body->GetBoneIndex(Name) : INDEX_NONE;
				if (Under == INDEX_NONE)
				{
					continue;
				}
				Now = Body->GetBoneTransform(Under);
			}
			Copy->SetBoneTransformByName(Name, Now.GetRelativeTransform(ToCopy), EBoneSpaces::ComponentSpace);
		}
		Piece->AddMesh(Copy);
		UE_LOG(LogTemp, Log, TEXT("Severed piece 0x%x: %s (%s), material %s, %.0f cm across"), Bit, *Source->GetName(), *Asset->GetName(),
			Copy->GetMaterial(0) ? *Copy->GetMaterial(0)->GetName() : TEXT("none"), Copy->Bounds.SphereRadius * 2.0f);
	}
	// What sat on the bone as a plain mesh (hair cards, a hat, horns) goes with the piece. Hair strands (grooms) cannot
	// be copied, so the hair cards stand in for them on a severed head even up close.
	// The hair, brows and beard of a head seen from close by are strands (grooms): each goes with the head as it grew
	// (unbound, so in its rest shape), set where the head is now. From further off the hair cards stand in (below).
	const bool bStrands = Bit == colony::kSeveredHead && Detail == ESimPawnDetail::Full;
	if (bStrands)
	{
		for (USceneComponent* Candidate : Grooms)
		{
			const UGroomComponent* Groom = Cast<UGroomComponent>(Candidate);
			const USkinnedMeshComponent* Face = Groom ? Cast<USkinnedMeshComponent>(Groom->GetAttachParent()) : nullptr;
			const USkeletalMesh* FaceAsset = Face ? Cast<USkeletalMesh>(Face->GetSkinnedAsset()) : nullptr;
			if (!FaceAsset || !Groom->GroomAsset || EmptyGrooms.Contains(Groom->GetFName()) || (bHairHidden && Groom->GetName().Contains(TEXT("Hair"))))
			{
				continue;
			}
			const int32 Head = FaceAsset->GetRefSkeleton().FindBoneIndex(Bone);
			const USkinnedMeshComponent* Pose = SeveredPoseOf(Face);
			const int32 From = Pose->GetBoneIndex(Bone);
			if (Head == INDEX_NONE || From == INDEX_NONE)
			{
				continue;
			}
			const FTransform Rest = FAnimationRuntime::GetComponentSpaceTransformRefPose(FaceAsset->GetRefSkeleton(), Head);
			UGroomComponent* Copy = NewObject<UGroomComponent>(Piece);
			Copy->SetGroomAsset(Groom->GroomAsset);
			for (int32 i = 0; i < Groom->GetNumMaterials(); ++i)
			{
				Copy->SetMaterial(i, Groom->GetMaterial(i));
			}
			Copy->SetupAttachment(Piece->GetRootComponent());
			Copy->RegisterComponent();
			Copy->SetWorldTransform(Groom->GetRelativeTransform() * Rest.Inverse() * Pose->GetBoneTransform(From));
			UE_LOG(LogTemp, Log, TEXT("Severed piece 0x%x: strands %s (%s)"), Bit, *Groom->GetName(), *Groom->GroomAsset->GetName());
		}
	}
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
		if (!On || Socket.IsNone() || !Part->GetStaticMesh() || (bStrands && Part == HairCards.Get()))
		{
			continue;
		}
		const FName Held = On->GetSocketBoneName(Socket);
		if (Held.IsNone() || !(Held == Bone || (On->GetBoneIndex(Bone) != INDEX_NONE && On->BoneIsChildOf(Held, Bone))))
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
	// Wet flesh over its cut end too.
	SeveredCap(Piece, Piece->GetRootComponent(), NAME_None, Cut, Bit == colony::kSeveredHead ? FVector::UpVector : Middle - Cut, SeveredCapRadius(Bit) * GetBodyScale());
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
