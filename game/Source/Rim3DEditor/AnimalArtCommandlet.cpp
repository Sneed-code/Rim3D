#include "AnimalArtCommandlet.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "IAssetTools.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
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
	 * Logs each imported material's parameters and, for an entry with "albedo" (the colour texture's asset name),
	 * makes the colour come through: some FBX files bring a black diffuse colour or leave the colour map empty.
	 */
	bool AnimalArtFixMaterials(USkeletalMesh* Mesh, const FString& Folder, const TSharedPtr<FJsonObject>& Entry)
	{
		// "albedo" is a picture file (repo-relative) imported next to the mesh as the body's colour map.
		FString AlbedoFile;
		const bool bFix = Entry->TryGetStringField(TEXT("albedo"), AlbedoFile);
		UTexture* Albedo = nullptr;
		if (bFix)
		{
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), AlbedoFile));
			Task->DestinationPath = Folder;
			Task->bAutomated = true;
			Task->bReplaceExisting = true;
			Task->bSave = true;
			FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
			for (UObject* Object : Task->GetObjects())
			{
				Albedo = Albedo ? Albedo : Cast<UTexture>(Object);
			}
			if (!Albedo)
			{
				UE_LOG(LogAnimalArt, Error, TEXT("%s: cannot import the colour map %s"), *Mesh->GetName(), *Task->Filename);
				return false;
			}
		}
		for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
		{
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Slot.MaterialInterface);
			if (!Instance)
			{
				UE_LOG(LogAnimalArt, Display, TEXT("%s: material %s is not an instance"), *Mesh->GetName(), Slot.MaterialInterface ? *Slot.MaterialInterface->GetPathName() : TEXT("none"));
				continue;
			}
			FString Info = FString::Printf(TEXT("parent %s;"), Instance->Parent ? *Instance->Parent->GetName() : TEXT("none"));
			bool bChanged = false;
			// The engine's FBX master has no skeletal or morph-target usage flags and cannot be changed: without them
			// the game draws the default material. A copy in the model's folder gets them, and the instances use it.
			UMaterial* Base = Instance->GetMaterial();
			const bool bFbx = Entry->GetStringField(TEXT("file")).EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase);
			if ((bFix || bFbx) && Base && Base->GetPathName().StartsWith(TEXT("/Interchange")))
			{
				const FString CopyName = Base->GetName() + TEXT("_R3D");
				UMaterial* Copy = LoadObject<UMaterial>(nullptr, *(Folder / CopyName + TEXT(".") + CopyName), nullptr, LOAD_Quiet | LOAD_NoWarn);
				if (!Copy)
				{
					Copy = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().DuplicateAsset(CopyName, Folder, Base));
					if (Copy)
					{
						Copy->bUsedWithSkeletalMesh = true;
						Copy->bUsedWithMorphTargets = true;
						Copy->PostEditChange();
						AnimalArtSave(Copy);
					}
				}
				if (Copy)
				{
					Instance->SetParentEditorOnly(Copy);
					bChanged = true;
				}
				else
				{
					UE_LOG(LogAnimalArt, Error, TEXT("%s: cannot copy %s"), *Mesh->GetName(), *Base->GetPathName());
				}
			}
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Instance->GetAllTextureParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& P : Infos)
			{
				UTexture* Value = nullptr;
				Instance->GetTextureParameterValue(P, Value);
				Info += FString::Printf(TEXT(" tex %s=%s;"), *P.Name.ToString(), Value ? *Value->GetName() : TEXT("none"));
				const FString N = P.Name.ToString();
				const bool bColour = N.Contains(TEXT("Diffuse")) || N.Contains(TEXT("BaseColor")) || N.Contains(TEXT("Albedo"));
				if (bFix && bColour && Slot.MaterialSlotName.ToString().Contains(TEXT("Body")) && Value != Albedo)
				{
					Instance->SetTextureParameterValueEditorOnly(P, Albedo);
					bChanged = true;
				}
			}
			Instance->GetAllVectorParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& P : Infos)
			{
				FLinearColor Value;
				Instance->GetVectorParameterValue(P, Value);
				Info += FString::Printf(TEXT(" vec %s=(%.2f %.2f %.2f);"), *P.Name.ToString(), Value.R, Value.G, Value.B);
				const FString N = P.Name.ToString();
				if (bFix && (N.Contains(TEXT("Diffuse")) || N.Contains(TEXT("BaseColor"))) && Value.R + Value.G + Value.B < 0.15f)
				{
					Instance->SetVectorParameterValueEditorOnly(P, FLinearColor::White);
					bChanged = true;
				}
			}
			Instance->GetAllScalarParameterInfo(Infos, Ids);
			for (const FMaterialParameterInfo& P : Infos)
			{
				float Value = 0.0f;
				Instance->GetScalarParameterValue(P, Value);
				Info += FString::Printf(TEXT(" num %s=%.2f;"), *P.Name.ToString(), Value);
				const FString N = P.Name.ToString();
				if (bFix && (N.Contains(TEXT("Metallic")) && !N.Contains(TEXT("Map"))) && Value > 0.5f)
				{
					Instance->SetScalarParameterValueEditorOnly(P, 0.0f);
					bChanged = true;
				}
				// "materialNumbers": {"<parameter>": value} sets numbers on every material of the model.
				double Wanted = 0.0;
				if (const TSharedPtr<FJsonObject>* Numbers = nullptr; Entry->TryGetObjectField(TEXT("materialNumbers"), Numbers) && (*Numbers)->TryGetNumberField(N, Wanted) && !FMath::IsNearlyEqual(Value, static_cast<float>(Wanted)))
				{
					Instance->SetScalarParameterValueEditorOnly(P, static_cast<float>(Wanted));
					bChanged = true;
				}
			}
			UE_LOG(LogAnimalArt, Display, TEXT("%s: material %s (slot %s): %s%s"), *Mesh->GetName(), *Instance->GetName(), *Slot.MaterialSlotName.ToString(), *Info, bChanged ? TEXT(" -> fixed") : TEXT(""));
			if (bChanged)
			{
				Instance->PostEditChange();
				AnimalArtSave(Instance);
			}
		}
		return true;
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
			Pipeline->MeshPipeline->CombineSkeletalMeshesBehavior = EInterchangeCombineSkeletalMeshesBehavior::BySkeleton;
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
		// `skipMeshes`: parts left out (the iguana's tongue, imported as Object_193, sits hundreds of metres from the body and hides the scene).
		if (const TArray<TSharedPtr<FJsonValue>>* Skip = nullptr; Entry->TryGetArrayField(TEXT("skipMeshes"), Skip))
		{
			Meshes.RemoveAll([Skip, &Model](const USkeletalMesh* Mesh)
			{
				for (const TSharedPtr<FJsonValue>& Word : *Skip)
				{
					if (Mesh->GetName().Contains(Word->AsString()))
					{
						UE_LOG(LogAnimalArt, Display, TEXT("%s: left out %s"), *Model, *Mesh->GetName());
						return true;
					}
				}
				return false;
			});
		}
		FString Names;
		for (const UAnimSequence* Clip : Clips)
		{
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::Printf(TEXT("%s (%.1f s)"), *Clip->GetName(), Clip->GetPlayLength());
		}
		UE_LOG(LogAnimalArt, Display, TEXT("%s: %d skeletal mesh, %d skeleton, %d clips: %s"), *Model, Meshes.Num(), Skeletons, Clips.Num(), *Names);
		if (Meshes.IsEmpty())
		{
			UE_LOG(LogAnimalArt, Error, TEXT("%s: the import made no skeletal mesh"), *Model);
			return false;
		}
		// If the parts did not combine, the body is the main mesh (<model>) and the rest are <model>_Part<N>, drawn
		// on the same skeleton following the body (ASimAnimalActor).
		Meshes.Sort([](const USkeletalMesh& A, const USkeletalMesh& B)
		{
			const bool bBodyA = A.GetName().Contains(TEXT("Body")), bBodyB = B.GetName().Contains(TEXT("Body"));
			return bBodyA != bBodyB ? bBodyA : A.GetBounds().SphereRadius > B.GetBounds().SphereRadius;
		});
		bool bOk = AnimalArtRename(Tools, Meshes[0], Folder, Model);
		for (int32 Part = 1; Part < Meshes.Num(); ++Part)
		{
			UE_LOG(LogAnimalArt, Display, TEXT("%s: part %d is %s"), *Model, Part - 1, *Meshes[Part]->GetName());
			bOk = AnimalArtRename(Tools, Meshes[Part], Folder, FString::Printf(TEXT("%s_Part%d"), *Model, Part - 1)) && bOk;
		}
		const FBox Box = Meshes[0]->GetBounds().GetBox();
		UE_LOG(LogAnimalArt, Display, TEXT("%s: mesh %.2f x %.2f x %.2f m, %d materials"), *Model, Box.GetSize().X / 100.0, Box.GetSize().Y / 100.0,
			Box.GetSize().Z / 100.0, Meshes[0]->GetMaterials().Num());
		bOk = AnimalArtFixMaterials(Meshes[0], Folder, Entry) && bOk;
		const TSharedPtr<FJsonObject>* Roles = nullptr;
		if (Entry->TryGetObjectField(TEXT("clips"), Roles))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Role : (*Roles)->Values)
			{
				const FString Wanted = Role.Value->AsString();
				UAnimSequence* Match = nullptr;
				for (UAnimSequence* Clip : Clips)
				{
					// Importers prefix clip names with the file or mesh name; the glTF animation name is at the end. The
					// shortest name that ends in the wanted one wins, so "Run" picks "Bear_Run", not "Trans_Stand_to_Run".
					if ((Clip->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || Clip->GetName().EndsWith(Wanted, ESearchCase::IgnoreCase)) &&
						(!Match || Clip->GetName().Len() < Match->GetName().Len()))
					{
						Match = Clip;
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
			// An entry with `assets` instead of `file` is already in Unreal's format (a Fab pack): nothing to import.
			if (Value->AsObject()->HasField(TEXT("file")) && Wanted(Value->AsObject()->GetStringField(TEXT("model"))) && !AnimalArtImportGltf(Tools, Value->AsObject()))
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
		// A re-import reuses the skeleton made the first time (none new among the objects): the mesh's own skeleton counts.
		bool bHasSkeleton = Skeletons > 0;
		for (UObject* Object : Task->GetObjects())
		{
			const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Object);
			bHasSkeleton = bHasSkeleton || (Mesh && Mesh->GetSkeleton());
		}
		UE_LOG(LogAnimalArt, Display, TEXT("%s: %d skeletal mesh, %d skeleton (%s), %d animations"), *Model, Meshes, Skeletons,
			bHasSkeleton ? TEXT("has one") : TEXT("NONE"), Anims);
		if (Meshes == 0 || !bHasSkeleton)
		{
			++Failed;
		}
	}
	UE_LOG(LogAnimalArt, Display, TEXT("AnimalArt %s"), Failed ? TEXT("FAILED") : TEXT("done"));
	return Failed ? 1 : 0;
}
