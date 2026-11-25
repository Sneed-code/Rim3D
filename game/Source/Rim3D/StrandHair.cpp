#include "StrandHair.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GroomAsset.h"
#include "GroomComponent.h"
#include "HAL/IConsoleManager.h"
#include "PawnFaces.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/SphereElem.h"
#include "PhysicsEngine/SphylElem.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "MakeHumanArt.h"
#include "PawnActor.h"
#include "ColonySim/Health.h"
#include "UObject/ObjectKey.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "AnimationRuntime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
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

	TAutoConsoleVariable<int32> GStrandHairCollision(TEXT("Pawns.StrandHairCollision"), 1,
		TEXT("What grown strand hair collides with: 1 shapes fitted inside the body's skin (grow_hair.py, grooms.json), grown out to ")
		TEXT("the worn clothes; 0 nothing; 2 the body's own physics asset (stands proud of the skin, so it throws the hair out: for comparison)."));
	TAutoConsoleVariable<int32> GStrandHairClothes(TEXT("Pawns.StrandHairClothes"), 1,
		TEXT("1: the hair's collision shapes under worn clothes grow out to the cloth (and its puff), so hair lies on the clothes ")
		TEXT("instead of through them. 0: they stay inside the skin (to compare)."));
	TAutoConsoleVariable<float> GStrandHairClothGap(TEXT("Pawns.StrandHairClothGap"), 0.3f,
		TEXT("Strand hair: cm past the cloth's surface that its collision shapes reach (hair strands follow the guide strands ")
		TEXT("that collide, give or take half a centimetre, so a little past the cloth keeps them all out of it)."));
	TAutoConsoleVariable<float> GStrandHairInertia(TEXT("Pawns.StrandHairInertia"), 0.3f,
		TEXT("0 to 1: the share of the body's starts and stops strand hair feels (Unreal's local simulation in the upper chest's frame, ")
		TEXT("its linear velocity scale; turning is felt in full). Walking and running start and stop within a tenth of a second, ")
		TEXT("a jolt of 6 to 7 g that flung long hair through the skull and back (sneed 2026-11-25); a head and neck soften a real ")
		TEXT("start. 1: the whole jolt, as in the world."));
	TAutoConsoleVariable<float> GStrandHairSwingHold(TEXT("Pawns.StrandHairSwingHold"), 350.0f,
		TEXT("cm/s: a hand moving faster than this within 45 cm of the head holds the strand hair at rest while it passes (the arm ")
		TEXT("shapes swept through it and threw it up, sneed 2026-11-23). 0: off."));
	TAutoConsoleVariable<float> GStrandHairTurnHold(TEXT("Pawns.StrandHairTurnHold"), 240.0f,
		TEXT("Degrees a second: while a strand-haired body turns faster than this (a turn round, a swing at someone) its hair is ")
		TEXT("held at its resting shape on the head, turning with the body, and swings freely again from there once the turn ends; ")
		TEXT("a frame longer than Pawns.StrandHairHitch does the same. Simulated through fast turns and hitches, long bare-skinned ")
		TEXT("hair passed into the back or blew into a frizzy cloud (PC 2026-11-22, #450); the resting shape lies clear of the body. ")
		TEXT("0: never held."));
	TAutoConsoleVariable<float> GStrandHairHitch(TEXT("Pawns.StrandHairHitch"), 0.1f,
		TEXT("Seconds: a frame longer than this holds strand hair at rest (as a fast turn does) instead of simulating one long step."));
	TAutoConsoleVariable<float> GStrandHairTurnHoldAfter(TEXT("Pawns.StrandHairTurnHoldAfter"), 0.08f,
		TEXT("Seconds the hair stays held after the body's turn slows below Pawns.StrandHairTurnHold (the drawn turn eases out)."));
	TAutoConsoleVariable<int32> GStrandHairBreasts(TEXT("Pawns.StrandHairBreasts"), 1,
		TEXT("1: a collision capsule in each breast, on its bone, sized to this person's bust (hair no longer through the chest)."));
	TAutoConsoleVariable<float> GStrandHairFullDetail(TEXT("Pawns.StrandHairFullDetail"), 8.0f,
		TEXT("Metres from the camera within which strand hair always draws every strand (its first LOD). Further off Unreal picks ")
		TEXT("the LOD by screen size, and its second LOD keeps only a third of the strands: in the colony creator and over the ")
		TEXT("shoulder the head was small enough on screen for that, so the scalp showed through while the Debug creator, closer ")
		TEXT("in, was fine (sneed 2026-11-25). The undercoat always draws all of its strands. 0: Unreal's pick everywhere."));

	TAutoConsoleVariable<int32> GStrandHairDebug(TEXT("Pawns.StrandHairDebug"), 0,
		TEXT("1: draw the shapes each strand-haired person's hair collides with (green inside the skin, yellow grown out to clothes)."));

	// Unreal's hair simulation reads at most 100 shapes from a physics asset (grow_hair.py keeps to 90).
	constexpr int32 StrandHairMaxShapes = 96;
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
	/** Pawns.StrandHairInertia: the groom is simulated in spine_03's frame, feeling that share of the chest's starts and
	 *  stops and all of its turning. Only the component's SimulationSetup is set (Unreal reads it whether or not the
	 *  component overrides the groom's settings): overriding them also replaces the groom's bend stiffness and collision
	 *  thickness with the component's defaults (thin, soft), which is what made the 2026-11-22 trial of local simulation
	 *  swing stiffly and sink. Set by text so it is checked in the log (a renamed engine field is a warning, not a broken build). */
	void StrandHairSetInertia(UGroomComponent& Hair)
	{
		FStructProperty* Property = FindFProperty<FStructProperty>(UGroomComponent::StaticClass(), TEXT("SimulationSettings"));
		FStructProperty* Setup = Property ? FindFProperty<FStructProperty>(Property->Struct, TEXT("SimulationSetup")) : nullptr;
		if (!Setup)
		{
			UE_LOG(LogTemp, Warning, TEXT("STRANDHAIR no SimulationSettings.SimulationSetup on the groom component: hair feels every start and stop"));
			return;
		}
		const float Inertia = FMath::Clamp(GStrandHairInertia.GetValueOnGameThread(), 0.0f, 1.0f);
		const FString Text = FString::Printf(TEXT("(bLocalSimulation=True,LinearVelocityScale=%.3f,AngularVelocityScale=1.000000,LocalBone=\"spine_03\")"), Inertia);
		void* Value = Setup->ContainerPtrToValuePtr<void>(Property->ContainerPtrToValuePtr<void>(&Hair));
		const TCHAR* End = Setup->ImportText_Direct(*Text, Value, &Hair, PPF_None);
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			FString Applied;
			Setup->ExportTextItem_Direct(Applied, Value, nullptr, &Hair, PPF_None);
			UE_LOG(LogTemp, Display, TEXT("STRANDHAIR inertia %.2f: simulation setup now %s%s"), Inertia, *Applied, End ? TEXT("") : TEXT(" (NOT taken)"));
		}
	}

	/** Whether Body's pawn has lost its head (body-health.md part F): no strands then. */
	bool StrandHairHeadGone(const USkeletalMeshComponent& Body)
	{
		const ASimPawnActor* Pawn = Cast<ASimPawnActor>(Body.GetOwner());
		return Pawn && (Pawn->GetSeveredShown() & colony::kSeveredHead) != 0;
	}

	/** The undercoat grown with a style (grow_hair.py <Style>Under, GA_<base>_<Style>Under), or null: short strands lying
	 *  flat on the scalp, never simulated, worn under the style so that wherever its hair parts or swings the scalp reads
	 *  as hair (sneed 2026-11-25: "shows hairline at scalp ... consistent with any size / gender"). Looked up once each. */
	UGroomAsset* StrandHairUndercoatOf(const UGroomAsset& Groom)
	{
		static TMap<FString, TWeakObjectPtr<UGroomAsset>> Known;
		static TSet<FString> Missing;
		const FString Name = Groom.GetName() + TEXT("Under");
		if (Missing.Contains(Name))
		{
			return nullptr;
		}
		if (const TWeakObjectPtr<UGroomAsset>* Found = Known.Find(Name); Found && Found->IsValid())
		{
			return Found->Get();
		}
		UGroomAsset* Under = LoadObject<UGroomAsset>(nullptr, *(FString(StrandHairGrownFolder) / Name + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Under)
		{
			Known.Add(Name, Under);
		}
		else
		{
			Missing.Add(Name);
		}
		return Under;
	}

	/** Hair's undercoat, as a groom riding on it (same place and size, shown and hidden with it), or none if its style has
	 *  none (a MetaHuman groom, the afro, buzz, the pressed under-a-hat styles). */
	void StrandHairWearUndercoat(UGroomComponent& Hair)
	{
		UGroomComponent* Under = nullptr;
		for (USceneComponent* Child : Hair.GetAttachChildren())
		{
			if (UGroomComponent* Groom = Cast<UGroomComponent>(Child); Groom && !Under)
			{
				Under = Groom;
			}
		}
		UGroomAsset* Wanted = Hair.GroomAsset ? StrandHairUndercoatOf(*Hair.GroomAsset) : nullptr;
		if (!Wanted)
		{
			if (Under && Under->GroomAsset)
			{
				Under->SetGroomAsset(nullptr);
				Under->SetVisibility(false);
			}
			return;
		}
		if (!Under)
		{
			UObject* Outer = Hair.GetOwner() ? static_cast<UObject*>(Hair.GetOwner()) : static_cast<UObject*>(&Hair);
			Under = NewObject<UGroomComponent>(Outer, MakeUniqueObjectName(Outer, UGroomComponent::StaticClass(), TEXT("StrandUnder")));
			Under->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Under->SetCanEverAffectNavigation(false);
			Under->SetupAttachment(&Hair);
			Under->RegisterComponent();
		}
		if (Under->GroomAsset != Wanted)
		{
			Under->SetGroomAsset(Wanted);
			// Every strand, at any distance: it is short and cheap, and it is what keeps the scalp from showing through
			// where the style's own strands are thinned out further off (sneed 2026-11-25).
			Under->SetForcedLOD(0);
		}
		Under->SetRelativeTransform(FTransform::Identity);
		Under->SetVisibility(Hair.IsVisible());
	}

	/** Puts Groom on Body's head: the groom's curves are in a space where the head they were made on is at SourceHead
	 *  (component space, cm) with a neck-to-head length of SourceNeck; they are moved onto Body's head bone, scaled by
	 *  the two lengths (within 15%), turned by the yaw cvar, then the offset cvars. */
	UGroomComponent* StrandHairFit(USkeletalMeshComponent* Body, UGroomAsset* Groom, const FVector& SourceHead, float SourceNeck, bool bFound,
		UGroomComponent* Existing, const FVector& FitScale = FVector::OneVector, const FVector& FitOffset = FVector::ZeroVector, UPhysicsAsset* Collision = nullptr)
	{
		const USkeletalMesh* BodyMesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
		FTransform BodyHead;
		float BodyNeck = 0.0f;
		if (!BodyMesh || !Groom || !bFound || !StrandHairHead(*BodyMesh, BodyHead, BodyNeck))
		{
			if (Existing)
			{
				Existing->SetVisibility(false, true);  // its undercoat too
			}
			return Existing;
		}
		const float Fitted = SourceNeck > 1.0f && BodyNeck > 1.0f ? FMath::Clamp(BodyNeck / SourceNeck, 0.85f, 1.15f) : 1.0f;
		const FVector Scale = FitScale * (Fitted * FMath::Clamp(GStrandHairScale.GetValueOnGameThread(), 0.5f, 2.0f));
		const FQuat Yaw(FVector::UpVector, FMath::DegreesToRadians(GStrandHairYaw.GetValueOnGameThread()));
		const FVector Offset(GStrandHairOffsetX.GetValueOnGameThread(), GStrandHairOffsetY.GetValueOnGameThread(), GStrandHairOffsetZ.GetValueOnGameThread());
		const FTransform ToSourceHead(FQuat::Identity, -SourceHead);
		const FTransform Fit(Yaw, BodyHead.GetLocation() + Offset + FitOffset, Scale);
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
		const bool bNewGroom = Hair->GroomAsset != Groom;
		if (bNewGroom)
		{
			Hair->SetGroomAsset(Groom);
		}
		// The strands collide with this one physics asset only. The body is not added as a collision component: that
		// brings in its own physics asset, whose shapes stand centimetres proud of the scalp and back, and the hair
		// grown lying on the skin started inside them and was thrown out into a puffball (PC, 2026-11-21).
		if (bNewGroom || Hair->PhysicsAsset != Collision)
		{
			StrandHairSetInertia(*Hair);
			Hair->ResetCollisionComponents();
			Hair->SetPhysicsAsset(Collision);
		}
		StrandHairWearUndercoat(*Hair);
		if (bNewGroom)
		{
			UE_LOG(LogTemp, Display, TEXT("STRANDHAIR %s on %s: fitted scale %s (neck to head %.1f / %.1f cm, fit %s, moved %s), body head at %s, groom head at %s, collides with %s"),
				*Groom->GetName(), *BodyMesh->GetName(), *Scale.ToString(), BodyNeck, SourceNeck, *FitScale.ToString(), *FitOffset.ToString(), *BodyHead.GetLocation().ToString(),
				*SourceHead.ToString(), Collision ? *Collision->GetName() : TEXT("nothing"));
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
	struct FStrandHairShape
	{
		FString Part;                         // head, neck, torso or a bone (clavicle_l, upperarm_l, lowerarm_r...)
		FVector A = FVector::ZeroVector;       // from the head bone, cm, the body's component axes (a sphere: A == B)
		FVector B = FVector::ZeroVector;
		float Radius = 0.0f;
		FVector Out = FVector::ZeroVector;     // outward normal of the skin it lies under (zero: never grown to clothes)
		bool bCapsule = false;
	};

	/** One slider's effect on the scalp (grow_hair.py head_axes): scale along each axis about the skull centre, the move
	 *  (cm), and what that leaves standing out past the hair at each scalp point (mm, along the skin's normal). */
	struct FStrandHairAxes
	{
		FVector Scale = FVector::ZeroVector;
		FVector Move = FVector::ZeroVector;
		TArray<float> LeftMm;
	};

	struct FStrandHairHeadFit
	{
		FVector SkullFromHead = FVector::ZeroVector;
		TMap<FName, FVector4f> Morphs;          // older grooms.json: the skull sphere's growth and move per slider
		TMap<FName, FStrandHairAxes> Axes;       // per axis, with leftovers (preferred when present)
		TArray<FVector> ScalpPoints;             // cm from the skull centre
		TArray<FVector> ScalpNormals;
		TArray<FStrandHairShape> Collision;
	};

	/** How a person's sliders move their hair: hair point p (cm from the head bone, the body's axes) goes to p * Scale + Offset. */
	struct FStrandHairHeadShape
	{
		FVector Scale = FVector::OneVector;
		FVector Offset = FVector::ZeroVector;
		float LiftMm = 0.0f;  // the largest leftover the lift covers, for the log
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
		const auto ReadVectors = [&Root](const TCHAR* Field, TArray<FVector>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Root->TryGetArrayField(Field, Values))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					const TArray<TSharedPtr<FJsonValue>>& V = Value->AsArray();
					Out.Add(V.Num() == 3 ? FVector(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber()) : FVector::ZeroVector);
				}
			}
		};
		ReadVectors(TEXT("scalp_points_cm"), HeadFit.ScalpPoints);
		ReadVectors(TEXT("scalp_normals"), HeadFit.ScalpNormals);
		const TSharedPtr<FJsonObject>* AxesObject = nullptr;
		const TSharedPtr<FJsonObject>* LeftObject = nullptr;
		Root->TryGetObjectField(TEXT("scalp_left_mm"), LeftObject);
		if (Root->TryGetObjectField(TEXT("head_axes"), AxesObject) && HeadFit.ScalpPoints.Num() == HeadFit.ScalpNormals.Num())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*AxesObject)->Values)
			{
				const TArray<TSharedPtr<FJsonValue>>& V = Entry.Value->AsArray();
				if (V.Num() != 6)
				{
					continue;
				}
				FStrandHairAxes& Axes = HeadFit.Axes.Add(FName(*Entry.Key));
				Axes.Scale = FVector(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber());
				Axes.Move = FVector(V[3]->AsNumber(), V[4]->AsNumber(), V[5]->AsNumber());
				const TArray<TSharedPtr<FJsonValue>>* Left = nullptr;
				if (LeftObject && (*LeftObject)->TryGetArrayField(Entry.Key, Left) && Left->Num() == HeadFit.ScalpPoints.Num())
				{
					for (const TSharedPtr<FJsonValue>& L : *Left)
					{
						Axes.LeftMm.Add(static_cast<float>(L->AsNumber()));
					}
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
		int32 Capsules = 0;
		if (Root->TryGetArrayField(TEXT("collision"), Shapes))
		{
			const auto ReadPoint = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
				{
					return false;
				}
				Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
				return true;
			};
			for (const TSharedPtr<FJsonValue>& Value : *Shapes)
			{
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				FVector Centre;
				if (Object.IsValid() && ReadPoint(Object, TEXT("centre_cm"), Centre))
				{
					FStrandHairShape& Shape = HeadFit.Collision.AddDefaulted_GetRef();
					Shape.Part = Object->GetStringField(TEXT("part"));
					Shape.A = Centre;
					Shape.B = Centre;
					Shape.Radius = static_cast<float>(Object->GetNumberField(TEXT("radius_cm")));
					Shape.bCapsule = ReadPoint(Object, TEXT("a_cm"), Shape.A) && ReadPoint(Object, TEXT("b_cm"), Shape.B);
					if (!Shape.bCapsule)
					{
						Shape.A = Shape.B = Centre;
					}
					Capsules += Shape.bCapsule ? 1 : 0;
					if (ReadPoint(Object, TEXT("out"), Shape.Out))
					{
						Shape.Out = Shape.Out.GetSafeNormal();
					}
				}
			}
		}
		UE_LOG(LogTemp, Display, TEXT("Strand hair: %s head fit, skull %s from the head bone, %d head sliders (%d per axis over %d scalp points), %d collision shapes (%d capsules)"),
			*Base, *HeadFit.SkullFromHead.ToString(), HeadFit.Morphs.Num(), HeadFit.Axes.Num(), HeadFit.ScalpPoints.Num(), HeadFit.Collision.Num(), Capsules);
		return HeadFit;
	}

	/** How this person's sliders move the hair (docs/design/hair-physics.md "Following the face and head sliders"): each
	 *  slider's scale per axis about the skull centre and move, added up by its morph weight, then a lift: the leftovers
	 *  (skin a slider moves that no scale or move follows, the brow slider's forehead) add up per scalp point, and the hair
	 *  grows about the skull centre until it clears the largest of them, less the hair's own 1 mm off the scalp. Older
	 *  grooms.json files (no head_axes): the skull sphere's even growth, as before. */
	FStrandHairHeadShape StrandHairHeadShapeOf(const USkeletalMeshComponent& Body, const FStrandHairHeadFit& HeadFit)
	{
		FStrandHairHeadShape Shape;
		const FVector C = HeadFit.SkullFromHead;
		if (HeadFit.Axes.IsEmpty())
		{
			float Grow = 0.0f;
			FVector Move = FVector::ZeroVector;
			for (const TPair<FName, FVector4f>& Morph : HeadFit.Morphs)
			{
				const float Weight = Body.GetMorphTarget(Morph.Key);
				Grow += Weight * Morph.Value.X;
				Move += Weight * FVector(Morph.Value.Y, Morph.Value.Z, Morph.Value.W);
			}
			Grow = FMath::Clamp(Grow, -0.3f, 0.3f);
			Shape.Scale = FVector(1.0f + Grow);
			Shape.Offset = Move - C * Grow;
			return Shape;
		}
		FVector S = FVector::ZeroVector;
		FVector T = FVector::ZeroVector;
		TArray<float> Left;
		Left.SetNumZeroed(HeadFit.ScalpPoints.Num());
		for (const TPair<FName, FStrandHairAxes>& Entry : HeadFit.Axes)
		{
			const float Weight = Body.GetMorphTarget(Entry.Key);
			if (FMath::Abs(Weight) < 1e-3f)
			{
				continue;
			}
			S += Entry.Value.Scale * Weight;
			T += Entry.Value.Move * Weight;
			for (int32 i = 0; i < Entry.Value.LeftMm.Num() && i < Left.Num(); ++i)
			{
				Left[i] += Entry.Value.LeftMm[i] * Weight;
			}
		}
		S = S.BoundToBox(FVector(-0.3), FVector(0.3));
		// The lift: grow about the skull centre by k so that each scalp point moves out (along its normal) by its leftover.
		constexpr float HairOffScalpMm = 1.0f;  // the hair lies at least 1.2 mm off the scalp, so a millimetre of skin hides under it
		float Lift = 0.0f;
		for (int32 i = 0; i < Left.Num(); ++i)
		{
			const float Out = Left[i] - HairOffScalpMm;
			const double Radius = HeadFit.ScalpPoints[i].Size();
			if (Out <= 0.0f || Radius < 1.0)
			{
				continue;
			}
			const double Facing = FMath::Max(0.3, FVector::DotProduct(HeadFit.ScalpNormals[i], HeadFit.ScalpPoints[i] / Radius));
			Lift = FMath::Max(Lift, static_cast<float>(Out * 0.1 / (Radius * Facing)));
			Shape.LiftMm = FMath::Max(Shape.LiftMm, Out);
		}
		Lift = FMath::Min(Lift, 0.12f);
		Shape.Scale = (FVector::OneVector + S) * (1.0f + Lift);
		// p' = C + (p - C) * Scale + T
		Shape.Offset = C - C * Shape.Scale + T;
		return Shape;
	}

	/** A garment the body wears (a MakeHuman garment following the body's pose) and how far its material pushes it out. */
	struct FStrandHairCloth
	{
		const USkeletalMesh* Mesh = nullptr;
		float PuffCm = 0.0f;
	};

	/** The garments Body wears now: skeletal meshes made by MakeHumanArt (SK_MHG_*) attached to it and following its pose. */
	TArray<FStrandHairCloth> StrandHairWornCloth(const USkeletalMeshComponent& Body)
	{
		TArray<FStrandHairCloth> Cloth;
		for (const USceneComponent* Child : Body.GetAttachChildren())
		{
			const USkeletalMeshComponent* Garment = Cast<USkeletalMeshComponent>(Child);
			const USkeletalMesh* Mesh = Garment ? Garment->GetSkeletalMeshAsset() : nullptr;
			if (!Mesh || !Garment->IsVisible() || Garment->LeaderPoseComponent.Get() != &Body || !Mesh->GetName().StartsWith(TEXT("SK_MHG_")))
			{
				continue;
			}
			const TArray<float>& Data = Garment->GetCustomPrimitiveData().Data;
			Cloth.Add({Mesh, Data.IsValidIndex(MakeHumanGarmentPuffIndex) ? FMath::Max(0.0f, Data[MakeHumanGarmentPuffIndex]) : 0.0f});
		}
		Cloth.Sort([](const FStrandHairCloth& A, const FStrandHairCloth& B) { return A.Mesh->GetFName().LexicalLess(B.Mesh->GetFName()); });
		return Cloth;
	}

	/** One number for an outfit (which garments, how puffed), so a body's collision is rebuilt only when it changes. */
	uint32 StrandHairClothKey(const TArray<FStrandHairCloth>& Cloth)
	{
		uint32 Key = 0x5eed0u + static_cast<uint32>(Cloth.Num());
		for (const FStrandHairCloth& Piece : Cloth)
		{
			Key = HashCombineFast(Key, HashCombineFast(GetTypeHash(Piece.Mesh), GetTypeHash(FMath::RoundToInt32(Piece.PuffCm * 10.0f))));
		}
		return Key;
	}

	/** A garment's vertices in the reference pose (component space, cm), or empty if the mesh keeps no copy to read. */
	const TArray<FVector3f>& StrandHairClothVertices(const USkeletalMesh& Mesh)
	{
		static TMap<FObjectKey, TArray<FVector3f>> Read;
		if (const TArray<FVector3f>* Known = Read.Find(FObjectKey(&Mesh)))
		{
			return *Known;
		}
		TArray<FVector3f>& Out = Read.Add(FObjectKey(&Mesh));
		const FSkeletalMeshRenderData* Data = Mesh.GetResourceForRendering();
		if (!Data || Data->LODRenderData.Num() == 0 || FPlatformProperties::RequiresCookedData())
		{
			return Out;
		}
		const FPositionVertexBuffer& Positions = Data->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer;
		if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData())
		{
			return Out;  // no CPU copy kept
		}
		Out.Reserve(Positions.GetNumVertices());
		for (uint32 V = 0; V < Positions.GetNumVertices(); ++V)
		{
			Out.Add(Positions.VertexPosition(V));
		}
		return Out;
	}

	/** How big a collision shape (segment A to B, radius cm) under the skin with outward normal Out must be to reach just
	 *  past the worn clothes over it: for each garment that covers it (at least 3 of its vertices within 6 cm of the
	 *  shape and within 50 degrees of Out), the distance from the shape's line to that garment's nearest vertex there,
	 *  plus its puff; never past that garment's nearest vertex anywhere round the shape (so it can't poke through a
	 *  close-fitting part of it). The outermost covering garment wins (hair lies on the top layer). At most 6 cm more. */
	float StrandHairClothRadius(const FVector& A, const FVector& B, float Radius, const FVector& Out, const TArray<FStrandHairCloth>& Cloth, float GapCm)
	{
		constexpr float ReachCm = 6.0f;
		constexpr float MaxGrowCm = 6.0f;
		const float ConeCos = FMath::Cos(FMath::DegreesToRadians(50.0f));
		const FBox Around = FBox(A.ComponentMin(B), A.ComponentMax(B)).ExpandBy(Radius + ReachCm);
		const FVector AB = B - A;
		const double AB2 = FMath::Max(AB.SizeSquared(), UE_SMALL_NUMBER);
		float Best = Radius;
		for (const FStrandHairCloth& Piece : Cloth)
		{
			const TArray<FVector3f>& Vertices = StrandHairClothVertices(*Piece.Mesh);
			float NearestAll = TNumericLimits<float>::Max();
			float NearestCone = TNumericLimits<float>::Max();
			int32 InCone = 0;
			for (const FVector3f& Vertex3f : Vertices)
			{
				const FVector Vertex(Vertex3f);
				if (!Around.IsInside(Vertex))
				{
					continue;
				}
				const FVector OnLine = A + AB * FMath::Clamp(FVector::DotProduct(Vertex - A, AB) / AB2, 0.0, 1.0);
				const FVector Away = Vertex - OnLine;
				const float Distance = static_cast<float>(Away.Size());
				if (Distance > Radius + ReachCm || Distance < UE_KINDA_SMALL_NUMBER)
				{
					continue;
				}
				NearestAll = FMath::Min(NearestAll, Distance);
				if (FVector::DotProduct(Away / Distance, Out) > ConeCos)
				{
					NearestCone = FMath::Min(NearestCone, Distance);
					++InCone;
				}
			}
			if (InCone >= 3)
			{
				Best = FMath::Max(Best, FMath::Min(FMath::Min(NearestCone, NearestAll) + Piece.PuffCm + GapCm, Radius + MaxGrowCm));
			}
		}
		return Best;
	}

	/** What grown hair collides with on a body of this mesh wearing these clothes: a physics asset of grow_hair.py's
	 *  shapes inside the skin (spheres and capsules), each on the bone of its part (head, neck_01, a named bone, or for
	 *  "torso" the spine bone nearest it), so the hair rests where it was grown and is only pushed back when it swings into
	 *  the body or the body moves into it. Shapes under worn clothes grow out to just past the cloth (sneed 2026-11-22:
	 *  hair through the shirt), so the hair lies on the clothes. Made once per mesh and outfit. */
	/** A capsule in each breast on its own bone (sneed 2026-11-23: "the hair still clips a bit of the chest"): grow_hair.py's
	 *  shapes are made on the base body, on the spine, so a larger bust than the base's (the breast sliders) or one
	 *  moving with its jiggle came out past them and the hair went in. Fitted from the mesh's own vertices round each
	 *  breast bone (from the bone towards the middle of the skin near it, to its tip, as thick as the breast's middle),
	 *  grown by this person's breast size (Scale), and out to the clothes like the other shapes. Returns shapes added. */
	int32 StrandHairAddBreasts(const USkeletalMesh& Mesh, UPhysicsAsset& Asset, TMap<int32, USkeletalBodySetup*>& Bodies, float Scale,
		bool bClothes, const TArray<FStrandHairCloth>& Cloth, float GapCm)
	{
		const TArray<FVector3f>& Vertices = StrandHairClothVertices(Mesh);
		const FReferenceSkeleton& Ref = Mesh.GetRefSkeleton();
		int32 Added = 0;
		for (const TCHAR* Name : {TEXT("breast_l"), TEXT("breast_r")})
		{
			const int32 Bone = Ref.FindBoneIndex(FName(Name));
			if (Bone == INDEX_NONE || Vertices.IsEmpty())
			{
				continue;
			}
			const FTransform BoneRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bone);
			const FVector Origin = BoneRef.GetLocation();
			FVector Sum = FVector::ZeroVector;
			int32 Near = 0;
			for (const FVector3f& V3 : Vertices)
			{
				const FVector V(V3);
				if (FVector::DistSquared(V, Origin) < FMath::Square(14.0))
				{
					Sum += V;
					++Near;
				}
			}
			if (Near < 20)
			{
				continue;
			}
			const FVector Axis = (Sum / Near - Origin).GetSafeNormal();
			if (Axis.IsNearlyZero())
			{
				continue;
			}
			// The tip: the furthest skin along the axis close to it; the thickness: the skin round the axis a third of the way out.
			double Tip = 0.0;
			for (const FVector3f& V3 : Vertices)
			{
				const FVector D = FVector(V3) - Origin;
				const double Along = FVector::DotProduct(D, Axis);
				if (Along > 0.0 && Along < 20.0 && (D - Axis * Along).SizeSquared() < FMath::Square(5.0))
				{
					Tip = FMath::Max(Tip, Along);
				}
			}
			TArray<float> Round;
			for (const FVector3f& V3 : Vertices)
			{
				const FVector D = FVector(V3) - Origin;
				const double Along = FVector::DotProduct(D, Axis);
				const double Off = (D - Axis * Along).Size();
				if (Along > Tip * 0.25 && Along < Tip * 0.6 && Off < 12.0)
				{
					Round.Add(static_cast<float>(Off));
				}
			}
			if (Tip < 2.0 || Round.Num() < 8)
			{
				continue;
			}
			Round.Sort();
			// Its barrier 2 mm out of the skin, like the body's other shapes (grow_hair.py BODY_MARGIN reaches further, but
			// a breast moves with its jiggle), less the guides' own thickness below.
			const float Radius = FMath::Clamp(Round[Round.Num() / 5] * Scale + 0.2f, 1.5f, 10.0f);
			const double Length = FMath::Max(0.2, Tip * Scale - Radius + 0.2);
			const FVector A = Origin;
			const FVector B = Origin + Axis * Length;
			float Grown = Radius;
			if (bClothes)
			{
				Grown = StrandHairClothRadius(A, B, Radius, Axis, Cloth, GapCm);
			}
			USkeletalBodySetup*& Setup = Bodies.FindOrAdd(Bone);
			if (!Setup)
			{
				Setup = NewObject<USkeletalBodySetup>(&Asset);
				Setup->BoneName = Ref.GetBoneName(Bone);
				Setup->PhysicsType = PhysType_Kinematic;
				Asset.SkeletalBodySetups.Add(Setup);
			}
			const FVector LocalA = BoneRef.InverseTransformPosition(A);
			const FVector LocalB = BoneRef.InverseTransformPosition(B);
			FKSphylElem Elem(FMath::Max(0.5f, Grown - StrandHairGuideRadiusCm), static_cast<float>(FVector::Dist(LocalA, LocalB)));
			Elem.SetName(TEXT("Breast"));
			Elem.Center = (LocalA + LocalB) * 0.5;
			Elem.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, (LocalB - LocalA).GetSafeNormal()).Rotator();
			Setup->AggGeom.SphylElems.Add(Elem);
			++Added;
			UE_LOG(LogTemp, Display, TEXT("STRANDHAIR breast %s on %s: tip %.1f cm, radius %.1f cm (size x%.2f, %.1f with clothes)"), Name, *Mesh.GetName(),
				Tip, Radius, Scale, Grown);
		}
		return Added;
	}

	UPhysicsAsset* StrandHairFittedCollision(const USkeletalMesh& Mesh, const FStrandHairHeadFit& HeadFit, const TArray<FStrandHairCloth>& Cloth, float BreastScale,
		const FStrandHairHeadShape& HeadShape)
	{
		const float GapCm = FMath::Clamp(GStrandHairClothGap.GetValueOnGameThread(), -1.0f, 2.0f);
		const bool bClothes = GStrandHairClothes.GetValueOnGameThread() != 0 && !Cloth.IsEmpty();
		// The head's shapes follow the person's head sliders like the hair does: to the millimetre, so dragging a slider
		// makes a new asset only every millimetre or so.
		const FIntVector HeadScaleKey(FMath::RoundToInt32(HeadShape.Scale.X * 500.0), FMath::RoundToInt32(HeadShape.Scale.Y * 500.0), FMath::RoundToInt32(HeadShape.Scale.Z * 500.0));
		const FIntVector HeadMoveKey(FMath::RoundToInt32(HeadShape.Offset.X * 10.0), FMath::RoundToInt32(HeadShape.Offset.Y * 10.0), FMath::RoundToInt32(HeadShape.Offset.Z * 10.0));
		uint32 Key = HashCombineFast(HashCombineFast(GetTypeHash(&Mesh), GetTypeHash(FMath::RoundToInt32(BreastScale * 20.0f))),
			bClothes ? HashCombineFast(StrandHairClothKey(Cloth), GetTypeHash(FMath::RoundToInt32(GapCm * 100.0f))) : 0u);
		Key = HashCombineFast(Key, HashCombineFast(GetTypeHash(HeadScaleKey), GetTypeHash(HeadMoveKey)));
		static TMap<uint32, TWeakObjectPtr<UPhysicsAsset>> Made;
		if (const TWeakObjectPtr<UPhysicsAsset>* Known = Made.Find(Key))
		{
			if (Known->IsValid())
			{
				return Known->Get();
			}
		}
		FTransform Head;
		float NeckToHead = 0.0f;
		if (HeadFit.Collision.IsEmpty() || !StrandHairHead(Mesh, Head, NeckToHead))
		{
			return nullptr;
		}
		const FReferenceSkeleton& Ref = Mesh.GetRefSkeleton();
		TArray<int32> Spine;
		for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
		{
			if (Ref.GetBoneName(Bone).ToString().StartsWith(TEXT("spine")))
			{
				Spine.Add(Bone);
			}
		}
		int32 ClothRead = 0;
		for (const FStrandHairCloth& Piece : Cloth)
		{
			ClothRead += bClothes && !StrandHairClothVertices(*Piece.Mesh).IsEmpty() ? 1 : 0;
		}
		UPackage* Outer = GetTransientPackage();
		UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(Outer, MakeUniqueObjectName(Outer, UPhysicsAsset::StaticClass(), TEXT("PA_StrandHair")));
		TMap<int32, USkeletalBodySetup*> Bodies;
		int32 Placed = 0;
		int32 Grown = 0;
		float GrownCm = 0.0f;
		for (const FStrandHairShape& Shape : HeadFit.Collision)
		{
			if (Placed >= StrandHairMaxShapes)
			{
				UE_LOG(LogTemp, Warning, TEXT("STRANDHAIR collision for %s: more than %d shapes, the rest left out"), *Mesh.GetName(), StrandHairMaxShapes);
				break;
			}
			const bool bHeadShape = Shape.Part == TEXT("head");
			// The head's spheres move and shrink with the person's head sliders (a narrower head would otherwise leave
			// them standing out through the scalp, a wider one sunk deep under it).
			const FVector A = Head.GetLocation() + (bHeadShape ? Shape.A * HeadShape.Scale + HeadShape.Offset : Shape.A);
			const FVector B = Head.GetLocation() + (bHeadShape ? Shape.B * HeadShape.Scale + HeadShape.Offset : Shape.B);
			const FVector Centre = (A + B) * 0.5;
			// head, neck, torso, or a bone by name (clavicles and arms: sneed 2026-11-22, hair through the arms as they move);
			// a named bone the skeleton lacks falls back to the spine.
			int32 Bone = Shape.Part == TEXT("head") ? Ref.FindBoneIndex(TEXT("head"))
				: Shape.Part == TEXT("neck") ? Ref.FindBoneIndex(TEXT("neck_01"))
				: Shape.Part == TEXT("torso") ? INDEX_NONE : Ref.FindBoneIndex(FName(*Shape.Part));
			if (Bone == INDEX_NONE && Shape.Part != TEXT("head"))
			{
				double Nearest = TNumericLimits<double>::Max();
				for (const int32 Candidate : Spine)
				{
					const double Distance = FVector::DistSquared(FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Candidate).GetLocation(), Centre);
					if (Distance < Nearest)
					{
						Nearest = Distance;
						Bone = Candidate;
					}
				}
			}
			if (Bone == INDEX_NONE)
			{
				continue;
			}
			// grow_hair.py's radius puts the shape's surface on the barrier (just outside the skin); the groom adds each
			// guide's own thickness (StrandHairGuideRadiusCm), so the physics shape is that much smaller.
			const float Barrier = bHeadShape ? Shape.Radius * static_cast<float>(HeadShape.Scale.GetMin()) : Shape.Radius;
			float Radius = Barrier;
			if (bClothes && ClothRead > 0 && !bHeadShape && !Shape.Out.IsNearlyZero())
			{
				Radius = StrandHairClothRadius(A, B, Barrier, Shape.Out, Cloth, GapCm);
				if (Radius > Barrier + 0.05f)
				{
					++Grown;
					GrownCm += Radius - Barrier;
				}
			}
			const bool bCloth = Radius > Barrier + 0.05f;
			Radius = FMath::Max(0.2f, Radius - StrandHairGuideRadiusCm);
			USkeletalBodySetup*& Setup = Bodies.FindOrAdd(Bone);
			if (!Setup)
			{
				Setup = NewObject<USkeletalBodySetup>(Asset);
				Setup->BoneName = Ref.GetBoneName(Bone);
				Setup->PhysicsType = PhysType_Kinematic;
				Asset->SkeletalBodySetups.Add(Setup);
			}
			const FTransform BoneRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(Ref, Bone);
			if (Shape.bCapsule && FVector::Dist(A, B) > 0.1)
			{
				// A capsule's length runs along its own Z.
				const FVector LocalA = BoneRef.InverseTransformPosition(A);
				const FVector LocalB = BoneRef.InverseTransformPosition(B);
				FKSphylElem Elem(Radius, static_cast<float>(FVector::Dist(LocalA, LocalB)));
				Elem.SetName(bCloth ? TEXT("Cloth") : TEXT("Skin"));
				Elem.Center = (LocalA + LocalB) * 0.5;
				Elem.Rotation = FQuat::FindBetweenNormals(FVector::UpVector, (LocalB - LocalA).GetSafeNormal()).Rotator();
				Setup->AggGeom.SphylElems.Add(Elem);
			}
			else
			{
				FKSphereElem Elem(Radius);
				Elem.SetName(bCloth ? TEXT("Cloth") : TEXT("Skin"));
				Elem.Center = BoneRef.InverseTransformPosition(Centre);
				Setup->AggGeom.SphereElems.Add(Elem);
			}
			++Placed;
		}
		if (GStrandHairBreasts.GetValueOnGameThread() != 0)
		{
			Placed += StrandHairAddBreasts(Mesh, *Asset, Bodies, BreastScale, bClothes && ClothRead > 0, Cloth, GapCm);
		}
		Asset->UpdateBodySetupIndexMap();
		UE_LOG(LogTemp, Display, TEXT("STRANDHAIR collision for %s: %d shapes inside the skin on %d bones; %d garments worn (%d read), %d shapes grown out to the clothes by %.1f cm on average%s"),
			*Mesh.GetName(), Placed, Bodies.Num(), Cloth.Num(), ClothRead, Grown, Grown > 0 ? GrownCm / Grown : 0.0f,
			bClothes && ClothRead < Cloth.Num() ? TEXT(" (a garment keeps no copy of its vertices: the hair can pass into it)") : TEXT(""));
		Made.Add(Key, Asset);
		return Asset;
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
	UPhysicsAsset* Mannequin = GStrandHairCollision.GetValueOnGameThread() != 0 ? LoadObject<UPhysicsAsset>(nullptr, StrandHairPhysicsAssetPath) : nullptr;
	return StrandHairFit(Body, Groom, FaceHead.GetLocation(), 0.0f, bFound, Existing, FVector(FMath::Clamp(GStrandHairMetaHumanFit.GetValueOnGameThread(), 0.5f, 1.5f)),
		FVector::ZeroVector, Mannequin);
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
	// This person's head and face sliders move the hair (and the head's collision) with the scalp: per axis, measured by
	// grow_hair.py on the base's own morphs (grooms.json head_axes), with a lift for what no scale follows.
	FStrandHairHeadShape HeadShape;
	UPhysicsAsset* Collision = nullptr;
	const int32 CollisionMode = GStrandHairCollision.GetValueOnGameThread();
	if (CollisionMode == 2 && Body && Body->GetSkeletalMeshAsset())
	{
		Collision = Body->GetSkeletalMeshAsset()->GetPhysicsAsset();
	}
	if (Base && Body)
	{
		FString BaseName = Base->GetName();
		BaseName.RemoveFromStart(TEXT("SK_MH_"));
		const FStrandHairHeadFit& HeadFit = StrandHairHeadFit(BaseName);
		HeadShape = StrandHairHeadShapeOf(*Body, HeadFit);
		if (CollisionMode == 1 && Body->GetSkeletalMeshAsset())
		{
			// This person's bust against the base's: the breast sliders (BreastsLarge grows it, BreastsSmall shrinks it).
			const float BreastScale = FMath::Clamp(1.0f + 0.45f * Body->GetMorphTarget(TEXT("BreastsLarge")) - 0.3f * Body->GetMorphTarget(TEXT("BreastsSmall")), 0.6f, 1.6f);
			Collision = StrandHairFittedCollision(*Body->GetSkeletalMeshAsset(), HeadFit, StrandHairWornCloth(*Body), BreastScale, HeadShape);
		}
		static int32 Logged = 0;
		if (HeadShape.LiftMm > 0.0f && Logged < 20)
		{
			++Logged;
			UE_LOG(LogTemp, Display, TEXT("STRANDHAIR head sliders on %s: hair scale %s, moved %s cm, lifted for %.1f mm of skin no scale follows"),
				*GetNameSafe(Body->GetOwner()), *HeadShape.Scale.ToString(), *HeadShape.Offset.ToString(), HeadShape.LiftMm);
		}
	}
	return StrandHairFit(Body, Groom, FVector::ZeroVector, BaseNeck, bFound, Existing, HeadShape.Scale, HeadShape.Offset, Collision);
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
	for (USceneComponent* Child : Groom->GetAttachChildren())
	{
		if (UGroomComponent* Under = Cast<UGroomComponent>(Child))
		{
			Under->SetScalarParameterValueOnMaterials(TEXT("HairMelanin"), Melanin);  // the undercoat
			Under->SetScalarParameterValueOnMaterials(TEXT("HairRedness"), Redness);
		}
	}
}

// ---- who wears strands ------------------------------------------------------------------------------------------------

namespace
{
	/** Pawns.StrandHairDebug: the collision shapes on the body as it is posed now (one frame). Shapes grown out to the
	 *  clothes (named so by StrandHairFittedCollision) are yellow, the rest green. */
	void StrandHairDrawShapes(const USkeletalMeshComponent& Body, const UPhysicsAsset& Asset)
	{
		UWorld* World = Body.GetWorld();
		if (!World)
		{
			return;
		}
		for (const USkeletalBodySetup* Setup : Asset.SkeletalBodySetups)
		{
			const int32 Bone = Setup ? Body.GetBoneIndex(Setup->BoneName) : INDEX_NONE;
			if (Bone == INDEX_NONE)
			{
				continue;
			}
			const FTransform At = Body.GetBoneTransform(Bone);
			for (const FKSphereElem& Elem : Setup->AggGeom.SphereElems)
			{
				const FColor Colour = Elem.GetName() == TEXT("Cloth") ? FColor::Yellow : FColor::Green;
				DrawDebugSphere(World, At.TransformPosition(Elem.Center), Elem.Radius * At.GetMaximumAxisScale(), 10, Colour, false, -1.0f, 0, 0.15f);
			}
			for (const FKSphylElem& Elem : Setup->AggGeom.SphylElems)
			{
				const FColor Colour = Elem.GetName() == TEXT("Cloth") ? FColor::Yellow : FColor::Green;
				const FTransform Shape = FTransform(Elem.Rotation, Elem.Center) * At;
				DrawDebugCapsule(World, Shape.GetLocation(), Elem.Length * 0.5f + Elem.Radius, Elem.Radius, Shape.GetRotation(), Colour, false, -1.0f, 0, 0.15f);
			}
		}
	}
}

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
		// A new style: the old strands go, the new ones come at the next sort. Only the strands: the apply calling this has
		// just shown the new style's wig and hidden the rest, and hiding through Show(false) put the OLD style's wig back
		// on the head under the new strands (its sections are still the wearer's until below).
		if (UGroomComponent* Old = Found->Strands.Get())
		{
			Old->SetVisibility(false, true);
		}
		Found->bStrands = false;
	}
	if (!Groom)
	{
		if (Found)
		{
			if (UGroomComponent* Strands = Found->Strands.Get())
			{
				Strands->SetVisibility(false, true);
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

bool UStrandHairSubsystem::GetHiddenWig(const USkeletalMeshComponent* Body, TArray<int32>& OutSections) const
{
	OutSections.Reset();
	const FWearer* Found = Body ? Wearers.FindByPredicate([Body](const FWearer& W) { return W.Body.Get() == Body; }) : nullptr;
	if (!Found || !Found->bStrands)
	{
		return false;
	}
	OutSections = Found->WigSections;
	return true;
}

UGroomComponent* UStrandHairSubsystem::Release(const USkeletalMeshComponent* Body)
{
	FWearer* Found = Body ? Wearers.FindByPredicate([Body](const FWearer& W) { return W.Body.Get() == Body; }) : nullptr;
	UGroomComponent* Strands = Found ? Found->Strands.Get() : nullptr;
	if (!Found || !Strands)
	{
		return nullptr;
	}
	const bool bShowing = Found->bStrands;
	Found->Strands = nullptr;
	Found->bStrands = false;
	return bShowing || Strands->IsVisible() ? Strands : nullptr;
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
	// A body whose head is off grows no hair: the strands hung on its folded head bone and came back whenever the
	// wearers were sorted again (the head went with its piece, which wears the wig instead).
	if (bStrands && StrandHairHeadGone(*Body))
	{
		bStrands = false;
	}
	if (bStrands)
	{
		UGroomComponent* Strands = StrandHairAttachGrown(Body, Wearer.Groom.Get(), Wearer.Base.Get(), Wearer.Strands.Get());
		Wearer.Strands = Strands;
		Wearer.ClothKey = StrandHairClothKey(StrandHairWornCloth(*Body));  // the outfit its collision was made for
		if (!Strands)
		{
			bStrands = false;
		}
		else if (!Wearer.bStrands)
		{
			StrandHairSetColour(Strands, Wearer.Colour);  // on coming into view; Want sets a changed colour
			// From the shape it was grown in: a groom shown again (or a new style on the same component) kept whatever its
			// simulation last had, from another style or from before the body moved or turned while it was hidden.
			Strands->ResetSimulation();
		}
	}
	else if (UGroomComponent* Strands = Wearer.Strands.Get())
	{
		Strands->SetVisibility(false, true);
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
	// Fast turns: the hair is held at its resting shape (reset every frame) while the body turns faster than
	// Pawns.StrandHairTurnHold, so the back never turns through it; it swings freely again from rest once the turn ends.
	const float HoldRate = GStrandHairTurnHold.GetValueOnGameThread();
	for (FWearer& Wearer : Wearers)
	{
		const float Yaw = static_cast<float>(Wearer.Body->GetComponentRotation().Yaw);
		UGroomComponent* Strands = Wearer.Strands.Get();
		const bool bHitch = DeltaTime > FMath::Max(0.02f, GStrandHairHitch.GetValueOnGameThread());
		if (Wearer.bHaveYaw && DeltaTime > 0.0f && HoldRate > 0.0f &&
			(bHitch || FMath::Abs(FMath::FindDeltaAngleDegrees(Wearer.LastYaw, Yaw)) > HoldRate * DeltaTime))
		{
			if (Wearer.TurnHold <= 0.0f && ++TurnHolds <= 40)
			{
				UE_LOG(LogTemp, Display, TEXT("STRANDHAIR turn hold %d on %s: turning %.0f degrees a second, frame %.3f s"), TurnHolds,
					*GetNameSafe(Wearer.Body->GetOwner()), FMath::Abs(FMath::FindDeltaAngleDegrees(Wearer.LastYaw, Yaw)) / DeltaTime, DeltaTime);
			}
			Wearer.TurnHold = FMath::Max(0.0f, GStrandHairTurnHoldAfter.GetValueOnGameThread()) + DeltaTime;
		}
		Wearer.LastYaw = Yaw;
		Wearer.bHaveYaw = true;
		// A hand swung fast past the head (an overhead club or hammer blow): the arms' collision shapes swept through
		// the hair and threw it straight up (sneed 2026-11-23). Held at rest while it passes, like a fast turn.
		{
			USkeletalMeshComponent* Body = Wearer.Body.Get();
			const FVector Head = Body->GetSocketLocation(TEXT("head"));
			const FName HandBones[2] = {TEXT("hand_l"), TEXT("hand_r")};
			const float SwingSpeed = FMath::Max(50.0f, GStrandHairSwingHold.GetValueOnGameThread());
			bool bSweep = false;
			for (int32 H = 0; H < 2; ++H)
			{
				if (Body->GetBoneIndex(HandBones[H]) == INDEX_NONE)
				{
					continue;
				}
				const FVector Hand = Body->GetSocketLocation(HandBones[H]) - Head;
				if (Wearer.bHaveHands && DeltaTime > 0.0f && Hand.Size() < 45.0 && (Hand - Wearer.LastHands[H]).Size() / DeltaTime > SwingSpeed)
				{
					bSweep = true;
				}
				Wearer.LastHands[H] = Hand;
			}
			Wearer.bHaveHands = true;
			if (bSweep && GStrandHairSwingHold.GetValueOnGameThread() > 0.0f)
			{
				if (Wearer.TurnHold <= 0.0f)
				{
					++SwingHolds;
				}
				Wearer.TurnHold = FMath::Max(Wearer.TurnHold, 0.12f + DeltaTime);
			}
		}
		if (Wearer.TurnHold > 0.0f)
		{
			Wearer.TurnHold -= DeltaTime;
			if (Wearer.bStrands && Strands && Strands->IsVisible())
			{
				Strands->ResetSimulation();
				++TurnHoldFrames;
			}
		}
	}
	if (GStrandHairDebug.GetValueOnGameThread() != 0)
	{
		for (const FWearer& Wearer : Wearers)
		{
			const UGroomComponent* Strands = Wearer.Strands.Get();
			if (Wearer.bStrands && Strands && Strands->PhysicsAsset)
			{
				StrandHairDrawShapes(*Wearer.Body.Get(), *Strands->PhysicsAsset);
			}
		}
	}
	SinceSort += DeltaTime;
	if (SinceSort < 0.25f || Wearers.IsEmpty())
	{
		return;
	}
	SinceSort = 0.0f;
	// Clothes put on or taken off: the strands' collision is remade for the new outfit (hair lies on the clothes).
	for (FWearer& Wearer : Wearers)
	{
		if (Wearer.bStrands && StrandHairClothKey(StrandHairWornCloth(*Wearer.Body.Get())) != Wearer.ClothKey)
		{
			Show(Wearer, true);
		}
	}
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
		const bool bVisible = Body->IsVisible() && Body->GetOwner() && !Body->GetOwner()->IsHidden() && !StrandHairHeadGone(*Body);
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
		// Near the camera every strand is drawn (Pawns.StrandHairFullDetail): the same for the game, the colony creator and
		// the Debug scenes, since they all wear hair through this subsystem.
		if (UGroomComponent* Strands = Wearer.bStrands ? Wearer.Strands.Get() : nullptr)
		{
			const float FullCm = FMath::Max(0.0f, GStrandHairFullDetail.GetValueOnGameThread()) * 100.0f;
			const float Away = static_cast<float>(FVector::Dist(Wearer.Body->GetComponentLocation(), Eye));
			const int32 Lod = Away < FullCm ? 0 : -1;
			if (Lod != Wearer.ForcedLod)
			{
				Strands->SetForcedLOD(Lod);
				Wearer.ForcedLod = Lod;
			}
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
