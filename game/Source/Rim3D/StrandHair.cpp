#include "StrandHair.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GroomAsset.h"
#include "GroomComponent.h"
#include "HAL/IConsoleManager.h"
#include "PawnFaces.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace
{
	TAutoConsoleVariable<float> GStrandHairScale(TEXT("Pawns.StrandHairScale"), 1.0f,
		TEXT("Strand hair on MakeHuman bodies: size times the fitted size."));
	TAutoConsoleVariable<float> GStrandHairOffsetX(TEXT("Pawns.StrandHairOffsetX"), 0.0f, TEXT("Strand hair: cm along the body's X."));
	TAutoConsoleVariable<float> GStrandHairOffsetY(TEXT("Pawns.StrandHairOffsetY"), 0.0f, TEXT("Strand hair: cm along the body's Y."));
	TAutoConsoleVariable<float> GStrandHairOffsetZ(TEXT("Pawns.StrandHairOffsetZ"), 0.0f, TEXT("Strand hair: cm along the body's Z."));
	TAutoConsoleVariable<float> GStrandHairYaw(TEXT("Pawns.StrandHairYaw"), 0.0f,
		TEXT("Strand hair: degrees turned about the head (if the body faces another way than the MetaHuman meshes)."));

	const TCHAR* const StrandHairPhysicsAssetPath = TEXT("/Game/Characters/UEFN_Mannequin/Rigs/PA_UEFN_Mannequin.PA_UEFN_Mannequin");

	/** A skeleton's head and neck_01 in the reference pose, component space. */
	bool StrandHairHead(const USkeletalMesh& Mesh, FTransform& OutHead, float& OutNeckToHead)
	{
		const FReferenceSkeleton& Ref = Mesh.GetRefSkeleton();
		const int32 Head = Ref.FindBoneIndex(TEXT("head"));
		const int32 Neck = Ref.FindBoneIndex(TEXT("neck_01"));
		if (Head == INDEX_NONE)
		{
			return false;
		}
		OutHead = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Head);
		OutNeckToHead = Neck == INDEX_NONE ? 0.0f
			: static_cast<float>((OutHead.GetLocation() - FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Neck).GetLocation()).Size());
		return true;
	}

	/** The MetaHuman head the grooms were made on: any shaped face mesh in the PawnFaces folder (SK_Face_*). */
	const USkeletalMesh* StrandHairMetaHumanFace()
	{
		static TWeakObjectPtr<const USkeletalMesh> Cached;
		if (Cached.IsValid())
		{
			return Cached.Get();
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.ScanPathsSynchronous({FString(PawnFacesFolder)}, false);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(PawnFacesFolder), Assets, false);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().StartsWith(TEXT("SK_Face_")) && Asset.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName())
			{
				Cached = Cast<USkeletalMesh>(Asset.GetAsset());
				if (Cached.IsValid())
				{
					break;
				}
			}
		}
		return Cached.Get();
	}
}

UGroomAsset* StrandHairFind(const FString& Name)
{
	return LoadObject<UGroomAsset>(nullptr, *PawnFacesAssetPath(Name));
}

UGroomComponent* StrandHairAttach(USkeletalMeshComponent* Body, UGroomAsset* Groom, UGroomComponent* Existing)
{
	const USkeletalMesh* BodyMesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
	const USkeletalMesh* Face = StrandHairMetaHumanFace();
	FTransform BodyHead;
	FTransform FaceHead;
	float BodyNeck = 0.0f;
	float FaceNeck = 0.0f;
	if (!BodyMesh || !Groom || !Face || !StrandHairHead(*BodyMesh, BodyHead, BodyNeck) || !StrandHairHead(*Face, FaceHead, FaceNeck))
	{
		if (Existing)
		{
			Existing->SetVisibility(false);
		}
		return Existing;
	}
	// From the MetaHuman's component space to this body's: the MetaHuman's head onto this one, scaled by the heads'
	// neck-to-head lengths (within 15%), turned by the yaw cvar, then the offset; the groom then rides on the head bone.
	const float Fitted = FaceNeck > 1.0f && BodyNeck > 1.0f ? FMath::Clamp(BodyNeck / FaceNeck, 0.85f, 1.15f) : 1.0f;
	const float Scale = Fitted * FMath::Clamp(GStrandHairScale.GetValueOnGameThread(), 0.5f, 2.0f);
	const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(GStrandHairYaw.GetValueOnGameThread()));
	const FVector Offset(GStrandHairOffsetX.GetValueOnGameThread(), GStrandHairOffsetY.GetValueOnGameThread(), GStrandHairOffsetZ.GetValueOnGameThread());
	const FTransform ToFaceHead(FQuat::Identity, -FaceHead.GetLocation());
	const FTransform Fit(Yaw, BodyHead.GetLocation() + Offset, FVector(Scale));
	const FTransform InBody = ToFaceHead * Fit;                      // groom space -> body component space
	const FTransform OnHead = InBody * BodyHead.Inverse();           // -> the head bone's space

	UGroomComponent* Hair = Existing;
	if (!Hair)
	{
		Hair = NewObject<UGroomComponent>(Body->GetOwner(), TEXT("StrandHair"));
		Hair->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Hair->SetCanEverAffectNavigation(false);
		Hair->SetupAttachment(Body, TEXT("head"));
		Hair->RegisterComponent();
	}
	Hair->SetRelativeTransform(OnHead);  // every call, so the tuning cvars show at once
	Hair->SetVisibility(true);
	if (Hair->GroomAsset != Groom)
	{
		Hair->SetGroomAsset(Groom);
		if (UPhysicsAsset* Physics = LoadObject<UPhysicsAsset>(nullptr, StrandHairPhysicsAssetPath))
		{
			Hair->PhysicsAsset = Physics;
		}
		Hair->AddCollisionComponent(Body);
		UE_LOG(LogTemp, Display, TEXT("STRANDHAIR %s on %s: fitted scale %.3f (neck to head %.1f / %.1f cm), body head at %s, face head at %s"),
			*Groom->GetName(), *BodyMesh->GetName(), Scale, BodyNeck, FaceNeck, *BodyHead.GetLocation().ToString(), *FaceHead.GetLocation().ToString());
	}
	return Hair;
}
