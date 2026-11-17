#include "ShieldArtCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogShieldArt, Log, All);

namespace
{
	const TCHAR* const ShieldArtFolder = TEXT("/Game/Rim3D/Generated/Shields");

	bool ShieldArtSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}
}

UShieldArtCommandlet::UShieldArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UShieldArtCommandlet::Main(const FString& Params)
{
	const FString SourceDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/Shields"));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(SourceDir / TEXT("SM_Shield_*.glb")), true, false);
	Files.Sort();
	if (Files.IsEmpty())
	{
		UE_LOG(LogShieldArt, Error, TEXT("No SM_Shield_*.glb in %s: run blender -b --python tools/shield_models.py -- %s"), *SourceDir, *SourceDir);
		return 1;
	}
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(ShieldArtFolder), false, true);
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Made = 0;
	for (const FString& Name : Files)
	{
		const FString Id = FPaths::GetBaseFilename(Name);
		const FString Folder = FString(ShieldArtFolder) / Id;
		// One static mesh per shield: plate, rim, boss, grip and strap joined.
		UInterchangeGenericAssetsPipeline* Pipeline = NewObject<UInterchangeGenericAssetsPipeline>();
		if (Pipeline->MeshPipeline)
		{
			Pipeline->MeshPipeline->CombineStaticMeshesBehavior = EInterchangeCombineStaticMeshesBehavior::All;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = SourceDir / Name;
		Task->DestinationPath = Folder;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->Options = Pipeline;
		Tools.ImportAssetTasks({Task});
		UStaticMesh* Mesh = nullptr;
		for (UObject* Object : Task->GetObjects())
		{
			if (UStaticMesh* Found = Cast<UStaticMesh>(Object))
			{
				Mesh = Found;
				break;
			}
		}
		if (!Mesh)
		{
			UE_LOG(LogShieldArt, Error, TEXT("Importing %s made no static mesh"), *Name);
			continue;
		}
		// Named after the file, so the defs can name it.
		if (Mesh->GetName() != Id)
		{
			TArray<FAssetRenameData> Rename;
			Rename.Emplace(Mesh, Folder, Id);
			Tools.RenameAssets(Rename);
		}
		Mesh->NaniteSettings.bEnabled = false;
		Mesh->PostEditChange();
		const bool bSaved = ShieldArtSave(Mesh);
		const FBox Box = Mesh->GetBoundingBox();
		UE_LOG(LogShieldArt, Display, TEXT("%s: %s, %d materials, %.0f x %.0f x %.0f cm%s"), *Id, *Mesh->GetPathName(), Mesh->GetStaticMaterials().Num(),
			Box.GetSize().X, Box.GetSize().Y, Box.GetSize().Z, bSaved ? TEXT("") : TEXT(" (NOT saved)"));
		Made += bSaved ? 1 : 0;
	}
	UE_LOG(LogShieldArt, Display, TEXT("Shields: %d of %d imported into %s"), Made, Files.Num(), ShieldArtFolder);
	return Made == Files.Num() ? 0 : 1;
}
