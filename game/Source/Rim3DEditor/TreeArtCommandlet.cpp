#include "TreeArtCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "IAssetTools.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogTreeArt, Log, All);

namespace
{
	const TCHAR* const TreeFolder = TEXT("/Game/Rim3D/Generated/Trees");

	bool TreeSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}
}

UTreeArtCommandlet::UTreeArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UTreeArtCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/tree_art.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogTreeArt, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(TreeFolder), false, true);
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Made = 0;
	int32 Wanted = 0;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("trees")))
	{
		const FString Id = Value->AsObject()->GetStringField(TEXT("id"));
		++Wanted;
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/Trees"), Id, Id + TEXT(".gltf"));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogTreeArt, Error, TEXT("%s is not downloaded: run python tools/tree_art.py"), *File);
			continue;
		}
		// One static mesh per model (trunk and leaves joined), named after the model.
		UInterchangeGenericAssetsPipeline* Pipeline = NewObject<UInterchangeGenericAssetsPipeline>();
		if (Pipeline->MeshPipeline)
		{
			Pipeline->MeshPipeline->CombineStaticMeshesBehavior = EInterchangeCombineStaticMeshesBehavior::All;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FString(TreeFolder) / Id;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->Options = Pipeline;
		Tools.ImportAssetTasks({Task});
		TArray<UStaticMesh*> Meshes;
		for (UObject* Object : Task->GetObjects())
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
			{
				Meshes.Add(Mesh);
			}
		}
		if (Meshes.IsEmpty())
		{
			UE_LOG(LogTreeArt, Error, TEXT("Importing %s made no static mesh"), *File);
			continue;
		}
		if (Meshes.Num() > 1)
		{
			// A set of separate pieces (the rock sets): each stays its own mesh, named <id>_<piece>.
			UE_LOG(LogTreeArt, Display, TEXT("%s: %d separate meshes"), *Id, Meshes.Num());
		}
		UStaticMesh* Mesh = Meshes[0];
		// Nanite draws every leaf; foliage keeps its area as it simplifies so far crowns do not thin out.
		bool bSaved = true;
		for (UStaticMesh* Each : Meshes)
		{
			Each->NaniteSettings.bEnabled = true;
			Each->NaniteSettings.ShapePreservation = ENaniteShapePreservation::PreserveArea;
			Each->PostEditChange();
			// The import saved the mesh, its materials and textures; save the mesh again with Nanite on.
			bSaved = TreeSave(Each) && bSaved;
			// Their materials must say they are used on Nanite and instanced meshes (the rock scatter draws them
			// instanced): a game can't add the flag itself, so it drew the default material (pale grey boxes).
			for (const FStaticMaterial& Slot : Each->GetStaticMaterials())
			{
				UMaterial* Base = Slot.MaterialInterface ? Slot.MaterialInterface->GetMaterial() : nullptr;
				if (!Base || !Base->GetPathName().StartsWith(TreeFolder))
				{
					continue;
				}
				bool bNeedsRecompile = false;
				const bool bNanite = Base->SetMaterialUsage(bNeedsRecompile, MATUSAGE_Nanite);
				const bool bInstanced = Base->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
				if (!bNanite || !bInstanced)
				{
					UE_LOG(LogTreeArt, Warning, TEXT("%s: could not set the Nanite or instanced usage"), *Base->GetPathName());
				}
				Base->PostEditChange();
				bSaved = TreeSave(Base) && bSaved;
			}
		}
		const FBox Box = Mesh->GetBoundingBox();
		UE_LOG(LogTreeArt, Display, TEXT("%s: %s, %d materials, %.1f x %.1f x %.1f m%s"), *Id, *Mesh->GetPathName(), Mesh->GetStaticMaterials().Num(),
			Box.GetSize().X / 100.0, Box.GetSize().Y / 100.0, Box.GetSize().Z / 100.0, bSaved ? TEXT("") : TEXT(" (NOT all saved)"));
		Made += bSaved ? 1 : 0;
	}
	UE_LOG(LogTreeArt, Display, TEXT("Trees: %d of %d imported into %s"), Made, Wanted, TreeFolder);
	return Made == Wanted ? 0 : 1;
}
