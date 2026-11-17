#include "SoftContactSubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"

namespace
{
	TAutoConsoleVariable<float> GSoftContactFar(TEXT("Pawns.SoftContactFar"), 600.0f,
		TEXT("Skin squish on contact runs on MakeHuman bodies within this many cm of the camera (the nearest 8)."), ECVF_Default);
	TAutoConsoleVariable<int32> GSoftContact(TEXT("Pawns.SoftContact"), 0,
		TEXT("1 lets what touches a MakeHuman body push its skin in (docs/design/soft-body.md), 0 off (the default until it looks right;\n"
			"the creator's soft-body demo turns it on)."), ECVF_Default);

	// A body's capsules (cm): which bone to which, and how thick. Hands and head are spheres (the same bone twice).
	struct FSoftCapsuleBone
	{
		const TCHAR* From;
		const TCHAR* To;
		float Radius;
	};
	const FSoftCapsuleBone SoftCapsuleBones[] = {
		{TEXT("hand_l"), TEXT("hand_l"), 5.0f},
		{TEXT("hand_r"), TEXT("hand_r"), 5.0f},
		{TEXT("lowerarm_l"), TEXT("hand_l"), 4.5f},
		{TEXT("lowerarm_r"), TEXT("hand_r"), 4.5f},
		{TEXT("upperarm_l"), TEXT("lowerarm_l"), 5.5f},
		{TEXT("upperarm_r"), TEXT("lowerarm_r"), 5.5f},
		{TEXT("thigh_l"), TEXT("calf_l"), 8.0f},
		{TEXT("thigh_r"), TEXT("calf_r"), 8.0f},
		{TEXT("calf_l"), TEXT("foot_l"), 6.0f},
		{TEXT("calf_r"), TEXT("foot_r"), 6.0f},
		{TEXT("head"), TEXT("head"), 10.0f},
		{TEXT("spine_01"), TEXT("spine_03"), 13.0f},
	};

	// How near two capsules come (surface to surface; negative when they overlap).
	float SoftContactGap(const FVector& A1, const FVector& B1, float R1, const FVector& A2, const FVector& B2, float R2)
	{
		FVector P1, P2;
		FMath::SegmentDistToSegmentSafe(A1, B1, A2, B2, P1, P2);
		return static_cast<float>(FVector::Dist(P1, P2)) - R1 - R2;
	}
}

void USoftContactSubsystem::SetProbe(const FVector& A, const FVector& B, float Radius)
{
	Probe.A = A;
	Probe.B = B;
	Probe.Radius = Radius;
}

void USoftContactSubsystem::SetHandPresser(const FVector& A, const FVector& B, float Radius, const USkeletalMeshComponent* Owner)
{
	HandPresser.A = A;
	HandPresser.B = B;
	HandPresser.Radius = Radius;
	HandPresser.Owner = Owner;
}

void USoftContactSubsystem::AddBodyCapsules(const USkeletalMeshComponent* Mesh, TArray<FCapsule>& Out) const
{
	for (const FSoftCapsuleBone& Bone : SoftCapsuleBones)
	{
		if (Mesh->GetBoneIndex(Bone.From) == INDEX_NONE || Mesh->GetBoneIndex(Bone.To) == INDEX_NONE)
		{
			continue;
		}
		FCapsule& C = Out.AddDefaulted_GetRef();
		C.A = Mesh->GetSocketLocation(Bone.From);
		C.B = Mesh->GetSocketLocation(Bone.To);
		C.Radius = Bone.Radius;
		C.Owner = Mesh;
	}
}

void USoftContactSubsystem::AddAttachedCapsules(const USkeletalMeshComponent* Mesh, TArray<FCapsule>& Out) const
{
	// Weapons, shields and props held or worn (static meshes attached to the body or on its actor): a capsule along the
	// longest side of each one's box, as thick as the thicker of the other two sides.
	TArray<USceneComponent*> Children;
	Mesh->GetChildrenComponents(true, Children);
	if (const AActor* Owner = Mesh->GetOwner())
	{
		TArray<UStaticMeshComponent*> OnActor;
		Owner->GetComponents(OnActor);
		for (UStaticMeshComponent* Static : OnActor)
		{
			Children.AddUnique(Static);
		}
	}
	for (USceneComponent* Child : Children)
	{
		const UStaticMeshComponent* Static = Cast<UStaticMeshComponent>(Child);
		if (!Static || !Static->IsVisible() || !Static->GetStaticMesh())
		{
			continue;
		}
		const FBoxSphereBounds Local = Static->GetStaticMesh()->GetBounds();
		const FVector Extent = Local.BoxExtent * Static->GetComponentScale().GetAbs();
		if (Extent.GetMax() < 2.0 || Extent.GetMax() > 150.0)
		{
			continue;  // specks and big scenery
		}
		const int32 Axis = Extent.X >= Extent.Y && Extent.X >= Extent.Z ? 0 : (Extent.Y >= Extent.Z ? 1 : 2);
		const float Half = static_cast<float>(Extent[Axis]);
		const float Thick = FMath::Clamp(static_cast<float>(FMath::Max(Extent[(Axis + 1) % 3], Extent[(Axis + 2) % 3])), 1.5f, 25.0f);
		FVector Along = FVector::ZeroVector;
		Along[Axis] = Local.BoxExtent[Axis] * FMath::Max(0.0f, 1.0f - Thick / FMath::Max(Half, 0.1f));
		const FTransform& T = Static->GetComponentTransform();
		FCapsule& C = Out.AddDefaulted_GetRef();
		C.A = T.TransformPosition(Local.Origin - Along);
		C.B = T.TransformPosition(Local.Origin + Along);
		C.Radius = Thick;
		C.Owner = Mesh;  // what a body holds doesn't squash that body (a hilt in the hand)
	}
}

void USoftContactSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || bTableMissing)
	{
		return;
	}
	if (!Table)
	{
		Table = LoadObject<UTexture2D>(nullptr, SoftContactTexturePath);
		if (!Table)
		{
			bTableMissing = true;
			UE_LOG(LogTemp, Display, TEXT("Soft contact: no %s (run the MakeHumanArt commandlet); squish off"), SoftContactTexturePath);
			return;
		}
	}
	if (!Table->GetResource())
	{
		return;
	}
	const USimPawnSubsystem* Pawns = World->GetSubsystem<USimPawnSubsystem>();
	// The soft bodies near the camera, nearest first.
	const bool bView = !World->ViewLocationsRenderedLastFrame.IsEmpty();
	const FVector View = bView ? World->ViewLocationsRenderedLastFrame[0] : FVector::ZeroVector;
	struct FSoftNear
	{
		USkeletalMeshComponent* Mesh;
		float Distance;
	};
	TArray<FSoftNear> Near;
	TArray<USkeletalMeshComponent*> AllBodies;
	if (Pawns && (GSoftContact.GetValueOnGameThread() != 0 || bForcedOn))
	{
		for (const auto& Pair : Pawns->GetPawns())
		{
			USkeletalMeshComponent* Mesh = Pair.Value ? Pair.Value->GetMakeHumanMesh() : nullptr;
			if (!Mesh || !Mesh->IsVisible())
			{
				continue;
			}
			AllBodies.Add(Mesh);
			const float Distance = bView ? static_cast<float>(FVector::Dist(View, Mesh->GetComponentLocation())) : 0.0f;
			const UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance());
			if (Distance <= GSoftContactFar.GetValueOnGameThread() && (!Anim || Anim->IsSoftTissueEnabled()))
			{
				Near.Add({Mesh, Distance});
			}
		}
	}
	Near.Sort([](const FSoftNear& L, const FSoftNear& R) { return L.Distance < R.Distance; });
	if (Near.Num() > SoftContactRows)
	{
		Near.SetNum(SoftContactRows);
	}
	// Everything that can press: every body's capsules (near or not), what they hold, and the probe.
	TArray<FCapsule> Pressers;
	for (USkeletalMeshComponent* Mesh : AllBodies)
	{
		AddBodyCapsules(Mesh, Pressers);
		AddAttachedCapsules(Mesh, Pressers);
	}
	if (Probe.Radius > 0.0f)
	{
		Pressers.Add(Probe);
	}
	if (HandPresser.Radius > 0.0f)
	{
		Pressers.Insert(HandPresser, 0);  // first, so a crowd of other capsules never pushes the palm out of a full row
	}
	// Fill the table: for each near body, the capsules of others that come within 3 cm of its flesh (its own capsules are
	// thin cores, so allow 8 cm of flesh over them).
	TArray<FLinearColor>* Texels = new TArray<FLinearColor>();
	Texels->SetNumZeroed(SoftContactRows * SoftContactCapsules * 2);
	TArray<FCapsule> Own;
	int32 Contacts = 0;
	TArray<TWeakObjectPtr<USkeletalMeshComponent>> Rows;
	for (int32 Row = 0; Row < Near.Num(); ++Row)
	{
		USkeletalMeshComponent* Mesh = Near[Row].Mesh;
		Rows.Add(Mesh);
		Own.Reset();
		AddBodyCapsules(Mesh, Own);
		int32 Used = 0;
		for (const FCapsule& P : Pressers)
		{
			if (Used >= SoftContactCapsules)
			{
				break;
			}
			if (P.Owner.Get() == Mesh)
			{
				continue;
			}
			bool bClose = false;
			for (const FCapsule& O : Own)
			{
				if (SoftContactGap(P.A, P.B, P.Radius, O.A, O.B, O.Radius + 8.0f) < 3.0f)
				{
					bClose = true;
					break;
				}
			}
			if (!bClose)
			{
				continue;
			}
			const int32 Texel = (Row * SoftContactCapsules + Used) * 2;
			(*Texels)[Texel] = FLinearColor(static_cast<float>(P.A.X), static_cast<float>(P.A.Y), static_cast<float>(P.A.Z), P.Radius);
			(*Texels)[Texel + 1] = FLinearColor(static_cast<float>(P.B.X), static_cast<float>(P.B.Y), static_cast<float>(P.B.Z), 1.0f);
			++Used;
		}
		Contacts += Used;
		MostContacts = FMath::Max(MostContacts, Used);
		Mesh->SetCustomPrimitiveDataFloat(SoftContactRowIndex, static_cast<float>(Row));
	}
	// Bodies that lost their row go back to none.
	for (const TWeakObjectPtr<USkeletalMeshComponent>& Old : LastRows)
	{
		USkeletalMeshComponent* Mesh = Old.Get();
		if (Mesh && !Rows.Contains(Old))
		{
			Mesh->SetCustomPrimitiveDataFloat(SoftContactRowIndex, -1.0f);
		}
	}
	LastRows = MoveTemp(Rows);
	BodiesLastFrame = Near.Num();
	ContactsLastFrame = Contacts;
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, SoftContactCapsules * 2, SoftContactRows);
	Table->UpdateTextureRegions(0, 1, Region, SoftContactCapsules * 2 * sizeof(FLinearColor), sizeof(FLinearColor),
		reinterpret_cast<uint8*>(Texels->GetData()), [Texels](uint8*, const FUpdateTextureRegion2D* Regions)
		{
			delete Texels;
			delete Regions;
		});
}
