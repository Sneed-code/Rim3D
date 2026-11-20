#include "WetLookCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "WetnessShared.h"

DEFINE_LOG_CATEGORY_STATIC(LogWetLook, Log, All);

namespace
{
	const TCHAR* const WetLookGeneratedRoot = TEXT("/Game/Rim3D/Generated");

	// Folders whose materials are left alone: decals and effects (translucent or unlit anyway, listed to be sure), the
	// MakeHuman bodies (they have their own wetness), clips, the sky and the weather assets themselves.
	const TCHAR* const WetLookSkipFolders[] = {
		TEXT("/Blood/"), TEXT("/Fights/"), TEXT("/Sky/"), TEXT("/MakeHuman"), TEXT("/Animations/"), TEXT("/Weather/"), TEXT("/Water/"),
	};

	// The tag on every node this commandlet adds, so a material is changed only once.
	const TCHAR* const WetLookTag = TEXT("WetLook");

	// How wet this pixel is, 0 to 1: the weather's wetness, nothing under a roof or upper floor (the cover texture, looked
	// up half a cell out along the normal, so a wall's outside face is wet and its inside face dry), faces up wettest,
	// sides a little less, undersides almost dry.
	const TCHAR* const WetLookAmountCode = TEXT(
		"float w = saturate(Wet);\n"
		"if (w <= 0.0) { return 0.0; }\n"
		"float3 n = normalize(N);\n"
		"float2 cell = (P.xy - Map.xy) * 0.01 + n.xy * 0.55;\n"
		"float2 uv = (floor(cell) + 0.5) * Map.w;\n"
		"float dry = 0.0;\n"
		"if (all(uv > 0.0) && all(uv < 1.0))\n"
		"{\n"
		"    float top = Texture2DSampleLevel(Cover, CoverSampler, uv, 0).r;\n"
		"    float z = (P.z - Map.z) * 0.01;\n"
		"    dry = z < top - 0.15 ? 1.0 : 0.0;\n"
		"}\n"
		"float facing = lerp(0.6, 1.0, saturate(n.z)) * (1.0 - 0.85 * saturate(-n.z));\n"
		"return w * (1.0 - dry) * facing;\n");

	// The same without the facing, for a material that draws its own wet look from a `Wetness` parameter (the ground).
	const TCHAR* const WetLookCoverCode = TEXT(
		"float w = saturate(Wet);\n"
		"if (w <= 0.0) { return 0.0; }\n"
		"float3 n = normalize(N);\n"
		"float2 cell = (P.xy - Map.xy) * 0.01 + n.xy * 0.55;\n"
		"float2 uv = (floor(cell) + 0.5) * Map.w;\n"
		"if (all(uv > 0.0) && all(uv < 1.0))\n"
		"{\n"
		"    float top = Texture2DSampleLevel(Cover, CoverSampler, uv, 0).r;\n"
		"    if ((P.z - Map.z) * 0.01 < top - 0.15) { return 0.0; }\n"
		"}\n"
		"return w;\n");

	// Worn things (clothes) take the wearer's wetness, faces up a little wetter.
	const TCHAR* const WetLookWornCode = TEXT(
		"return saturate(Wet) * lerp(0.8, 1.0, saturate(normalize(N).z));\n");

	// Water fills pores and fibres: rough (porous) surfaces darken most, smooth ones (metal, glass, varnish) hardly at all.
	const TCHAR* const WetLookColourCode = TEXT(
		"float porous = saturate(Rough * 1.4);\n"
		"return Base * lerp(1.0, 0.55, saturate(A) * porous);\n");

	// A film of water is glossy whatever is under it.
	const TCHAR* const WetLookRoughCode = TEXT(
		"return lerp(Rough, min(Rough, 0.12), saturate(A));\n");

	bool WetLookSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogWetLook, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	/** MPC_Weather with its two parameters, made once. */
	UMaterialParameterCollection* WetLookCollection(IAssetTools& Tools)
	{
		const FString Folder = WetnessShared::Folder;
		const FString Path = Folder / WetnessShared::CollectionName + TEXT(".") + WetnessShared::CollectionName;
		UMaterialParameterCollection* Collection = LoadObject<UMaterialParameterCollection>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Collection)
		{
			Collection = Cast<UMaterialParameterCollection>(Tools.CreateAsset(WetnessShared::CollectionName, Folder, UMaterialParameterCollection::StaticClass(), NewObject<UMaterialParameterCollectionFactoryNew>()));
		}
		if (!Collection)
		{
			UE_LOG(LogWetLook, Error, TEXT("Could not make %s"), *Path);
			return nullptr;
		}
		bool bChanged = false;
		if (!Collection->GetParameterId(WetnessShared::WetnessParam).IsValid())
		{
			FCollectionScalarParameter& Parameter = Collection->ScalarParameters.AddDefaulted_GetRef();
			Parameter.ParameterName = WetnessShared::WetnessParam;
			Parameter.DefaultValue = 0.0f;
			bChanged = true;
		}
		if (!Collection->GetParameterId(WetnessShared::CoverMapParam).IsValid())
		{
			FCollectionVectorParameter& Parameter = Collection->VectorParameters.AddDefaulted_GetRef();
			Parameter.ParameterName = WetnessShared::CoverMapParam;
			Parameter.DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f / static_cast<float>(WetnessShared::CoverSize));
			bChanged = true;
		}
		if (bChanged)
		{
			Collection->PostEditChange();
			WetLookSave(Collection);
		}
		return Collection;
	}

	/** T_WeatherCover: single floats, nearest, no mips, never streamed, so the game can overwrite it (UpdateTextureRegions). */
	UTexture2D* WetLookCoverTexture()
	{
		const FString Folder = WetnessShared::Folder;
		const FString Path = Folder / WetnessShared::CoverName + TEXT(".") + WetnessShared::CoverName;
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*(Folder / WetnessShared::CoverName));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, WetnessShared::CoverName, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		TArray<float> Open;
		Open.Init(WetnessShared::OpenSky, WetnessShared::CoverSize * WetnessShared::CoverSize);
		Texture->Source.Init(WetnessShared::CoverSize, WetnessShared::CoverSize, 1, 1, TSF_R32F, reinterpret_cast<const uint8*>(Open.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_SingleFloat;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->Filter = TF_Nearest;
		Texture->NeverStream = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->LODGroup = TEXTUREGROUP_Pixels2D;
		Texture->PostEditChange();
		WetLookSave(Texture);
		return Texture;
	}

	enum class EWetLookMode : uint8
	{
		World,  // the weather, masked by roofs
		Worn,   // the wearer's wetness (custom primitive data 12)
	};

	UMaterialExpressionCustom* WetLookCustom(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Output, const TCHAR* Code, int32 X, int32 Y)
	{
		auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), X, Y));
		Custom->Description = FString(WetLookTag) + Description;
		Custom->OutputType = Output;
		Custom->Code = Code;
		Custom->Inputs.Reset();
		return Custom;
	}

	void WetLookInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, const FExpressionInput& From)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input = From;
	}

	void WetLookInputNode(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(0, From);
	}

	/** Adds P, N and (for the world) Map and Cover, the inputs every wet-amount node takes. */
	void WetLookPlaceInputs(UMaterial* Material, UMaterialExpressionCustom* Custom, EWetLookMode Mode, UMaterialParameterCollection* Collection, UTexture2D* Cover, int32 Y)
	{
		WetLookInputNode(Custom, TEXT("P"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), -1300, Y));
		WetLookInputNode(Custom, TEXT("N"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -1300, Y + 60));
		if (Mode != EWetLookMode::World)
		{
			return;
		}
		auto* Map = Cast<UMaterialExpressionCollectionParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCollectionParameter::StaticClass(), -1300, Y + 120));
		Map->Collection = Collection;
		Map->ParameterName = WetnessShared::CoverMapParam;
		Map->ParameterId = Collection->GetParameterId(WetnessShared::CoverMapParam);
		WetLookInputNode(Custom, TEXT("Map"), Map);
		auto* Tex = Cast<UMaterialExpressionTextureObject>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObject::StaticClass(), -1300, Y + 180));
		Tex->Texture = Cover;
		Tex->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Cover);
		WetLookInputNode(Custom, TEXT("Cover"), Tex);
	}

	bool WetLookAlreadyDone(const UMaterial* Material)
	{
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression.Get());
			if (Custom && Custom->Description.StartsWith(WetLookTag))
			{
				return true;
			}
		}
		return false;
	}

	/** The material's own `Wetness` scalar parameter, if it has one. */
	UMaterialExpressionScalarParameter* WetLookOwnWetness(const UMaterial* Material)
	{
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			UMaterialExpressionScalarParameter* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression.Get());
			if (Scalar && Scalar->ParameterName == WetnessShared::WetnessParam)
			{
				return Scalar;
			}
		}
		return nullptr;
	}

	/**
	 * A material that draws its own wet look (the ground's `Wetness`): everything that read the parameter now reads it
	 * masked by the roofs, so the floor of a roofed room stays dry in the rain.
	 */
	bool WetLookMaskOwnWetness(UMaterial* Material, UMaterialExpressionScalarParameter* Own, UMaterialParameterCollection* Collection, UTexture2D* Cover)
	{
		auto* Masked = WetLookCustom(Material, TEXT("Cover"), CMOT_Float1, WetLookCoverCode, Own->MaterialExpressionEditorX + 250, Own->MaterialExpressionEditorY);
		WetLookInputNode(Masked, TEXT("Wet"), Own);
		WetLookPlaceInputs(Material, Masked, EWetLookMode::World, Collection, Cover, Own->MaterialExpressionEditorY + 100);
		int32 Rewired = 0;
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			if (!Expression || Expression.Get() == Masked)
			{
				continue;
			}
			for (int32 Index = 0;; ++Index)
			{
				FExpressionInput* Input = Expression->GetInput(Index);
				if (!Input)
				{
					break;
				}
				if (Input->Expression == Own)
				{
					Input->Expression = Masked;
					Input->OutputIndex = 0;
					++Rewired;
				}
			}
		}
		return Rewired > 0;
	}

	/** Darker and glossier by the wet amount: wraps whatever feeds BaseColor and Roughness. */
	bool WetLookWrap(UMaterial* Material, EWetLookMode Mode, UMaterialParameterCollection* Collection, UTexture2D* Cover)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		if (!Data || !Data->BaseColor.Expression)
		{
			return false;
		}
		// The wet amount.
		auto* Amount = WetLookCustom(Material, TEXT("Amount"), CMOT_Float1, Mode == EWetLookMode::World ? WetLookAmountCode : WetLookWornCode, -700, 1700);
		if (Mode == EWetLookMode::World)
		{
			auto* Wet = Cast<UMaterialExpressionCollectionParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCollectionParameter::StaticClass(), -1300, 1600));
			Wet->Collection = Collection;
			Wet->ParameterName = WetnessShared::WetnessParam;
			Wet->ParameterId = Collection->GetParameterId(WetnessShared::WetnessParam);
			WetLookInputNode(Amount, TEXT("Wet"), Wet);
		}
		else
		{
			auto* Wet = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -1300, 1600));
			Wet->ParameterName = WetnessShared::WetnessParam;
			Wet->DefaultValue = 0.0f;
			Wet->bUseCustomPrimitiveData = true;
			Wet->PrimitiveDataIndex = WetnessShared::WornDataIndex;
			WetLookInputNode(Amount, TEXT("Wet"), Wet);
		}
		WetLookPlaceInputs(Material, Amount, Mode, Collection, Cover, 1700);

		// The roughness as it was (a constant if nothing fed it).
		FExpressionInput RoughIn;
		if (Data->Roughness.Expression)
		{
			RoughIn = Data->Roughness;
		}
		else
		{
			auto* Constant = Cast<UMaterialExpressionConstant>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionConstant::StaticClass(), -700, 1500));
			Constant->R = Data->Roughness.UseConstant ? Data->Roughness.Constant : 0.5f;
			RoughIn.Connect(0, Constant);
		}

		auto* Colour = WetLookCustom(Material, TEXT("Colour"), CMOT_Float3, WetLookColourCode, -150, -700);
		WetLookInput(Colour, TEXT("Base"), Data->BaseColor);
		WetLookInput(Colour, TEXT("Rough"), RoughIn);
		WetLookInputNode(Colour, TEXT("A"), Amount);

		auto* Rough = WetLookCustom(Material, TEXT("Roughness"), CMOT_Float1, WetLookRoughCode, -150, 1500);
		WetLookInput(Rough, TEXT("Rough"), RoughIn);
		WetLookInputNode(Rough, TEXT("A"), Amount);

		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		return true;
	}

	bool WetLookSkipped(const FString& PackageName)
	{
		for (const TCHAR* Folder : WetLookSkipFolders)
		{
			if (PackageName.Contains(Folder))
			{
				return true;
			}
		}
		return false;
	}
}

UWetLookCommandlet::UWetLookCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UWetLookCommandlet::Main(const FString& Params)
{
	const bool bListOnly = Params.Contains(TEXT("-List"));
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMaterialParameterCollection* Collection = WetLookCollection(Tools);
	UTexture2D* Cover = WetLookCoverTexture();
	if (!Collection || !Cover)
	{
		UE_LOG(LogWetLook, Error, TEXT("No MPC_Weather or T_WeatherCover; nothing changed"));
		return 1;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(WetLookGeneratedRoot));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
	TArray<FAssetData> Found;
	Registry.GetAssets(Filter, Found);

	int32 ByWeather = 0;
	int32 ByWearer = 0;
	int32 Masked = 0;
	int32 Skipped = 0;
	int32 Done = 0;
	int32 Failed = 0;
	for (const FAssetData& Asset : Found)
	{
		const FString Name = Asset.PackageName.ToString();
		if (WetLookSkipped(Name))
		{
			++Skipped;
			continue;
		}
		UMaterial* Material = Cast<UMaterial>(Asset.GetAsset());
		if (!Material)
		{
			continue;
		}
		const bool bSurface = Material->MaterialDomain == MD_Surface;
		const bool bSolid = Material->BlendMode == BLEND_Opaque || Material->BlendMode == BLEND_Masked;
		if (!bSurface || !bSolid || Material->GetShadingModels().IsUnlit() || Material->bUseMaterialAttributes)
		{
			++Skipped;
			continue;
		}
		if (WetLookAlreadyDone(Material))
		{
			++Done;
			continue;
		}
		bool bChanged = false;
		const TCHAR* What = TEXT("");
		if (UMaterialExpressionScalarParameter* Own = WetLookOwnWetness(Material))
		{
			if (Own->bUseCustomPrimitiveData)
			{
				++Skipped;  // already wet by its wearer (the MakeHuman bodies, wherever they live)
				continue;
			}
			What = TEXT("own wetness masked by roofs");
			bChanged = bListOnly || WetLookMaskOwnWetness(Material, Own, Collection, Cover);
			Masked += bChanged ? 1 : 0;
		}
		else
		{
			const EWetLookMode Mode = Name.Contains(TEXT("/Clothes/")) ? EWetLookMode::Worn : EWetLookMode::World;
			What = Mode == EWetLookMode::Worn ? TEXT("wet by its wearer") : TEXT("wet by the weather");
			bChanged = bListOnly ? Material->GetEditorOnlyData()->BaseColor.Expression != nullptr : WetLookWrap(Material, Mode, Collection, Cover);
			(Mode == EWetLookMode::Worn ? ByWearer : ByWeather) += bChanged ? 1 : 0;
		}
		if (!bChanged)
		{
			++Skipped;
			continue;
		}
		UE_LOG(LogWetLook, Display, TEXT("%s: %s"), *Name, What);
		if (bListOnly)
		{
			continue;
		}
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (!WetLookSave(Material))
		{
			++Failed;
		}
	}
	UE_LOG(LogWetLook, Display, TEXT("WetLook%s: %d by the weather, %d by the wearer, %d ground-style masked, %d done before, %d skipped, %d failed"),
		bListOnly ? TEXT(" (list only)") : TEXT(""), ByWeather, ByWearer, Masked, Done, Skipped, Failed);
	return Failed == 0 ? 0 : 1;
}
