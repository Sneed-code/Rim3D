#include "GunArtCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "Math/RotationMatrix.h"
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
	/** Where the "vehicles" of a list go (assets/shuttle.json, docs/design/shuttle-draw.md). */
	const TCHAR* const GunArtVehicleRoot = TEXT("/Game/Rim3D/Generated/Vehicles");
	// Buildings and items with a Codex model (assets/codex_art.json): one combined mesh each, SM_<name>, named by the def's visual.mesh.
	const TCHAR* const GunArtBuildingRoot = TEXT("/Game/Rim3D/Generated/Buildings");

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

	/** An engine texture by its full path ("/Engine/EngineResources/WhiteSquareTexture"). */
	UTexture2D* GunArtDefaultTexture(const TCHAR* Path)
	{
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *FString::Printf(TEXT("%s.%s"), Path, *FPaths::GetBaseFilename(Path)));
		if (!Texture)
		{
			UE_LOG(LogGunArt, Error, TEXT("No engine texture %s"), Path);
		}
		return Texture;
	}

	/** A 4x4 grey mask texture (linear, TC_Masks) for the master's mask defaults: the engine's squares are sRGB colour
	 *  textures, which a Masks sampler refuses. */
	UTexture2D* GunArtMaskTexture(const TCHAR* Name, uint8 Value)
	{
		const FString PackageName = FString(GunArtRoot) / Name;
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Texture)
		{
			UPackage* Package = CreatePackage(*PackageName);
			Texture = NewObject<UTexture2D>(Package, Name, RF_Public | RF_Standalone);
		}
		constexpr int32 Size = 4;
		TArray<uint8> Pixels;
		Pixels.Init(Value, Size * Size * 4);
		for (int32 i = 3; i < Pixels.Num(); i += 4)
		{
			Pixels[i] = 255;
		}
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Pixels.GetData());
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_Masks;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->PostEditChange();
		GunArtSave(Texture);
		return Texture;
	}

	/** M_GunPBR: base colour x tint, normal, roughness and metallic from their maps or from an ORM map (UseORM 1). */
	UMaterial* GunArtMaster(IAssetTools& Tools)
	{
		const FString Path = FString(GunArtRoot) / TEXT("M_GunPBR");
		// Built again on every run (an older one is emptied), so a fix here reaches the saved material.
		UMaterial* Material = LoadObject<UMaterial>(nullptr, *(Path + TEXT(".M_GunPBR")), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Material)
		{
			UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
		}
		else
		{
			Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_GunPBR"), GunArtRoot, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		}
		if (!Material)
		{
			return nullptr;
		}
		// Buildings and items drawn with these maps go through instanced meshes (BuildingSubsystem); set it here so a
		// packaged game has the shader permutation (the editor would add it on first use).
		Material->bUsedWithInstancedStaticMeshes = true;
		const auto Sample = [Material](const TCHAR* Name, int32 Y, UTexture2D* Default, EMaterialSamplerType Type)
		{
			auto* Node = GunArtNode<UMaterialExpressionTextureSampleParameter2D>(Material, -900, Y);
			Node->ParameterName = Name;
			Node->Texture = Default;
			Node->SamplerType = Type;
			return Node;
		};
		// The grey maps are imported as masks (TC_Masks, linear), so their samplers are Masks, with mask defaults.
		auto* Base = Sample(TEXT("BaseColorMap"), -400, GunArtDefaultTexture(TEXT("/Engine/EngineResources/WhiteSquareTexture")), SAMPLERTYPE_Color);
		auto* Normal = Sample(TEXT("NormalMap"), -100, GunArtDefaultTexture(TEXT("/Engine/EngineMaterials/DefaultNormal")), SAMPLERTYPE_Normal);
		auto* Rough = Sample(TEXT("RoughnessMap"), 200, GunArtMaskTexture(TEXT("T_GunMaskWhite"), 255), SAMPLERTYPE_Masks);
		auto* Metal = Sample(TEXT("MetallicMap"), 500, GunArtMaskTexture(TEXT("T_GunMaskBlack"), 0), SAMPLERTYPE_Masks);
		auto* Orm = Sample(TEXT("ORMMap"), 800, GunArtMaskTexture(TEXT("T_GunMaskWhite"), 255), SAMPLERTYPE_Masks);
		// Glow (lit panels, screens, nozzle rings): the emissive map times EmissiveStrength, 0 unless a model has one.
		auto* Glow = Sample(TEXT("EmissiveMap"), 1400, GunArtDefaultTexture(TEXT("/Engine/EngineResources/Black")), SAMPLERTYPE_Color);
		auto* GlowStrength = GunArtNode<UMaterialExpressionScalarParameter>(Material, -600, 1500);
		GlowStrength->ParameterName = TEXT("EmissiveStrength");
		GlowStrength->DefaultValue = 0.0f;
		auto* GlowOut = GunArtNode<UMaterialExpressionMultiply>(Material, -200, 1450);
		GunArtConnect(Glow, TEXT("RGB"), GlowOut, TEXT("A"));
		GunArtConnect(GlowStrength, TEXT(""), GlowOut, TEXT("B"));
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
		GunArtConnectProperty(GlowOut, TEXT(""), MP_EmissiveColor);
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
	// -List=assets/shuttle.json reads another list (relative to the repo); its "guns" and "vehicles" are both imported.
	FString ListName = TEXT("assets/guns.json");
	FParse::Value(*Params, TEXT("List="), ListName);
	const FString ListFile = GunArtResolve(ListName);
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogGunArt, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	FString Only;
	FParse::Value(*Params, TEXT("Guns="), Only, /*bShouldStopOnSeparator=*/ false);
	TArray<FString> OnlyNames;
	Only.ParseIntoArray(OnlyNames, TEXT(","));

	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMaterial* Master = GunArtMaster(Tools);
	int32 Failed = Master ? 0 : 1;
	TArray<TPair<TSharedPtr<FJsonValue>, const TCHAR*>> Entries;
	for (const TPair<const TCHAR*, const TCHAR*> Section : {TPair<const TCHAR*, const TCHAR*>(TEXT("guns"), GunArtRoot), TPair<const TCHAR*, const TCHAR*>(TEXT("melee"), GunArtRoot), TPair<const TCHAR*, const TCHAR*>(TEXT("vehicles"), GunArtVehicleRoot), TPair<const TCHAR*, const TCHAR*>(TEXT("buildings"), GunArtBuildingRoot)})
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Root->TryGetArrayField(Section.Key, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				Entries.Emplace(Value, Section.Value);
			}
		}
	}
	for (const TPair<TSharedPtr<FJsonValue>, const TCHAR*>& ListEntry : Entries)
	{
		const TSharedPtr<FJsonValue>& Value = ListEntry.Key;
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
		const FString Dest = FString(ListEntry.Value) / Name;
		if (FString(ListEntry.Value) == GunArtBuildingRoot)
		{
			// Start clean so the combined mesh can be renamed to SM_<name> again (it imports as <file name>).
			IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(Dest), false, true);
		}

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
		// "importRotation" [pitch, yaw, roll] and "importOffset" [x, y, z] cm (applied after the rotation) put a model into the
		// frame the game draws it in. "autoOrient": the longest side becomes +X with the far end (the tip, away from the grip
		// at the origin) forward and the second longest +Z (a blade's edge), checked after a first import.
		const auto ArtVec = [&Art](const TCHAR* Key, FVector& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
			if (Art->TryGetArrayField(Key, V) && V->Num() == 3)
			{
				Out = FVector((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber());
				return true;
			}
			return false;
		};
		FVector RotationIn = FVector::ZeroVector;
		FVector OffsetIn = FVector::ZeroVector;
		ArtVec(TEXT("importRotation"), RotationIn);
		ArtVec(TEXT("importOffset"), OffsetIn);
		Options->StaticMeshImportData->ImportRotation = FRotator(RotationIn.X, RotationIn.Y, RotationIn.Z);
		Options->StaticMeshImportData->ImportTranslation = OffsetIn;
		TArray<UStaticMesh*> Meshes;
		const auto RunImport = [&]()
		{
			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = File;
			Task->DestinationPath = Dest;
			Task->bAutomated = true;
			Task->bReplaceExisting = true;
			Task->bSave = true;
			Task->Options = Options;
			Tools.ImportAssetTasks({Task});
			Meshes.Reset();
			for (UObject* Object : Task->GetObjects())
			{
				if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
				{
					Meshes.Add(Mesh);
				}
			}
		};
		RunImport();
		if (Meshes.Num() == 1 && Art->HasField(TEXT("autoOrient")) && Art->GetBoolField(TEXT("autoOrient")))
		{
			// Measured with no offset: undo it from the bounds (the rotation is redone from scratch below).
			const FBox Box = Meshes[0]->GetBoundingBox().ShiftBy(-OffsetIn);
			const FVector Size = Box.GetSize();
			int32 Long = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				Long = Size[Axis] > Size[Long] ? Axis : Long;
			}
			int32 Second = Long == 0 ? 1 : 0;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				Second = Axis != Long && Size[Axis] > Size[Second] ? Axis : Second;
			}
			const auto AxisDir = [&Box](int32 Axis)
			{
				FVector Dir = FVector::ZeroVector;
				Dir[Axis] = FMath::Abs(Box.Max[Axis]) >= FMath::Abs(Box.Min[Axis]) ? 1.0 : -1.0;
				return Dir;
			};
			const FVector SrcX = AxisDir(Long);
			const FVector SrcZ = AxisDir(Second);
			// Rotation taking the model's tip direction to +X and its edge direction to +Z, on top of the one given.
			const FQuat Fix = FRotationMatrix::MakeFromXZ(SrcX, SrcZ).ToQuat().Inverse();
			if (!Fix.Equals(FQuat::Identity, 1e-3))
			{
				const FQuat Total = Fix * Options->StaticMeshImportData->ImportRotation.Quaternion();
				Options->StaticMeshImportData->ImportRotation = Total.Rotator();
				UE_LOG(LogGunArt, Display, TEXT("%s: turned to the game's frame (long side was %s, edge %s): import rotation %s"), *Name,
					*SrcX.ToString(), *SrcZ.ToString(), *Total.Rotator().ToString());
				RunImport();
			}
		}
		for (const UStaticMesh* Mesh : Meshes)
		{
			const FBox Box = Mesh->GetBoundingBox();
			UE_LOG(LogGunArt, Display, TEXT("%s: %s bounds %s .. %s cm"), *Name, *Mesh->GetName(), *Box.Min.ToString(), *Box.Max.ToString());
		}
		if (Meshes.IsEmpty())
		{
			UE_LOG(LogGunArt, Error, TEXT("%s: no static mesh came out of %s"), *Name, *File);
			++Failed;
			continue;
		}

		// Its material: M_GunPBR with the listed maps (none listed: the FBX's own materials stay). "meshTextures" gives
		// meshes whose name has a piece in it their own maps (a separate magazine with its own texture set).
		const auto MakeInstance = [&](const TSharedPtr<FJsonObject>& Tex, const FString& MiName) -> UMaterialInstanceConstant*
		{
			UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(Dest / MiName + TEXT(".") + MiName), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Instance)
			{
				UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
				Factory->InitialParent = Master;
				Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(MiName, Dest, UMaterialInstanceConstant::StaticClass(), Factory));
			}
			if (!Instance)
			{
				return nullptr;
			}
			Instance->SetParentEditorOnly(Master);
			const bool bFlipGreen = Tex->HasField(TEXT("normalOpenGL")) && Tex->GetBoolField(TEXT("normalOpenGL"));
			const auto Map = [&](const TCHAR* Key, const TCHAR* Param, bool bNormal, bool bLinear)
			{
				FString Path;
				if (Tex->TryGetStringField(Key, Path))
				{
					if (UTexture2D* Texture = GunArtTexture(Tools, GunArtResolve(Path), Dest, bNormal, bLinear))
					{
						if (bNormal && bFlipGreen && !Texture->bFlipGreenChannel)
						{
							Texture->bFlipGreenChannel = true;
							Texture->PostEditChange();
							GunArtSave(Texture);
						}
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
			if (Map(TEXT("emissive"), TEXT("EmissiveMap"), false, false))
			{
				const float Strength = Tex->HasField(TEXT("emissiveStrength")) ? static_cast<float>(Tex->GetNumberField(TEXT("emissiveStrength"))) : 1.0f;
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissiveStrength")), Strength);
			}
			if (Map(TEXT("orm"), TEXT("ORMMap"), false, true))
			{
				Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("UseORM")), 1.0f);
			}
			Instance->PostEditChange();
			GunArtSave(Instance);
			return Instance;
		};
		const auto ApplyInstance = [](UStaticMesh* Mesh, UMaterialInstanceConstant* Instance)
		{
			for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
			{
				Mesh->SetMaterial(Slot, Instance);
			}
		};
		const TSharedPtr<FJsonObject>* TexPtr = nullptr;
		if (Master && Art->TryGetObjectField(TEXT("textures"), TexPtr))
		{
			if (UMaterialInstanceConstant* Instance = MakeInstance(*TexPtr, TEXT("MI_") + Name))
			{
				for (UStaticMesh* Mesh : Meshes)
				{
					ApplyInstance(Mesh, Instance);
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* MeshTex = nullptr;
		if (Master && Art->TryGetArrayField(TEXT("meshTextures"), MeshTex))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *MeshTex)
			{
				const TSharedPtr<FJsonObject> E = Entry->AsObject();
				const TSharedPtr<FJsonObject>* SetPtr = nullptr;
				FString Match;
				if (!E.IsValid() || !E->TryGetStringField(TEXT("match"), Match) || !E->TryGetObjectField(TEXT("textures"), SetPtr))
				{
					continue;
				}
				if (UMaterialInstanceConstant* Instance = MakeInstance(*SetPtr, TEXT("MI_") + Name + TEXT("_") + Match))
				{
					for (UStaticMesh* Mesh : Meshes)
					{
						if (Mesh->GetName().Contains(Match))
						{
							ApplyInstance(Mesh, Instance);
						}
					}
				}
			}
		}
		// "slotTextureDir": each material slot named S takes <dir>/T_S_BaseColor.png and, when there, T_S_Normal, T_S_ORM and
		// T_S_Emissive (tools/codex_prep.py writes them so, one set per material of a Codex model; "normalOpenGL" on the art).
		FString SlotDir;
		if (Master && Art->TryGetStringField(TEXT("slotTextureDir"), SlotDir))
		{
			SlotDir = GunArtResolve(SlotDir);
			for (UStaticMesh* Mesh : Meshes)
			{
				for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
				{
					const FString SlotName = Mesh->GetStaticMaterials()[Slot].ImportedMaterialSlotName.ToString();
					const FString Stem = SlotDir / (TEXT("T_") + SlotName + TEXT("_"));
					if (!FPaths::FileExists(Stem + TEXT("BaseColor.png")))
					{
						UE_LOG(LogGunArt, Warning, TEXT("%s: no %sBaseColor.png for slot %s; left as imported"), *Name, *Stem, *SlotName);
						continue;
					}
					TSharedPtr<FJsonObject> Set = MakeShared<FJsonObject>();
					Set->SetStringField(TEXT("baseColor"), Stem + TEXT("BaseColor.png"));
					for (const TCHAR* Key : {TEXT("Normal"), TEXT("ORM"), TEXT("Emissive")})
					{
						if (FPaths::FileExists(Stem + Key + TEXT(".png")))
						{
							Set->SetStringField(FString(Key) == TEXT("ORM") ? TEXT("orm") : FString(Key).ToLower(), Stem + Key + TEXT(".png"));
						}
					}
					bool bOpenGL = false;
					Art->TryGetBoolField(TEXT("normalOpenGL"), bOpenGL);
					Set->SetBoolField(TEXT("normalOpenGL"), bOpenGL);
					double Glow = 1.0;
					if (Art->TryGetNumberField(TEXT("emissiveStrength"), Glow))
					{
						Set->SetNumberField(TEXT("emissiveStrength"), Glow);
					}
					if (UMaterialInstanceConstant* Instance = MakeInstance(Set, TEXT("MI_") + SlotName))
					{
						Mesh->SetMaterial(Slot, Instance);
					}
				}
			}
		}
		// A building's or item's single mesh goes to <root>/<name>/SM_<name>, the path its def's visual.mesh names.
		if (FString(ListEntry.Value) == GunArtBuildingRoot)
		{
			if (Meshes.Num() != 1)
			{
				UE_LOG(LogGunArt, Error, TEXT("%s: a building needs one combined mesh (\"combine\": true), got %d"), *Name, Meshes.Num());
				++Failed;
				continue;
			}
			const FString Want = TEXT("SM_") + Name;
			if (Meshes[0]->GetName() != Want)
			{
				TArray<FAssetRenameData> Rename;
				Rename.Emplace(Meshes[0], Dest, Want);
				Tools.RenameAssets(Rename);
			}
			Meshes[0]->NaniteSettings.bEnabled = false;
			Meshes[0]->PostEditChange();
		}
		for (UStaticMesh* Mesh : Meshes)
		{
			const FBox Box = Mesh->GetBoundingBox();
			UE_LOG(LogGunArt, Display, TEXT("%s: mesh %s, %d triangles, bounds %s .. %s cm"), *Name, *Mesh->GetPathName(),
				Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() ? Mesh->GetRenderData()->LODResources[0].GetNumTriangles() : -1,
				*Box.Min.ToCompactString(), *Box.Max.ToCompactString());
			for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
			{
				UE_LOG(LogGunArt, Display, TEXT("%s:   slot %s"), *Name, *Slot.ImportedMaterialSlotName.ToString());
			}
			GunArtSave(Mesh);
		}
	}
	UE_LOG(LogGunArt, Display, TEXT("GunArt %s"), Failed ? TEXT("FAILED") : TEXT("done"));
	return Failed ? 1 : 0;
}
