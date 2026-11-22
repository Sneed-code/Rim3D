#include "PawnSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/PackageName.h"
#include "PoseSearch/PoseSearchDatabase.h"

#include "LevelsSubsystem.h"
#include "PawnActor.h"
#include "PawnFaces.h"
#include "CreatorPeople.h"
#include "MakeHumanArt.h"
#include "Retargeter/IKRetargeter.h"
#include "PawnLooks.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogSimPawns, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GPawnsFullDetail(
		TEXT("Pawns.FullDetail"), 20, TEXT("How many pawns nearest the camera get full motion matching."), ECVF_Default);
	TAutoConsoleVariable<float> GPawnsNearDistance(
		TEXT("Pawns.NearDistance"), 40.0f, TEXT("Metres: past this, pawns update their animation 8 times a second instead of 20."), ECVF_Default);
	TAutoConsoleVariable<int32> GPawnsMetaHumans(
		TEXT("Pawns.MetaHumans"), 0, TEXT("1 draws pawns as MetaHumans (driven by a hidden mannequin), 0 as mannequins."), ECVF_Default);
	TAutoConsoleVariable<float> GPawnsSkinRoughness(
		TEXT("Look.SkinRoughness"), -1.0f, TEXT("Skin roughness for every pawn (lower is shinier); negative uses data/look.json \"skin\"."), ECVF_Default);
	TAutoConsoleVariable<float> GPawnsSkinSpecular(
		TEXT("Look.SkinSpecular"), -1.0f, TEXT("Skin specular for every pawn (0.5 is the engine's default); negative uses data/look.json \"skin\"."), ECVF_Default);
	TAutoConsoleVariable<int32> GPawnsShowDetail(
		TEXT("Pawns.ShowDetail"), 0, TEXT("1 marks each pawn's animation detail over its head: green full, yellow near, red far."), ECVF_Default);

	constexpr float PawnNearTickInterval = 1.0f / 20.0f;
	constexpr float PawnFarTickInterval = 1.0f / 8.0f;

	const TCHAR* const PawnBodyMeshPath = TEXT("/Game/Characters/UEFN_Mannequin/Meshes/SKM_UEFN_Mannequin.SKM_UEFN_Mannequin");
	const TCHAR* const PawnMetaHumanClassPath = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");
	// Standing still: calm Mixamo standing idles with the feet planted, retargeted onto the mannequin by the MakeHumanArt
	// commandlet (docs/design/pawns.md "Standing still"). Each was measured from its FBX (2026-11-18): neither foot moves
	// more than 0.3 cm over the clip. Clips with the same Feet number stand on the same footprint (to the millimetre); a
	// pawn drifts between any of them while it stands (its feet are held where they stand, the legs bend). Left
	// out on purpose: Action Adventure "idle (2)" (one foot well forward), "idle (3)" (steps with the right foot), the
	// fighting crouches and the bow and magic idles with raised arms. The sample's own idles shuffled a foot.
	struct FPawnStandIdle
	{
		const TCHAR* Path;
		int32 Feet;
	};
	const FPawnStandIdle PawnStandIdles[] = {
		{TEXT("/Game/Rim3D/Generated/Animations/Mixamo/MeleeAxe/A_ProMeleeAxeUnarmedIdle.A_ProMeleeAxeUnarmedIdle"), 0},
		{TEXT("/Game/Rim3D/Generated/Animations/Mixamo/MeleeAxe/A_ProMeleeAxeUnarmedIdleLookingVer1.A_ProMeleeAxeUnarmedIdleLookingVer1"), 0},
		{TEXT("/Game/Rim3D/Generated/Animations/Mixamo/MeleeAxe/A_ProMeleeAxeUnarmedIdleLookingVer2.A_ProMeleeAxeUnarmedIdleLookingVer2"), 0},
		{TEXT("/Game/Rim3D/Generated/Animations/Mixamo/Longbow/A_ProLongbowUnarmedIdle01.A_ProLongbowUnarmedIdle01"), 1},
	};
	const TCHAR* const PawnIdlePath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Idle/M_Neutral_Stand_Idle_Loop.M_Neutral_Stand_Idle_Loop");
	const TCHAR* const PawnWalkPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Walk/M_Neutral_Walk_Loop_F.M_Neutral_Walk_Loop_F");
	const TCHAR* const PawnRunPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Run/M_Neutral_Run_Loop_F.M_Neutral_Run_Loop_F");
	// Placeholder for picking up and putting down until there are real action animations: a short crouch.
	const TCHAR* const PawnCrouchPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Idle/M_Neutral_Crouch_Idle_Loop.M_Neutral_Crouch_Idle_Loop");

	// The sample's standing locomotion databases (its crouch, jump and traversal ones are left out). The sprint ones only
	// match the player's body sprinting (Shift): every other pawn moves at its cruise speed or slower.
	// Not the sample's idle or turn-in-place databases: their clips shift the weight, step a foot forward and shuffle, and
	// motion matching reached for them whenever a pawn was nearly still (turning to face, between swings, just stopped),
	// so the shuffle kept coming back after the standing idle was added (sneed, 2026-11-17 and 2026-11-18). A standing
	// pawn plays the standing idles above. Nor the starts and pivots: their "reface" clips turn the body round by their
	// own root, which is dropped here (the actor's yaw follows the sim), so a pawn setting off or turning back spun its
	// legs and feet round under it (the `idle` smoke test, 2026-11-19: feet jumping 10 cm a frame). Setting off now
	// blends from the standing idle straight into the walk, run or sprint loop, and a turn is the loop turning with the
	// body; stops stay (they plant the feet the standing idle then holds).
	const TCHAR* const PawnDatabaseNames[] = {
		TEXT("PSD_Dense_Stand_Walk_Loops"),
		TEXT("PSD_Dense_Stand_Walk_Stops"),
		TEXT("PSD_Dense_Stand_Run_Loops"),
		TEXT("PSD_Dense_Stand_Run_Stops"),
		TEXT("PSD_Dense_Stand_Sprint_Loops"),
		TEXT("PSD_Dense_Stand_Sprint_Stops"),
	};
	const TCHAR* const PawnDatabaseFolder = TEXT("/Game/Characters/UEFN_Mannequin/Animations/MotionMatchingData/Databases/Dense/");

	uint32 PawnKey(colony::Entity Thing)
	{
		return static_cast<uint32>(entt::to_integral(Thing));
	}

	/** Ground speed of a loop from its root motion, cm per second. */
	float PawnLoopSpeed(const UAnimSequence* Loop, float Fallback)
	{
		if (!Loop || Loop->GetPlayLength() <= 0.0f)
		{
			return Fallback;
		}
		const FTransform Motion = Loop->ExtractRootMotionFromRange(0.0, Loop->GetPlayLength(), FAnimExtractContext());
		const float Speed = Motion.GetTranslation().Size2D() / Loop->GetPlayLength();
		return Speed > 10.0f ? Speed : Fallback;
	}
}

bool USimPawnSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USimPawnSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USimPawnSubsystem, STATGROUP_Tickables);
}

void USimPawnSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &USimPawnSubsystem::OnSimEvent);
		EventSource = Bridge;
	}
}

void USimPawnSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = EventSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	// The world is going away and takes the actors with it.
	Pawns.Reset();
	Super::Deinitialize();
}

void USimPawnSubsystem::LoadAssets()
{
	if (bAssetsTried)
	{
		return;
	}
	bAssetsTried = true;
	BodyMesh = LoadObject<USkeletalMesh>(nullptr, PawnBodyMeshPath);
	IdleLoops.Reset();
	IdleFeet.Reset();
	for (const FPawnStandIdle& Stand : PawnStandIdles)
	{
		if (UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, Stand.Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			IdleLoops.Add(Idle);
			IdleFeet.Add(Stand.Feet);
		}
		else
		{
			UE_LOG(LogSimPawns, Warning, TEXT("Standing idle missing (run -run=MakeHumanArt -Clips): %s"), Stand.Path);
		}
	}
	if (IdleLoops.IsEmpty())
	{
		UE_LOG(LogSimPawns, Warning, TEXT("No Mixamo standing idle (run -run=MakeHumanArt -Clips): standing pawns use the sample's idle"));
		if (UAnimSequence* Idle = LoadObject<UAnimSequence>(nullptr, PawnIdlePath))
		{
			IdleLoops.Add(Idle);
			IdleFeet.Add(0);
		}
	}
	IdleLoop = IdleLoops.IsEmpty() ? nullptr : IdleLoops[0].Get();
	WalkLoop = LoadObject<UAnimSequence>(nullptr, PawnWalkPath);
	RunLoop = LoadObject<UAnimSequence>(nullptr, PawnRunPath);
	CrouchPose = LoadObject<UAnimSequence>(nullptr, PawnCrouchPath);
	for (const TCHAR* Name : PawnDatabaseNames)
	{
		const FString Path = FString::Printf(TEXT("%s%s.%s"), PawnDatabaseFolder, Name, Name);
		if (const UPoseSearchDatabase* Database = LoadObject<UPoseSearchDatabase>(nullptr, *Path))
		{
			Databases.Add(Database);
		}
		else
		{
			UE_LOG(LogSimPawns, Warning, TEXT("Motion matching database missing: %s"), *Path);
		}
	}
	WalkLoopSpeed = PawnLoopSpeed(WalkLoop, 150.0f);
	RunLoopSpeed = PawnLoopSpeed(RunLoop, 400.0f);
	bAssetsLoaded = BodyMesh && IdleLoop && WalkLoop && RunLoop && !Databases.IsEmpty();
	if (bAssetsLoaded)
	{
		UE_LOG(LogSimPawns, Display, TEXT("Pawn animation: %d motion matching databases, walk loop %.0f cm/s, run loop %.0f cm/s, %d standing idles (first %s)"),
			Databases.Num(), WalkLoopSpeed, RunLoopSpeed, IdleLoops.Num(), *IdleLoop->GetName());
	}
	else
	{
		UE_LOG(LogSimPawns, Error, TEXT("Pawn assets missing (mesh %d, loops %d %d %d, %d databases): is the Game Animation Sample in game/Content?"),
			BodyMesh != nullptr, IdleLoop != nullptr, WalkLoop != nullptr, RunLoop != nullptr, Databases.Num());
	}
}

namespace
{
	/** Loads a generated looks asset by name, or null (without a warning) if it isn't there. */
	template <typename T>
	T* PawnLoadLooksAsset(const FString& Name)
	{
		const FString Package = FString(PawnLooksFolder) / Name;
		return FPackageName::DoesPackageExist(Package) ? LoadObject<T>(nullptr, *PawnLooksAssetPath(Name)) : nullptr;
	}

	FLinearColor PawnLinear(const colony::LooksColor& C)
	{
		return FLinearColor::FromSRGBColor(FColor(FMath::RoundToInt32(FMath::Clamp(C.r, 0.0f, 1.0f) * 255.0f),
			FMath::RoundToInt32(FMath::Clamp(C.g, 0.0f, 1.0f) * 255.0f), FMath::RoundToInt32(FMath::Clamp(C.b, 0.0f, 1.0f) * 255.0f)));
	}
}

TArray<TPair<FName, float>> FSimPawnLooks::MorphWeights() const
{
	TArray<TPair<FName, float>> Out;
	for (const TPair<FName, FString>& Type : PawnLooksBodyTypes())
	{
		Out.Add({Type.Key, 0.0f});
	}
	// Build (thin, normal, heavy) and stature (short, medium, tall) pick a cell of the gender's grid; the medium normal
	// corner of the male grid is Kellan's own body (no morph).
	const float B = FMath::Clamp(FMath::Abs(Weight), 0.0f, 1.0f);
	const float S = FMath::Clamp(FMath::Abs(Stature), 0.0f, 1.0f);
	const int32 Build = Weight < 0.0f ? -1 : 1;
	const int32 Tallness = Stature < 0.0f ? -1 : 1;
	const TPair<FName, float> Corners[] = {
		{PawnLooksBodyMorph(bFemale, 0, 0), (1.0f - S) * (1.0f - B)},
		{PawnLooksBodyMorph(bFemale, 0, Build), (1.0f - S) * B},
		{PawnLooksBodyMorph(bFemale, Tallness, 0), S * (1.0f - B)},
		{PawnLooksBodyMorph(bFemale, Tallness, Build), S * B},
	};
	for (const TPair<FName, float>& Corner : Corners)
	{
		if (TPair<FName, float>* Found = Out.FindByPredicate([&](const TPair<FName, float>& M) { return M.Key == Corner.Key; }))
		{
			Found->Value += Corner.Value;
		}
	}
	return Out;
}

TArray<TPair<FName, float>> FSimPawnLooks::FaceMorphWeights() const
{
	if (!FacePreset.IsEmpty())
	{
		// The preset is the whole face: no female shift, sliders at the average.
		return {{PawnFacesMorphFemale, 0.0f}, {PawnFacesMorphShape, 0.0f}, {PawnFacesMorphBrow, 0.0f}, {PawnFacesMorphNose, 0.0f}, {PawnFacesMorphJaw, 0.0f},
			{PawnFacesPresetMorph(bFemale, FacePreset), 1.0f}};
	}
	const auto Slider = [](float Value) { return FMath::Clamp(Value, 0.0f, 1.0f) * 2.0f - 1.0f; };
	return {{PawnFacesMorphFemale, bFemale ? 1.0f : 0.0f}, {PawnFacesMorphShape, Slider(FaceShape)}, {PawnFacesMorphBrow, Slider(Brow)},
		{PawnFacesMorphNose, Slider(Nose)}, {PawnFacesMorphJaw, Slider(Jaw)}};
}

FSimPawnLooks USimPawnSubsystem::MakeLooks(const colony::PawnLooksView& View, const colony::DefDatabase* Defs)
{
	FSimPawnLooks Looks;
	Looks.Height = FMath::Clamp(View.height, 0.5f, 1.5f);
	Looks.Weight = FMath::Clamp(View.weight, -1.0f, 1.0f);
	Looks.bFemale = View.female;
	// Stature: where the height falls in the gender's range (LooksRules heightMale / heightFemale); hulks are a little taller.
	{
		const char* const Key = View.female ? "heightFemale" : "heightMale";
		const colony::Def* Rules = Defs ? Defs->find("Rules", "LooksRules") : nullptr;
		const float Low = Rules ? static_cast<float>(Rules->numberAt(Key, 0, View.female ? 0.88 : 0.95)) : (View.female ? 0.88f : 0.95f);
		const float High = Rules ? static_cast<float>(Rules->numberAt(Key, 1, View.female ? 0.97 : 1.05)) : (View.female ? 0.97f : 1.05f);
		Looks.Stature = High > Low ? FMath::Clamp((Looks.Height - (Low + High) * 0.5f) / ((High - Low) * 0.5f), -1.0f, 1.0f) : 0.0f;
	}
	Looks.Skin = PawnLinear(View.skin);
	Looks.Hair = PawnLinear(View.hair);
	Looks.FaceShape = View.faceShape;
	Looks.Brow = View.brow;
	Looks.Nose = View.nose;
	Looks.Jaw = View.jaw;
	Looks.Eyes = FColor(FMath::RoundToInt32(FMath::Clamp(View.eyes.r, 0.0f, 1.0f) * 255.0f), FMath::RoundToInt32(FMath::Clamp(View.eyes.g, 0.0f, 1.0f) * 255.0f),
		FMath::RoundToInt32(FMath::Clamp(View.eyes.b, 0.0f, 1.0f) * 255.0f));
	Looks.HairStyle = UTF8_TO_TCHAR(View.hairStyle.c_str());
	Looks.Beard = UTF8_TO_TCHAR(View.beard.c_str());
	Looks.BodyHair = FMath::Clamp(View.bodyHair, 0.0f, 1.0f);
	const auto Marks = [](const std::vector<colony::LooksMark>& From, TArray<FSimPawnMark>& To)
	{
		for (const colony::LooksMark& Mark : From)
		{
			To.Add({static_cast<uint8>(Mark.region), Mark.style, Mark.u, Mark.v, Mark.size});
		}
	};
	Marks(View.tattoos, Looks.Tattoos);
	Marks(View.scars, Looks.Scars);
	if (Defs && View.lineage.valid())
	{
		// Horns are a gene of the lineage (X1): the last of its genes that has them.
		const colony::Def& Lineage = Defs->get(View.lineage);
		const colony::Def* HornGene = colony::lineageGeneWith(*Defs, Lineage, "visual.horns.length");
		Looks.HornLength = HornGene ? static_cast<float>(HornGene->number("visual.horns.length", 0.0) * 100.0) : 0.0f;
		if (HornGene && Looks.HornLength > 0.0f)
		{
			Looks.HornRadius = static_cast<float>(HornGene->number("visual.horns.radius", 0.03) * 100.0);
			Looks.HornSpread = static_cast<float>(HornGene->number("visual.horns.spread", 0.05) * 100.0);
			Looks.HornColor = PawnLinear(colony::LooksColor{static_cast<float>(HornGene->numberAt("visual.horns.color", 0, 0.1)),
				static_cast<float>(HornGene->numberAt("visual.horns.color", 1, 0.08)), static_cast<float>(HornGene->numberAt("visual.horns.color", 2, 0.07))});
		}
	}
	return Looks;
}

namespace
{
	/** How torn a piece with this share of its hit points looks: whole above 70%, in tatters at none, in sixteenths. */
	float PawnWearOf(float HitPointShare)
	{
		const float Wear = FMath::Clamp((0.7f - HitPointShare) / 0.7f, 0.0f, 1.0f);
		return FMath::RoundToFloat(Wear * 16.0f) * (1.0f / 16.0f);
	}

	const colony::ApparelView* PawnApparelAt(const colony::Snapshot& Snapshot, const colony::ThingView& View, uint32 Index)
	{
		const size_t At = static_cast<size_t>(View.firstApparel) + Index;
		return At < Snapshot.apparel.size() ? &Snapshot.apparel[At] : nullptr;
	}
}

uint64 USimPawnSubsystem::ApparelKey(const colony::Snapshot& Snapshot, const colony::ThingView& View)
{
	uint64 Key = 1469598103934665603ULL;
	const auto Mix = [&Key](uint64 Value) { Key = (Key ^ Value) * 1099511628211ULL; };
	Mix(View.apparelCount);
	Mix(View.hideGear ? 1u : 0u);
	for (uint32 I = 0; I < View.apparelCount; ++I)
	{
		if (const colony::ApparelView* Piece = PawnApparelAt(Snapshot, View, I))
		{
			Mix(entt::to_integral(Piece->item));
			Mix(Piece->def.value);
			Mix(Piece->stuff.value);
			Mix(static_cast<uint64>(PawnWearOf(Piece->hitPointShare) * 16.0f + 0.5f));
		}
	}
	return Key;
}

FSimPawnApparel USimPawnSubsystem::MakeApparel(const colony::Snapshot& Snapshot, const colony::ThingView& View, const colony::DefDatabase* Defs)
{
	FSimPawnApparel Out;
	if (!Defs || View.hideGear) // the look-only switch: worn gear still counts, it just isn't drawn
	{
		return Out;
	}
	for (uint32 I = 0; I < View.apparelCount; ++I)
	{
		const colony::ApparelView* Piece = PawnApparelAt(Snapshot, View, I);
		if (!Piece || !Piece->def.valid() || Piece->def.value >= Defs->size())
		{
			continue;
		}
		const colony::Def& Def = Defs->get(Piece->def);
		FSimWornPiece Worn;
		Worn.Item = static_cast<uint32>(entt::to_integral(Piece->item));
		Worn.Def = Piece->def.value;
		Worn.Stuff = Piece->stuff.value;
		Worn.Wear = PawnWearOf(Piece->hitPointShare);
		Worn.bHidesHair = Def.flag("apparel.hidesHair", false);
		bool bOverLegs = false;
		bool bUnderShell = false;
		bool bOverTorso = false;
		int32 LayerRank = 0;
		for (size_t G = 0; G < Def.count("apparel.bodyPartGroups"); ++G)
		{
			const std::string Group = Def.textAt("apparel.bodyPartGroups", G);
			bOverLegs = bOverLegs || Group == "Legs";
			bOverTorso = bOverTorso || Group == "Torso";
		}
		for (size_t L = 0; L < Def.count("apparel.layers"); ++L)
		{
			const std::string Layer = Def.textAt("apparel.layers", L);
			LayerRank = FMath::Max(LayerRank, Layer == "Shell" ? 2 : (Layer == "Middle" ? 1 : 0));
			bUnderShell = bUnderShell || Def.textAt("apparel.layers", L) != "Shell";
			Worn.bHeadwear = Worn.bHeadwear || Def.textAt("apparel.layers", L) == "Overhead";
		}
		Worn.bCoversCrotch = bOverLegs && bUnderShell;
		// Puff (cm along the normal): tops sit over waistbands and belts; legwear sits over the skin kept
		// at its top edge (the cover bake keeps skin near uncovered skin, which pokes through a waistband
		// left bare by a short top).
		Worn.Puff = (bOverTorso ? 1.5f : (bOverLegs ? 0.8f : 0.0f)) + 0.6f * static_cast<float>(LayerRank);
		if (Piece->stuff.valid() && Piece->stuff.value < Defs->size())
		{
			const colony::Def& Stuff = Defs->get(Piece->stuff);
			if (Stuff.count("visual.color") >= 3)
			{
				Worn.bHasTint = true;
				Worn.Tint = VisualMesh::ReadColor(Stuff, "visual.color", FLinearColor::White);
			}
		}
		// MakeHuman bodies: the cuts this piece can be drawn as ("visual.cuts": ["Garment", "Top+Trousers", ...]).
		const size_t CutCount = Def.count("visual.cuts");
		for (size_t C = 0; C < CutCount; ++C)
		{
			const std::string Cut = Def.text(("visual.cuts." + std::to_string(C)).c_str());
			if (!Cut.empty())
			{
				Worn.Cuts.Add(UTF8_TO_TCHAR(Cut.c_str()));
			}
		}
		const size_t Count = Def.count("visual.worn");
		for (size_t M = 0; M < Count; ++M)
		{
			const std::string Base = "visual.worn." + std::to_string(M);
			FSimWornMesh Mesh;
			Mesh.Path = UTF8_TO_TCHAR(Def.text((Base + ".mesh").c_str()).c_str());
			const std::string Bone = Def.text((Base + ".bone").c_str());
			Mesh.Bone = Bone.empty() ? NAME_None : FName(UTF8_TO_TCHAR(Bone.c_str()));
			if (!Mesh.Path.IsEmpty())
			{
				Worn.Meshes.Add(MoveTemp(Mesh));
			}
		}
		Out.Pieces.Add(MoveTemp(Worn));
	}
	return Out;
}

UObject* USimPawnSubsystem::GetClothesAsset(const FString& Path)
{
	if (const TObjectPtr<UObject>* Found = ClothesAssets.Find(Path))
	{
		return *Found;
	}
	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	ClothesAssets.Add(Path, Asset);
	if (!Asset)
	{
		UE_LOG(LogSimPawns, Warning, TEXT("Clothes mesh %s is missing: run python tools/clothes_art.py and the ClothesArt commandlet (docs/design/looks.md)"), *Path);
	}
	return Asset;
}

USkeletalMesh* USimPawnSubsystem::GetMorphMesh(const USkeletalMesh* Original)
{
	if (!Original)
	{
		return nullptr;
	}
	if (const TObjectPtr<USkeletalMesh>* Found = MorphMeshes.Find(Original->GetFName()))
	{
		return *Found;
	}
	USkeletalMesh* Mesh = PawnLoadLooksAsset<USkeletalMesh>(PawnLooksMorphMeshName(Original->GetName()));
	MorphMeshes.Add(Original->GetFName(), Mesh);
	if (!Mesh && !bLooksMissingLogged && Original->GetName() == TEXT("m_med_nrw_body"))
	{
		bLooksMissingLogged = true;
		UE_LOG(LogSimPawns, Warning, TEXT("No pawn looks assets in %s: run the PawnLooks commandlet (docs/design/looks.md); pawns all look like Kellan"),
			PawnLooksFolder);
	}
	return Mesh;
}

UMaterialInterface* USimPawnSubsystem::GetSkinMaterial(const UMaterialInterface* Original)
{
	if (!Original)
	{
		return nullptr;
	}
	if (const TObjectPtr<UMaterialInterface>* Found = SkinMaterials.Find(Original->GetFName()))
	{
		return *Found;
	}
	UMaterialInterface* Material = GetShinySkin(PawnLoadLooksAsset<UMaterialInterface>(PawnLooksSkinInstanceName(Original->GetName())));
	SkinMaterials.Add(Original->GetFName(), Material);
	return Material;
}

void USimPawnSubsystem::ReadSkinShine()
{
	bSkinShineRead = true;
	const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr;
	FString Text;
	TSharedPtr<FJsonObject> Root;
	const TSharedPtr<FJsonObject>* Skin = nullptr;
	if (Bridge && FFileHelper::LoadFileToString(Text, *(Bridge->GetDataRoot() / TEXT("look.json"))) &&
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid() && Root->TryGetObjectField(TEXT("skin"), Skin))
	{
		double Value = 0.0;
		if ((*Skin)->TryGetNumberField(TEXT("roughness"), Value))
		{
			SkinRoughness = static_cast<float>(Value);
		}
		if ((*Skin)->TryGetNumberField(TEXT("specular"), Value))
		{
			SkinSpecular = static_cast<float>(Value);
		}
	}
	UE_LOG(LogSimPawns, Display, TEXT("Skin shine: roughness %.2f, specular %.2f (negative keeps the material's own)"), SkinRoughness, SkinSpecular);
}

void USimPawnSubsystem::ApplySkinShine(UMaterialInstanceDynamic* Instance)
{
	if (!Instance)
	{
		return;
	}
	if (!bSkinShineRead)
	{
		ReadSkinShine();
	}
	const float RoughnessOverride = GPawnsSkinRoughness.GetValueOnGameThread();
	const float SpecularOverride = GPawnsSkinSpecular.GetValueOnGameThread();
	const float Roughness = RoughnessOverride >= 0.0f ? RoughnessOverride : SkinRoughness;
	const float Specular = SpecularOverride >= 0.0f ? SpecularOverride : SkinSpecular;
	if (Roughness >= 0.0f)
	{
		Instance->SetScalarParameterValue(TEXT("Roughness"), Roughness);
	}
	if (Specular >= 0.0f)
	{
		Instance->SetScalarParameterValue(TEXT("Specular"), Specular);
	}
}

UMaterialInterface* USimPawnSubsystem::GetShinySkin(UMaterialInterface* Skin)
{
	if (!Skin)
	{
		return nullptr;
	}
	if (const TObjectPtr<UMaterialInstanceDynamic>* Found = ShinySkins.Find(Skin))
	{
		return *Found;
	}
	// Only our skins (M_PawnSkin and its instances) have these two parameters.
	float Unused = 0.0f;
	if (!Skin->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Roughness")), Unused))
	{
		return Skin;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Skin, this);
	ApplySkinShine(Instance);
	ShinySkins.Add(Skin, Instance);
	return Instance;
}

UMaterialInterface* USimPawnSubsystem::GetClothMaterial(const UMaterialInterface* Original)
{
	if (!Original)
	{
		return nullptr;
	}
	if (const TObjectPtr<UMaterialInterface>* Found = ClothMaterials.Find(Original->GetFName()))
	{
		return *Found;
	}
	UMaterialInterface* Material = PawnLoadLooksAsset<UMaterialInterface>(PawnLooksClothInstanceName(Original->GetName()));
	ClothMaterials.Add(Original->GetFName(), Material);
	return Material;
}

UStaticMesh* USimPawnSubsystem::GetHornMesh()
{
	if (!bHornTried)
	{
		bHornTried = true;
		HornMesh = PawnLoadLooksAsset<UStaticMesh>(PawnLooksHornMesh);
	}
	return HornMesh;
}

USkeletalMesh* USimPawnSubsystem::GetMakeHumanMesh(const FString& Name, UIKRetargeter*& OutRetargeter)
{
	OutRetargeter = nullptr;
	for (const TCHAR* Folder : {MakeHumanFolder, MakeHumanLocalFolder})
	{
		if (FPackageName::DoesPackageExist(FString(Folder) / MakeHumanMeshName(Name)))
		{
			OutRetargeter = LoadObject<UIKRetargeter>(nullptr, *MakeHumanAssetPath(Folder, MakeHumanRetargeterName(Name)));
			return LoadObject<USkeletalMesh>(nullptr, *MakeHumanAssetPath(Folder, MakeHumanMeshName(Name)));
		}
	}
	return nullptr;
}

UObject* USimPawnSubsystem::GetFacesAsset(const FString& Name)
{
	if (const TObjectPtr<UObject>* Found = FacesAssets.Find(Name))
	{
		return *Found;
	}
	const FString Package = FString(PawnFacesFolder) / Name;
	UObject* Asset = FPackageName::DoesPackageExist(Package) ? LoadObject<UObject>(nullptr, *PawnFacesAssetPath(Name)) : nullptr;
	FacesAssets.Add(Name, Asset);
	if (!Asset && !bFacesMissingLogged && Name.StartsWith(TEXT("SK_Face_")))
	{
		bFacesMissingLogged = true;
		UE_LOG(LogSimPawns, Warning, TEXT("No pawn faces assets in %s: run the PawnFaces commandlet (docs/design/looks.md); pawns keep Kellan's face and hair"),
			PawnFacesFolder);
	}
	return Asset;
}

UAnimSequence* USimPawnSubsystem::GetActionAnimation(colony::PawnAction Action) const
{
	switch (Action)
	{
		case colony::PawnAction::PickUp:
		case colony::PawnAction::PutDown:
		case colony::PawnAction::Eat:
		case colony::PawnAction::LieDown:
		case colony::PawnAction::GetUp:
		case colony::PawnAction::Build:
		case colony::PawnAction::Deconstruct:
		case colony::PawnAction::Cook:
			return CrouchPose;
		default:
			break;
	}
	// Everything else (a block, a dodge, a reload, a weapon switch, a throw, an alert, a dive for cover...) has no
	// crouch: it used to fall through to it, so fighters ducked for half a second whenever they parried, reloaded or
	// called out, and slid along the ground on crouched legs if they moved meanwhile (sneed 2026-11-22: "if enemies
	// crouch and move they slide").
	return nullptr;
}

USkeletalMesh* USimPawnSubsystem::GetGarmentMesh(const FString& Base, const FString& Garment)
{
	const FString Name = MakeHumanGarmentName(Base, Garment);
	if (const TObjectPtr<USkeletalMesh>* Found = GarmentMeshes.Find(Name))
	{
		return *Found;
	}
	USkeletalMesh* Mesh = nullptr;
	if (FPackageName::DoesPackageExist(FString(MakeHumanGarmentFolder) / Name))
	{
		Mesh = LoadObject<USkeletalMesh>(nullptr, *MakeHumanAssetPath(MakeHumanGarmentFolder, Name));
	}
	GarmentMeshes.Add(Name, Mesh);
	return Mesh;
}

void USimPawnSubsystem::SetLooksOverride(colony::Entity Thing, const FSimPawnLooks& Looks)
{
	LooksOverrides.Add(PawnKey(Thing), Looks);
}

void USimPawnSubsystem::SetApparelPreview(colony::Entity Thing, const TArray<FString>& Pieces)
{
	ApparelPreviews.Add(PawnKey(Thing), Pieces);
}

void USimPawnSubsystem::ClearApparelPreview(colony::Entity Thing)
{
	ApparelPreviews.Remove(PawnKey(Thing));
}

ASimPawnActor* USimPawnSubsystem::FindPawn(colony::Entity Thing) const
{
	const TObjectPtr<ASimPawnActor>* Found = Pawns.Find(PawnKey(Thing));
	return Found ? Found->Get() : nullptr;
}

void USimPawnSubsystem::RemoveAll()
{
	for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	Pawns.Reset();
	DetailCounts = FSimPawnDetailCounts();
}

void USimPawnSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::PawnAction)
	{
		return;
	}
	++ActionEvents;
	if (ASimPawnActor* Pawn = FindPawn(Event.thing))
	{
		const int32 Before = Pawn->GetActionsPlayed();
		Pawn->PlayAction(Event.action);
		ActionsStarted += Pawn->GetActionsPlayed() - Before;
	}
}

void USimPawnSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !Bridge->GetSim())
	{
		RemoveAll();
		return;
	}
	// This frame's steps and blend first, if the sim has not ticked yet this frame (the order tickables run in is not set).
	Bridge->AdvanceFrame(DeltaTime);
	// A new sim reuses entity numbers; a new look needs new actors.
	const bool bMetaHumans = GPawnsMetaHumans.GetValueOnGameThread() != 0;
	if (Bridge->GetSim() != LastSim || bMetaHumans != bMetaHumansShown)
	{
		RemoveAll();
		LastSim = Bridge->GetSim();
		bMetaHumansShown = bMetaHumans;
	}
	LoadAssets();
	if (!bAssetsLoaded)
	{
		return;
	}
	if (bMetaHumans && !MetaHumanClass)
	{
		MetaHumanClass = LoadClass<AActor>(nullptr, PawnMetaHumanClassPath);
		if (!MetaHumanClass)
		{
			UE_LOG(LogSimPawns, Error, TEXT("Could not load %s; drawing mannequins"), PawnMetaHumanClassPath);
		}
	}

	const colony::Snapshot& Now = Bridge->GetCurrentSnapshot();
	const colony::Snapshot& Before = Bridge->GetPreviousSnapshot();
	const colony::DefDatabase* Defs = Bridge->GetDefs().get();
	// The levels above the one the overseer looks at are cut away (docs/design/levels-draw.md).
	const int32 TopLevel = ULevelsSubsystem::TopShownLevel(GetWorld());
	TSet<uint32> Seen;
	for (const colony::ThingView& View : Now.things)
	{
		// Animals (life stage 0 to 2) are drawn by USimAnimalSubsystem.
		if (View.category != colony::ThingCategory::Pawn || View.lifeStage >= 0)
		{
			continue;
		}
		const uint32 Key = PawnKey(View.id);
		Seen.Add(Key);
		TObjectPtr<ASimPawnActor>& Pawn = Pawns.FindOrAdd(Key);
		if (!IsValid(Pawn))
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			FVector Location;
			Bridge->GetBlendedLocation(View.id, Location);
			Pawn = GetWorld()->SpawnActor<ASimPawnActor>(Location, FRotator::ZeroRotator, Params);
			const double MetresPerStep = Defs && View.def.valid() ? Defs->get(View.def).number("stats.moveSpeed", 0.0) : 0.0;
			const float Cruise = MetresPerStep > 0.0 ? static_cast<float>(MetresPerStep * colony::kStepsPerRealSecondAt1x * 100.0) : 400.0f;
			Pawn->Setup(*this, View.id, Cruise, bMetaHumans);
		}
		Pawn->UpdateFromSim(*Bridge, View, Before.find(View.id), DeltaTime, Bridge->GetSpeed());
		Pawn->SetCutAway(View.level > TopLevel);
		if (const FSimPawnLooks* Override = LooksOverrides.Find(PawnKey(View.id)))
		{
			Pawn->ApplyLooks(*this, *Override);
		}
		else if (View.looks >= 0 && View.looks < static_cast<int32>(Now.looks.size()))
		{
			const colony::PawnLooksView& LooksView = Now.looks[static_cast<size_t>(View.looks)];
			FSimPawnLooks Looks = MakeLooks(LooksView, Defs);
			// A colonist from the character screen (docs/design/new-game.md): drawn as the creator's MakeHuman person.
			CreatorPeople::FLook Look;
			const FString CreatorText = LooksView.creator.empty() ? FString() : FString(UTF8_TO_TCHAR(LooksView.creator.c_str()));
			const bool bCreator = !CreatorText.IsEmpty() && CreatorPeople::Decode(CreatorText, Look);
			if (bCreator)
			{
				Looks = CreatorPeople::LooksFor(Look, Looks);
			}
			Pawn->ApplyLooks(*this, Looks);
			if (bCreator)
			{
				Pawn->ApplyCreatorLook(CreatorText);
			}
		}
		// Worn clothes (U16): rebuilt only when what is worn, or how torn it is, changed.
		if (const TArray<FString>* Preview = ApparelPreviews.Find(PawnKey(View.id)); Preview && Defs)
		{
			// The character screen's "clothes on": made-up pieces in a snapshot of their own, so they take exactly the
			// path real clothes take (MakeApparel), keyed apart from anything the pawn really wears.
			colony::Snapshot Dressed;
			colony::ThingView Shown = View;
			Shown.firstApparel = 0;
			Shown.apparelCount = 0;
			Shown.hideGear = false;
			for (int32 P = 0; P < Preview->Num(); ++P)
			{
				FString DefName;
				FString StuffName;
				if (!(*Preview)[P].Split(TEXT(":"), &DefName, &StuffName))
				{
					DefName = (*Preview)[P];
				}
				colony::ApparelView Piece;
				Piece.item = static_cast<colony::Entity>(0x7FFF0000u + static_cast<uint32>(P));
				Piece.def = Defs->idOf("Things", TCHAR_TO_UTF8(*DefName));
				Piece.stuff = StuffName.IsEmpty() ? colony::DefId{} : Defs->idOf("Things", TCHAR_TO_UTF8(*StuffName));
				if (Piece.def.valid())
				{
					Dressed.apparel.push_back(Piece);
					++Shown.apparelCount;
				}
			}
			const uint64 PreviewKey = ApparelKey(Dressed, Shown) ^ 0x5052455649455721ULL;
			if (!Pawn->HasApparel() || Pawn->GetApparelKey() != PreviewKey)
			{
				Pawn->ApplyApparel(*this, MakeApparel(Dressed, Shown, Defs), PreviewKey);
			}
		}
		else
		{
			const uint64 WornKey = ApparelKey(Now, View);
			if (!Pawn->HasApparel() || Pawn->GetApparelKey() != WornKey)
			{
				Pawn->ApplyApparel(*this, MakeApparel(Now, View, Defs), WornKey);
			}
		}
		Pawn->SyncGarmentMorphs();
	}
	for (auto It = Pawns.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			if (IsValid(It.Value()))
			{
				It.Value()->Destroy();
			}
			It.RemoveCurrent();
		}
	}

	FVector Camera = FVector::ZeroVector;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
	{
		Camera = PC->PlayerCameraManager->GetCameraLocation();
	}
	AssignDetail(Camera);
}

void USimPawnSubsystem::AssignDetail(const FVector& CameraLocation)
{
	TArray<TPair<double, ASimPawnActor*>> ByDistance;
	ByDistance.Reserve(Pawns.Num());
	for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns)
	{
		ByDistance.Emplace(FVector::DistSquared(Pair.Value->GetActorLocation(), CameraLocation), Pair.Value.Get());
	}
	ByDistance.Sort([](const TPair<double, ASimPawnActor*>& A, const TPair<double, ASimPawnActor*>& B)
	{
		return A.Key < B.Key;
	});

	const int32 FullCount = FMath::Max(0, GPawnsFullDetail.GetValueOnGameThread());
	const double Near = FMath::Square(GPawnsNearDistance.GetValueOnGameThread() * 100.0);
	const bool bShow = GPawnsShowDetail.GetValueOnGameThread() != 0;
	DetailCounts = FSimPawnDetailCounts();
	for (int32 i = 0; i < ByDistance.Num(); ++i)
	{
		ASimPawnActor* Pawn = ByDistance[i].Value;
		FColor Mark;
		if (i < FullCount)
		{
			Pawn->SetDetail(ESimPawnDetail::Full, 0.0f);
			++DetailCounts.Full;
			Mark = FColor::Green;
		}
		else if (ByDistance[i].Key < Near)
		{
			Pawn->SetDetail(ESimPawnDetail::Simple, PawnNearTickInterval);
			++DetailCounts.Near;
			Mark = FColor::Yellow;
		}
		else
		{
			Pawn->SetDetail(ESimPawnDetail::Simple, PawnFarTickInterval);
			++DetailCounts.Far;
			Mark = FColor::Red;
		}
		if (bShow)
		{
			DrawDebugPoint(GetWorld(), Pawn->GetActorLocation() + FVector(0.0, 0.0, 220.0), 12.0f, Mark);
		}
	}
}
