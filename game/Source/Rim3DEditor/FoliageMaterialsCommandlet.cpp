#include "FoliageMaterialsCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoliageMaterials, Log, All);

namespace
{
	const TCHAR* const FolFolder = TEXT("/Game/Rim3D/Generated/Foliage");
	const TCHAR* const FolPlants = TEXT("/Game/Megascans/3D_Plants");

	// A light sway: the higher a point is above the plant's foot the further it moves; the phase follows the world
	// position so neighbours do not move together. P is the pixel's world position, O the instance's, T the time.
	const TCHAR* const FolWindCode = TEXT(
		"float h = max(P.z - O.z, 0.0) / 100.0;\n"
		"float ph = dot(O.xy, float2(0.0131, 0.0173));\n"
		"float s = sin(T * Speed + ph) + 0.35 * sin(T * Speed * 2.3 + ph * 1.7 + P.x * 0.02);\n"
		"return float3(0.8, 0.6, 0.0) * (s * Amount * h * min(h, 2.0));\n");

	// The shuttle's downwash (docs/design/shuttle-draw.md): each Wash is (x, y, radius, strength) in world cm, written by
	// UShuttleSubsystem into MPC_Downwash; plants inside the radius lean away from the middle and flutter, the more the
	// nearer and the taller. O is the plant's foot, P the pixel, T the time.
	const TCHAR* const FolDownwashCode = TEXT(
		"float3 acc = 0;\n"
		"float h = max(P.z - O.z, 0.0) / 100.0;\n"
		"float4 W[2] = { W0, W1 };\n"
		"for (int i = 0; i < 2; ++i) {\n"
		"  float2 d = O.xy - W[i].xy;\n"
		"  float r = length(d);\n"
		"  float f = saturate(1.0 - r / max(W[i].z, 1.0)) * W[i].w;\n"
		"  float2 dir = r > 1.0 ? d / r : float2(1.0, 0.0);\n"
		"  float flutter = 0.65 + 0.35 * sin(T * 14.0 + dot(O.xy, float2(0.051, 0.073)));\n"
		"  acc.xy += dir * (f * flutter * h * min(h, 1.5) * 60.0);\n"
		"  acc.z -= f * h * h * 10.0;\n"
		"}\n"
		"return acc * Amount;\n");

	bool FolSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	UMaterialExpressionTextureSampleParameter2D* FolTexture(UMaterial* Material, const TCHAR* Name, EMaterialSamplerType Type, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -800, Y));
		Node->ParameterName = Name;
		Node->SamplerType = Type;
		return Node;
	}

	UMaterialExpressionScalarParameter* FolScalar(UMaterial* Material, const TCHAR* Name, float Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -800, Y));
		Node->ParameterName = Name;
		Node->DefaultValue = Value;
		return Node;
	}

	void FolInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 Output = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(Output, From);
	}

	/** MPC_Downwash: two Wash vectors, made once (the game writes them every frame). */
	UMaterialParameterCollection* FolDownwashCollection()
	{
		const FString Path = FString(FolFolder) / TEXT("MPC_Downwash");
		UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *(Path + TEXT(".MPC_Downwash")), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Collection)
		{
			IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			Collection = Cast<UMaterialParameterCollection>(Tools.CreateAsset(TEXT("MPC_Downwash"), FolFolder, UMaterialParameterCollection::StaticClass(), NewObject<UMaterialParameterCollectionFactoryNew>()));
		}
		if (!Collection)
		{
			return nullptr;
		}
		for (const TCHAR* Name : {TEXT("Wash0"), TEXT("Wash1")})
		{
			if (!Collection->GetParameterId(FName(Name)).IsValid())
			{
				FCollectionVectorParameter& Parameter = Collection->VectorParameters.AddDefaulted_GetRef();
				Parameter.ParameterName = FName(Name);
				Parameter.DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
			}
		}
		Collection->PostEditChange();
		FolSave(Collection);
		return Collection;
	}

	UMaterialExpressionCustom* FolCustom(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Output, const FString& Code, int32 Y)
	{
		auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, Y));
		Custom->Description = Description;
		Custom->OutputType = Output;
		Custom->Code = Code;
		Custom->Inputs.Reset();
		return Custom;
	}

	/** The master. Its texture parameters start with one plant's virtual textures (every Megascans texture is one). */
	UMaterial* FolMakeMaster(const TCHAR* Name, bool bVirtual, UTexture* Albedo, UTexture* Normal, UTexture* Art, UTexture* Translucency)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, FolFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		Material->BlendMode = BLEND_Masked;
		Material->TwoSided = true;
		Material->SetShadingModel(MSM_TwoSidedFoliage);
		Material->OpacityMaskClipValue = 0.33f;
		Material->bUsedWithInstancedStaticMeshes = true;
		Material->bUsedWithNanite = true;

		auto* AlbedoNode = FolTexture(Material, TEXT("Albedo"), bVirtual ? SAMPLERTYPE_VirtualColor : SAMPLERTYPE_Color, 0);
		AlbedoNode->Texture = Albedo;
		auto* NormalNode = FolTexture(Material, TEXT("Normal"), bVirtual ? SAMPLERTYPE_VirtualNormal : SAMPLERTYPE_Normal, 300);
		NormalNode->Texture = Normal;
		auto* ArtNode = FolTexture(Material, TEXT("ART"), bVirtual ? SAMPLERTYPE_VirtualMasks : SAMPLERTYPE_Masks, 600);
		ArtNode->Texture = Art;
		auto* TransNode = FolTexture(Material, TEXT("Translucency"), bVirtual ? SAMPLERTYPE_VirtualColor : SAMPLERTYPE_Color, 900);
		TransNode->Texture = Translucency ? Translucency : Albedo;

		// Colour (times a tint) and opacity from the albedo's alpha (UseOpacity 0 for plants made without one).
		auto* Surface = FolCustom(Material, TEXT("FoliageSurface"), CMOT_Float4, TEXT(
			"float3 c = A.rgb * Tint;\n"
			"float o = lerp(1.0, A.a, UseOpacity);\n"
			"return float4(c, o);\n"), 0);
		FolInput(Surface, TEXT("A"), AlbedoNode, 5);  // RGBA
		FolInput(Surface, TEXT("Tint"), FolScalar(Material, TEXT("Brightness"), 0.8f, 150));
		FolInput(Surface, TEXT("UseOpacity"), FolScalar(Material, TEXT("UseOpacity"), 1.0f, 200));
		auto* Rgb = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionComponentMask::StaticClass(), 0, 0));
		Rgb->R = Rgb->G = Rgb->B = 1;
		Rgb->A = 0;
		Rgb->Input.Connect(0, Surface);
		auto* Alpha = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionComponentMask::StaticClass(), 0, 100));
		Alpha->R = Alpha->G = Alpha->B = 0;
		Alpha->A = 1;
		Alpha->Input.Connect(0, Surface);

		// Light through the leaves: the translucency map times the albedo, a little.
		auto* Through = FolCustom(Material, TEXT("FoliageThrough"), CMOT_Float3, TEXT("return T.rgb * A.rgb * Amount * (0.4 + 0.6 * ART.b);\n"), 900);
		FolInput(Through, TEXT("T"), TransNode, 0);
		FolInput(Through, TEXT("A"), AlbedoNode, 0);
		FolInput(Through, TEXT("ART"), ArtNode, 0);
		FolInput(Through, TEXT("Amount"), FolScalar(Material, TEXT("Translucency Amount"), 0.25f, 1000));

		auto* Rough = FolCustom(Material, TEXT("FoliageRough"), CMOT_Float1, TEXT("return saturate(ART.g * Scale);\n"), 600);
		FolInput(Rough, TEXT("ART"), ArtNode, 0);
		FolInput(Rough, TEXT("Scale"), FolScalar(Material, TEXT("Roughness Scale"), 1.0f, 700));
		auto* Ao = FolCustom(Material, TEXT("FoliageAO"), CMOT_Float1, TEXT("return ART.r;\n"), 750);
		FolInput(Ao, TEXT("ART"), ArtNode, 0);

		auto* Wind = FolCustom(Material, TEXT("FoliageWind"), CMOT_Float3, FolWindCode, 1200);
		FolInput(Wind, TEXT("P"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), -800, 1200));
		FolInput(Wind, TEXT("O"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionObjectPositionWS::StaticClass(), -800, 1260));
		FolInput(Wind, TEXT("T"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTime::StaticClass(), -800, 1320));
		FolInput(Wind, TEXT("Speed"), FolScalar(Material, TEXT("Wind Speed"), 1.6f, 1380));
		FolInput(Wind, TEXT("Amount"), FolScalar(Material, TEXT("Wind Amount"), 2.5f, 1440));

		UMaterialEditingLibrary::ConnectMaterialProperty(Rgb, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Alpha, TEXT(""), MP_OpacityMask);
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalNode, TEXT("RGB"), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Ao, TEXT(""), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(Through, TEXT(""), MP_SubsurfaceColor);
		// The shuttle's downwash on top of the wind (none without MPC_Downwash).
		UMaterialExpression* Offset = Wind;
		if (UMaterialParameterCollection* Collection = FolDownwashCollection())
		{
			auto* Wash = FolCustom(Material, TEXT("FoliageDownwash"), CMOT_Float3, FolDownwashCode, 1600);
			FolInput(Wash, TEXT("P"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), -800, 1600));
			FolInput(Wash, TEXT("O"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionObjectPositionWS::StaticClass(), -800, 1660));
			FolInput(Wash, TEXT("T"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTime::StaticClass(), -800, 1720));
			for (const TCHAR* Name : {TEXT("Wash0"), TEXT("Wash1")})
			{
				auto* Node = Cast<UMaterialExpressionCollectionParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCollectionParameter::StaticClass(), -800, Name[4] == '0' ? 1780 : 1840));
				Node->Collection = Collection;
				Node->ParameterName = FName(Name);
				Node->ParameterId = Collection->GetParameterId(FName(Name));
				FolInput(Wash, Name[4] == '0' ? TEXT("W0") : TEXT("W1"), Node);
			}
			FolInput(Wash, TEXT("Amount"), FolScalar(Material, TEXT("Downwash Amount"), 1.0f, 1900));
			auto* Sum = Cast<UMaterialExpressionAdd>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionAdd::StaticClass(), -100, 1400));
			Sum->A.Connect(0, Wind);
			Sum->B.Connect(0, Wash);
			Offset = Sum;
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Offset, TEXT(""), MP_WorldPositionOffset);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	UTexture* FolParam(UMaterialInstanceConstant* Instance, const TCHAR* Name)
	{
		UTexture* Texture = nullptr;
		Instance->GetTextureParameterValue(FMaterialParameterInfo(Name), Texture);
		return Texture;
	}
}

UFoliageMaterialsCommandlet::UFoliageMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFoliageMaterialsCommandlet::Main(const FString& Params)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);
	TArray<FAssetData> Found;
	Registry.GetAssetsByPath(FName(FolPlants), Found, true);
	TArray<UMaterialInstanceConstant*> Sources;
	for (const FAssetData& Asset : Found)
	{
		if (Asset.AssetClassPath == UMaterialInstanceConstant::StaticClass()->GetClassPathName())
		{
			if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Asset.GetAsset()))
			{
				Sources.Add(Instance);
			}
		}
	}
	if (Sources.IsEmpty())
	{
		UE_LOG(LogFoliageMaterials, Error, TEXT("No material instances under %s: run tools/electric_dreams.ps1"), FolPlants);
		return 1;
	}
	// Start clean: saving over the last run's files fails while they exist.
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(FolFolder), false, true);

	UMaterialInstanceConstant* First = nullptr;
	for (UMaterialInstanceConstant* Source : Sources)
	{
		if (FolParam(Source, TEXT("Albedo")) && FolParam(Source, TEXT("Normal")) && FolParam(Source, TEXT("ART")))
		{
			First = Source;
			break;
		}
	}
	if (!First)
	{
		UE_LOG(LogFoliageMaterials, Error, TEXT("No plant instance has Albedo, Normal and ART textures"));
		return 1;
	}
	UMaterial* Master = FolMakeMaster(TEXT("M_Foliage"), true, FolParam(First, TEXT("Albedo")), FolParam(First, TEXT("Normal")), FolParam(First, TEXT("ART")), FolParam(First, TEXT("Translucency")));
	if (!Master)
	{
		return 1;
	}
	// The L6 trees (TreeArt commandlet, run before this) have ordinary textures: a second master samples those.
	TArray<FAssetData> TreeTextures;
	Registry.GetAssetsByPath(FName(TEXT("/Game/Rim3D/Generated/Trees")), TreeTextures, true);
	UTexture* AnyTree[3] = {nullptr, nullptr, nullptr};
	TMap<FString, UTexture*> TreeTex;
	for (const FAssetData& Asset : TreeTextures)
	{
		if (UTexture* Texture = Asset.AssetClassPath == UTexture2D::StaticClass()->GetClassPathName() ? Cast<UTexture>(Asset.GetAsset()) : nullptr)
		{
			const FString Name = Texture->GetName();
			const int32 Kind = Name.Contains(TEXT("_nor")) ? 1 : (Name.Contains(TEXT("_rough")) ? 2 : (Name.Contains(TEXT("_diff")) ? 0 : -1));
			if (Kind < 0)
			{
				continue;
			}
			// Set each up the way the master reads it (the glTF import guesses).
			Texture->VirtualTextureStreaming = false;
			Texture->CompressionSettings = Kind == 1 ? TC_Normalmap : (Kind == 2 ? TC_Masks : TC_Default);
			Texture->SRGB = Kind == 0;
			Texture->PostEditChange();
			FolSave(Texture);
			AnyTree[Kind] = AnyTree[Kind] ? AnyTree[Kind] : Texture;
			TreeTex.Add(Name, Texture);
		}
	}
	UMaterial* TreeMaster = AnyTree[0] && AnyTree[1] && AnyTree[2] ? FolMakeMaster(TEXT("M_FoliageTex"), false, AnyTree[0], AnyTree[1], AnyTree[2], AnyTree[0]) : nullptr;
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	bool bErrors = false;
	if (const FMaterialResource* Resource = Master->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		for (const FString& Error : Resource->GetCompileErrors())
		{
			UE_LOG(LogFoliageMaterials, Error, TEXT("Material: %s"), *Error);
			bErrors = true;
		}
	}
	bool bSaved = FolSave(Master) && (!TreeMaster || FolSave(TreeMaster));

	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Made = 0;
	for (UMaterialInstanceConstant* Source : Sources)
	{
		UTexture* Albedo = FolParam(Source, TEXT("Albedo"));
		if (!Albedo)
		{
			UE_LOG(LogFoliageMaterials, Display, TEXT("%s: no Albedo texture, skipped"), *Source->GetName());
			continue;
		}
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Master;
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(Source->GetName() + TEXT("_R3D"), FolFolder, UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			continue;
		}
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Albedo"), Albedo);
		for (const TCHAR* Name : {TEXT("Normal"), TEXT("ART"), TEXT("Translucency")})
		{
			if (UTexture* Texture = FolParam(Source, Name))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Name, Texture);
			}
		}
		// The sample's "no opacity" masters are for broad leaves drawn solid.
		const FString ParentName = Source->Parent ? Source->Parent->GetName() : FString();
		if (ParentName.Contains(TEXT("NoOpacity")))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("UseOpacity"), 0.0f);
		}
		// Plants the sample drew without wind (ground cover, moss) stay still here too.
		if (ParentName.Contains(TEXT("NoWPO")))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Wind Amount"), 0.0f);
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		bSaved = FolSave(Instance) && bSaved;
		++Made;
	}
	// One instance per tree material slot, named after the slot's material, with the textures named after it
	// (Poly Haven: <material>_diff, _nor_gl, _rough). Leaves are masked and see-through; bark is solid. The whole
	// tree sways a little, all slots alike, so leaves stay on their branches.
	int32 TreeMade = 0;
	TSet<FString> TreeDone; // a rock set's pieces share one material: make its copy once
	TArray<FAssetData> TreeMeshes;
	Registry.GetAssetsByClass(UStaticMesh::StaticClass()->GetClassPathName(), TreeMeshes, true);
	for (const FAssetData& Asset : TreeMeshes)
	{
		if (!TreeMaster || !Asset.PackagePath.ToString().StartsWith(TEXT("/Game/Rim3D/Generated/Trees")))
		{
			continue;
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
		// Trees (a slot with leaves or branch cards) sway; rocks get solid, still copies too: the glTF import's own
		// materials sit on an engine master without the Nanite and instanced usages, so they drew as the default material.
		bool bTree = false;
		for (int32 Slot = 0; Mesh && Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
		{
			const FString SlotMaterial = Mesh->GetMaterial(Slot) ? Mesh->GetMaterial(Slot)->GetName() : FString();
			bTree |= SlotMaterial.Contains(TEXT("leaves")) || SlotMaterial.Contains(TEXT("leaf")) || SlotMaterial.Contains(TEXT("branch"));
		}
		for (int32 Slot = 0; Mesh && Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
		{
			UMaterialInterface* Original = Mesh->GetMaterial(Slot);
			if (!Original)
			{
				continue;
			}
			// The textures carry the material's name, or a shorter one (branches -> branch; the trunk often has the model's).
			FString Base = Original->GetName();
			const auto HasDiff = [&TreeTex](const FString& Name)
			{
				for (const TPair<FString, UTexture*>& Pair : TreeTex)
				{
					if (Pair.Key.StartsWith(Name + TEXT("_diff")))
					{
						return true;
					}
				}
				return false;
			};
			const FString MaterialName = Base;
			if (!HasDiff(Base) && HasDiff(Base.Replace(TEXT("branches"), TEXT("branch"))))
			{
				Base = Base.Replace(TEXT("branches"), TEXT("branch"));
			}
			else if (!HasDiff(Base) && HasDiff(Mesh->GetName()))
			{
				Base = Mesh->GetName();
			}
			// A rock set's pieces (rock_moss_set_01_a ...) share the set's textures: drop trailing _parts until one matches.
			for (FString Shorter = MaterialName; !HasDiff(Base) && Shorter.Contains(TEXT("_"));)
			{
				Shorter = Shorter.Left(Shorter.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd));
				if (HasDiff(Shorter))
				{
					Base = Shorter;
				}
			}
			const auto Find = [&TreeTex, &Base](const TCHAR* Suffix) -> UTexture*
			{
				for (const TPair<FString, UTexture*>& Pair : TreeTex)
				{
					if (Pair.Key.StartsWith(Base + Suffix))
					{
						return Pair.Value;
					}
				}
				return nullptr;
			};
			UTexture* Diff = Find(TEXT("_diff"));
			if (!Diff)
			{
				UE_LOG(LogFoliageMaterials, Warning, TEXT("%s: no %s_diff texture"), *Mesh->GetName(), *Base);
				continue;
			}
			if (TreeDone.Contains(MaterialName))
			{
				continue;
			}
			TreeDone.Add(MaterialName);
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
			Factory->InitialParent = TreeMaster;
			UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(MaterialName + TEXT("_R3D"), FolFolder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (!Instance)
			{
				continue;
			}
			const bool bLeaves = MaterialName.Contains(TEXT("leaves")) || MaterialName.Contains(TEXT("leaf")) || MaterialName.Contains(TEXT("branch"));
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Albedo"), Diff);
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Translucency"), Diff);
			if (UTexture* Nor = Find(TEXT("_nor")))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("Normal"), Nor);
			}
			if (UTexture* Rough = Find(TEXT("_rough")))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("ART"), Rough);
			}
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("UseOpacity"), bLeaves ? 1.0f : 0.0f);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Translucency Amount"), bLeaves ? 0.3f : 0.0f);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Brightness"), 1.0f);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Wind Amount"), bTree ? 0.35f : 0.0f);
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			bSaved = FolSave(Instance) && bSaved;
			++TreeMade;
		}
	}
	UE_LOG(LogFoliageMaterials, Display, TEXT("Trees: %d tree materials on M_FoliageTex%s"), TreeMade, TreeMaster ? TEXT("") : TEXT(" (no tree textures found: run the TreeArt commandlet first)"));
	UE_LOG(LogFoliageMaterials, Display, TEXT("%s %s/M_Foliage and %d plant materials (of %d found)%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), FolFolder, Made, Sources.Num(), bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
