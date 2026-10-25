#include "AnimalArtCommandlet.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimalArt, Log, All);

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
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("models")))
	{
		const FString Model = Value->AsString();
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/Animals"), Model + TEXT(".fbx"));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogAnimalArt, Error, TEXT("%s is not downloaded: run python tools/animal_art.py"), *File);
			++Failed;
			continue;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FString(TEXT("/Game/Rim3D/Generated/Animals/")) + Model;
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
