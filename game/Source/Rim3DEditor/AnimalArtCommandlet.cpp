#include "AnimalArtCommandlet.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimalArt, Log, All);

namespace
{
	const TCHAR* const AnimalArtRoot = TEXT("/Game/Rim3D/Generated/Animals/");

	bool AnimalArtSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	// Moves an imported asset to <folder>/<NewName> (the name the game loads) and saves it there.
	bool AnimalArtRename(IAssetTools& Tools, UObject* Object, const FString& Folder, const FString& NewName)
	{
		if (Object->GetName() != NewName)
		{
			if (!Tools.RenameAssets({FAssetRenameData(Object, Folder, NewName)}))
			{
				UE_LOG(LogAnimalArt, Error, TEXT("Cannot rename %s to %s"), *Object->GetPathName(), *NewName);
				return false;
			}
		}
		return AnimalArtSave(Object);
	}

	/**
	 * One realistic model from the art table's `imports` (glTF from assets/raw/sketchfab): imports it into
	 * <root>/<model>, names the mesh <model> and the clips <model>AnimalArmature_<role> like the Quaternius ones.
	 */
	bool AnimalArtImportGltf(IAssetTools& Tools, const TSharedPtr<FJsonObject>& Entry)
	{
		const FString Model = Entry->GetStringField(TEXT("model"));
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), Entry->GetStringField(TEXT("file"))));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogAnimalArt, Error, TEXT("%s is missing (sneed's Sketchfab download, assets/raw/sketchfab)"), *File);
			return false;
		}
		const FString Folder = FString(AnimalArtRoot) + Model;
		IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Folder), false, true);
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = Folder;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		// The body, eyes, tongue, teeth and fur cards are separate skinned parts in these files: one mesh for all of them.
		UInterchangeGenericAssetsPipeline* Pipeline = NewObject<UInterchangeGenericAssetsPipeline>();
		if (Pipeline->MeshPipeline)
		{
			Pipeline->MeshPipeline->bCombineSkeletalMeshes = true;
		}
		Task->Options = Pipeline;
		Tools.ImportAssetTasks({Task});

		TArray<USkeletalMesh*> Meshes;
		TArray<UAnimSequence*> Clips;
		int32 Skeletons = 0;
		for (UObject* Object : Task->GetObjects())
		{
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object)) { Meshes.Add(Mesh); }
			if (UAnimSequence* Clip = Cast<UAnimSequence>(Object)) { Clips.Add(Clip); }
			Skeletons += Cast<USkeleton>(Object) ? 1 : 0;
		}
		if (Meshes.IsEmpty())
		{
			// Some importers hand back nothing: look in the folder.
			TArray<FAssetData> Found;
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(*Folder), Found, true);
			for (const FAssetData& Data : Found)
			{
				UObject* Object = Data.GetAsset();
				if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object)) { Meshes.Add(Mesh); }
				if (UAnimSequence* Clip = Cast<UAnimSequence>(Object)) { Clips.Add(Clip); }
				Skeletons += Cast<USkeleton>(Object) ? 1 : 0;
			}
		}
		FString Names;
		for (const UAnimSequence* Clip : Clips)
		{
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::Printf(TEXT("%s (%.1f s)"), *Clip->GetName(), Clip->GetPlayLength());
		}
		UE_LOG(LogAnimalArt, Display, TEXT("%s: %d skeletal mesh, %d skeleton, %d clips: %s"), *Model, Meshes.Num(), Skeletons, Clips.Num(), *Names);
		if (Meshes.Num() != 1)
		{
			UE_LOG(LogAnimalArt, Error, TEXT("%s: wanted one skeletal mesh, got %d"), *Model, Meshes.Num());
			return false;
		}
		bool bOk = AnimalArtRename(Tools, Meshes[0], Folder, Model);
		const FBox Box = Meshes[0]->GetBounds().GetBox();
		UE_LOG(LogAnimalArt, Display, TEXT("%s: mesh %.2f x %.2f x %.2f m, %d materials"), *Model, Box.GetSize().X / 100.0, Box.GetSize().Y / 100.0,
			Box.GetSize().Z / 100.0, Meshes[0]->GetMaterials().Num());
		const TSharedPtr<FJsonObject>* Roles = nullptr;
		if (Entry->TryGetObjectField(TEXT("clips"), Roles))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Role : (*Roles)->Values)
			{
				const FString Wanted = Role.Value->AsString();
				UAnimSequence* Match = nullptr;
				for (UAnimSequence* Clip : Clips)
				{
					// Importers prefix clip names with the file or mesh name; the glTF animation name is at the end.
					if (Clip->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || Clip->GetName().EndsWith(Wanted, ESearchCase::IgnoreCase))
					{
						Match = Clip;
						break;
					}
				}
				if (!Match)
				{
					UE_LOG(LogAnimalArt, Error, TEXT("%s: no clip named %s for %s"), *Model, *Wanted, *Role.Key);
					bOk = false;
					continue;
				}
				bOk = AnimalArtRename(Tools, Match, Folder, Model + TEXT("AnimalArmature_") + Role.Key) && bOk;
			}
		}
		return bOk;
	}
}

UAnimalArtCommandlet::UAnimalArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UAnimalArtCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/animal_art.json"));
	if (!FFileHelper::LoadFileToString(Text, *FPaths::ConvertRelativePathToFull(ListFile)))
	{
		UE_LOG(LogAnimalArt, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogAnimalArt, Error, TEXT("Cannot parse %s"), *ListFile);
		return 1;
	}

	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Failed = 0;
	// -Models=A,B imports only those (either list); with no -Models every model is imported.
	FString Only;
	FParse::Value(*Params, TEXT("Models="), Only, false);
	TArray<FString> OnlyList;
	Only.ParseIntoArray(OnlyList, TEXT(","));
	const auto Wanted = [&OnlyList](const FString& Model) { return OnlyList.IsEmpty() || OnlyList.Contains(Model); };

	const TArray<TSharedPtr<FJsonValue>>* Imports = nullptr;
	if (Root->TryGetArrayField(TEXT("imports"), Imports))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Imports)
		{
			if (Wanted(Value->AsObject()->GetStringField(TEXT("model"))) && !AnimalArtImportGltf(Tools, Value->AsObject()))
			{
				++Failed;
			}
		}
	}
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("models")))
	{
		const FString Model = Value->AsString();
		if (!Wanted(Model))
		{
			continue;
		}
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/Animals"), Model + TEXT(".fbx"));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogAnimalArt, Error, TEXT("%s is not downloaded: run python tools/animal_art.py"), *File);
			++Failed;
			continue;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FString(AnimalArtRoot) + Model;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Tools.ImportAssetTasks({Task});
		int32 Meshes = 0, Anims = 0, Skeletons = 0;
		for (UObject* Object : Task->GetObjects())
		{
			Meshes += Cast<USkeletalMesh>(Object) ? 1 : 0;
			Anims += Cast<UAnimSequence>(Object) ? 1 : 0;
			Skeletons += Cast<USkeleton>(Object) ? 1 : 0;
		}
		UE_LOG(LogAnimalArt, Display, TEXT("%s: %d skeletal mesh, %d skeleton, %d animations"), *Model, Meshes, Skeletons, Anims);
		if (Meshes == 0 || Skeletons == 0)
		{
			++Failed;
		}
	}
	UE_LOG(LogAnimalArt, Display, TEXT("AnimalArt %s"), Failed ? TEXT("FAILED") : TEXT("done"));
	return Failed ? 1 : 0;
}
