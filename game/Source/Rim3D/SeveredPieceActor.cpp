#include "SeveredPieceActor.h"

#include "Components/PoseableMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GroomComponent.h"

#include "BloodSubsystem.h"
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

	/** Wet flesh over a cut: a ball of the blood drops' glossy red where the folded mesh closes on a point. */
	UStaticMeshComponent* SeveredCap(AActor* Owner, USceneComponent* Parent, FName Socket, const FVector& At, float RadiusCm)
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
		Cap->SetupAttachment(Parent, Socket);
		Cap->RegisterComponent();
		Cap->SetWorldLocation(At);
		Cap->SetWorldScale3D(FVector(RadiusCm / 50.0f));
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
				Hit.Damage = 12.0f;
				Hit.Force = 0.7f;
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
	SetActorLocationAndRotation(At, (GetActorQuat() * (Spin * Step).Quaternion()).GetNormalized());
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
		for (USkinnedMeshComponent* Mesh : Skinned)
		{
			if (Mesh && Mesh->GetBoneIndex(Bone) != INDEX_NONE)
			{
				Mesh->HideBoneByName(Bone, PBO_None);
			}
		}
		// Wet flesh over the stump, on the bone above the cut.
		USkeletalMeshComponent* Skin = GetDecalMesh();
		if (!Skin || Skin->GetBoneIndex(Bone) == INDEX_NONE)
		{
			Skin = Body;
		}
		if (Skin && Skin->GetBoneIndex(Bone) != INDEX_NONE &&
			SeveredCap(this, Skin, Skin->GetParentBone(Bone), Skin->GetBoneLocation(Bone, EBoneSpaces::WorldSpace), SeveredCapRadius(Bit) * GetBodyScale()))
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
			if (!Held.IsNone() && (Held == Bone || On->BoneIsChildOf(Held, Bone)))
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
		if (!Asset || !Source->IsVisible())
		{
			continue;
		}
		const FReferenceSkeleton& Skeleton = Asset->GetRefSkeleton();
		const int32 CutIndex = Skeleton.FindBoneIndex(Bone);
		if (CutIndex == INDEX_NONE)
		{
			continue;
		}
		UPoseableMeshComponent* Copy = NewObject<UPoseableMeshComponent>(Piece);
		Copy->SetSkinnedAssetAndUpdate(Asset);
		Copy->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Copy->SetCanEverAffectNavigation(false);
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
			Copy->SetBoneTransformByName(Name, Pose->GetBoneTransform(From).GetRelativeTransform(ToCopy), EBoneSpaces::ComponentSpace);
		}
		Piece->AddMesh(Copy);
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
		Hit.Damage = 60.0f;
		Hit.Force = 2.5f;
		Hit.Victim = Thing;
		Hit.Mesh = GetDecalMesh();
		Blood->AddHit(Hit);
	}
	// Wet flesh over its cut end too.
	SeveredCap(Piece, Piece->GetRootComponent(), NAME_None, Cut, SeveredCapRadius(Bit) * GetBodyScale());
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
	Piece->Throw(Velocity, Spin, Bit == colony::kSeveredHead ? 10.0f : 6.0f);
	return Piece;
}
