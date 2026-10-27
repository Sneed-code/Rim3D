#include "PawnSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Camera/PlayerCameraManager.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PoseSearch/PoseSearchDatabase.h"

#include "PawnActor.h"
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
	TAutoConsoleVariable<int32> GPawnsShowDetail(
		TEXT("Pawns.ShowDetail"), 0, TEXT("1 marks each pawn's animation detail over its head: green full, yellow near, red far."), ECVF_Default);

	constexpr float PawnNearTickInterval = 1.0f / 20.0f;
	constexpr float PawnFarTickInterval = 1.0f / 8.0f;

	const TCHAR* const PawnBodyMeshPath = TEXT("/Game/Characters/UEFN_Mannequin/Meshes/SKM_UEFN_Mannequin.SKM_UEFN_Mannequin");
	const TCHAR* const PawnMetaHumanClassPath = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");
	const TCHAR* const PawnIdlePath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Idle/M_Neutral_Stand_Idle_Loop.M_Neutral_Stand_Idle_Loop");
	const TCHAR* const PawnWalkPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Walk/M_Neutral_Walk_Loop_F.M_Neutral_Walk_Loop_F");
	const TCHAR* const PawnRunPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Run/M_Neutral_Run_Loop_F.M_Neutral_Run_Loop_F");
	// Placeholder for picking up and putting down until there are real action animations: a short crouch.
	const TCHAR* const PawnCrouchPath = TEXT("/Game/Characters/UEFN_Mannequin/Animations/Idle/M_Neutral_Crouch_Idle_Loop.M_Neutral_Crouch_Idle_Loop");

	// The sample's standing locomotion databases (its crouch, jump and traversal ones are left out).
	const TCHAR* const PawnDatabaseNames[] = {
		TEXT("PSD_Dense_Stand_Idles"),
		TEXT("PSD_Dense_Stand_TurnInPlace"),
		TEXT("PSD_Dense_Stand_Walk_Loops"),
		TEXT("PSD_Dense_Stand_Walk_Starts"),
		TEXT("PSD_Dense_Stand_Walk_Stops"),
		TEXT("PSD_Dense_Stand_Walk_Pivots"),
		TEXT("PSD_Dense_Stand_Run_Loops"),
		TEXT("PSD_Dense_Stand_Run_Starts"),
		TEXT("PSD_Dense_Stand_Run_Stops"),
		TEXT("PSD_Dense_Stand_Run_Pivots"),
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
	IdleLoop = LoadObject<UAnimSequence>(nullptr, PawnIdlePath);
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
		UE_LOG(LogSimPawns, Display, TEXT("Pawn animation: %d motion matching databases, walk loop %.0f cm/s, run loop %.0f cm/s"),
			Databases.Num(), WalkLoopSpeed, RunLoopSpeed);
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
	const float Thin = FMath::Max(0.0f, -Weight);
	const float Heavy = FMath::Max(0.0f, Weight);
	if (bFemale)
	{
		return {{TEXT("Thin"), 0.0f}, {TEXT("Heavy"), 0.0f}, {TEXT("Female"), 1.0f - Thin - Heavy}, {TEXT("FemaleThin"), Thin},
			{TEXT("FemaleHeavy"), Heavy}};
	}
	return {{TEXT("Thin"), Thin}, {TEXT("Heavy"), Heavy}, {TEXT("Female"), 0.0f}, {TEXT("FemaleThin"), 0.0f}, {TEXT("FemaleHeavy"), 0.0f}};
}

FSimPawnLooks USimPawnSubsystem::MakeLooks(const colony::PawnLooksView& View, const colony::DefDatabase* Defs)
{
	FSimPawnLooks Looks;
	Looks.Height = FMath::Clamp(View.height, 0.5f, 1.5f);
	Looks.Weight = FMath::Clamp(View.weight, -1.0f, 1.0f);
	Looks.bFemale = View.female;
	Looks.Skin = PawnLinear(View.skin);
	Looks.Hair = PawnLinear(View.hair);
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
	if (!Defs)
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
		if (Piece->stuff.valid() && Piece->stuff.value < Defs->size())
		{
			const colony::Def& Stuff = Defs->get(Piece->stuff);
			if (Stuff.count("visual.color") >= 3)
			{
				Worn.bHasTint = true;
				Worn.Tint = VisualMesh::ReadColor(Stuff, "visual.color", FLinearColor::White);
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
	UMaterialInterface* Material = PawnLoadLooksAsset<UMaterialInterface>(PawnLooksSkinInstanceName(Original->GetName()));
	SkinMaterials.Add(Original->GetFName(), Material);
	return Material;
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
	}
	return CrouchPose;
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
		if (View.looks >= 0 && View.looks < static_cast<int32>(Now.looks.size()))
		{
			Pawn->ApplyLooks(*this, MakeLooks(Now.looks[static_cast<size_t>(View.looks)], Defs));
		}
		// Worn clothes (U16): rebuilt only when what is worn, or how torn it is, changed.
		const uint64 WornKey = ApparelKey(Now, View);
		if (!Pawn->HasApparel() || Pawn->GetApparelKey() != WornKey)
		{
			Pawn->ApplyApparel(*this, MakeApparel(Now, View, Defs), WornKey);
		}
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
