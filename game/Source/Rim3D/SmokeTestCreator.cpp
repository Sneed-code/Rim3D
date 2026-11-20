// Not a check: `creator` (tools/play-creator.bat) is the live character creator (docs/design/makehuman.md, "Character
// creator"): colonists on the MakeHuman creator bases standing in a row with the graphics up, and a panel that changes
// the picked one as you drag: gender (which base), kind (baseline, imp, orcoid), body sliders, pubic hair and skin
// (morph targets on its mesh, its height and skin colour). They can walk back and forth to show the jiggle and the tail.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"
#include "Misc/Parse.h"

#include "CreatorPeople.h"
#include "CreatorWeapons.h"
#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "PawnLooks.h"
#include "PawnSubsystem.h"
#include "MakeHumanArt.h"
#include "SoftContactSubsystem.h"
#include "StrandHair.h"
#include "GroomComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <imgui.h>
#include <vector>

namespace
{
	enum class ECreatorKind : int32 { Baseline, Imp, Orcoid, Beast, Drow };
	// Drow skin: a range of its own, from a light ash violet through slate and midnight purple to obsidian (the slider
	// picks along it).
	const FLinearColor CreatorDrowSkins[] = {FLinearColor(0.47f, 0.43f, 0.52f), FLinearColor(0.34f, 0.30f, 0.40f), FLinearColor(0.25f, 0.20f, 0.32f),
		FLinearColor(0.17f, 0.13f, 0.24f), FLinearColor(0.11f, 0.10f, 0.15f), FLinearColor(0.07f, 0.065f, 0.08f)};

	// Jewellery (mh_common.add_jewellery): each piece a mesh on the bases with a material slot Jewel_<key>.
	struct FCreatorJewel
	{
		const char* Label;
		const TCHAR* Keys[2];
	};
	const FCreatorJewel CreatorJewels[] = {
		{"Lobe hoops", {TEXT("Jewel_EarringL"), TEXT("Jewel_EarringR")}}, {"Lobe studs", {TEXT("Jewel_EarStudL"), TEXT("Jewel_EarStudR")}},
		{"Ear dangles", {TEXT("Jewel_EarDangleL"), TEXT("Jewel_EarDangleR")}}, {"Helix rings", {TEXT("Jewel_HelixL"), TEXT("Jewel_HelixR")}},
		{"Tragus studs", {TEXT("Jewel_TragusL"), TEXT("Jewel_TragusR")}}, {"Industrial bars", {TEXT("Jewel_IndustrialL"), TEXT("Jewel_IndustrialR")}},
		{"Nose ring", {TEXT("Jewel_NoseRing"), nullptr}},
		{"Nipple piercings", {TEXT("Jewel_NippleL"), TEXT("Jewel_NippleR")}}, {"Navel ring", {TEXT("Jewel_Navel"), nullptr}},
		{"Navel gem", {TEXT("Jewel_NavelGem"), nullptr}}, {"Navel dangle", {TEXT("Jewel_NavelDangle"), nullptr}},
		{"Genital piercing", {TEXT("Jewel_Genital"), nullptr}}, {"Finger ring", {TEXT("Jewel_Ring"), nullptr}},
		{"Necklace", {TEXT("Jewel_Necklace"), nullptr}}, {"Collar", {TEXT("Jewel_Collar"), nullptr}},
		{"Wrist cuffs", {TEXT("Jewel_CuffL"), TEXT("Jewel_CuffR")}}, {"Ankle shackles", {TEXT("Jewel_ShackleL"), TEXT("Jewel_ShackleR")}}};
	constexpr int32 CreatorJewelCount = UE_ARRAY_COUNT(CreatorJewels);
	constexpr int32 CreatorEarJewels = 6;  // the first six are in the ears (none on the cat kind: no human ears)
	const char* const CreatorMetalNames[] = {"Gold", "Silver", "Steel", "Iron", "Copper", "Black"};
	const FLinearColor CreatorMetalColours[] = {FLinearColor(1.0f, 0.77f, 0.34f), FLinearColor(0.95f, 0.93f, 0.88f), FLinearColor(0.56f, 0.57f, 0.58f),
		FLinearColor(0.33f, 0.32f, 0.31f), FLinearColor(0.95f, 0.64f, 0.54f), FLinearColor(0.04f, 0.04f, 0.04f)};

	// Markings (tools/makehuman/markings.py): the atlas's 16 designs, the kind of mark each makes (0 tattoo ink, 1 scar,
	// 2 brand, 3 paint), and where they go (areas.json beside each base, from mh_export.py).
	const char* const CreatorMarkNames[16] = {"Tribal band", "Tribal flame", "Mandala", "Star", "Script", "Spiral", "Runes", "Dot ring",
		"Scar: slash", "Scar: claws", "Scar: burn", "Scar: stitched", "Brand", "Paint: stripes", "Paint: dots", "Paint: hand"};
	int32 CreatorMarkKind(int32 Design) { return Design < 8 ? 0 : (Design < 12 ? 1 : (Design == 12 ? 2 : 3)); }
	struct FCreatorMark
	{
		bool bOn = false;
		int32 Design = 0;
		FString Area = TEXT("UpperArmL");
		float Size = 12.0f;        // cm across
		float Angle = 0.0f;        // degrees
		float Opacity = 0.9f;
		float Colour[3] = {0.08f, 0.10f, 0.14f};  // ink; paint colour for paint
	};
	struct FCreatorArea
	{
		float U = 0.5f, V = 0.5f, Angle = 0.0f, Scale = 1.0f, ScaleAcross = 1.0f;
	};
	/** The marking areas of a base (areas.json), read once. */
	const TMap<FString, FCreatorArea>& CreatorAreas(const FString& Base)
	{
		static TMap<FString, TMap<FString, FCreatorArea>> Cache;
		if (const TMap<FString, FCreatorArea>* Found = Cache.Find(Base))
		{
			return *Found;
		}
		TMap<FString, FCreatorArea>& Areas = Cache.Add(Base);
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (FFileHelper::LoadFileToString(Text, *(FPaths::ProjectSavedDir() / TEXT("MakeHuman") / Base / TEXT("areas.json"))) &&
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Json->Values)
			{
				const TSharedPtr<FJsonObject> A = Entry.Value->AsObject();
				FCreatorArea Area;
				Area.U = A->GetNumberField(TEXT("u"));
				Area.V = A->GetNumberField(TEXT("v"));
				Area.Angle = A->GetNumberField(TEXT("angle"));
				Area.Scale = A->GetNumberField(TEXT("scale"));
				Area.ScaleAcross = A->HasField(TEXT("scale_across")) ? A->GetNumberField(TEXT("scale_across")) : Area.Scale;
				Areas.Add(Entry.Key, Area);
			}
		}
		return Areas;
	}

	// Scalp hair styles: each a mesh on the bases (MPFB hairs, creator.json "hairs"), found by its material slot's name;
	// the creator shows one (or none: bald).
	struct FCreatorHairStyle
	{
		const char* Label;
		const TCHAR* Key;  // in the slot name
	};
	const FCreatorHairStyle CreatorHairStyles[] = {
		{"Bald", nullptr}, {"Long", TEXT("long01")}, {"Bob", TEXT("bob02")}, {"Ponytail", TEXT("ponytail01")}, {"Braid", TEXT("braid01")},
		{"Short", TEXT("short02")}, {"Crop", TEXT("short04")}, {"Messy", TEXT("short_messy")}, {"Afro", TEXT("afro")}};
	bool CreatorIsHairSlot(const FString& Slot)
	{
		for (const FCreatorHairStyle& Style : CreatorHairStyles)
		{
			if (Style.Key && Slot.Contains(Style.Key))
			{
				return true;
			}
		}
		return false;
	}

	// The face: MakeHuman's 34 expression units, each a morph Face_<unit> on the creator bases (mh_creator.py), and
	// expressions made of them (unit index, weight).
	constexpr int32 CreatorUnitCount = 34;
	const TCHAR* const CreatorUnits[CreatorUnitCount] = {TEXT("EyeLeftClosure"), TEXT("EyeLeftOpenedUp"), TEXT("EyeLeftSlit"), TEXT("EyeRightClosure"), TEXT("EyeRightOpenedUp"), TEXT("EyeRightSlit"), TEXT("EyebrowsLeftDown"), TEXT("EyebrowsLeftExternUp"), TEXT("EyebrowsLeftInnerUp"), TEXT("EyebrowsLeftUp"), TEXT("EyebrowsRightDown"), TEXT("EyebrowsRightExternUp"), TEXT("EyebrowsRightInnerUp"), TEXT("EyebrowsRightUp"), TEXT("MouthCompression"), TEXT("MouthCornerPuller"), TEXT("MouthDepression"), TEXT("MouthDepressionRetraction"), TEXT("MouthElevation"), TEXT("MouthEversion"), TEXT("MouthOpen"), TEXT("MouthParling"), TEXT("MouthPartLater"), TEXT("MouthProtusion"), TEXT("MouthPursing"), TEXT("MouthRetraction"), TEXT("MouthUpwardRetraction"), TEXT("NeckPlatysma"), TEXT("NoseCompression"), TEXT("NoseDepression"), TEXT("NoseLeftDilatation"), TEXT("NoseLeftElevation"), TEXT("NoseRightDilatation"), TEXT("NoseRightElevation")};
	struct FCreatorExpression
	{
		const char* Name;
		TArray<TPair<int32, float>> Units;
	};
	const FCreatorExpression CreatorExpressions[] = {
		{"Neutral", {}},
		{"Smile", {{15, 0.80f}, {18, 0.20f}, {2, 0.25f}, {5, 0.25f}, {31, 0.10f}, {33, 0.10f}}},
		// A natural smile showing the teeth (sneed): the corners up, the upper lip lifted off the teeth, the lips a little
		// apart and the mouth 10 to 15% wider, the eyes narrowing with it.
		{"Smile (teeth)", {{15, 0.85f}, {26, 0.35f}, {20, 0.12f}, {25, 0.15f}, {2, 0.30f}, {5, 0.30f}}},
		{"Grin", {{15, 0.95f}, {26, 0.25f}, {20, 0.15f}, {25, 0.28f}, {2, 0.40f}, {5, 0.40f}}},
		{"Frown", {{16, 0.60f}, {6, 0.70f}, {10, 0.70f}, {28, 0.30f}}},
		{"Sad", {{16, 0.55f}, {8, 0.65f}, {12, 0.65f}, {6, 0.15f}, {10, 0.15f}, {2, 0.20f}, {5, 0.20f}, {19, 0.15f}}},
		{"Angry", {{6, 1.00f}, {10, 1.00f}, {31, 0.50f}, {33, 0.50f}, {14, 0.50f}, {25, 0.30f}, {2, 0.30f}, {5, 0.30f}, {27, 0.20f}}},
		{"Surprised", {{9, 0.80f}, {13, 0.80f}, {8, 0.20f}, {12, 0.20f}, {1, 0.70f}, {4, 0.70f}, {20, 0.45f}}},
		{"Afraid", {{8, 0.80f}, {12, 0.80f}, {9, 0.40f}, {13, 0.40f}, {1, 0.80f}, {4, 0.80f}, {25, 0.50f}, {20, 0.25f}, {27, 0.60f}}},
		{"Disgusted", {{31, 0.70f}, {33, 0.70f}, {26, 0.35f}, {16, 0.25f}, {6, 0.40f}, {10, 0.40f}, {2, 0.30f}, {5, 0.30f}}},
		// Pain: eyes squeezed, brows drawn down and in with the inner ends raised, teeth clenched behind lips pulled
		// back and a little apart, the neck tight.
		{"In pain", {{0, 0.65f}, {3, 0.65f}, {6, 0.55f}, {10, 0.55f}, {8, 0.40f}, {12, 0.40f}, {28, 0.35f}, {25, 0.55f}, {16, 0.30f}, {20, 0.12f}, {27, 0.45f}, {31, 0.25f}, {33, 0.25f}}},
		// Pout: the lower lip pushed out, the corners down a little, the brows' inner ends up.
		{"Pout", {{19, 0.60f}, {16, 0.35f}, {23, 0.20f}, {8, 0.30f}, {12, 0.30f}}},
		// Lips rounded and pushed forward (sipping, sucking, whistling): what "pout" was.
		{"Sip", {{24, 0.80f}, {23, 0.60f}}},
		{"Asleep", {{0, 1.00f}, {3, 1.00f}, {20, 0.05f}}},
	};
	enum { CreatorUnitEyeLeftClosure = 0, CreatorUnitEyeRightClosure = 3,
		CreatorUnitMouthOpen = 20, CreatorUnitMouthPursing = 24 };
	const char* const CreatorKindNames[] = {"Baseline", "Imp", "Orcoid", "Beast-Strain (cat)", "Drow"};
	const char* const CreatorPubicNames[] = {"None", "Natural", "Trimmed", "Strip"};

	/** One character in the creator: its gender (which base), kind, slider values, and its colonist. */
	struct FCreatorPerson
	{
		colony::Entity Thing = colony::kNoEntity;
		FSimPawnLooks Looks;      // what the colonist is drawn with (its base)
		bool bFemale = true;
		ECreatorKind Kind = ECreatorKind::Baseline;
		float Height = 1.0f;      // scale of the mesh
		float Sex = 0.0f;         // 0 .. 1 towards the other sex (Masculine on the woman's base, Feminine on the man's)
		float Older = 0.0f;
		float Weight = 0.0f;      // -1 thin .. 1 heavy
		float Muscle = 0.0f;      // -1 soft .. 1 muscular
		float Breasts = 0.0f;     // -1 small .. 1 large (woman's base)
		float ImpEars = 0.0f;
		float ImpFace = 0.0f;
		float OrcFace = 0.0f;
		float OrcBody = 0.0f;
		bool bShaved = false;     // pubic stubble only
		float Horns = 0.0f;
		float Tusks = 0.0f;
		float Tail = 0.0f;
		float Genital = 0.0f;     // -1 smaller .. 1 larger (the man's genitals, the woman's labia)
		float Pigment = 0.5f;     // 0 .. 1: how much darker the areolae and genitals are than the skin
		int32 Pubic = 1;          // 0 none, else grown pubic hair with the four controls below
		float PubicCoverage = 1.0f;
		float PubicDensity = 1.0f;
		float PubicLength = 1.0f;  // 1 natural .. 0.35 trimmed (PubicShort)
		float PubicCurl = 1.0f;    // 1 curly .. 0 straighter (PubicStraight)
		bool bJiggle = true;
		bool Jewels[CreatorJewelCount] = {};  // none by default (never generated)
		int32 Metal[CreatorJewelCount] = {};
		FCreatorMark Marks[4];     // tattoos, scars, brands, paint: only what the player picks (never generated)
		int32 HairStyle = -1;      // CreatorHairStyles; -1: the base's first (long for the woman, short for the man)
		int32 Expression = 0;      // CreatorExpressions
		float ExpressionStrength = 1.0f;
		float Units[CreatorUnitCount] = {};  // fine tuning on top of the expression
		bool bBlink = true;
		bool bTalk = false;
		float FaceTime = 0.0f;
		float NextBlink = 2.0f;
		float PubicLow = 0.0f;
		float Skin[3] = {0.91f, 0.80f, 0.75f};
		float Hair[3] = {0.10f, 0.08f, 0.07f};
		bool bOwnPubicColour = false;  // else a little darker than the head hair, as is usual
		float PubicHair[3] = {0.07f, 0.055f, 0.05f};
		// Beast-Strain fur (cat ears and tail): the body hair's colour with a shade of its own, or a colour of its own.
		float FurShade = 0.25f;        // -1 darker .. 1 lighter than the hair
		float Wet = 0.0f;              // 0 dry .. 1 soaked (skin, hair and jewellery darker and glossier)
		float ElfEars = 0.0f;          // long pointed ears (Drow)
		float DrowTone = 0.35f;        // along CreatorDrowSkins, light .. obsidian
		bool bOwnIris = false;         // the eyes' own iris colour, else the base's
		float Iris[3] = {0.8f, 0.05f, 0.04f};
		float IrisStrength = 1.0f;     // how strongly the colour shows (Drow: faint to vivid red)
		float BreastJiggle = 0.0f;     // 0 the default (firm) .. 1 looser and bouncier
		int32 EarCover = 0;            // headgear test: 0 none, 1 covers the top of the ears, 2 covers them fully
		bool bOwnFurColour = false;
		float Fur[3] = {0.55f, 0.36f, 0.2f};

		FString Label(int32 Index) const
		{
			return FString::Printf(TEXT("%d %s %s"), Index + 1, bFemale ? TEXT("woman") : TEXT("man"), UTF8_TO_TCHAR(CreatorKindNames[static_cast<int32>(Kind)]));
		}
	};

	struct FCreatorState
	{
		TArray<FCreatorPerson> People;
		int32 Selected = 0;
		float CameraYaw = 180.0f; // the free view: looking along this yaw (180: from +X, in front of them) ...
		float CameraPitch = -8.0f; // ... and pitch, round a point by the picked one moved by FocusOffset
		FVector FocusOffset = FVector::ZeroVector;
		FVector2D LastMouse = FVector2D::ZeroVector;
		bool bHadMouse = false;
		bool bUiWantsMouse = false;
		bool bUiWantsKeys = false;
		bool bFaced = false;
		bool b4KSkin = false;      // the 4K skin, to compare with the 2K one
		bool bAutoPlayed = false;  // -CreatorPlay done
		// The animation panel: the clips made by the MakeHumanArt commandlet (-Clips), what plays, how.
		TArray<FSoftObjectPath> Clips;
		TArray<FString> ClipNames;
		TArray<FString> ClipCategories;  // the folder under Animations/<set>/ (Mixamo packs and groups), or "Mocap"
		char ClipFilter[64] = {};
		bool bClipsListed = false;
		int32 Clip = 0;
		bool bLoop = true;
		bool bEveryone = false;
		float ClipSpeed = 1.0f;
		TArray<int32> Playing;     // per person: the clip it plays, or -1
		int32 IdleClip = -1;       // standing still: a seamless breathing idle, looped (not the game's idle, which restarts)
		float DemoTime = -1.0f;   // -CreatorDemo: seconds into the self-running demo
		int32 Recorded = 0;
		int32 PanelFrames = 0;
		int32 BaseSwitches = 0;
		float CameraDistance = 420.0f;
		bool bWalk = false;
		float LegTime = 99.0f;
		int32 Leg = 0;
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		float LargestTailSwing = 0.0f;
		float LargestJiggle = 0.0f;
		float DeepestDent = 0.0f;
		// Soft-body tests (docs/design/soft-body.md): a blast 3 m in front of the picked one, a slap on its belly.
		float BlastEvery = 0.0f;   // -CreatorBlast=<seconds>: a blast that often (for clips)
		float BlastTimer = 0.0f;
		bool bBlastNow = false;
		bool bSlapNow = false;
		int32 Blasts = 0;
		float SoftDemoT = -1.0f;   // -CreatorSoftDemo: seconds into the soft-body showcase
		int32 SoftDemoStep = 0;
		TWeakObjectPtr<AActor> SoftBall;  // the soft-body demo's pressing ball
		float HairDemoT = -1.0f;   // -CreatorHairDemo: seconds into the long hair showcase
		int32 HairDemoStep = 0;
		int32 HairDemoShot = 0;
		float HairDeepest[3] = {0.0f, 0.0f, 0.0f};          // per part of the demo: hair into the body, cm
		float HairAnimatedDeepest[3] = {0.0f, 0.0f, 0.0f};  // the same hair as animated (no physics)
		int32 SoftPressFrames = 0;      // frames in which the ball's press reached the skin table
	};

	const FVector CreatorOrigin(-1695000.0, 4800.0, 0.0);

	/** The soft-body demo's ball (5 cm, a fist's size) pressing into Which (0 belly, 1 cheek, 2 thigh, -1 none) of the picked
	 *  one, Phase 0..1: in from 6 cm off the skin to 4 cm into it and back out. It is shown as a grey sphere and handed to
	 *  USoftContactSubsystem as its probe. */
	void CreatorSoftPress(FCreatorState& S, const FCreatorPerson& P, USimPawnSubsystem* Pawns, int32 Which, float Phase)
	{
		UWorld* World = Pawns ? Pawns->GetWorld() : nullptr;
		USoftContactSubsystem* Contacts = World ? World->GetSubsystem<USoftContactSubsystem>() : nullptr;
		const ASimPawnActor* Pawn = Pawns ? Pawns->FindPawn(P.Thing) : nullptr;
		const USkeletalMeshComponent* Mesh = Pawn ? Pawn->GetMakeHumanMesh() : nullptr;
		if (!Contacts || !Mesh || Which < 0)
		{
			if (Contacts)
			{
				Contacts->SetProbe(FVector::ZeroVector, FVector::ZeroVector, 0.0f);
			}
			if (AActor* Ball = S.SoftBall.Get())
			{
				Ball->SetActorHiddenInGame(true);
			}
			S.CameraYaw = 215.0f;
			return;
		}
		const float Radius = 5.0f;
		const FVector Fwd = Pawn->GetActorForwardVector();
		const FVector Right = Pawn->GetActorRightVector();
		FVector Surface;
		if (Which == 0)
		{
			// The belly bone sits 7 cm inside the skin (mh_common.add_body_soft_bones).
			Surface = Mesh->GetBoneIndex(TEXT("belly")) != INDEX_NONE ? Mesh->GetSocketLocation(TEXT("belly")) + Fwd * 7.0f : Mesh->GetSocketLocation(TEXT("spine_01")) + Fwd * 15.0f;
		}
		else if (Which == 1)
		{
			// The cheek: in front of and to the side of the head bone (which sits low in the skull, at the top of the neck).
			Surface = Mesh->GetSocketLocation(TEXT("head")) - FVector::UpVector * 3.0f + Fwd * 9.0f + Right * 4.5f;  // 7 cm up landed on the brow
		}
		else
		{
			// The front of the thigh: its soft bone lies on the thigh's axis, about 8 cm under the skin.
			Surface = (Mesh->GetBoneIndex(TEXT("thigh_soft_l")) != INDEX_NONE ? Mesh->GetSocketLocation(TEXT("thigh_soft_l")) : Mesh->GetSocketLocation(TEXT("thigh_l"))) + Fwd * 8.0f;
		}
		const float In = FMath::Sin(PI * FMath::Clamp(Phase, 0.0f, 1.0f));
		const FVector Centre = Surface + Fwd * (Radius + 6.0f - 10.0f * In);
		Contacts->SetProbe(Centre, Centre, Radius);
		if (Contacts->GetContactsLastFrame() > 0)
		{
			++S.SoftPressFrames;
		}
		static int32 LastLogged = -1;
		if (LastLogged != Which)
		{
			LastLogged = Which;
			UE_LOG(LogTemp, Display, TEXT("Soft body demo: ball pressing %s at %s"), Which == 0 ? TEXT("belly") : (Which == 1 ? TEXT("cheek") : TEXT("thigh")), *Surface.ToString());
		}
		// Seen from the side while it presses, so the skin giving round it shows (from the front the ball hid it).
		S.CameraYaw = 260.0f;
		AActor* Ball = S.SoftBall.Get();
		if (!Ball)
		{
			UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			AStaticMeshActor* Spawned = Sphere ? World->SpawnActor<AStaticMeshActor>(Centre, FRotator::ZeroRotator) : nullptr;
			if (Spawned)
			{
				Spawned->SetMobility(EComponentMobility::Movable);
				Spawned->GetStaticMeshComponent()->SetStaticMesh(Sphere);
				Spawned->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Spawned->SetActorScale3D(FVector(Radius / 50.0));  // the engine sphere is 100 cm across
				S.SoftBall = Spawned;
				Ball = Spawned;
			}
		}
		if (Ball)
		{
			Ball->SetActorHiddenInGame(false);
			Ball->SetActorLocation(Centre);
		}
	}

	void CreatorSetSkin(FCreatorPerson& P, float R, float G, float B)
	{
		P.Skin[0] = R;
		P.Skin[1] = G;
		P.Skin[2] = B;
	}

	/** Drow skin: the colour at the person's DrowTone along CreatorDrowSkins. */
	void CreatorDrowSkin(FCreatorPerson& P)
	{
		const float T = FMath::Clamp(P.DrowTone, 0.0f, 1.0f) * (UE_ARRAY_COUNT(CreatorDrowSkins) - 1);
		const int32 K = FMath::Min(static_cast<int32>(T), static_cast<int32>(UE_ARRAY_COUNT(CreatorDrowSkins)) - 2);
		const FLinearColor C = FMath::Lerp(CreatorDrowSkins[K], CreatorDrowSkins[K + 1], T - K);
		CreatorSetSkin(P, C.R, C.G, C.B);
	}

	/** A kind's look on top of the person's gender: the inhuman parts, a build and a skin colour. Body sliders reset. */
	void CreatorSetKind(FCreatorPerson& P, ECreatorKind Kind)
	{
		P.Kind = Kind;
		P.Sex = P.Older = P.Breasts = P.Genital = 0.0f;
		P.Pigment = 0.5f;
		P.ImpEars = P.ImpFace = P.OrcFace = P.OrcBody = P.Horns = P.Tusks = P.Tail = P.ElfEars = 0.0f;
		P.bOwnIris = false;
		P.Weight = P.bFemale ? -0.2f : 0.0f;
		P.Muscle = 0.0f;
		P.Height = P.bFemale ? 0.94f : 1.02f;
		if (P.bFemale)
		{
			CreatorSetSkin(P, 0.91f, 0.80f, 0.75f);
			P.Hair[0] = 0.10f, P.Hair[1] = 0.08f, P.Hair[2] = 0.07f;  // black
		}
		else
		{
			CreatorSetSkin(P, 0.85f, 0.70f, 0.60f);
			P.Hair[0] = 0.32f, P.Hair[1] = 0.22f, P.Hair[2] = 0.14f;  // brown
		}
		if (Kind == ECreatorKind::Imp)
		{
			P.Height = P.bFemale ? 0.80f : 0.84f;
			P.Weight = -0.6f;
			P.Muscle = 0.6f;
			P.ImpEars = P.ImpFace = P.Horns = P.Tail = 1.0f;
			CreatorSetSkin(P, 0.72f, 0.17f, 0.12f);
		}
		else if (Kind == ECreatorKind::Drow)
		{
			// Long pointed ears, platinum hair, red irises, skin from the Drow range.
			P.Height = P.bFemale ? 0.97f : 1.02f;
			P.Weight = -0.3f;
			P.Muscle = 0.3f;
			P.ElfEars = 1.0f;
			P.DrowTone = 0.35f;
			CreatorDrowSkin(P);
			P.Hair[0] = 0.92f, P.Hair[1] = 0.91f, P.Hair[2] = 0.88f;
			P.bOwnIris = true;
			P.Iris[0] = 0.85f, P.Iris[1] = 0.05f, P.Iris[2] = 0.04f;
			P.IrisStrength = 1.0f;
		}
		else if (Kind == ECreatorKind::Orcoid)
		{
			P.Height = P.bFemale ? 1.0f : 1.08f;
			P.Weight = 0.25f;
			P.Muscle = 1.0f;
			P.OrcFace = P.Tusks = P.OrcBody = 1.0f;
			P.ImpEars = 0.45f;
			CreatorSetSkin(P, 0.42f, 0.55f, 0.32f);
		}
	}

	/**
	 * A random baseline human (sneed: every demo and test pawn comes from the creator's randomizer; baseline humans only
	 * for now): build, age and proportions spread round the average, a hair style and a natural hair colour, and a skin
	 * tone weighted very heavily towards fair, with tan levels but never dark.
	 */
	void CreatorRandomize(FCreatorPerson& P, FRandomStream& R)
	{
		CreatorSetKind(P, ECreatorKind::Baseline);
		const auto Gauss = [&R]() { return (R.FRand() + R.FRand() + R.FRand() - 1.5f) * 2.0f; };  // about -3 .. 3, spread 1
		P.Height = (P.bFemale ? 0.94f : 1.02f) * (1.0f + 0.035f * Gauss());
		P.Sex = R.FRandRange(0.0f, 0.15f);
		P.Older = R.FRand() < 0.75f ? 0.0f : R.FRandRange(0.1f, 0.6f);
		P.Weight = FMath::Clamp((P.bFemale ? -0.15f : 0.0f) + 0.32f * Gauss(), -0.8f, 0.9f);
		P.Muscle = FMath::Clamp(0.3f * Gauss(), -0.7f, 0.8f);
		P.Breasts = P.bFemale ? FMath::Clamp(0.4f * Gauss(), -0.9f, 0.9f) : 0.0f;
		P.Pigment = R.FRandRange(0.3f, 0.7f);
		P.PubicDensity = R.FRandRange(0.7f, 1.0f);
		P.PubicCurl = R.FRandRange(0.5f, 1.0f);
		// Skin: fair to tanned, most of them fair (the tone's share cubed), a touch of warmth or rosiness.
		const float Tone = FMath::Pow(R.FRand(), 3.0f);
		const float Warm = R.FRandRange(-0.015f, 0.015f);
		CreatorSetSkin(P, FMath::Lerp(0.96f, 0.80f, Tone), FMath::Lerp(0.86f, 0.62f, Tone) + Warm, FMath::Lerp(0.80f, 0.50f, Tone) - Warm);
		// Hair: natural colours, darker ones commoner.
		struct FShade { float Weight; float C[3]; };
		static const FShade Shades[] = {{30, {0.17f, 0.11f, 0.07f}}, {24, {0.32f, 0.22f, 0.14f}}, {14, {0.07f, 0.06f, 0.055f}}, {12, {0.48f, 0.36f, 0.24f}},
			{10, {0.78f, 0.66f, 0.45f}}, {6, {0.45f, 0.20f, 0.10f}}, {3, {0.62f, 0.27f, 0.12f}}};
		float Pick = R.FRand() * 99.0f;
		for (const FShade& Shade : Shades)
		{
			Pick -= Shade.Weight;
			if (Pick <= 0.0f)
			{
				P.Hair[0] = Shade.C[0], P.Hair[1] = Shade.C[1], P.Hair[2] = Shade.C[2];
				break;
			}
		}
		// Styles each base has (mh_creator's "hairs"): long, bob, ponytail, braid, short for women; short, crop, messy, long,
		// ponytail for men, now and then bald.
		static const int32 Women[] = {1, 1, 2, 3, 4, 5};
		static const int32 Men[] = {5, 5, 6, 7, 1, 3, 0};
		P.HairStyle = P.bFemale ? Women[R.RandRange(0, UE_ARRAY_COUNT(Women) - 1)] : Men[R.RandRange(0, UE_ARRAY_COUNT(Men) - 1)];
	}

	/** The skin material to show: the 2K one (the mesh's own) or the 4K one (MakeHumanArt makes both). */
	UMaterialInterface* CreatorSkinMaterial(const FString& Base, bool b4K)
	{
		static TMap<FString, TWeakObjectPtr<UMaterialInterface>> Cache;
		const FString Name = MakeHumanSkinName(Base) + (b4K ? TEXT("4K") : TEXT(""));
		if (const TWeakObjectPtr<UMaterialInterface>* Found = Cache.Find(Name); Found && Found->IsValid())
		{
			return Found->Get();
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Rim3D/Generated/MakeHuman/%s.%s"), *Name, *Name));
		Cache.Add(Name, Material);
		return Material;
	}

	/** Puts a person's sliders on its mesh: morphs, height, skin colour (and the 2K or 4K skin). */
	void CreatorApply(const FCreatorPerson& P, ASimPawnActor& Pawn, bool b4KSkin)
	{
		USkeletalMeshComponent* Mesh = Pawn.GetMakeHumanMesh();
		if (!Mesh || Pawn.GetLooks().MakeHuman != P.Looks.MakeHuman)
		{
			return;  // its new base is not on yet
		}
		if (UMaterialInterface* Wanted = CreatorSkinMaterial(P.Looks.MakeHuman, b4KSkin))
		{
			// The skin as its own instance (for the markings), made again when the 2K / 4K skin is swapped.
			const TMap<FString, FCreatorArea>& Areas = CreatorAreas(P.Looks.MakeHuman);
			for (int32 i = 0; i < Mesh->GetNumMaterials(); ++i)
			{
				UMaterialInterface* Now = Mesh->GetMaterial(i);
				UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Now);
				const UMaterialInterface* Base = Dynamic ? Dynamic->Parent.Get() : Now;
				if (!Base || !Base->GetName().StartsWith(MakeHumanSkinName(P.Looks.MakeHuman)))
				{
					continue;
				}
				if (!Dynamic || Base != Wanted)
				{
					Dynamic = UMaterialInstanceDynamic::Create(Wanted, Mesh);
					Mesh->SetMaterial(i, Dynamic);
				}
				for (int32 m = 0; m < 4; ++m)
				{
					const FCreatorMark& Mark = P.Marks[m];
					const FCreatorArea* Area = Areas.Find(Mark.Area);
					if (!Mark.bOn || !Area)
					{
						Dynamic->SetVectorParameterValue(*FString::Printf(TEXT("Mark%dB"), m), FLinearColor(0, 0, 0, 0));
						continue;
					}
					// Texture across and up the stamp (the skin's texture stretches differently each way).
					const float Across = Area->ScaleAcross * Mark.Size / 100.0f;
					const float Up = Area->Scale * Mark.Size / 100.0f;
					Dynamic->SetVectorParameterValue(*FString::Printf(TEXT("Mark%dA"), m),
						FLinearColor(Area->U, Area->V, Area->Angle + FMath::DegreesToRadians(Mark.Angle), Across));
					Dynamic->SetVectorParameterValue(*FString::Printf(TEXT("Mark%dB"), m),
						FLinearColor(static_cast<float>(Mark.Design), Mark.Opacity, static_cast<float>(CreatorMarkKind(Mark.Design)), Up));
					Dynamic->SetVectorParameterValue(*FString::Printf(TEXT("Mark%dC"), m),
						FLinearColor::FromSRGBColor(FLinearColor(Mark.Colour[0], Mark.Colour[1], Mark.Colour[2]).ToFColor(false)));
				}
			}
		}
		const auto Set = [Mesh](const TCHAR* Name, float Value) { Mesh->SetMorphTarget(Name, FMath::Clamp(Value, 0.0f, 1.0f), false); };
		Set(P.bFemale ? TEXT("Masculine") : TEXT("Feminine"), P.Sex);
		Set(TEXT("Older"), P.Older);
		Set(TEXT("Heavy"), P.Weight);
		Set(TEXT("Thin"), -P.Weight);
		Set(TEXT("Muscular"), P.Muscle);
		Set(TEXT("Soft"), -P.Muscle);
		Set(TEXT("BreastsLarge"), P.bFemale ? P.Breasts : 0.0f);
		Set(TEXT("BreastsSmall"), P.bFemale ? -P.Breasts : 0.0f);
		// Headgear that covers the ears tucks pointed ears in: fully covered, back to a human ear's size (nothing pokes out
		// of a helmet); only the top covered, their tips pulled down under it.
		const float EarKeep = P.EarCover == 2 ? 0.0f : (P.EarCover == 1 ? 0.35f : 1.0f);
		Set(TEXT("ImpEars"), P.ImpEars * EarKeep);
		Set(TEXT("ElfEars"), P.ElfEars * EarKeep);
		Set(TEXT("ImpFace"), P.ImpFace);
		Set(TEXT("OrcFace"), P.OrcFace);
		Set(TEXT("OrcBody"), P.OrcBody);
		Set(TEXT("NoHorns"), 1.0f - P.Horns);
		Set(TEXT("NoTusks"), 1.0f - P.Tusks);
		Set(TEXT("NoTail"), 1.0f - P.Tail);
		Set(TEXT("CatHead"), P.Kind == ECreatorKind::Beast ? 1.0f : 0.0f);  // no human ears
		Set(TEXT("PubicShort"), P.bShaved ? 0.0f : (1.0f - P.PubicLength) / 0.65f);
		Set(TEXT("PubicStraight"), P.bShaved ? 0.0f : 1.0f - P.PubicCurl);
		Set(TEXT("PubicStubble"), P.bShaved ? 1.0f : 0.0f);
		Mesh->SetCustomPrimitiveDataFloat(4, P.Pubic == 0 ? 0.0f : P.PubicDensity);
		Mesh->SetCustomPrimitiveDataFloat(5, P.PubicCoverage);
		Mesh->SetCustomPrimitiveDataFloat(12, P.Wet);  // MakeHumanArt's MhAddWetness on every material
		if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()))
		{
			Anim->SetSoftTissueEnabled(P.bJiggle);
			Anim->SetBreastJiggle(P.BreastJiggle);
			Anim->SetBodySoftness(P.Weight, P.Muscle, P.Older);
		}
		Set(TEXT("GenitalLarge"), P.Genital);
		Set(TEXT("GenitalSmall"), -P.Genital);
		Mesh->SetCustomPrimitiveDataFloat(3, P.Pigment);
		// The face: the expression, the fine tuning, a blink now and then, talking.
		float Face[CreatorUnitCount];
		for (int32 u = 0; u < CreatorUnitCount; ++u)
		{
			Face[u] = P.Units[u];
		}
		for (const TPair<int32, float>& Unit : CreatorExpressions[FMath::Clamp(P.Expression, 0, static_cast<int32>(UE_ARRAY_COUNT(CreatorExpressions)) - 1)].Units)
		{
			Face[Unit.Key] += Unit.Value * P.ExpressionStrength;
		}
		if (P.bBlink && P.NextBlink - P.FaceTime < 0.14f && P.NextBlink > P.FaceTime)
		{
			Face[CreatorUnitEyeLeftClosure] = Face[CreatorUnitEyeRightClosure] = 1.0f;
		}
		if (P.bTalk)
		{
			const float T = P.FaceTime;
			Face[CreatorUnitMouthOpen] += FMath::Max(0.0f, 0.22f * FMath::Sin(T * 13.0f) + 0.12f * FMath::Sin(T * 7.3f + 1.0f) + 0.05f);
			Face[CreatorUnitMouthPursing] += FMath::Max(0.0f, 0.25f * FMath::Sin(T * 5.1f + 2.0f));
		}
		for (int32 u = 0; u < CreatorUnitCount; ++u)
		{
			Set(*(FString(TEXT("Face_")) + CreatorUnits[u]), Face[u]);
		}
		// Wrinkles by how far each group's units are on: forehead (brows up), brow furrow (brows down), smile (mouth
		// corners, squint), nose (scrunch). The skin material reads them as custom primitive data 8 to 11.
		const auto U = [&Face](const TCHAR* Name)
		{
			for (int32 u = 0; u < CreatorUnitCount; ++u)
			{
				if (FCString::Strcmp(CreatorUnits[u], Name) == 0)
				{
					return FMath::Clamp(Face[u], 0.0f, 1.0f);
				}
			}
			return 0.0f;
		};
		// The forehead creases run across the whole brow: full for a brow raise, less for the outer ends, little for the
		// inner ends (sad, afraid: across the whole forehead it looked wrong), and all a little softer.
		const float Forehead = 0.8f * FMath::Max3(FMath::Max(U(TEXT("EyebrowsLeftUp")), U(TEXT("EyebrowsRightUp"))),
			0.35f * FMath::Max(U(TEXT("EyebrowsLeftInnerUp")), U(TEXT("EyebrowsRightInnerUp"))), 0.7f * FMath::Max(U(TEXT("EyebrowsLeftExternUp")), U(TEXT("EyebrowsRightExternUp"))));
		const float Furrow = FMath::Max3(U(TEXT("EyebrowsLeftDown")), U(TEXT("EyebrowsRightDown")), U(TEXT("NoseCompression")));
		const float Smile = FMath::Max3(U(TEXT("MouthCornerPuller")), 0.7f * FMath::Max(U(TEXT("EyeLeftSlit")), U(TEXT("EyeRightSlit"))), U(TEXT("MouthUpwardRetraction")));
		const float Nose = FMath::Max3(U(TEXT("NoseLeftElevation")), U(TEXT("NoseRightElevation")), U(TEXT("NoseDepression")));
		Mesh->SetCustomPrimitiveDataVector4(8, FVector4(Forehead, Furrow, Smile, Nose));
		Mesh->SetRelativeScale3D(FVector(P.Height));
		// Hair colours: head hair and brows the picked colour (brows a touch darker), pubic hair a little darker than the head
		// hair unless it has its own. The hair materials' Tint multiplies their texture.
		const auto Linear = [](const float* C) { return FLinearColor::FromSRGBColor(FLinearColor(C[0], C[1], C[2]).ToFColor(false)); };
		const float PubicDefault[3] = {P.Hair[0] * 0.72f, P.Hair[1] * 0.70f, P.Hair[2] * 0.70f};
		const FLinearColor HairColour = Linear(P.Hair);
		const FLinearColor PubicColour = Linear(P.bOwnPubicColour ? P.PubicHair : PubicDefault);
		const TArray<FName> Slots = Mesh->GetMaterialSlotNames();
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			const FString Slot = Slots[i].ToString();
			const bool bPubic = Slot.Contains(TEXT("Pubic"));
			const bool bBrow = Slot.Contains(TEXT("eyebrow"));
			const bool bHead = !bPubic && !Slot.Contains(TEXT("lash")) && CreatorIsHairSlot(Slot);
			if (!bPubic && !bBrow && !bHead)
			{
				continue;
			}
			UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
			if (!Dynamic)
			{
				Dynamic = Mesh->CreateDynamicMaterialInstance(i);
			}
			if (Dynamic)
			{
				Dynamic->SetVectorParameterValue(TEXT("Tint"), bPubic ? PubicColour : (bBrow ? HairColour * 0.8f : HairColour));
			}
		}
		// Only the chosen jewellery shows, in its metal.
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			const FString Slot = Slots[i].ToString();
			if (!Slot.Contains(TEXT("Jewel_")))
			{
				continue;
			}
			bool bShow = false;
			int32 MetalIndex = 0;
			for (int32 j = 0; j < CreatorJewelCount; ++j)
			{
				for (const TCHAR* Key : CreatorJewels[j].Keys)
				{
					if (Key && Slot.EndsWith(Key))
					{
						bShow = P.Jewels[j] && !(j < CreatorEarJewels && P.Kind == ECreatorKind::Beast);  // ear piercings: no human ears
						MetalIndex = P.Metal[j];
					}
				}
			}
			Mesh->ShowMaterialSection(i, 0, bShow, 0);
			if (bShow)
			{
				UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
				if (!Dynamic)
				{
					Dynamic = Mesh->CreateDynamicMaterialInstance(i);
				}
				if (Dynamic)
				{
					Dynamic->SetVectorParameterValue(TEXT("Tint"), CreatorMetalColours[FMath::Clamp(MetalIndex, 0, 5)]);
					Dynamic->SetScalarParameterValue(TEXT("Roughness"), MetalIndex == 3 ? 0.55f : (MetalIndex == 5 ? 0.35f : 0.22f));
				}
			}
		}
		// Eyes: the iris in the person's own colour (Drow red), or the base's.
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			const FString Slot = Slots[i].ToString();
			if (!Slot.Contains(TEXT("high_poly")) && !Slot.Contains(TEXT("low_poly")))
			{
				continue;
			}
			UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
			if (!Dynamic)
			{
				Dynamic = Mesh->CreateDynamicMaterialInstance(i);
			}
			if (Dynamic)
			{
				Dynamic->SetVectorParameterValue(TEXT("IrisColour"), Linear(P.Iris));
				Dynamic->SetScalarParameterValue(TEXT("IrisAmount"), P.bOwnIris ? FMath::Clamp(P.IrisStrength, 0.0f, 1.0f) : 0.0f);
			}
		}
		// Beast-Strain parts (cat ears, claws, the furred tail) only on that kind; the fur in the hair's colour, shaded.
		{
			const bool bBeast = P.Kind == ECreatorKind::Beast;
			const float Shade = P.FurShade;
			const float FurShaded[3] = {
				FMath::Clamp(Shade >= 0.0f ? FMath::Lerp(P.Hair[0], 0.95f, Shade * 0.6f) : P.Hair[0] * (1.0f + Shade * 0.6f), 0.0f, 1.0f),
				FMath::Clamp(Shade >= 0.0f ? FMath::Lerp(P.Hair[1], 0.85f, Shade * 0.6f) : P.Hair[1] * (1.0f + Shade * 0.6f), 0.0f, 1.0f),
				FMath::Clamp(Shade >= 0.0f ? FMath::Lerp(P.Hair[2], 0.72f, Shade * 0.6f) : P.Hair[2] * (1.0f + Shade * 0.6f), 0.0f, 1.0f)};
			const FLinearColor FurColour = Linear(P.bOwnFurColour ? P.Fur : FurShaded);
			const float Inner[3] = {FMath::Lerp(P.Skin[0], 0.92f, 0.45f), FMath::Lerp(P.Skin[1], 0.62f, 0.45f), FMath::Lerp(P.Skin[2], 0.62f, 0.45f)};
			for (int32 i = 0; i < Slots.Num(); ++i)
			{
				const FString Slot = Slots[i].ToString();
				const bool bFur = Slot.Contains(TEXT("CatFur"));
				const bool bInner = Slot.Contains(TEXT("CatEarInner"));
				if (!bFur && !bInner && !Slot.Contains(TEXT("CatClaw")))
				{
					continue;
				}
				Mesh->ShowMaterialSection(i, 0, bBeast, 0);
				if (bBeast && (bFur || bInner))
				{
					UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
					if (!Dynamic)
					{
						Dynamic = Mesh->CreateDynamicMaterialInstance(i);
					}
					if (Dynamic)
					{
						Dynamic->SetVectorParameterValue(TEXT("Tint"), bFur ? FurColour : Linear(Inner));
					}
				}
			}
		}
		// -CreatorGroom=<GA_Hair_...>: strand hair (a MetaHuman groom fitted to this head) instead of the hair meshes.
		FString GroomName;
		const bool bGroom = FParse::Value(FCommandLine::Get(), TEXT("CreatorGroom="), GroomName) && !GroomName.IsEmpty();
		if (bGroom)
		{
			UGroomComponent* Existing = nullptr;
			TArray<UGroomComponent*> Grooms;
			Pawn.GetComponents(Grooms);
			for (UGroomComponent* Candidate : Grooms)
			{
				Existing = Candidate->GetFName() == TEXT("StrandHair") ? Candidate : Existing;
			}
			StrandHairAttach(Mesh, StrandHairFind(GroomName), Existing);
		}
		// Only the chosen hair style shows (none under strand hair).
		const int32 Style = bGroom ? 0 : (P.HairStyle >= 0 ? P.HairStyle : (P.bFemale ? 1 : 5));
		const FCreatorHairStyle& Chosen = CreatorHairStyles[FMath::Clamp(Style, 0, static_cast<int32>(UE_ARRAY_COUNT(CreatorHairStyles)) - 1)];
		TArray<int32> WigSections;
		for (int32 i = 0; i < Slots.Num(); ++i)
		{
			const FString Slot = Slots[i].ToString();
			if (CreatorIsHairSlot(Slot))
			{
				const bool bShown = Chosen.Key && Slot.Contains(Chosen.Key);
				Mesh->ShowMaterialSection(i, 0, bShown, 0);
				if (bShown)
				{
					WigSections.Add(i);
				}
			}
		}
		// Our own strand hair for that style on people near the camera (the wig further off): docs/design/hair-physics.md
		// "Near and far". The MetaHuman groom test above replaces it.
		if (UStrandHairSubsystem* Strands = Pawn.GetWorld() ? Pawn.GetWorld()->GetSubsystem<UStrandHairSubsystem>() : nullptr)
		{
			FString Base = Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetName() : FString();
			Base.RemoveFromStart(TEXT("SK_MH_"));
			Strands->Want(Mesh, Base, bGroom || !Chosen.Key ? FString() : FString(UTF8_TO_TCHAR(Chosen.Label)), HairColour, WigSections);
		}
		// The panel's colour is as seen (sRGB); the skin material takes it linear, as FSimPawnLooks::Skin is.
		const FLinearColor Skin = FLinearColor::FromSRGBColor(FLinearColor(P.Skin[0], P.Skin[1], P.Skin[2]).ToFColor(false));
		Mesh->SetCustomPrimitiveDataVector3(PawnLooksSkinDataIndex, FVector(Skin.R, Skin.G, Skin.B));
	}

	/** The game's animation clips (assets/animations.json, made by the MakeHumanArt commandlet with -Clips): A_<name>. */
	void CreatorListClips(FCreatorState& State)
	{
		State.bClipsListed = true;
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FString Folder = TEXT("/Game/Rim3D/Generated/Animations");
		Registry.ScanPathsSynchronous({Folder}, true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			const FString Name = Asset.AssetName.ToString();
			if (Asset.AssetClassPath == UAnimSequence::StaticClass()->GetClassPathName() && Name.StartsWith(TEXT("A_")) && !Name.StartsWith(TEXT("A_SK_")))
			{
				// Animations/<clip>/A_<clip> (single clips) or Animations/<set>/<category>/A_<name> (sets).
				TArray<FString> Parts;
				Asset.PackagePath.ToString().Mid(Folder.Len() + 1).ParseIntoArray(Parts, TEXT("/"));
				State.Clips.Add(Asset.GetSoftObjectPath());
				FString Shown = Name.Mid(2);
				Shown.RemoveFromEnd(TEXT("_Anim"));
				State.ClipNames.Add(Shown);
				State.ClipCategories.Add(Parts.Num() >= 2 ? Parts.Last() : TEXT("Mocap"));
			}
		}
		// In order: category, then name.
		TArray<int32> Order;
		for (int32 i = 0; i < State.Clips.Num(); ++i)
		{
			Order.Add(i);
		}
		Order.Sort([&State](int32 A, int32 B)
		{
			return State.ClipCategories[A] != State.ClipCategories[B] ? State.ClipCategories[A] < State.ClipCategories[B] : State.ClipNames[A] < State.ClipNames[B];
		});
		TArray<FSoftObjectPath> Clips;
		TArray<FString> Names, Categories;
		for (const int32 i : Order)
		{
			Clips.Add(State.Clips[i]);
			Names.Add(State.ClipNames[i]);
			Categories.Add(State.ClipCategories[i]);
		}
		State.Clips = MoveTemp(Clips);
		State.ClipNames = MoveTemp(Names);
		State.ClipCategories = MoveTemp(Categories);
	}

	/** Plays a clip on a person's body (its mannequin, which every body follows), or stops it (Clip -1). */
	void CreatorPlay(FCreatorState& State, int32 Person, int32 Clip, ASimPawnActor* Pawn, bool bAlwaysLoop = false)
	{
		USimPawnAnimInstance* Anim = Pawn ? Pawn->GetAnim() : nullptr;
		if (!Anim)
		{
			return;
		}
		const int32 Before = State.Playing.Num();
		State.Playing.SetNum(State.People.Num());
		for (int32 i = Before; i < State.Playing.Num(); ++i)
		{
			State.Playing[i] = -1;
		}
		Anim->StopSlotAnimation(0.25f, FAnimSlotGroup::DefaultSlotName);
		State.Playing[Person] = -1;
		if (!State.Clips.IsValidIndex(Clip))
		{
			return;
		}
		if (UAnimSequence* Sequence = Cast<UAnimSequence>(State.Clips[Clip].TryLoad()))
		{
			Anim->PlaySlotAnimationAsDynamicMontage(Sequence, FAnimSlotGroup::DefaultSlotName, 0.25f, 0.25f, bAlwaysLoop ? 1.0f : State.ClipSpeed, State.bLoop || bAlwaysLoop ? 100000 : 1);
			State.Playing[Person] = Clip;
		}
	}

	/** Gender: the person's base (the woman's or the man's); its kind and sliders stay. */
	void CreatorSetGender(FCreatorPerson& P, bool bFemale, USimPawnSubsystem& Pawns)
	{
		P.bFemale = bFemale;
		P.HairStyle = -1;
		P.Looks.bFemale = bFemale;
		P.Looks.MakeHuman = bFemale ? TEXT("CreatorFemale") : TEXT("CreatorMale");
		Pawns.SetLooksOverride(P.Thing, P.Looks);
	}
}

void USmokeTestSubsystem::BuildCreatorPhases()
{
	const TSharedRef<FCreatorState> State = MakeShared<FCreatorState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };

	FPhase Play;
	Play.Name = TEXT("creator");
	Play.WarmupSeconds = 6.0f;
	float Minutes = 240.0f;
	FParse::Value(FCommandLine::Get(), TEXT("CreatorMinutes="), Minutes);
	Play.MeasureSeconds = Minutes * 60.0f;
	Play.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			for (const TCHAR* Command : {TEXT("sg.ShadowQuality 4"), TEXT("sg.GlobalIlluminationQuality 4"), TEXT("sg.ReflectionQuality 4"),
				TEXT("sg.PostProcessQuality 4"), TEXT("sg.TextureQuality 4"), TEXT("sg.EffectsQuality 4"), TEXT("sg.ShadingQuality 4"),
				TEXT("sg.AntiAliasingQuality 4"), TEXT("r.ScreenPercentage 100"), TEXT("r.AntiAliasingMethod 4"), TEXT("r.DynamicGlobalIlluminationMethod 1"),
				TEXT("r.ReflectionMethod 1"), TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor 8"), TEXT("r.Shadow.Virtual.Enable 1"),
				TEXT("r.SSS.Quality 1"), TEXT("r.SSS.HalfRes 0"), TEXT("r.SSS.Checkerboard 0"), TEXT("r.ContactShadows 1"), TEXT("Pawns.FullDetail 20"),
				TEXT("Overseer.UI 0"), TEXT("DisableAllScreenMessages"), TEXT("r.SetRes 1920x1080w")})
			{
				PC->ConsoleCommand(Command);
			}
		}
		// The time of day: the morning sun lights them from the front, where the view starts (-CreatorHour= to change).
		float Hour = 7.5f;
		FParse::Value(FCommandLine::Get(), TEXT("CreatorHour="), Hour);
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), Hour),
			TPair<const TCHAR*, float>(TEXT("Sky.DayOfYear"), 22.5f)})
		{
			if (IConsoleVariable* Found = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Found->Set(Var.Value, ECVF_SetByCode);
			}
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(CreatorOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		B->StepOnce();
		State->Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		const colony::CellPos Middle = State->Middle;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - Middle.x) < 24.0f && FMath::Abs(View.y - Middle.y) < 24.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		colony::Sim* Sim = const_cast<colony::Sim*>(B->GetSim());
		for (int32 x = -24; x <= 24; ++x)
		{
			for (int32 y = -24; y <= 24; ++y)
			{
				Sim->plants().remove(colony::CellPos{Middle.x + x, Middle.y + y, 0});
			}
		}
		B->StepOnce();
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TArray<const colony::ThingView*> Colonists;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0)
			{
				Colonists.Add(&View);
			}
		}
		Colonists.Sort([](const colony::ThingView& A, const colony::ThingView& Bv) { return A.id < Bv.id; });
		if (Colonists.Num() < 1)
		{
			Fail(TEXT("the Crashlanded start has no colonists"));
			return;
		}
		// A baseline woman and man, a woman imp and a man orcoid (gender and kind are chosen apart).
		const int32 Count = FMath::Min(4, Colonists.Num());
		const colony::DefDatabase* Defs = B->GetDefs().get();
		for (int32 i = 0; i < Colonists.Num(); ++i)
		{
			if (i >= Count)
			{
				B->Submit(colony::DestroyThingCommand{Colonists[i]->id});
				continue;
			}
			FCreatorPerson Person;
			Person.Thing = Colonists[i]->id;
			Person.bFemale = i == 0 || i == 2;
			// Random baseline humans from the creator's randomizer (seeded: -CreatorSeed=<n>, the same people every run).
			int32 Seed = 42;
			FParse::Value(FCommandLine::Get(), TEXT("CreatorSeed="), Seed);
			FRandomStream PersonRandom(Seed * 7919 + i * 104729);
			CreatorRandomize(Person, PersonRandom);
			Person.Looks = Colonists[i]->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(Colonists[i]->looks)], Defs) : FSimPawnLooks();
			Person.Looks.Height = 1.0f;
			Person.Looks.Skin = FLinearColor(Person.Skin[0], Person.Skin[1], Person.Skin[2]);
			CreatorSetGender(Person, Person.bFemale, *Pawns);
			State->People.Add(Person);
			for (const colony::Entity Item : Sim->apparel().worn(Person.Thing))
			{
				std::string Error;
				Sim->apparel().takeOff(Person.Thing, Item, Error);
				B->Submit(colony::DestroyThingCommand{Item});
			}
			B->Submit(colony::DraftCommand{Person.Thing, true});
			B->Submit(colony::MoveOrderCommand{Person.Thing, colony::CellPos{Middle.x - 1, Middle.y + 6 - 3 + i * 2, 0}});
		}
		B->StepOnce();
		B->SetSpeed(1);
		// For pictures: -CreatorSelect=<0-2>, -CreatorWalk, -CreatorYaw=<degrees>, -CreatorDistance=<cm>.
		FParse::Value(FCommandLine::Get(), TEXT("CreatorSelect="), State->Selected);
		State->bWalk = FParse::Param(FCommandLine::Get(), TEXT("CreatorWalk"));
		FParse::Value(FCommandLine::Get(), TEXT("CreatorBlast="), State->BlastEvery);
		float Yaw = 0.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorYaw="), Yaw))
		{
			State->CameraYaw += Yaw;
		}
		FParse::Value(FCommandLine::Get(), TEXT("CreatorFocusZ="), State->FocusOffset.Z);
		State->b4KSkin = FParse::Param(FCommandLine::Get(), TEXT("Creator4K"));
		// -CreatorJewels=0,3,7 (indices into CreatorJewels) for pictures; -CreatorMetal=<0-5>.
		FString JewelArg;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorJewels="), JewelArg, false))
		{
			int32 MetalArg = 0;
			FParse::Value(FCommandLine::Get(), TEXT("CreatorMetal="), MetalArg);
			TArray<FString> Items;
			JewelArg.ParseIntoArray(Items, TEXT(","));
			for (FCreatorPerson& Person : State->People)
			{
				for (const FString& Item : Items)
				{
					const int32 j = FCString::Atoi(*Item);
					if (j >= 0 && j < CreatorJewelCount)
					{
						Person.Jewels[j] = true;
						Person.Metal[j] = MetalArg;
					}
				}
			}
		}
		FString MarkArg;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorMark="), MarkArg, false))
		{
			// -CreatorMark=design:area[:size],design:area[:size]...
			TArray<FString> Items;
			MarkArg.ParseIntoArray(Items, TEXT(","));
			for (FCreatorPerson& Person : State->People)
			{
				for (int32 m = 0; m < FMath::Min(4, Items.Num()); ++m)
				{
					TArray<FString> Parts;
					Items[m].ParseIntoArray(Parts, TEXT(":"));
					if (Parts.Num() >= 2)
					{
						Person.Marks[m].bOn = true;
						Person.Marks[m].Design = FCString::Atoi(*Parts[0]);
						Person.Marks[m].Area = Parts[1];
						Person.Marks[m].Size = Parts.Num() > 2 ? FCString::Atof(*Parts[2]) : 14.0f;
						if (CreatorMarkKind(Person.Marks[m].Design) == 3)
						{
							Person.Marks[m].Colour[0] = 0.75f, Person.Marks[m].Colour[1] = 0.12f, Person.Marks[m].Colour[2] = 0.08f;
						}
					}
				}
			}
		}
		FString HairLabel;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorHair="), HairLabel))
		{
			for (int32 h = 0; h < static_cast<int32>(UE_ARRAY_COUNT(CreatorHairStyles)); ++h)
			{
				if (HairLabel == UTF8_TO_TCHAR(CreatorHairStyles[h].Label))
				{
					for (FCreatorPerson& Person : State->People)
					{
						Person.HairStyle = h;
					}
				}
			}
		}
		FString Face;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorFace="), Face))
		{
			for (int32 e = 0; e < static_cast<int32>(UE_ARRAY_COUNT(CreatorExpressions)); ++e)
			{
				if (Face == UTF8_TO_TCHAR(CreatorExpressions[e].Name) || (Face.IsNumeric() && FCString::Atoi(*Face) == e))  // by name or number
				{
					for (FCreatorPerson& Person : State->People)
					{
						Person.Expression = e;
						Person.bBlink = false;
					}
				}
			}
		}
		FParse::Value(FCommandLine::Get(), TEXT("CreatorPitch="), State->CameraPitch);
		// -CreatorWet=<0-1> for everyone, for pictures.
		float WetArg = -1.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorWet="), WetArg))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Wet = FMath::Clamp(WetArg, 0.0f, 1.0f);
			}
		}
		// -CreatorKind=<0-3> (Baseline, Imp, Orcoid, Beast-Strain) for everyone, for pictures.
		int32 KindArg = -1;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorKind="), KindArg) && KindArg >= 0 && KindArg < static_cast<int32>(UE_ARRAY_COUNT(CreatorKindNames)))
		{
			for (FCreatorPerson& Person : State->People)
			{
				CreatorSetKind(Person, static_cast<ECreatorKind>(KindArg));
			}
		}
		float Genital = 0.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorGenital="), Genital))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Genital = Genital;
			}
		}
		int32 Pubic = -1;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorPubic="), Pubic))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Pubic = Pubic;
			}
		}
		FParse::Value(FCommandLine::Get(), TEXT("CreatorDistance="), State->CameraDistance);
		float Pigment = -1.0f;
		if (FParse::Value(FCommandLine::Get(), TEXT("CreatorPigment="), Pigment))
		{
			for (FCreatorPerson& Person : State->People)
			{
				Person.Pigment = Pigment;
			}
		}
	};
	Play.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns || State->People.IsEmpty())
		{
			return;
		}
		// Once they stand in their row, a step towards +X: they face the view (which looks from +X).
		State->LegTime += GetWorld()->GetDeltaSeconds();
		if (!State->bFaced && PhaseTime > 3.0f)
		{
			State->bFaced = true;
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				B->Submit(colony::MoveOrderCommand{State->People[i].Thing, colony::CellPos{State->Middle.x, State->Middle.y + 6 - 3 + i * 2, 0}});
			}
		}
		// -CreatorPlay=<clip name>: everyone plays that clip once their bodies are there (for tests and pictures).
		if (!State->bClipsListed)
		{
			CreatorListClips(*State);
		}
		FString PlayName;
		if (State->bFaced && PhaseTime > 4.5f && !State->bAutoPlayed && FParse::Value(FCommandLine::Get(), TEXT("CreatorPlay="), PlayName))
		{
			const int32 Index = State->ClipNames.IndexOfByKey(PlayName);
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				CreatorPlay(*State, i, Index, Pawns->FindPawn(State->People[i].Thing));
			}
			while (State->Playing.Num() < State->People.Num()) { State->Playing.Add(-1); }  // -1: not playing
			State->bAutoPlayed = true;
			Notes.Add(FString::Printf(TEXT("creator: playing %s (%d clips)"), *PlayName, State->Clips.Num()));
		}
		// Standing: anyone not walking and not playing a clip breathes in a seamless idle; walking stops it.
		if (State->bClipsListed && State->bFaced && PhaseTime > 4.5f)
		{
			if (State->IdleClip < 0)
			{
				for (const TCHAR* Name : {TEXT("ProMeleeAxeUnarmedIdle"), TEXT("ProLongbowUnarmedIdle01"), TEXT("ActionAdventureIdle")})
				{
					State->IdleClip = State->IdleClip >= 0 ? State->IdleClip : State->ClipNames.IndexOfByKey(Name);
				}
			}
			while (State->Playing.Num() < State->People.Num()) { State->Playing.Add(-1); }
			for (int32 i = 0; i < State->People.Num() && State->IdleClip >= 0; ++i)
			{
				if (!State->bWalk && State->Playing[i] < 0)
				{
					CreatorPlay(*State, i, State->IdleClip, Pawns->FindPawn(State->People[i].Thing), true);
				}
				else if (State->bWalk && State->Playing[i] == State->IdleClip)
				{
					CreatorPlay(*State, i, -1, Pawns->FindPawn(State->People[i].Thing));
				}
			}
		}
		// -CreatorRecord=<seconds> with -CreatorPlay: every frame of the clip at a fixed 30 a second (creator/rec_0000.png ...).
		float PlayRecord = 0.0f;
		if (State->bAutoPlayed && !FParse::Param(FCommandLine::Get(), TEXT("CreatorDemo")) && !FParse::Param(FCommandLine::Get(), TEXT("CreatorSoftDemo")) &&
			!FParse::Param(FCommandLine::Get(), TEXT("CreatorHairDemo")) && FParse::Value(FCommandLine::Get(), TEXT("CreatorRecord="), PlayRecord)
			&& State->Recorded < FMath::RoundToInt32(PlayRecord * 30.0f))
		{
			if (State->Recorded == 0)
			{
				FApp::SetUseFixedTimeStep(true);
				FApp::SetFixedDeltaTime(1.0 / 30.0);
			}
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/creator") / FString::Printf(TEXT("rec_%04d.png"), State->Recorded++), false, false);
		}
		// Walking: back and forth along X, side by side, so the jiggle and the tail show.
		if (State->bWalk && State->LegTime > 4.0f)
		{
			State->LegTime = 0.0f;
			const int32 X = State->Leg % 2 == 0 ? -8 : 2;
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				B->Submit(colony::MoveOrderCommand{State->People[i].Thing, colony::CellPos{State->Middle.x + X, State->Middle.y + 6 - 3 + i * 2, 0}});
			}
			++State->Leg;
		}
		// -CreatorDemo: the sliders move by themselves on the woman (body, then into an imp), then they all walk; with
		// -CreatorRecord=<seconds> every frame is saved at a fixed 30 a second (creator/rec_0000.png ...).
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorDemo")) && PhaseTime > 4.5f && State->People.Num() >= 2)
		{
			float RecordSeconds = 0.0f;
			FParse::Value(FCommandLine::Get(), TEXT("CreatorRecord="), RecordSeconds);
			if (State->DemoTime < 0.0f)
			{
				State->DemoTime = 0.0f;
				if (RecordSeconds > 0.0f)
				{
					FApp::SetUseFixedTimeStep(true);
					FApp::SetFixedDeltaTime(1.0 / 30.0);
				}
			}
			State->DemoTime += GetWorld()->GetDeltaSeconds();
			const float T = State->DemoTime;
			FCreatorPerson& W = State->People[0];
			const auto Wave = [T](float From, float To) { return T < From || T > To ? 0.0f : FMath::Sin(PI * 2.0f * (T - From) / (To - From)); };
			// Body sliders on the woman, then her gender and kind changed: man, imp, orcoid, back to a woman orcoid; then all walk.
			State->Selected = T < 21.0f ? 0 : (T < 25.0f ? FMath::Min(2, State->People.Num() - 1) : FMath::Min(3, State->People.Num() - 1));
			State->CameraDistance = T < 21.0f ? 260.0f : 380.0f;
			State->CameraYaw = T < 21.0f ? 180.0f : 255.0f;
			if (T < 9.0f)
			{
				W.Weight = -0.2f + Wave(0.0f, 3.0f);
				W.Muscle = Wave(3.0f, 6.0f);
				W.Breasts = Wave(6.0f, 9.0f);
			}
			const int32 Step = T < 11.0f ? 0 : (T < 13.0f ? 1 : (T < 15.0f ? 2 : (T < 17.0f ? 3 : (T < 19.0f ? 4 : 5))));
			static const bool StepFemale[] = {true, false, false, false, true, true};
			static const ECreatorKind StepKind[] = {ECreatorKind::Baseline, ECreatorKind::Baseline, ECreatorKind::Imp, ECreatorKind::Orcoid,
				ECreatorKind::Orcoid, ECreatorKind::Orcoid};
			if (W.bFemale != StepFemale[Step])
			{
				CreatorSetGender(W, StepFemale[Step], *Pawns);
				CreatorSetKind(W, StepKind[Step]);
			}
			if (W.Kind != StepKind[Step])
			{
				CreatorSetKind(W, StepKind[Step]);
			}
			if (T > 20.0f && !State->bWalk)
			{
				State->bWalk = true;
				State->LegTime = 99.0f;
			}
			if (RecordSeconds > 0.0f && State->Recorded < FMath::RoundToInt32(RecordSeconds * 30.0f))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/creator") / FString::Printf(TEXT("rec_%04d.png"), State->Recorded++), false, false);
			}
		}
		// -CreatorSoftDemo (docs/design/soft-body.md): the picked one stands still, close up, with a heavier soft body; slaps on
		// the belly and blasts with the soft parts on, then the same with them off, for a side-by-side judgement. With
		// -CreatorRecord=<seconds> every frame is saved at a fixed 30 a second (creator/rec_0000.png ...).
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorSoftDemo")) && PhaseTime > 4.5f && State->People.IsValidIndex(State->Selected))
		{
			float RecordSeconds = 0.0f;
			FParse::Value(FCommandLine::Get(), TEXT("CreatorRecord="), RecordSeconds);
			FCreatorPerson& W = State->People[State->Selected];
			if (State->SoftDemoT < 0.0f)
			{
				State->SoftDemoT = 0.0f;
				State->bWalk = false;
				State->BlastEvery = 0.0f;
				State->CameraDistance = 170.0f;
				State->CameraYaw = 215.0f;
				IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.SoftContact"))->Set(1);  // off by default until it looks right
				W.Weight = 0.6f;
				W.Muscle = -0.4f;
				if (RecordSeconds > 0.0f)
				{
					FApp::SetUseFixedTimeStep(true);
					FApp::SetFixedDeltaTime(1.0 / 30.0);
				}
			}
			State->SoftDemoT += GetWorld()->GetDeltaSeconds();
			// Soft parts on: slap 1.5 s, blast 4 s, slap 6.5 s, then a fist-sized ball presses into the belly (8-10.5 s), a
			// cheek (11-13.5 s) and a thigh (14-16.5 s); off from 17 s: slap 18 s, blast 20 s, the ball on the belly 21-23.5 s.
			static const float SoftTimes[] = {1.5f, 4.0f, 6.5f, 18.0f, 20.0f};
			static const bool SoftBlast[] = {false, true, false, false, true};
			W.bJiggle = State->SoftDemoT < 17.0f;
			{
				const float T = State->SoftDemoT;
				const int32 Press = T >= 8.0f && T < 10.5f ? 0 : (T >= 11.0f && T < 13.5f ? 1 : (T >= 14.0f && T < 16.5f ? 2 : (T >= 21.0f && T < 23.5f ? 0 : -1)));
				const float Start = Press < 0 ? 0.0f : (T >= 21.0f ? 21.0f : (Press == 0 ? 8.0f : (Press == 1 ? 11.0f : 14.0f)));
				CreatorSoftPress(*State, W, Pawns, Press, (T - Start) / 2.5f);
			}
			if (State->SoftDemoStep < static_cast<int32>(UE_ARRAY_COUNT(SoftTimes)) && State->SoftDemoT >= SoftTimes[State->SoftDemoStep])
			{
				(SoftBlast[State->SoftDemoStep] ? State->bBlastNow : State->bSlapNow) = true;
				UE_LOG(LogTemp, Display, TEXT("Soft body demo: %s at %.1f s, soft parts %s"), SoftBlast[State->SoftDemoStep] ? TEXT("blast") : TEXT("slap"),
					State->SoftDemoT, W.bJiggle ? TEXT("on") : TEXT("off"));
				++State->SoftDemoStep;
			}
			if (RecordSeconds > 0.0f && State->Recorded < FMath::RoundToInt32(RecordSeconds * 30.0f))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/creator") / FString::Printf(TEXT("rec_%04d.png"), State->Recorded++), false, false);
			}
		}
		// -CreatorHairDemo (docs/design/hair-physics.md): the picked one, seen from behind, fights the air with long hair:
		// physics on (0-12 s), the same with the hair as animated (12-24 s), then on with the collision capsules drawn
		// (24-36 s). Pictures at set moments (creator/hair_NN_*.png); with -CreatorRecord=<seconds> every frame at 30 a second.
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorHairDemo")) && PhaseTime > 4.5f && State->bClipsListed && State->People.IsValidIndex(State->Selected))
		{
			float RecordSeconds = 0.0f;
			FParse::Value(FCommandLine::Get(), TEXT("CreatorRecord="), RecordSeconds);
			if (State->HairDemoT < 0.0f)
			{
				State->HairDemoT = 0.0f;
				State->bWalk = false;
				State->CameraDistance = 230.0f;
				State->CameraYaw = 30.0f;  // from behind, a little to her left
				State->CameraPitch = -8.0f;
				if (RecordSeconds > 0.0f)
				{
					FApp::SetUseFixedTimeStep(true);
					FApp::SetFixedDeltaTime(1.0 / 30.0);
				}
			}
			State->HairDemoT += GetWorld()->GetDeltaSeconds();
			const float T = State->HairDemoT;
			const int32 Part = T < 12.0f ? 0 : (T < 24.0f ? 1 : 2);
			// Looked up once (a lookup every frame makes the engine warn).
			static IConsoleVariable* const HairPhysicsVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.HairPhysics"));
			static IConsoleVariable* const HairDebugVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.HairDebug"));
			if (HairPhysicsVar && HairPhysicsVar->GetInt() != (Part == 1 ? 0 : 2))
			{
				HairPhysicsVar->Set(Part == 1 ? 0 : 2);
			}
			if (HairDebugVar && HairDebugVar->GetInt() != (Part == 2 ? 1 : 0))
			{
				HairDebugVar->Set(Part == 2 ? 1 : 0);
			}
			// Each part: shadowboxing (turns, punches, ducks), then sword swings (big arm and shoulder moves).
			static const float ClipTimes[] = {0.0f, 6.0f, 12.0f, 18.0f, 24.0f, 30.0f};
			static const TCHAR* const ClipNames[] = {TEXT("Shadowbox"), TEXT("SwordSwings"), TEXT("Shadowbox"), TEXT("SwordSwings"), TEXT("Shadowbox"), TEXT("SwordSwings")};
			if (State->HairDemoStep < static_cast<int32>(UE_ARRAY_COUNT(ClipTimes)) && T >= ClipTimes[State->HairDemoStep])
			{
				const int32 Clip = State->ClipNames.IndexOfByKey(FString(ClipNames[State->HairDemoStep]));
				while (State->Playing.Num() < State->People.Num()) { State->Playing.Add(-1); }
				CreatorPlay(*State, State->Selected, Clip, Pawns->FindPawn(State->People[State->Selected].Thing), true);
				UE_LOG(LogTemp, Display, TEXT("Hair demo: %s at %.1f s, hair physics %s"), ClipNames[State->HairDemoStep], T, Part == 1 ? TEXT("off") : TEXT("on"));
				++State->HairDemoStep;
			}
			static const float ShotTimes[] = {2.0f, 4.0f, 8.0f, 10.0f, 14.0f, 16.0f, 20.0f, 22.0f, 26.0f, 32.0f};
			bool bShotThisFrame = false;  // one screenshot request a frame: a later one replaces it, so the recording skips this frame
			if (State->HairDemoShot < static_cast<int32>(UE_ARRAY_COUNT(ShotTimes)) && T >= ShotTimes[State->HairDemoShot])
			{
				static const TCHAR* const PartNames[] = {TEXT("on"), TEXT("off"), TEXT("capsules")};
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/creator") /
					FString::Printf(TEXT("hair_%02d_%s.png"), State->HairDemoShot, PartNames[Part]), false, false);
				++State->HairDemoShot;
				bShotThisFrame = true;
			}
			if (const ASimPawnActor* Pawn = Pawns->FindPawn(State->People[State->Selected].Thing))
			{
				const USkeletalMeshComponent* Mesh = Pawn->GetMakeHumanMesh();
				if (UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr)
				{
					float Animated = 0.0f;
					State->HairDeepest[Part] = FMath::Max(State->HairDeepest[Part], Anim->TakeLargestHairPenetration(&Animated));
					State->HairAnimatedDeepest[Part] = FMath::Max(State->HairAnimatedDeepest[Part], Animated);
				}
			}
			if (!bShotThisFrame && RecordSeconds > 0.0f && State->Recorded < FMath::RoundToInt32(RecordSeconds * 30.0f))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/creator") / FString::Printf(TEXT("rec_%04d.png"), State->Recorded++), false, false);
			}
		}
		// Soft-body tests: a blast in front of the picked one (everyone feels it, less further away), or a slap on its belly.
		if (State->BlastEvery > 0.0f)
		{
			State->BlastTimer += GetWorld()->GetDeltaSeconds();
			if (State->BlastTimer >= State->BlastEvery)
			{
				State->BlastTimer = 0.0f;
				State->bBlastNow = true;
			}
		}
		if ((State->bBlastNow || State->bSlapNow) && State->People.IsValidIndex(State->Selected))
		{
			const ASimPawnActor* Picked = Pawns->FindPawn(State->People[State->Selected].Thing);
			const USkeletalMeshComponent* PickedMesh = Picked ? Picked->GetMakeHumanMesh() : nullptr;
			if (PickedMesh)
			{
				const FVector Forward = PickedMesh->GetOwner()->GetActorForwardVector();
				const FVector Blast = PickedMesh->GetComponentLocation() + Forward * 300.0f + FVector(0, 0, 60.0f);
				// The belly bone sits 7 cm inside the skin (mh_common.add_body_soft_bones), so this is on the skin.
				const bool bBellyBone = PickedMesh->GetBoneIndex(TEXT("belly")) != INDEX_NONE;
				const FVector Belly = bBellyBone ? PickedMesh->GetSocketLocation(TEXT("belly")) + Forward * 7.0f : PickedMesh->GetSocketLocation(TEXT("spine_01")) + Forward * 15.0f;
				for (const FCreatorPerson& Other : State->People)
				{
					const ASimPawnActor* Pawn = Pawns->FindPawn(Other.Thing);
					const USkeletalMeshComponent* Mesh = Pawn ? Pawn->GetMakeHumanMesh() : nullptr;
					UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr;
					if (!Anim)
					{
						continue;
					}
					if (State->bBlastNow)
					{
						Anim->AddBlastKick(Blast, 900.0f, 900.0f);
					}
					else if (Mesh == PickedMesh)
					{
						Anim->AddHitKick(Belly, -Forward * 300.0f + FVector(0, 0, 40.0f), 35.0f);
					}
				}
				if (State->bBlastNow)
				{
					++State->Blasts;
					UE_LOG(LogTemp, Display, TEXT("Soft body: blast %d at %s"), State->Blasts, *Blast.ToString());
				}
			}
			State->bBlastNow = State->bSlapNow = false;
		}
		for (FCreatorPerson& P : State->People)
		{
			P.FaceTime += GetWorld()->GetDeltaSeconds();
			if (P.FaceTime > P.NextBlink)
			{
				P.NextBlink = P.FaceTime + FMath::FRandRange(2.5f, 5.5f);
			}
			if (ASimPawnActor* Pawn = Pawns->FindPawn(P.Thing))
			{
				CreatorApply(P, *Pawn, State->b4KSkin);
				if (const USkeletalMeshComponent* Mesh = Pawn->GetMakeHumanMesh())
				{
					if (UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()))
					{
						State->LargestTailSwing = FMath::Max(State->LargestTailSwing, Anim->TakeLargestTailSwing());
						State->LargestJiggle = FMath::Max(State->LargestJiggle, Anim->TakeLargestJiggle());
						State->DeepestDent = FMath::Max(State->DeepestDent, Anim->TakeDeepestDent());
					}
				}
			}
		}
		// The free view round the picked one: middle mouse turns it, the wheel moves closer or further, right mouse drags the
		// point it looks at (as do WASD, Q and E), not while the mouse or keys are on the panel.
		APlayerController* ViewPC = GetWorld()->GetFirstPlayerController();
		const float Dt = GetWorld()->GetDeltaSeconds();
		if (ViewPC)
		{
			float MouseX = 0.0f;
			float MouseY = 0.0f;
			const bool bMouse = ViewPC->GetMousePosition(MouseX, MouseY);
			const FVector2D Mouse(MouseX, MouseY);
			const FVector2D Delta = bMouse && State->bHadMouse ? Mouse - State->LastMouse : FVector2D::ZeroVector;
			State->LastMouse = Mouse;
			State->bHadMouse = bMouse;
			const FRotator Look(0.0, State->CameraYaw, 0.0);
			if (!State->bUiWantsMouse)
			{
				if (ViewPC->IsInputKeyDown(EKeys::MiddleMouseButton))
				{
					State->CameraYaw += Delta.X * 0.3f;
					State->CameraPitch = FMath::Clamp(State->CameraPitch - Delta.Y * 0.3f, -85.0f, 85.0f);
				}
				if (ViewPC->IsInputKeyDown(EKeys::RightMouseButton))
				{
					const FVector Right = FRotationMatrix(Look).GetUnitAxis(EAxis::Y);
					State->FocusOffset += (-Right * Delta.X + FVector::UpVector * Delta.Y) * State->CameraDistance * 0.0015;
				}
				if (ViewPC->WasInputKeyJustPressed(EKeys::MouseScrollUp))
				{
					State->CameraDistance *= 0.87f;
				}
				if (ViewPC->WasInputKeyJustPressed(EKeys::MouseScrollDown))
				{
					State->CameraDistance /= 0.87f;
				}
				State->CameraDistance = FMath::Clamp(State->CameraDistance, 40.0f, 2500.0f);
			}
			if (!State->bUiWantsKeys)
			{
				const FVector Forward = Look.Vector();
				const FVector Right = FRotationMatrix(Look).GetUnitAxis(EAxis::Y);
				FVector Move = FVector::ZeroVector;
				Move += ViewPC->IsInputKeyDown(EKeys::W) ? Forward : FVector::ZeroVector;
				Move -= ViewPC->IsInputKeyDown(EKeys::S) ? Forward : FVector::ZeroVector;
				Move += ViewPC->IsInputKeyDown(EKeys::D) ? Right : FVector::ZeroVector;
				Move -= ViewPC->IsInputKeyDown(EKeys::A) ? Right : FVector::ZeroVector;
				Move += ViewPC->IsInputKeyDown(EKeys::E) ? FVector::UpVector : FVector::ZeroVector;
				Move -= ViewPC->IsInputKeyDown(EKeys::Q) ? FVector::UpVector : FVector::ZeroVector;
				State->FocusOffset += Move * FMath::Max(150.0f, State->CameraDistance) * Dt;
			}
		}
		FCreatorPerson& Sel = State->People[FMath::Clamp(State->Selected, 0, State->People.Num() - 1)];
		if (const ASimPawnActor* Pawn = Pawns->FindPawn(Sel.Thing))
		{
			const FVector Focus = Pawn->GetActorLocation() + FVector(0.0, 0.0, 100.0 * Sel.Height) + State->FocusOffset;
			const FRotator View(State->CameraPitch, State->CameraYaw, 0.0);
			ViewFrom(Focus - View.Vector() * State->CameraDistance, View);
		}
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->bShowMouseCursor = true;
		}

		// The WEAPONS window's weapons, preview, fight demo and blood (CreatorWeapons.cpp).
		{
			TArray<ASimPawnActor*> CwPeople;
			for (const FCreatorPerson& Person : State->People)
			{
				CwPeople.Add(Pawns->FindPawn(Person.Thing));
			}
			CreatorWeapons::Tick(GetWorld(), CwPeople, State->Selected, GetWorld()->GetDeltaSeconds(), State->bWalk);
		}
		// The panel (-CreatorNoPanel: none, for pictures).
		if (FParse::Param(FCommandLine::Get(), TEXT("CreatorNoPanel")))
		{
			return;
		}
		const ImGui::FScopedContext ScopedContext;
		if (!ScopedContext)
		{
			return;
		}
		const ImGuiViewport* Viewport = ImGui::GetMainViewport();
		const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
		ImGui::GetStyle().FontScaleMain = 1.53f * Scale;  // larger than the other panels (sneed: x1.5, then 15% smaller)
		State->bUiWantsMouse = ImGui::GetIO().WantCaptureMouse;
		State->bUiWantsKeys = ImGui::GetIO().WantCaptureKeyboard;
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + 16.0f * Scale, Viewport->Pos.y + 16.0f * Scale), ImGuiCond_Once);
		ImGui::SetNextWindowBgAlpha(0.88f);
		// A look of its own: warm dark panel, rounded corners, copper accents (popped after End).
		const ImVec4 Copper(0.86f, 0.52f, 0.30f, 1.0f);
		const ImVec4 CopperDim(0.55f, 0.32f, 0.18f, 1.0f);
		const ImVec4 Panel(0.09f, 0.08f, 0.08f, 1.0f);
		const ImVec4 Field(0.20f, 0.16f, 0.14f, 1.0f);
		const ImVec4 FieldHover(0.30f, 0.23f, 0.19f, 1.0f);
		const ImGuiStyleVar Vars[] = {ImGuiStyleVar_WindowRounding, ImGuiStyleVar_FrameRounding, ImGuiStyleVar_GrabRounding, ImGuiStyleVar_TabRounding};
		for (const ImGuiStyleVar Var : Vars)
		{
			ImGui::PushStyleVar(Var, 7.0f * Scale);
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * Scale, 12.0f * Scale));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * Scale, 5.0f * Scale));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(9.0f * Scale, 7.0f * Scale));
		ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14.0f * Scale);
		const TPair<ImGuiCol, ImVec4> Colours[] = {
			{ImGuiCol_WindowBg, Panel}, {ImGuiCol_TitleBg, CopperDim}, {ImGuiCol_TitleBgActive, CopperDim}, {ImGuiCol_TitleBgCollapsed, CopperDim},
			{ImGuiCol_FrameBg, Field}, {ImGuiCol_FrameBgHovered, FieldHover}, {ImGuiCol_FrameBgActive, FieldHover},
			{ImGuiCol_SliderGrab, Copper}, {ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.68f, 0.45f, 1.0f)}, {ImGuiCol_CheckMark, Copper},
			{ImGuiCol_Button, Field}, {ImGuiCol_ButtonHovered, CopperDim}, {ImGuiCol_ButtonActive, Copper},
			{ImGuiCol_Tab, Field}, {ImGuiCol_TabHovered, CopperDim}, {ImGuiCol_TabSelected, CopperDim}, {ImGuiCol_TabSelectedOverline, Copper},
			{ImGuiCol_Header, Field}, {ImGuiCol_HeaderHovered, FieldHover}, {ImGuiCol_Separator, CopperDim}, {ImGuiCol_Border, CopperDim},
			{ImGuiCol_Text, ImVec4(0.95f, 0.91f, 0.86f, 1.0f)}, {ImGuiCol_TextDisabled, ImVec4(0.62f, 0.56f, 0.50f, 1.0f)}};
		for (const TPair<ImGuiCol, ImVec4>& Colour : Colours)
		{
			ImGui::PushStyleColor(Colour.Key, Colour.Value);
		}
		if (ImGui::Begin("CHARACTER CREATOR", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			for (int32 i = 0; i < State->People.Num(); ++i)
			{
				if (i > 0)
				{
					ImGui::SameLine();
				}
				if (ImGui::RadioButton(TCHAR_TO_UTF8(*FString::Printf(TEXT("%d##person"), i + 1)), State->Selected == i))
				{
					State->Selected = i;
				}
			}
			FCreatorPerson& P = State->People[FMath::Clamp(State->Selected, 0, State->People.Num() - 1)];
			ImGui::SameLine();
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*P.Label(State->Selected)));
			if (ImGui::BeginTabBar("##creator"))
			{
				if (ImGui::BeginTabItem("Gender"))
				{
					if (ImGui::RadioButton("Woman", P.bFemale) && !P.bFemale)
					{
						CreatorSetGender(P, true, *Pawns);
						++State->BaseSwitches;
					}
					ImGui::SameLine();
					if (ImGui::RadioButton("Man", !P.bFemale) && P.bFemale)
					{
						CreatorSetGender(P, false, *Pawns);
						++State->BaseSwitches;
					}
					ImGui::SliderFloat(P.bFemale ? "More masculine" : "More feminine", &P.Sex, 0.0f, 1.0f, "%.2f");
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Kind"))
				{
					for (int32 k = 0; k < static_cast<int32>(UE_ARRAY_COUNT(CreatorKindNames)); ++k)
					{
						if (k % 2 == 1)
						{
							ImGui::SameLine(200.0f * Scale);  // two to a row: in one row the last ones fell off the panel
						}
						if (ImGui::RadioButton(CreatorKindNames[k], static_cast<int32>(P.Kind) == k))
						{
							CreatorSetKind(P, static_cast<ECreatorKind>(k));
						}
					}
					ImGui::SeparatorText("Features");
					ImGui::SliderFloat("Pointed ears", &P.ImpEars, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Imp face", &P.ImpFace, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Orc face", &P.OrcFace, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Orc build", &P.OrcBody, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Horns", &P.Horns, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Tusks", &P.Tusks, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Tail", &P.Tail, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Long pointed ears", &P.ElfEars, 0.0f, 1.0f, "%.2f");
					if (P.Kind == ECreatorKind::Drow)
					{
						ImGui::SeparatorText("Drow");
						if (ImGui::SliderFloat("Skin (light .. obsidian)", &P.DrowTone, 0.0f, 1.0f, "%.2f"))
						{
							CreatorDrowSkin(P);
						}
						ImGui::SliderFloat("Red of the eyes", &P.IrisStrength, 0.2f, 1.0f, "%.2f");
					}
					ImGui::SeparatorText("Headgear test");
					ImGui::RadioButton("No headgear", &P.EarCover, 0);
					ImGui::SameLine();
					ImGui::RadioButton("Ear tops covered", &P.EarCover, 1);
					ImGui::SameLine();
					ImGui::RadioButton("Ears covered", &P.EarCover, 2);
					if (P.Kind == ECreatorKind::Beast)
					{
						ImGui::SeparatorText("Fur (ears and tail)");
						ImGui::SliderFloat("Shade from hair", &P.FurShade, -1.0f, 1.0f, "%.2f");
						ImGui::Checkbox("Own fur colour", &P.bOwnFurColour);
						if (P.bOwnFurColour)
						{
							ImGui::ColorEdit3("Fur", P.Fur);
						}
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Body"))
				{
					ImGui::SliderFloat("Height", &P.Height, 0.75f, 1.15f, "%.2f");
					ImGui::SliderFloat("Older", &P.Older, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Thin / heavy", &P.Weight, -1.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Soft / muscular", &P.Muscle, -1.0f, 1.0f, "%.2f");
					if (P.bFemale)
					{
						ImGui::SliderFloat("Breasts", &P.Breasts, -1.0f, 1.0f, "%.2f");
					}
					ImGui::SliderFloat(P.bFemale ? "Labia" : "Genitals", &P.Genital, -1.0f, 1.0f, "%.2f");
					ImGui::Checkbox("Jiggle (off: soft parts stay as animated)", &P.bJiggle);
					if (P.bFemale)
					{
						ImGui::SliderFloat("Breast jiggle", &P.BreastJiggle, 0.0f, 1.0f, "%.2f");
					}
					// Soft body step 1: the body's softness follows thin/heavy, soft/muscular and older.
					ImGui::Text("Flesh softness %.2f (from the body sliders)", 0.5f + 0.4f * P.Weight - 0.3f * P.Muscle + 0.1f * P.Older);
					if (ImGui::Button("Slap belly"))
					{
						State->bSlapNow = true;
					}
					ImGui::SameLine();
					if (ImGui::Button("Blast nearby"))
					{
						State->bBlastNow = true;
					}
					if (ImGui::Button("Random person"))
					{
						FRandomStream R(FPlatformTime::Cycles());
						CreatorRandomize(P, R);
					}
					ImGui::SameLine();
					if (ImGui::Button("Random body"))
					{
						FRandomStream R(FPlatformTime::Cycles());
						P.Height = R.FRandRange(0.85f, 1.1f);
						P.Sex = R.FRandRange(0.0f, 0.3f);
						P.Older = R.FRand() < 0.3f ? R.FRandRange(0.2f, 1.0f) : 0.0f;
						P.Weight = R.FRandRange(-0.8f, 0.8f);
						P.Muscle = R.FRandRange(-0.6f, 0.8f);
						P.Breasts = R.FRandRange(-0.8f, 0.8f);
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Face"))
				{
					ImGui::SeparatorText("Expression");
					for (int32 e = 0; e < static_cast<int32>(UE_ARRAY_COUNT(CreatorExpressions)); ++e)
					{
						if (e % 4 != 0)
						{
							ImGui::SameLine();
						}
						if (ImGui::RadioButton(CreatorExpressions[e].Name, P.Expression == e))
						{
							P.Expression = e;
						}
					}
					ImGui::SliderFloat("Strength", &P.ExpressionStrength, 0.0f, 1.5f, "%.2f");
					ImGui::Checkbox("Blink", &P.bBlink);
					ImGui::SameLine();
					ImGui::Checkbox("Talk", &P.bTalk);
					if (ImGui::CollapsingHeader("Face units (fine tuning)"))
					{
						for (int32 u = 0; u < CreatorUnitCount; ++u)
						{
							ImGui::SliderFloat(TCHAR_TO_UTF8(CreatorUnits[u]), &P.Units[u], 0.0f, 1.0f, "%.2f");
						}
						if (ImGui::Button("Clear units"))
						{
							for (float& Unit : P.Units)
							{
								Unit = 0.0f;
							}
						}
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Markings"))
				{
					const TMap<FString, FCreatorArea>& Areas = CreatorAreas(P.Looks.MakeHuman);
					if (Areas.IsEmpty())
					{
						ImGui::TextDisabled("No body areas for this base (rebuild it with mh_creator.py)");
					}
					TArray<FString> AreaNames;
					Areas.GetKeys(AreaNames);
					AreaNames.Sort();
					for (int32 m = 0; m < 4; ++m)
					{
						FCreatorMark& Mark = P.Marks[m];
						ImGui::PushID(m);
						ImGui::SeparatorText(TCHAR_TO_UTF8(*FString::Printf(TEXT("Mark %d"), m + 1)));
						ImGui::Checkbox("On", &Mark.bOn);
						if (Mark.bOn)
						{
							ImGui::SameLine();
							ImGui::SetNextItemWidth(220.0f * Scale);
							ImGui::Combo("Design", &Mark.Design, CreatorMarkNames, 16);
							int32 AreaIndex = FMath::Max(0, AreaNames.IndexOfByKey(Mark.Area));
							TArray<FTCHARToUTF8> Utf8;
							TArray<const char*> Items;
							for (const FString& Name : AreaNames)
							{
								Utf8.Emplace(*Name);
							}
							for (const FTCHARToUTF8& Item : Utf8)
							{
								Items.Add(Item.Get());
							}
							if (!Items.IsEmpty() && ImGui::Combo("Where", &AreaIndex, Items.GetData(), Items.Num()))
							{
								Mark.Area = AreaNames[AreaIndex];
							}
							ImGui::SliderFloat("Size (cm)", &Mark.Size, 3.0f, 40.0f, "%.0f");
							ImGui::SliderFloat("Turn", &Mark.Angle, -180.0f, 180.0f, "%.0f");
							ImGui::SliderFloat("Strength", &Mark.Opacity, 0.0f, 1.0f, "%.2f");
							if (CreatorMarkKind(Mark.Design) == 0 || CreatorMarkKind(Mark.Design) == 3)
							{
								ImGui::ColorEdit3(CreatorMarkKind(Mark.Design) == 0 ? "Ink" : "Paint", Mark.Colour);
							}
						}
						ImGui::PopID();
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Jewellery"))
				{
					for (int32 j = 0; j < CreatorJewelCount; ++j)
					{
						ImGui::PushID(j);
						ImGui::Checkbox(CreatorJewels[j].Label, &P.Jewels[j]);
						if (P.Jewels[j])
						{
							ImGui::SameLine(260.0f * Scale);
							ImGui::SetNextItemWidth(140.0f * Scale);
							ImGui::Combo("##metal", &P.Metal[j], CreatorMetalNames, UE_ARRAY_COUNT(CreatorMetalNames));
						}
						ImGui::PopID();
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Hair and skin"))
				{
					ImGui::SliderFloat("Wet (rain, swimming)", &P.Wet, 0.0f, 1.0f, "%.2f");
					ImGui::SeparatorText("Hair");
					{
						const int32 Current = P.HairStyle >= 0 ? P.HairStyle : (P.bFemale ? 1 : 5);
						int32 Shown = 0;
						for (int32 h = 0; h < static_cast<int32>(UE_ARRAY_COUNT(CreatorHairStyles)); ++h)
						{
							// Only the styles this base has (and bald).
							const TCHAR* Key = CreatorHairStyles[h].Key;
							bool bHas = Key == nullptr;
							if (const ASimPawnActor* Pawn = Pawns->FindPawn(P.Thing); Pawn && Pawn->GetMakeHumanMesh() && Key)
							{
								for (const FName& Slot : Pawn->GetMakeHumanMesh()->GetMaterialSlotNames())
								{
									bHas |= Slot.ToString().Contains(Key);
								}
							}
							if (!bHas)
							{
								continue;
							}
							if (Shown++ % 5 != 0)
							{
								ImGui::SameLine();
							}
							if (ImGui::RadioButton(CreatorHairStyles[h].Label, Current == h))
							{
								P.HairStyle = h;
							}
						}
					}
					ImGui::ColorEdit3("Hair colour", P.Hair);
					ImGui::SeparatorText("Eyes");
					ImGui::Checkbox("Own iris colour", &P.bOwnIris);
					if (P.bOwnIris)
					{
						ImGui::ColorEdit3("Iris", P.Iris);
						ImGui::SliderFloat("Iris strength", &P.IrisStrength, 0.2f, 1.0f, "%.2f");
					}
					ImGui::SeparatorText("Pubic hair");
					bool bPubic = P.Pubic != 0;
					if (ImGui::Checkbox("Pubic hair", &bPubic))
					{
						P.Pubic = bPubic ? 1 : 0;
					}
					ImGui::SameLine();
					ImGui::Checkbox("Shaved (stubble)", &P.bShaved);
					ImGui::SliderFloat("Coverage", &P.PubicCoverage, 0.3f, 1.0f, "%.2f");
					ImGui::SliderFloat("Density", &P.PubicDensity, 0.0f, 1.0f, "%.2f");
					ImGui::SliderFloat("Length (trimmed .. natural)", &P.PubicLength, 0.35f, 1.0f, "%.2f");
					ImGui::SliderFloat("Straight .. curly", &P.PubicCurl, 0.0f, 1.0f, "%.2f");
					ImGui::Checkbox("Own pubic hair colour", &P.bOwnPubicColour);
					if (P.bOwnPubicColour)
					{
						ImGui::ColorEdit3("Pubic hair colour", P.PubicHair);
					}
					else
					{
						ImGui::TextUnformatted("(a little darker than the hair)");
					}
					ImGui::SeparatorText("Skin");
					if (P.Kind == ECreatorKind::Drow)
					{
						if (ImGui::SliderFloat("Drow skin (light .. obsidian)", &P.DrowTone, 0.0f, 1.0f, "%.2f"))
						{
							CreatorDrowSkin(P);
						}
					}
					else
					{
						ImGui::ColorEdit3("Skin", P.Skin);
					}
					ImGui::SliderFloat("Areola and genital pigment", &P.Pigment, 0.0f, 1.0f, "%.2f");
					if (ImGui::Button("Random skin tone"))
					{
						// Mostly fair, sometimes tanned, never dark (as CreatorRandomize; the colour picker still goes anywhere).
						const float Tone = FMath::Pow(FRandomStream(FPlatformTime::Cycles()).FRand(), 3.0f);
						CreatorSetSkin(P, FMath::Lerp(0.96f, 0.80f, Tone), FMath::Lerp(0.86f, 0.62f, Tone), FMath::Lerp(0.80f, 0.50f, Tone));
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			ImGui::Separator();
			ImGui::Checkbox("Walk back and forth", &State->bWalk);
			ImGui::SameLine();
			ImGui::Checkbox("4K skin", &State->b4KSkin);
			ImGui::SeparatorText("View");
			ImGui::TextDisabled("middle mouse turn, wheel zoom, right mouse or WASD/QE move");
			if (ImGui::Button("Reset view"))
			{
				State->CameraYaw = 180.0f;
				State->CameraPitch = -8.0f;
				State->CameraDistance = 420.0f;
				State->FocusOffset = FVector::ZeroVector;
			}
		}
		ImGui::End();
		// The animation panel, on the right: a clip on the picked person (or everyone), looped or once, at a speed.
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - 16.0f * Scale, Viewport->Pos.y + 16.0f * Scale), ImGuiCond_Once, ImVec2(1.0f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.88f);
		if (ImGui::Begin("ANIMATIONS", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
		{
			while (State->Playing.Num() < State->People.Num()) { State->Playing.Add(-1); }  // -1: not playing
			if (State->Clips.IsEmpty())
			{
				ImGui::TextDisabled("No clips yet: run the MakeHumanArt commandlet with -Clips");
			}
			// Grouped by category (folded), with a search box that shows every match unfolded.
			ImGui::SetNextItemWidth(300.0f * Scale);
			ImGui::InputTextWithHint("##filter", "search", State->ClipFilter, sizeof(State->ClipFilter));
			const FString Filter = UTF8_TO_TCHAR(State->ClipFilter);
			int32 Picked = -1;
			ImGui::BeginChild("##clips", ImVec2(320.0f * Scale, 520.0f * Scale), ImGuiChildFlags_Borders);
			for (int32 c = 0; c < State->Clips.Num();)
			{
				const FString Category = State->ClipCategories[c];
				int32 End = c;
				while (End < State->Clips.Num() && State->ClipCategories[End] == Category)
				{
					++End;
				}
				const bool bOpen = !Filter.IsEmpty() || ImGui::CollapsingHeader(TCHAR_TO_UTF8(*FString::Printf(TEXT("%s (%d)"), *Category, End - c)));
				if (bOpen)
				{
					for (int32 i = c; i < End; ++i)
					{
						if (!Filter.IsEmpty() && !State->ClipNames[i].Contains(Filter) && !Category.Contains(Filter))
						{
							continue;
						}
						if (ImGui::Selectable(TCHAR_TO_UTF8(*State->ClipNames[i]), State->Clip == i))
						{
							State->Clip = i;
							Picked = i;
						}
					}
				}
				c = End;
			}
			ImGui::EndChild();
			ImGui::SeparatorText("Play");
			ImGui::Checkbox("Loop", &State->bLoop);
			ImGui::SameLine();
			ImGui::Checkbox("Everyone", &State->bEveryone);
			ImGui::SliderFloat("Speed", &State->ClipSpeed, 0.25f, 2.0f, "%.2f");
			const auto Apply = [&](int32 ClipIndex)
			{
				for (int32 i = 0; i < State->People.Num(); ++i)
				{
					if (State->bEveryone || i == State->Selected)
					{
						CreatorPlay(*State, i, ClipIndex, Pawns->FindPawn(State->People[i].Thing));
					}
				}
			};
			// Picking another clip while one plays switches to it straight away.
			const int32 Current = State->Playing.IsValidIndex(State->Selected) ? State->Playing[State->Selected] : -1;
			if (Picked >= 0 && Current >= 0 && Current != State->IdleClip)
			{
				Apply(Picked);
			}
			if (ImGui::Button("Play"))
			{
				State->bWalk = false;  // standing still while they act it out
				Apply(State->Clip);
			}
			ImGui::SameLine();
			if (ImGui::Button("Stop"))
			{
				Apply(-1);  // back to the breathing idle (the tick starts it)
			}
			const int32 Now = State->Playing.IsValidIndex(State->Selected) ? State->Playing[State->Selected] : -1;
			ImGui::TextDisabled("%s", Now >= 0 && Now != State->IdleClip ? TCHAR_TO_UTF8(*(TEXT("Playing: ") + State->ClipNames[Now])) : "Standing (breathing idle)");
		}
		ImGui::End();
		CreatorWeapons::DrawWindow(Scale);
		ImGui::PopStyleColor(UE_ARRAY_COUNT(Colours));
		ImGui::PopStyleVar(UE_ARRAY_COUNT(Vars) + 4);
		++State->PanelFrames;
	};
	Play.Verify = [=, this]()
	{
		Notes.Add(FString::Printf(TEXT("creator: %d people, panel drawn %d frames, tail swung up to %.0f degrees, soft tissue up to %.1f cm, skin dents up to %.1f cm, ball pressing the skin %d frames"), State->People.Num(),
			State->PanelFrames, State->LargestTailSwing, State->LargestJiggle, State->DeepestDent, State->SoftPressFrames));
		for (int32 i = 0; i < State->People.Num(); ++i)
		{
			Notes.Add(FString::Printf(TEXT("creator: %s"), *State->People[i].Label(i)));
		}
		if (State->HairDemoT >= 0.0f)
		{
			static const TCHAR* const PartNames[] = {TEXT("physics on"), TEXT("as animated (measured with physics off: none)"), TEXT("physics on, capsules drawn")};
			for (int32 p = 0; p < 3; ++p)
			{
				Notes.Add(FString::Printf(TEXT("creator hair demo, %s: hair up to %.1f cm into the body; as animated it would have gone %.1f cm in"),
					PartNames[p], State->HairDeepest[p], State->HairAnimatedDeepest[p]));
			}
		}
	};
	Phases.Add(MoveTemp(Play));
}

// ---------------------------------------------------------------------------------------------------------------
// The randomizer for other scenes (CreatorPeople.h).

namespace
{
	FCreatorPerson CreatorPeopleMake(int32 Seed, bool bFemale)
	{
		FCreatorPerson Person;
		Person.bFemale = bFemale;
		FRandomStream Random(Seed);
		CreatorRandomize(Person, Random);
		Person.Looks.bFemale = bFemale;
		Person.Looks.MakeHuman = bFemale ? TEXT("CreatorFemale") : TEXT("CreatorMale");
		return Person;
	}
}

FSimPawnLooks CreatorPeople::RandomLooks(int32 Seed, bool bFemale, const FSimPawnLooks& From)
{
	const FCreatorPerson Person = CreatorPeopleMake(Seed, bFemale);
	FSimPawnLooks Looks = From;
	Looks.bFemale = bFemale;
	Looks.MakeHuman = Person.Looks.MakeHuman;
	Looks.Height = 1.0f;
	Looks.Skin = FLinearColor(Person.Skin[0], Person.Skin[1], Person.Skin[2]);
	return Looks;
}

FSimPawnLooks CreatorPeople::DefaultLooks(bool bFemale, const FSimPawnLooks& From)
{
	const FCreatorPerson Person;
	FSimPawnLooks Looks = From;
	Looks.bFemale = bFemale;
	Looks.MakeHuman = bFemale ? TEXT("CreatorFemale") : TEXT("CreatorMale");
	Looks.Height = 1.0f;
	Looks.Skin = FLinearColor(Person.Skin[0], Person.Skin[1], Person.Skin[2]);
	return Looks;
}

bool CreatorPeople::ApplyDefault(bool bFemale, ASimPawnActor& Pawn)
{
	FCreatorPerson Person;
	Person.bFemale = bFemale;
	Person.Looks.bFemale = bFemale;
	Person.Looks.MakeHuman = bFemale ? TEXT("CreatorFemale") : TEXT("CreatorMale");
	if (!Pawn.GetMakeHumanMesh() || Pawn.GetLooks().MakeHuman != Person.Looks.MakeHuman)
	{
		return false;
	}
	Person.bBlink = false;
	CreatorApply(Person, Pawn, false);
	return true;
}

bool CreatorPeople::ApplyRandom(int32 Seed, bool bFemale, ASimPawnActor& Pawn)
{
	FCreatorPerson Person = CreatorPeopleMake(Seed, bFemale);
	if (!Pawn.GetMakeHumanMesh() || Pawn.GetLooks().MakeHuman != Person.Looks.MakeHuman)
	{
		return false;
	}
	Person.bBlink = false;
	CreatorApply(Person, Pawn, false);
	return true;
}
