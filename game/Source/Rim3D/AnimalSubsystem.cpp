#include "AnimalSubsystem.h"

#include "Animation/AnimSequence.h"
#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "AnimalActor.h"
#include "SimSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogSimAnimals, Log, All);

namespace
{
	TAutoConsoleVariable<float> GAnimalsNearDistance(TEXT("Animals.NearDistance"), 40.0f,
		TEXT("Metres within which animals animate every frame."));
	TAutoConsoleVariable<float> GAnimalsMidDistance(TEXT("Animals.MidDistance"), 120.0f,
		TEXT("Metres within which animals animate 8 times a second; beyond it they hold a frozen pose."));
	TAutoConsoleVariable<float> GAnimalsHideDistance(TEXT("Animals.HideDistance"), 400.0f,
		TEXT("Metres beyond which animals are not drawn."));

	const TCHAR* const AnimalArtFolder = TEXT("/Game/Rim3D/Generated/Animals/");
	// The model used for a def that is not in the art table (no model yet, or a new def).
	const TCHAR* const AnimalFallbackModel = TEXT("Husky");

	uint32 AnimalKey(colony::Entity Thing)
	{
		return static_cast<uint32>(entt::to_integral(Thing));
	}

	/** Loads "/Game/Folder/Asset" (the asset named like its package) or a full "/Game/Folder/Asset.Asset" path. */
	template <typename T>
	T* AnimalLoadPath(const FString& Path)
	{
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		if (!FPackageName::DoesPackageExist(Package))
		{
			return nullptr;
		}
		return LoadObject<T>(nullptr, *(Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	template <typename T>
	T* AnimalLoad(const FString& Model, const FString& Name)
	{
		const FString Package = FString(AnimalArtFolder) / Model / Name;
		return FPackageName::DoesPackageExist(Package) ? LoadObject<T>(nullptr, *(Package + TEXT(".") + Name)) : nullptr;
	}
}

bool USimAnimalSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USimAnimalSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USimAnimalSubsystem, STATGROUP_Tickables);
}

void USimAnimalSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
}

void USimAnimalSubsystem::LoadArtTable()
{
	bArtTried = true;
	// assets/ sits beside the sim's data folder, so a packaged game started with -SimData=<repo>\data finds it too.
	const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr;
	const FString Repo = Bridge && !Bridge->GetDataRoot().IsEmpty() ? Bridge->GetDataRoot() / TEXT("..") : FPaths::ProjectDir() / TEXT("..");
	FString File = FPaths::ConvertRelativePathToFull(Repo / TEXT("assets/animal_art.json"));
	FPaths::CollapseRelativeDirectories(File);
	if (!FPaths::FileExists(File))
	{
		File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../assets/animal_art.json"));  // tests on fixture defs
	}
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogSimAnimals, Error, TEXT("Cannot read %s; animals are drawn as %s"), *File, AnimalFallbackModel);
		return;
	}
	if (const TSharedPtr<FJsonObject>* Lengths = nullptr; Root->TryGetObjectField(TEXT("lengthM"), Lengths))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Lengths)->Values)
		{
			LengthsCm.Add(FName(*Pair.Key), static_cast<float>(Pair.Value->AsNumber() * 100.0));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Imports = nullptr;
	if (Root->TryGetArrayField(TEXT("imports"), Imports))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Imports)
		{
			const TSharedPtr<FJsonObject> Entry = Value->AsObject();
			if (!Entry.IsValid())
			{
				continue;
			}
			double YawValue = -90.0, Walk = 140.0, Gallop = 450.0;
			Entry->TryGetNumberField(TEXT("yaw"), YawValue);
			Entry->TryGetNumberField(TEXT("walkCm"), Walk);
			Entry->TryGetNumberField(TEXT("gallopCm"), Gallop);
			const FName ModelName(*Entry->GetStringField(TEXT("model")));
			ImportLooks.Add(ModelName, FVector3f(YawValue, Walk, Gallop));
			if (FString Fit; Entry->TryGetStringField(TEXT("fit"), Fit) && Fit == TEXT("pose"))
			{
				FitFromPose.Add(ModelName);
			}
			if (double Fix = 1.0; Entry->TryGetNumberField(TEXT("sizeFix"), Fix) && Fix > 0.0)
			{
				SizeFixes.Add(ModelName, static_cast<float>(Fix));
			}
			if (const TSharedPtr<FJsonObject>* Paths = nullptr; Entry->TryGetObjectField(TEXT("assets"), Paths))
			{
				TMap<FString, FString>& Roles = ImportAssets.Add(ModelName);
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Paths)->Values)
				{
					Roles.Add(Pair.Key, Pair.Value->AsString());
				}
			}
			if (const TArray<TSharedPtr<FJsonValue>>* PartList = nullptr; Entry->TryGetArrayField(TEXT("parts"), PartList))
			{
				TArray<FString>& Paths = ImportParts.Add(ModelName);
				for (const TSharedPtr<FJsonValue>& Part : *PartList)
				{
					Paths.Add(Part->AsString());
				}
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Defs = nullptr;
	if (!Root->TryGetArrayField(TEXT("defs"), Defs))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Defs)
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (!Entry.IsValid())
		{
			continue;
		}
		FSimAnimalArt Item;
		Item.Model = FName(*Entry->GetStringField(TEXT("model")));
		Item.Scale = Entry->HasField(TEXT("scale")) ? static_cast<float>(Entry->GetNumberField(TEXT("scale"))) : 1.0f;
		Item.bStandIn = Entry->HasField(TEXT("stand_in")) && Entry->GetBoolField(TEXT("stand_in"));
		Art.Add(FName(*Entry->GetStringField(TEXT("def"))), Item);
	}
}

const FSimAnimalModel* USimAnimalSubsystem::GetModelFor(FName DefName, FSimAnimalArt& OutArt)
{
	if (!bArtTried)
	{
		LoadArtTable();
	}
	if (const FSimAnimalArt* Found = Art.Find(DefName))
	{
		OutArt = *Found;
	}
	else
	{
		OutArt = FSimAnimalArt();
		OutArt.Model = AnimalFallbackModel;
		OutArt.bStandIn = true;
		if (!Warned.Contains(DefName))
		{
			Warned.Add(DefName);
			UE_LOG(LogSimAnimals, Display, TEXT("%s has no model in assets/animal_art.json; drawn as a %s"), *DefName.ToString(), AnimalFallbackModel);
		}
	}
	FSimAnimalModel& Model = Models.FindOrAdd(OutArt.Model);
	if (!Model.bLoaded && !Warned.Contains(OutArt.Model))
	{
		const FString Name = OutArt.Model.ToString();
		Model.Mesh = AnimalLoad<USkeletalMesh>(Name, Name);
		for (int32 Part = 0; Part < 32; ++Part)
		{
			USkeletalMesh* Piece = AnimalLoad<USkeletalMesh>(Name, FString::Printf(TEXT("%s_Part%d"), *Name, Part));
			if (!Piece)
			{
				break;
			}
			Model.Parts.Add(Piece);
			KeepAlive.Add(Piece);
		}
		const FString Prefix = Name + TEXT("AnimalArmature_");
		Model.Idle = AnimalLoad<UAnimSequence>(Name, Prefix + TEXT("Idle"));
		Model.Walk = AnimalLoad<UAnimSequence>(Name, Prefix + TEXT("Walk"));
		Model.Gallop = AnimalLoad<UAnimSequence>(Name, Prefix + TEXT("Gallop"));
		Model.Eat = AnimalLoad<UAnimSequence>(Name, Prefix + TEXT("Eating"));
		Model.Death = AnimalLoad<UAnimSequence>(Name, Prefix + TEXT("Death"));
		if (const TMap<FString, FString>* Paths = ImportAssets.Find(OutArt.Model))
		{
			// A pack already in Unreal's format: the art table names each asset by its path.
			const auto Clip = [Paths](const TCHAR* Role) { return Paths->Contains(Role) ? AnimalLoadPath<UAnimSequence>((*Paths)[Role]) : nullptr; };
			Model.Mesh = Paths->Contains(TEXT("mesh")) ? AnimalLoadPath<USkeletalMesh>((*Paths)[TEXT("mesh")]) : nullptr;
			Model.Idle = Clip(TEXT("Idle"));
			Model.Walk = Clip(TEXT("Walk"));
			Model.Gallop = Clip(TEXT("Gallop"));
			Model.Eat = Clip(TEXT("Eating"));
			Model.Death = Clip(TEXT("Death"));
			Model.Parts.Reset();
			for (const FString& Path : ImportParts.FindRef(OutArt.Model))
			{
				if (USkeletalMesh* Piece = AnimalLoadPath<USkeletalMesh>(Path))
				{
					Model.Parts.Add(Piece);
					KeepAlive.Add(Piece);
				}
			}
		}
		// A model with no walk clip (the free Buffalo_A1) walks on its idle: the body glides, but nothing spins or jerks.
		if (!Model.Walk) { Model.Walk = Model.Idle; }
		if (!Model.Mesh || !Model.Idle)
		{
			UE_LOG(LogSimAnimals, Error, TEXT("Model %s is missing its mesh or idle clip: run the AnimalArt commandlet (assets/animal_art.json)"), *Name);
			Warned.Add(OutArt.Model);
			return nullptr;
		}
		// A clip that is missing falls back on the idle.
		if (!Model.Gallop) { Model.Gallop = Model.Walk; }
		if (!Model.Eat) { Model.Eat = Model.Idle; }
		Model.bHasDeath = Model.Death != nullptr;
		if (!Model.Death) { Model.Death = Model.Idle; }
		if (const FVector3f* Looks = ImportLooks.Find(OutArt.Model))
		{
			Model.YawOffset = Looks->X;
			Model.WalkClipSpeed = FMath::Max(Looks->Y, 10.0f);
			Model.GallopClipSpeed = FMath::Max(Looks->Z, 10.0f);
		}
		Model.Extent = Model.Mesh->GetBounds().BoxExtent;
		const float NativeLength = FMath::Max(Model.Extent.X, Model.Extent.Y) * 2.0f;
		const float* WantedLength = LengthsCm.Find(OutArt.Model);
		Model.FitScale = WantedLength && NativeLength > 1.0f ? *WantedLength / NativeLength : 1.0f;
		Model.WantedLengthCm = WantedLength ? *WantedLength : 0.0f;
		Model.bFitFromPose = Model.WantedLengthCm > 0.0f && FitFromPose.Contains(OutArt.Model);
		Model.SizeFix = SizeFixes.Contains(OutArt.Model) ? SizeFixes[OutArt.Model] : 1.0f;
		Model.FitScale *= Model.SizeFix;
		Model.bLoaded = true;
		KeepAlive.Add(Model.Mesh);
		KeepAlive.Add(Model.Idle);
		KeepAlive.Add(Model.Walk);
		KeepAlive.Add(Model.Gallop);
		KeepAlive.Add(Model.Eat);
		KeepAlive.Add(Model.Death);
		UE_LOG(LogSimAnimals, Display, TEXT("Model %s loaded: half size %.0f x %.0f x %.0f cm, fitted by %.2f"), *Name, Model.Extent.X, Model.Extent.Y, Model.Extent.Z, Model.FitScale);
	}
	return Model.bLoaded ? &Model : nullptr;
}

int32 USimAnimalSubsystem::GetModelsLoaded() const
{
	int32 Count = 0;
	for (const TPair<FName, FSimAnimalModel>& Pair : Models)
	{
		Count += Pair.Value.bLoaded ? 1 : 0;
	}
	return Count;
}

ASimAnimalActor* USimAnimalSubsystem::FindAnimal(colony::Entity Thing) const
{
	const TObjectPtr<ASimAnimalActor>* Found = Animals.Find(AnimalKey(Thing));
	return Found ? Found->Get() : nullptr;
}

void USimAnimalSubsystem::RemoveAll()
{
	for (const TPair<uint32, TObjectPtr<ASimAnimalActor>>& Pair : Animals)
	{
		if (IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	Animals.Reset();
	DetailCounts = FSimAnimalDetailCounts();
}

void USimAnimalSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !Bridge->GetSim())
	{
		RemoveAll();
		return;
	}
	// A new sim reuses entity numbers.
	if (Bridge->GetSim() != LastSim)
	{
		RemoveAll();
		LastSim = Bridge->GetSim();
	}

	const colony::Snapshot& Now = Bridge->GetCurrentSnapshot();
	const colony::Snapshot& Before = Bridge->GetPreviousSnapshot();
	const colony::DefDatabase* Defs = Bridge->GetDefs().get();
	TSet<uint32> Seen;
	for (const colony::ThingView& View : Now.things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.lifeStage < 0 || !Defs || !View.def.valid())
		{
			continue;
		}
		const uint32 Key = AnimalKey(View.id);
		TObjectPtr<ASimAnimalActor>* Existing = Animals.Find(Key);
		if (!Existing || !IsValid(*Existing))
		{
			FSimAnimalArt Looks;
			const FSimAnimalModel* Model = GetModelFor(FName(UTF8_TO_TCHAR(Defs->get(View.def).defName().c_str())), Looks);
			if (!Model)
			{
				continue;  // the log says which model is missing
			}
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			FVector Location;
			Bridge->GetBlendedLocation(View.id, Location);
			ASimAnimalActor* Animal = GetWorld()->SpawnActor<ASimAnimalActor>(Location, FRotator::ZeroRotator, Params);
			Animal->Setup(*Model, Looks.Scale, View.id);
			Existing = &Animals.Add(Key, Animal);
		}
		Seen.Add(Key);
		(*Existing)->UpdateFromSim(*Bridge, View, Before.find(View.id), DeltaTime, Bridge->GetSpeed());
	}
	for (auto It = Animals.CreateIterator(); It; ++It)
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

void USimAnimalSubsystem::AssignDetail(const FVector& CameraLocation)
{
	const double Near = FMath::Square(GAnimalsNearDistance.GetValueOnGameThread() * 100.0);
	const double Mid = FMath::Square(GAnimalsMidDistance.GetValueOnGameThread() * 100.0);
	const double Hide = FMath::Square(GAnimalsHideDistance.GetValueOnGameThread() * 100.0);
	DetailCounts = FSimAnimalDetailCounts();
	for (const TPair<uint32, TObjectPtr<ASimAnimalActor>>& Pair : Animals)
	{
		ASimAnimalActor* Animal = Pair.Value;
		const double Distance = FVector::DistSquared(Animal->GetActorLocation(), CameraLocation);
		if (Distance < Near)
		{
			Animal->SetDetail(ESimAnimalDetail::Near);
			++DetailCounts.Near;
		}
		else if (Distance < Mid)
		{
			Animal->SetDetail(ESimAnimalDetail::Mid);
			++DetailCounts.Mid;
		}
		else if (Distance < Hide)
		{
			Animal->SetDetail(ESimAnimalDetail::Far);
			++DetailCounts.Far;
		}
		else
		{
			Animal->SetDetail(ESimAnimalDetail::Hidden);
			++DetailCounts.Hidden;
		}
	}
}
