#include "GunArtCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogGunArt, Log, All);

namespace
{
	const TCHAR* const GunArtRoot = TEXT("/Game/Rim3D/Generated/Guns");

	bool GunArtSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogGunArt, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	void GunArtConnect(UMaterialExpression* From, const TCHAR* FromPin, UMaterialExpression* To, const TCHAR* ToPin)
	{
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromPin, To, ToPin))
		{
			UE_LOG(LogGunArt, Error, TEXT("Could not connect %s to %s pin '%s'"), *From->GetName(), *To->GetName(), ToPin);
		}
	}

	void GunArtConnectProperty(UMaterialExpression* From, const TCHAR* FromPin, EMaterialProperty Property)
	{
		if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromPin, Property))
		{
			UE_LOG(LogGunArt, Error, TEXT("Could not connect %s to a material output"), *From->GetName());
		}
	}

	template <typename T>
	T* GunArtNode(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	UTexture2D* GunArtDefaultTexture(const TCHAR* Name)
	{
		return LoadObject<UTexture2D>(nullptr, *FString::Printf(TEXT("/Engine/EngineMaterials/%s.%s"), Name, Name));
	}

	/** M_GunPBR: base colour x tint, normal, roughness and metallic from their maps or from an ORM map (UseORM 1). */
	UMaterial* GunArtMaster(IAssetTools& Tools)
	{
		const FString Path = FString(GunArtRoot) / TEXT("M_GunPBR");
		if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, *(Path + TEXT(".M_GunPBR"))))
		{
			return Existing;
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_GunPBR"), GunArtRoot, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		const auto Sample = [Material](const TCHAR* Name, int32 Y, UTexture2D* Default, EMaterialSamplerType Type)
		{
			auto* Node = GunArtNode<UMaterialExpressionTextureSampleParameter2D>(Material, -900, Y);
			Node->ParameterName = Name;
			Node->Texture = Default;
			Node->SamplerType = Type;
			return Node;
		};
		auto* Base = Sample(TEXT("BaseColorMap"), -400, GunArtDefaultTexture(TEXT("DefaultDiffuse")), SAMPLERTYPE_Color);
		auto* Normal = Sample(TEXT("NormalMap"), -100, GunArtDefaultTexture(TEXT("DefaultNormal")), SAMPLERTYPE_Normal);
		auto* Rough = Sample(TEXT("RoughnessMap"), 200, GunArtDefaultTexture(TEXT("WhiteSquareTexture")), SAMPLERTYPE_LinearColor);
		auto* Metal = Sample(TEXT("MetallicMap"), 500, GunArtDefaultTexture(TEXT("BlackSquareTexture")), SAMPLERTYPE_LinearColor);
		auto* Orm = Sample(TEXT("ORMMap"), 800, GunArtDefaultTexture(TEXT("WhiteSquareTexture")), SAMPLERTYPE_LinearColor);
		auto* UseOrm = GunArtNode<UMaterialExpressionScalarParameter>(Material, -600, 1100);
		UseOrm->ParameterName = TEXT("UseORM");
		UseOrm->DefaultValue = 0.0f;
		auto* RoughScale = GunArtNode<UMaterialExpressionScalarParameter>(Material, -300, 1100);
		RoughScale->ParameterName = TEXT("RoughnessScale");
		RoughScale->DefaultValue = 1.0f;

		const auto Channel = [Material](UMaterialExpression* From, bool R, bool G, bool B, int32 Y)
		{
			auto* Mask = GunArtNode<UMaterialExpressionComponentMask>(Material, -600, Y);
			Mask->R = R;
			Mask->G = G;
			Mask->B = B;
			Mask->A = false;
			GunArtConnect(From, TEXT("RGB"), Mask, TEXT(""));
			return Mask;
		};
		auto* RoughR = Channel(Rough, true, false, false, 200);
		auto* MetalR = Channel(Metal, true, false, false, 500);
		auto* OrmG = Channel(Orm, false, true, false, 800);
		auto* OrmB = Channel(Orm, false, false, true, 900);
		auto* OrmR = Channel(Orm, true, false, false, 1000);

		auto* RoughPick = GunArtNode<UMaterialExpressionLinearInterpolate>(Material, -400, 250);
		GunArtConnect(RoughR, TEXT(""), RoughPick, TEXT("A"));
		GunArtConnect(OrmG, TEXT(""), RoughPick, TEXT("B"));
		GunArtConnect(UseOrm, TEXT(""), RoughPick, TEXT("Alpha"));
		auto* RoughOut = GunArtNode<UMaterialExpressionMultiply>(Material, -200, 250);
		GunArtConnect(RoughPick, TEXT(""), RoughOut, TEXT("A"));
		GunArtConnect(RoughScale, TEXT(""), RoughOut, TEXT("B"));
		auto* MetalPick = GunArtNode<UMaterialExpressionLinearInterpolate>(Material, -400, 550);
		GunArtConnect(MetalR, TEXT(""), MetalPick, TEXT("A"));
		GunArtConnect(OrmB, TEXT(""), MetalPick, TEXT("B"));
		GunArtConnect(UseOrm, TEXT(""), MetalPick, TEXT("Alpha"));
		auto* One = GunArtNode<UMaterialExpressionScalarParameter>(Material, -600, 1200);
		One->ParameterName = TEXT("NoAO");
		One->DefaultValue = 1.0f;
		auto* AoPick = GunArtNode<UMaterialExpressionLinearInterpolate>(Material, -400, 950);
		GunArtConnect(One, TEXT(""), AoPick, TEXT("A"));
		GunArtConnect(OrmR, TEXT(""), AoPick, TEXT("B"));
		GunArtConnect(UseOrm, TEXT(""), AoPick, TEXT("Alpha"));

		GunArtConnectProperty(Base, TEXT("RGB"), MP_BaseColor);
		GunArtConnectProperty(Normal, TEXT("RGB"), MP_Normal);
		GunArtConnectProperty(RoughOut, TEXT(""), MP_Roughness);
		GunArtConnectProperty(MetalPick, TEXT(""), MP_Metallic);
		GunArtConnectProperty(AoPick, TEXT(""), MP_AmbientOcclusion);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		GunArtSave(Material);
		return Material;
	}

	FString GunArtResolve(const FString& File)
	{
		if (FPaths::IsRelative(File))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), File));
		}
		return File;
	}

	UTexture2D* GunArtTexture(IAssetTools& Tools, const FString& File, const FString& Dest, bool bNormal, bool bLinear)
	{
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogGunArt, Error, TEXT("No texture %s"), *File);
			return nullptr;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = Dest;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		Tools.ImportAssetTasks({Task});
		for (UObject* Object : Task->GetObjects())
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Object))
			{
				if (bNormal)
				{
					Texture->CompressionSettings = TC_Normalmap;
					Texture->SRGB = false;
					Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
				}
				else if (bLinear)
				{
					Texture->CompressionSettings = TC_Masks;
					Texture->SRGB = false;
				}
				Texture->PostEditChange();
				GunArtSave(Texture);
				return Texture;
			}
		}
		UE_LOG(LogGunArt, Error, TEXT("Texture %s did not import"), *File);
		return nullptr;
	}
}

UGunArtCommandlet::UGunArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UGunArtCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/guns.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogGunArt, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	FString Only;
	FParse::Value(*Params, TEXT("Guns="), Only);
	TArray<FString> OnlyNames;
	Only.ParseIntoArray(OnlyNames, TEXT(","));

	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMaterial* Master = GunArtMaster(Tools);
	int32 Failed = Master ? 0 : 1;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("guns")))
	{
		const TSharedPtr<FJsonObject> Gun = Value->AsObject();
		const TSharedPtr<FJsonObject>* ArtPtr = nullptr;
		if (!Gun.IsValid() || !Gun->TryGetObjectField(TEXT("art"), ArtPtr))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Art = *ArtPtr;
		const FString Name = Art->GetStringField(TEXT("name"));
		if (!OnlyNames.IsEmpty() && !OnlyNames.Contains(Name))
		{
			continue;
		}
		const FString File = GunArtResolve(Art->GetStringField(TEXT("fbx")));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogGunArt, Warning, TEXT("%s: %s is not there (download it first, docs/design/gun-range.md); skipped"), *Name, *File);
			continue;
		}
		const FString Dest = FString(GunArtRoot) / Name;

		// The model: every object its own static mesh, no collision, its materials made but replaced below.
		UFbxImportUI* Options = NewObject<UFbxImportUI>();
		Options->bImportMesh = true;
		Options->bImportAsSkeletal = false;
		Options->bImportAnimations = false;
		Options->bImportMaterials = true;
		Options->bImportTextures = true;
		Options->bAutomatedImportShouldDetectType = false;
		Options->MeshTypeToImport = FBXIT_StaticMesh;
		Options->StaticMeshImportData->bCombineMeshes = Art->HasField(TEXT("combine")) ? Art->GetBoolField(TEXT("combine")) : false;
		Options->StaticMeshImportData->bAutoGenerateCollision = false;
		Options->StaticMeshImportData->bGenerateLightmapUVs = false;
		Options->StaticMeshImportData->ImportUniformScale = Art->HasField(TEXT("scale")) ? static_cast<float>(Art->GetNumberField(TEXT("scale"))) : 1.0f;
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = Dest;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->Options = Options;
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
			UE_LOG(LogGunArt, Error, TEXT("%s: no static mesh came out of %s"), *Name, *File);
			++Failed;
			continue;
		}

		// Its material: M_GunPBR with the listed maps (none listed: the FBX's own materials stay).
		const TSharedPtr<FJsonObject>* TexPtr = nullptr;
		if (Master && Art->TryGetObjectField(TEXT("textures"), TexPtr))
		{
			const TSharedPtr<FJsonObject> Tex = *TexPtr;
			const FString MiName = TEXT("MI_") + Name;
			UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(Dest / MiName + TEXT(".") + MiName));
			if (!Instance)
			{
				UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Master;
				Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(MiName, Dest, UMaterialInstanceConstant::StaticClass(), Factory));
			}
			if (Instance)
			{
				Instance->SetParentEditorOnly(Master);
				const auto Map = [&](const TCHAR* Key, const TCHAR* Param, bool bNormal, bool bLinear)
				{
					FString Path;
					if (Tex->TryGetStringField(Key, Path))
					{
						if (UTexture2D* Texture = GunArtTexture(Tools, GunArtResolve(Path), Dest, bNormal, bLinear))
						{
							Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(Param), Texture);
							return true;
						}
					}
					return false;
				};
				Map(TEXT("baseColor"), TEXT("BaseColorMap"), false, false);
				Map(TEXT("normal"), TEXT("NormalMap"), true, false);
				Map(TEXT("roughness"), TEXT("RoughnessMap"), false, true);
				Map(TEXT("metallic"), TEXT("MetallicMap"), false, true);
				if (Map(TEXT("orm"), TEXT("ORMMap"), false, true))
				{
					Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("UseORM")), 1.0f);
				}
				Instance->PostEditChange();
				GunArtSave(Instance);
				for (UStaticMesh* Mesh : Meshes)
				{
					for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
					{
						Mesh->SetMaterial(Slot, Instance);
					}
				}
			}
		}
		for (UStaticMesh* Mesh : Meshes)
		{
			const FBox Box = Mesh->GetBoundingBox();
			UE_LOG(LogGunArt, Display, TEXT("%s: mesh %s, %d triangles, bounds %s .. %s cm"), *Name, *Mesh->GetPathName(),
				Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() ? Mesh->GetRenderData()->LODResources[0].GetNumTriangles() : -1,
				*Box.Min.ToCompactString(), *Box.Max.ToCompactString());
			GunArtSave(Mesh);
		}
	}
	UE_LOG(LogGunArt, Display, TEXT("GunArt %s"), Failed ? TEXT("FAILED") : TEXT("done"));
	return Failed ? 1 : 0;
}
