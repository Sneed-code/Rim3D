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
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TAutoConsoleVariable<float> GStrandHairScale(TEXT("Pawns.StrandHairScale"), 1.0f,
		TEXT("Strand hair on MakeHuman bodies: size times the fitted size."));
	TAutoConsoleVariable<float> GStrandHairOffsetX(TEXT("Pawns.StrandHairOffsetX"), 0.0f, TEXT("Strand hair: cm along the body's X."));
	TAutoConsoleVariable<float> GStrandHairOffsetY(TEXT("Pawns.StrandHairOffsetY"), 0.0f, TEXT("Strand hair: cm along the body's Y."));
	TAutoConsoleVariable<float> GStrandHairOffsetZ(TEXT("Pawns.StrandHairOffsetZ"), 0.0f, TEXT("Strand hair: cm along the body's Z."));
	TAutoConsoleVariable<float> GStrandHairYaw(TEXT("Pawns.StrandHairYaw"), 0.0f,
		TEXT("Strand hair: degrees turned about the head (if the body faces another way than the MetaHuman meshes)."));

	TAutoConsoleVariable<int32> GStrandHairOn(TEXT("Pawns.StrandHair"), 1,
		TEXT("1: the people nearest the camera wear strand hair (our own grown grooms), the rest their wig meshes. 0: wigs for everyone."));
	TAutoConsoleVariable<float> GStrandHairNear(TEXT("Pawns.StrandHairNear"), 10.0f,
		TEXT("Strand hair: metres from the camera within which people wear strands (a metre's grace before they switch back)."));
	TAutoConsoleVariable<int32> GStrandHairMax(TEXT("Pawns.StrandHairMax"), 6,
		TEXT("Strand hair: at most this many people wear strands at once, the nearest first."));

	TAutoConsoleVariable<float> GStrandHairMetaHumanFit(TEXT("Pawns.StrandHairMetaHumanFit"), 0.85f,
		TEXT("Strand hair: size of a MetaHuman groom (-CreatorGroom) on our heads, times the MetaHuman's."));

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

namespace
{
	/** Puts Groom on Body's head: the groom's curves are in a space where the head they were made on is at SourceHead
	 *  (component space, cm) with a neck-to-head length of SourceNeck; they are moved onto Body's head bone, scaled by
	 *  the two lengths (within 15%), turned by the yaw cvar, then the offset cvars. */
	UGroomComponent* StrandHairFit(USkeletalMeshComponent* Body, UGroomAsset* Groom, const FVector& SourceHead, float SourceNeck, bool bFound,
		UGroomComponent* Existing, float FitScale = 1.0f, const FVector& FitOffset = FVector::ZeroVector)
	{
		const USkeletalMesh* BodyMesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
		FTransform BodyHead;
		float BodyNeck = 0.0f;
		if (!BodyMesh || !Groom || !bFound || !StrandHairHead(*BodyMesh, BodyHead, BodyNeck))
		{
			if (Existing)
			{
				Existing->SetVisibility(false);
			}
			return Existing;
		}
		const float Fitted = SourceNeck > 1.0f && BodyNeck > 1.0f ? FMath::Clamp(BodyNeck / SourceNeck, 0.85f, 1.15f) : 1.0f;
		const float Scale = Fitted * FitScale * FMath::Clamp(GStrandHairScale.GetValueOnGameThread(), 0.5f, 2.0f);
		const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(GStrandHairYaw.GetValueOnGameThread()));
		const FVector Offset(GStrandHairOffsetX.GetValueOnGameThread(), GStrandHairOffsetY.GetValueOnGameThread(), GStrandHairOffsetZ.GetValueOnGameThread());
		const FTransform ToSourceHead(FQuat::Identity, -SourceHead);
		const FTransform Fit(Yaw, BodyHead.GetLocation() + Offset + FitOffset, FVector(Scale));
		const FTransform InBody = ToSourceHead * Fit;                    // groom space -> body component space
		const FTransform OnHead = InBody * BodyHead.Inverse();           // -> the head bone's space

		UGroomComponent* Hair = Existing;
		if (!Hair && Body->GetOwner())
		{
			// The one this body already wears (callers that re-apply every frame must not stack up new grooms: the
			// creator's apply runs often, and a new groom each time took 4 people from 30 to 5 fps).
			TArray<UGroomComponent*> Worn;
			Body->GetOwner()->GetComponents(Worn);
			for (UGroomComponent* Candidate : Worn)
			{
				if (!Hair && Candidate->GetAttachParent() == Body && Candidate->GetName().StartsWith(TEXT("StrandHair")))
				{
					Hair = Candidate;
				}
			}
		}
		if (!Hair)
		{
			UObject* Outer = Body->GetOwner() ? static_cast<UObject*>(Body->GetOwner()) : static_cast<UObject*>(Body);
			Hair = NewObject<UGroomComponent>(Outer, MakeUniqueObjectName(Outer, UGroomComponent::StaticClass(), TEXT("StrandHair")));
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
			UE_LOG(LogTemp, Display, TEXT("STRANDHAIR %s on %s: fitted scale %.3f (neck to head %.1f / %.1f cm, fit %.3f, moved %s), body head at %s, groom head at %s"),
				*Groom->GetName(), *BodyMesh->GetName(), Scale, BodyNeck, SourceNeck, FitScale, *FitOffset.ToString(), *BodyHead.GetLocation().ToString(),
				*SourceHead.ToString());
		}
		return Hair;
	}

	/** assets/hair_grooms.json "creator": the creator's hair style -> our grown style. */
	const TMap<FString, FString>& StrandHairCreatorStyles()
	{
		static TMap<FString, FString> Styles;
		static bool bRead = false;
		if (!bRead)
		{
			bRead = true;
			FString Text;
			TSharedPtr<FJsonObject> Root;
			const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/hair_grooms.json"));
			if (FFileHelper::LoadFileToString(Text, *File) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid())
			{
				const TSharedPtr<FJsonObject>* Creator = nullptr;
				if (Root->TryGetObjectField(TEXT("creator"), Creator))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Creator)->Values)
					{
						Styles.Add(Entry.Key, Entry.Value->AsString());
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Strand hair: cannot read %s; everyone keeps their wig"), *File);
			}
		}
		return Styles;
	}

	/** grow_hair.py's grooms.json for a base: the skull centre from the head bone and each slider's change to the skull
	 *  (share it grows, centre's move in cm), in the body's component axes. */
	struct FStrandHairHeadFit
	{
		FVector SkullFromHead = FVector::ZeroVector;
		TMap<FName, FVector4f> Morphs;
	};

	const FStrandHairHeadFit& StrandHairHeadFit(const FString& Base)
	{
		static TMap<FString, FStrandHairHeadFit> Fits;
		if (const FStrandHairHeadFit* Known = Fits.Find(Base))
		{
			return *Known;
		}
		FStrandHairHeadFit& HeadFit = Fits.Add(Base);
		FString Text;
		TSharedPtr<FJsonObject> Root;
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Grooms") / Base / TEXT("grooms.json"));
		if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("Strand hair: no %s, so the hair does not follow head sliders"), *File);
			return HeadFit;
		}
		const TArray<TSharedPtr<FJsonValue>>* Skull = nullptr;
		if (Root->TryGetArrayField(TEXT("skull_from_head_cm"), Skull) && Skull->Num() == 3)
		{
			HeadFit.SkullFromHead = FVector((*Skull)[0]->AsNumber(), (*Skull)[1]->AsNumber(), (*Skull)[2]->AsNumber());
		}
		const TSharedPtr<FJsonObject>* Morphs = nullptr;
		if (Root->TryGetObjectField(TEXT("head_morphs"), Morphs))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Morphs)->Values)
			{
				const TArray<TSharedPtr<FJsonValue>>& V = Entry.Value->AsArray();
				if (V.Num() == 4)
				{
					HeadFit.Morphs.Add(FName(*Entry.Key), FVector4f(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber(), V[3]->AsNumber()));
				}
			}
		}
		UE_LOG(LogTemp, Display, TEXT("Strand hair: %s head fit, skull %s from the head bone, %d head sliders"), *Base, *HeadFit.SkullFromHead.ToString(),
			HeadFit.Morphs.Num());
		return HeadFit;
	}
}

UGroomComponent* StrandHairAttach(USkeletalMeshComponent* Body, UGroomAsset* Groom, UGroomComponent* Existing)
{
	const USkeletalMesh* Face = StrandHairMetaHumanFace();
	FTransform FaceHead;
	float FaceNeck = 0.0f;
	const bool bFound = Face && StrandHairHead(*Face, FaceHead, FaceNeck);
	// Not sized by the necks: our bodies have longer necks but smaller heads than the MetaHumans, so that blew the
	// hair up 15% when it should shrink (sneed, 2026-11-21: "made to fit a head 35% bigger"). A set fit instead.
	return StrandHairFit(Body, Groom, FaceHead.GetLocation(), 0.0f, bFound, Existing, FMath::Clamp(GStrandHairMetaHumanFit.GetValueOnGameThread(), 0.5f, 1.5f));
}

UGroomAsset* StrandHairFindGrown(const FString& Base, const FString& CreatorStyle, bool bUnderHat)
{
	const FString* Style = StrandHairCreatorStyles().Find(CreatorStyle);
	if (!Style || Base.IsEmpty())
	{
		return nullptr;
	}
	if (bUnderHat)
	{
		const FString HatName = StrandHairGrownName(Base, *Style + TEXT("Hat"));
		if (UGroomAsset* Pressed = LoadObject<UGroomAsset>(nullptr, *(FString(StrandHairGrownFolder) / HatName + TEXT(".") + HatName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Pressed;
		}
		// No under-a-hat version (Buzz needs none): the plain one.
	}
	const FString Name = StrandHairGrownName(Base, *Style);
	const FString Path = FString(StrandHairGrownFolder) / Name + TEXT(".") + Name;
	return LoadObject<UGroomAsset>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
}

UGroomComponent* StrandHairAttachGrown(USkeletalMeshComponent* Body, UGroomAsset* Groom, const USkeletalMesh* Base, UGroomComponent* Existing)
{
	// The curves are relative to the base's head bone (grow_hair.py), so the source head is at the origin.
	FTransform BaseHead;
	float BaseNeck = 0.0f;
	const bool bFound = !Base || StrandHairHead(*Base, BaseHead, BaseNeck);
	// This person's head sliders: how much each grows the skull and moves its centre (measured by grow_hair.py on the
	// base's own morphs, grooms.json); the hair grows with the skull about its centre.
	float Grow = 0.0f;
	FVector Move = FVector::ZeroVector;
	FVector Skull = FVector::ZeroVector;
	if (Base && Body)
	{
		FString BaseName = Base->GetName();
		BaseName.RemoveFromStart(TEXT("SK_MH_"));
		const FStrandHairHeadFit& HeadFit = StrandHairHeadFit(BaseName);
		Skull = HeadFit.SkullFromHead;
		for (const TPair<FName, FVector4f>& Morph : HeadFit.Morphs)
		{
			const float Weight = Body->GetMorphTarget(Morph.Key);
			Grow += Weight * Morph.Value.X;
			Move += Weight * FVector(Morph.Value.Y, Morph.Value.Z, Morph.Value.W);
		}
		Grow = FMath::Clamp(Grow, -0.3f, 0.3f);
	}
	return StrandHairFit(Body, Groom, FVector::ZeroVector, BaseNeck, bFound, Existing, 1.0f + Grow, Move - Skull * Grow);
}

void StrandHairSetColour(UGroomComponent* Groom, const FLinearColor& Hair)
{
	if (!Groom)
	{
		return;
	}
	// The MetaHuman hair material is coloured by melanin and redness (as ASimPawnActor does for the MetaHuman grooms).
	const float Lightness = Hair.GetLuminance();
	const float Melanin = FMath::Clamp(1.0f - FMath::Sqrt(Lightness) * 1.6f, 0.0f, 1.0f);
	const float Redness = FMath::Clamp((Hair.R - Hair.B) / FMath::Max(Hair.R, 0.01f), 0.0f, 1.0f) * 0.8f;
	Groom->SetScalarParameterValueOnMaterials(TEXT("HairMelanin"), Melanin);
	Groom->SetScalarParameterValueOnMaterials(TEXT("HairRedness"), Redness);
}

// ---- who wears strands ------------------------------------------------------------------------------------------------

void UStrandHairSubsystem::Want(USkeletalMeshComponent* Body, const FString& Base, const FString& CreatorStyle, const FLinearColor& Colour,
	const TArray<int32>& WigSections, bool bUnderHat)
{
	if (!Body)
	{
		return;
	}
	FWearer* Found = Wearers.FindByPredicate([Body](const FWearer& W) { return W.Body.Get() == Body; });
	UGroomAsset* Groom = CreatorStyle.IsEmpty() ? nullptr : StrandHairFindGrown(Base, CreatorStyle, bUnderHat);
	if (Found && Found->bStrands && Found->Groom.Get() != Groom)
	{
		Show(*Found, false);  // a new style: the old strands go, the new ones come at the next sort
	}
	if (!Groom)
	{
		if (Found)
		{
			if (UGroomComponent* Strands = Found->Strands.Get())
			{
				Strands->SetVisibility(false);
			}
			Wearers.RemoveAll([Body](const FWearer& W) { return W.Body.Get() == Body; });
		}
		return;
	}
	if (!Found)
	{
		Found = &Wearers.AddDefaulted_GetRef();
		Found->Body = Body;
	}
	// The creator's apply calls this often with nothing changed: then only the wig it just showed is hidden again.
	const bool bSame = Found->Groom.Get() == Groom && Found->Colour.Equals(Colour) && Found->WigSections == WigSections && Found->Base.IsValid();
	if (bSame)
	{
		if (Found->bStrands)
		{
			Show(*Found, true);  // cheap: same groom, so only the fit (head sliders may have moved) and the wig
		}
		return;
	}
	const bool bNewColour = !Found->Colour.Equals(Colour);
	Found->Groom = Groom;
	if (!Found->Base.IsValid() || !Found->Base->GetName().EndsWith(Base))
	{
		Found->Base = LoadObject<USkeletalMesh>(nullptr, *(FString(TEXT("/Game/Rim3D/Generated/MakeHuman/SK_MH_")) + Base + TEXT(".SK_MH_") + Base), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
	}
	Found->Colour = Colour;
	Found->WigSections = WigSections;
	if (Found->bStrands)
	{
		if (bNewColour)
		{
			StrandHairSetColour(Found->Strands.Get(), Colour);
		}
		Show(*Found, true);  // the wig the apply just showed goes again
	}
	SinceSort = 1.0f;  // sort at the next tick
}

int32 UStrandHairSubsystem::NumWearing() const
{
	int32 Count = 0;
	for (const FWearer& Wearer : Wearers)
	{
		Count += Wearer.bStrands ? 1 : 0;
	}
	return Count;
}

void UStrandHairSubsystem::Show(FWearer& Wearer, bool bStrands)
{
	USkeletalMeshComponent* Body = Wearer.Body.Get();
	if (!Body)
	{
		return;
	}
	if (bStrands)
	{
		UGroomComponent* Strands = StrandHairAttachGrown(Body, Wearer.Groom.Get(), Wearer.Base.Get(), Wearer.Strands.Get());
		Wearer.Strands = Strands;
		if (!Strands)
		{
			bStrands = false;
		}
		else if (!Wearer.bStrands)
		{
			StrandHairSetColour(Strands, Wearer.Colour);  // on coming into view; Want sets a changed colour
		}
	}
	else if (UGroomComponent* Strands = Wearer.Strands.Get())
	{
		Strands->SetVisibility(false);
	}
	for (const int32 Section : Wearer.WigSections)
	{
		Body->ShowMaterialSection(Section, 0, !bStrands, 0);
	}
	Wearer.bStrands = bStrands;
}

void UStrandHairSubsystem::Tick(float DeltaTime)
{
	Wearers.RemoveAll([](const FWearer& W) { return !W.Body.IsValid(); });
	SinceSort += DeltaTime;
	if (SinceSort < 0.25f || Wearers.IsEmpty())
	{
		return;
	}
	SinceSort = 0.0f;
	const bool bOn = GStrandHairOn.GetValueOnGameThread() != 0;
	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
	const FVector Eye = Camera ? Camera->GetCameraLocation() : FVector::ZeroVector;
	const float NearCm = FMath::Max(0.0f, GStrandHairNear.GetValueOnGameThread()) * 100.0f;
	const int32 MaxWearers = FMath::Max(0, GStrandHairMax.GetValueOnGameThread());
	// Nearest first; those wearing strands now keep them a metre further (no flicker at the edge).
	TArray<TPair<float, int32>> Order;
	for (int32 i = 0; i < Wearers.Num(); ++i)
	{
		const USkeletalMeshComponent* Body = Wearers[i].Body.Get();
		const bool bVisible = Body->IsVisible() && Body->GetOwner() && !Body->GetOwner()->IsHidden();
		const float Distance = static_cast<float>(FVector::Dist(Body->GetComponentLocation(), Eye)) - (Wearers[i].bStrands ? 100.0f : 0.0f);
		Order.Emplace(bVisible && bOn && Distance < NearCm ? Distance : TNumericLimits<float>::Max(), i);
	}
	Order.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
	for (int32 Rank = 0; Rank < Order.Num(); ++Rank)
	{
		FWearer& Wearer = Wearers[Order[Rank].Value];
		const bool bWant = Rank < MaxWearers && Order[Rank].Key < TNumericLimits<float>::Max();
		if (bWant != Wearer.bStrands)
		{
			Show(Wearer, bWant);
		}
	}
}

TStatId UStrandHairSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UStrandHairSubsystem, STATGROUP_Tickables);
}

bool UStrandHairSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}
