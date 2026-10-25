#include "PlaceholderArtCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "IAssetTools.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlaceholderArt, Log, All);

namespace
{
	const TCHAR* const ArtFolder = TEXT("/Game/Rim3D/Generated/Art");

	/** Imports one downloaded glTF and returns its static mesh, or null. */
	UStaticMesh* PlaArtImportRaw(const FString& Id)
	{
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/PlaceholderArt"), Id, Id + TEXT("_1k.gltf"));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogPlaceholderArt, Error, TEXT("%s is not downloaded: run python tools/placeholder_art.py"), *File);
			return nullptr;
		}
		const FString Destination = FString(ArtFolder) / TEXT("Raw") / Id;
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = Destination;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Tools.ImportAssetTasks({Task});
		UStaticMesh* Found = nullptr;
		for (UObject* Object : Task->GetObjects())
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
			{
				Found = Mesh;
				break;
			}
		}
		if (!Found)
		{
			UE_LOG(LogPlaceholderArt, Error, TEXT("Importing %s made no static mesh"), *File);
		}
		return Found;
	}

	bool PlaArtSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	/** Our own lit material with one colour texture, marked for instanced meshes, two sided; masked reads the texture's alpha as leaves. */
	UMaterial* PlaArtMaster(bool bMasked)
	{
		static UMaterial* Masters[2] = {nullptr, nullptr};
		UMaterial*& Master = Masters[bMasked ? 1 : 0];
		if (Master)
		{
			return Master;
		}
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Master = Cast<UMaterial>(Tools.CreateAsset(bMasked ? TEXT("M_ArtLitMasked") : TEXT("M_ArtLit"), *(FString(ArtFolder) / TEXT("Materials")), UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Master)
		{
			return nullptr;
		}
		Master->TwoSided = true;
		Master->BlendMode = bMasked ? BLEND_Masked : BLEND_Opaque;
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Master, MATUSAGE_InstancedStaticMeshes, bRecompile);
		auto* Sample = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -500, 0));
		Sample->ParameterName = TEXT("BaseColorTexture");
		Sample->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Sample, TEXT("RGB"), MP_BaseColor);
		if (bMasked)
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Sample, TEXT("A"), MP_OpacityMask);
		}
		auto* Rough = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Master, UMaterialExpressionConstant::StaticClass(), -500, 300));
		Rough->R = 0.85f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::RecompileMaterial(Master);
		PlaArtSave(Master);
		return Master;
	}

	UMaterialInterface* PlaArtMakeMaterial(const FString& Name, UTexture* Color, bool bMasked)
	{
		UMaterial* Master = Color ? PlaArtMaster(bMasked) : nullptr;
		if (!Master)
		{
			return nullptr;
		}
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Master;
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(Name, *(FString(ArtFolder) / TEXT("Materials")), UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			return nullptr;
		}
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("BaseColorTexture"), Color);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		PlaArtSave(Instance);
		return Instance;
	}

	bool PlaArtMakeDefMesh(UStaticMesh* Raw, const TSharedPtr<FJsonObject>& Entry)
	{
		const FString Def = Entry->GetStringField(TEXT("def"));
		const TArray<TSharedPtr<FJsonValue>>& SizeValues = Entry->GetArrayField(TEXT("size"));
		const FVector Size(SizeValues[0]->AsNumber() * 100.0, SizeValues[1]->AsNumber() * 100.0, SizeValues[2]->AsNumber() * 100.0);
		const FString Long = Entry->HasField(TEXT("long")) ? Entry->GetStringField(TEXT("long")) : FString();
		bool bStretch = false;
		Entry->TryGetBoolField(TEXT("stretch"), bStretch);
		bool bLieFlat = false;
		Entry->TryGetBoolField(TEXT("lieFlat"), bLieFlat);
		double Yaw = 0.0;
		Entry->TryGetNumberField(TEXT("yaw"), Yaw);
		int32 TargetTris = 0;
		Entry->TryGetNumberField(TEXT("tris"), TargetTris);

		const FString PackageName = FString(ArtFolder) / (TEXT("SM_") + Def);
		UPackage* Package = CreatePackage(*PackageName);
		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Raw, Package, *(TEXT("SM_") + Def));
		Mesh->SetFlags(RF_Public | RF_Standalone);
		Mesh->NaniteSettings.bEnabled = false;

		FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!Description)
		{
			UE_LOG(LogPlaceholderArt, Error, TEXT("%s: the mesh has no mesh description"), *Def);
			return false;
		}
		Mesh->ModifyMeshDescription(0);
		FStaticMeshAttributes Attributes(*Description);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();

		const auto Turn = [&](const FRotator& Rotation)
		{
			const FQuat Quat = Rotation.Quaternion();
			for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
			{
				Positions[Vertex] = FVector3f(Quat.RotateVector(FVector(Positions[Vertex])));
			}
			for (const FVertexInstanceID Instance : Description->VertexInstances().GetElementIDs())
			{
				Normals[Instance] = FVector3f(Quat.RotateVector(FVector(Normals[Instance])));
				Tangents[Instance] = FVector3f(Quat.RotateVector(FVector(Tangents[Instance])));
			}
		};
		const auto BoundsNow = [&]()
		{
			FBox Box(ForceInit);
			for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
			{
				Box += FVector(Positions[Vertex]);
			}
			return Box;
		};
		if (bLieFlat)
		{
			// The longest axis is up in the file; roll it down onto x or y.
			Turn(FRotator(0.0, 0.0, 90.0));
		}
		if (!Long.IsEmpty())
		{
			const FVector Now = BoundsNow().GetSize();
			if ((Long == TEXT("x")) != (Now.X >= Now.Y))
			{
				Turn(FRotator(0.0, 90.0, 0.0));
			}
		}
		if (Yaw != 0.0)
		{
			Turn(FRotator(0.0, Yaw, 0.0));
		}

		const FBox Box = BoundsNow();
		const FVector Extent = Box.GetSize();
		FVector Scale(1.0);
		if (bStretch)
		{
			Scale = FVector(Size.X / FMath::Max(Extent.X, 0.01), Size.Y / FMath::Max(Extent.Y, 0.01), Size.Z / FMath::Max(Extent.Z, 0.01));
		}
		else
		{
			Scale = FVector(FMath::Min3(Size.X / FMath::Max(Extent.X, 0.01), Size.Y / FMath::Max(Extent.Y, 0.01), Size.Z / FMath::Max(Extent.Z, 0.01)));
		}
		const FVector Middle((Box.Min.X + Box.Max.X) * 0.5, (Box.Min.Y + Box.Max.Y) * 0.5, Box.Min.Z);
		for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
		{
			Positions[Vertex] = FVector3f((FVector(Positions[Vertex]) - Middle) * Scale);
		}

		const int32 Triangles = Description->Triangles().Num();
		FStaticMeshSourceModel& Source = Mesh->GetSourceModel(0);
		Source.ReductionSettings.PercentTriangles = TargetTris > 0 ? FMath::Clamp(static_cast<float>(TargetTris) / static_cast<float>(FMath::Max(Triangles, 1)), 0.001f, 1.0f) : 1.0f;
		Source.ReductionSettings.MaxDeviation = 0.0f;
		Source.BuildSettings.bRecomputeNormals = false;
		Source.BuildSettings.bRecomputeTangents = false;
		Mesh->CommitMeshDescription(0);
		Mesh->Build(true);
		Mesh->PostEditChange();

		// The file's materials are instances of the engine's glTF material, which the game's instanced meshes can't use
		// (the engine draws the default dark one); make our own from the same colour texture.
		for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
		{
			const UMaterialInstance* Instance = Cast<UMaterialInstance>(Mesh->GetStaticMaterials()[Slot].MaterialInterface);
			UTexture* Color = nullptr;
			if (Instance)
			{
				for (const FTextureParameterValue& Texture : Instance->TextureParameterValues)
				{
					if (Texture.ParameterInfo.Name == TEXT("BaseColorTexture"))
					{
						Color = Texture.ParameterValue;
					}
				}
			}
			const bool bMasked = Instance && Instance->Parent && Instance->Parent->GetName().Contains(TEXT("Mask"));
			UMaterialInterface* Ours = PlaArtMakeMaterial(FString::Printf(TEXT("MI_%s_%d"), *Def, Slot), Color, bMasked);
			if (Ours)
			{
				Mesh->SetMaterial(Slot, Ours);
			}
			else
			{
				UE_LOG(LogPlaceholderArt, Warning, TEXT("%s: no colour texture in material slot %d, left as imported"), *Def, Slot);
			}
		}

		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Mesh, *File, Args);
		UE_LOG(LogPlaceholderArt, Display, TEXT("%s %s: %d triangles in the file, target %d, stands in %.2f x %.2f x %.2f m"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *Def, Triangles, TargetTris,
			Extent.X * Scale.X / 100.0, Extent.Y * Scale.Y / 100.0, Extent.Z * Scale.Z / 100.0);
		return bSaved;
	}
}

UPlaceholderArtCommandlet::UPlaceholderArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UPlaceholderArtCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/placeholder_art.json")));
	if (!FFileHelper::LoadFileToString(Text, *ListFile))
	{
		UE_LOG(LogPlaceholderArt, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogPlaceholderArt, Error, TEXT("%s is not valid JSON"), *ListFile);
		return 1;
	}
	// Start clean: saving over the last run's files fails while they exist.
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(ArtFolder), false, true);
	int32 Failed = 0;
	TMap<FString, UStaticMesh*> Imported;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("meshes")))
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		const FString Id = Entry->GetStringField(TEXT("id"));
		UStaticMesh* Raw = Imported.FindRef(Id);
		if (!Raw)
		{
			Raw = PlaArtImportRaw(Id);
			Imported.Add(Id, Raw);
		}
		if (!Raw || !PlaArtMakeDefMesh(Raw, Entry))
		{
			++Failed;
		}
	}
	UE_LOG(LogPlaceholderArt, Display, TEXT("%s"), Failed == 0 ? TEXT("All meshes made.") : TEXT("Some meshes failed."));
	return Failed == 0 ? 0 : 1;
}
