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
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionVertexColor.h"
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

	// A building losing hit points (docs/design/destruction.md; per-instance custom data, 0 for anything that isn't a
	// damaged building): a network of cracks that spreads and widens with the damage (the edges of a world-space cell
	// pattern about 40 cm across, on the plane the face lies in, more of the cells cracked the more is lost), dark in the
	// cracks, and the whole face a little greyer and dirtier.
	const TCHAR* const WetLookDamageCode = TEXT(
		"float d = saturate(D);\n"
		"if (d <= 0.02) { return Base; }\n"
		"float3 n = abs(normalize(N));\n"
		"float2 uv = (n.z > n.x && n.z > n.y) ? P.xy : (n.x > n.y ? P.yz : P.xz);\n"
		"uv *= 0.025;\n"
		"float2 ip = floor(uv);\n"
		"float2 fp = frac(uv);\n"
		"float f1 = 8.0;\n"
		"float f2 = 8.0;\n"
		"float2 nearest = ip;\n"
		"for (int j = -1; j <= 1; ++j)\n"
		"{\n"
		"    for (int i = -1; i <= 1; ++i)\n"
		"    {\n"
		"        float2 g = float2(i, j);\n"
		"        float2 h = frac(sin(float2(dot(ip + g, float2(127.1, 311.7)), dot(ip + g, float2(269.5, 183.3)))) * 43758.5453);\n"
		"        float dd = length(g + h - fp);\n"
		"        if (dd < f1) { f2 = f1; f1 = dd; nearest = ip + g; } else if (dd < f2) { f2 = dd; }\n"
		"    }\n"
		"}\n"
		"float edge = f2 - f1;\n"
		"float pick = frac(sin(dot(nearest, float2(12.9898, 78.233))) * 43758.5453);\n"
		"float width = lerp(0.012, 0.05, d);\n"
		"float crack = (1.0 - smoothstep(0.0, width, edge)) * step(pick, d * 1.4);\n"
		"float3 grimy = lerp(Base, dot(Base, float3(0.3, 0.59, 0.11)).xxx * 0.85, d * 0.35);\n"
		"return lerp(grimy, grimy * 0.1, crack);\n");

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

	/** A new node in the material, marked as ours (Desc) so a later run can take it out again. */
	UMaterialExpression* WetLookNode(UMaterial* Material, UClass* Class, int32 X, int32 Y)
	{
		UMaterialExpression* Node = UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
		Node->Desc = WetLookTag;
		return Node;
	}

	UMaterialExpressionCustom* WetLookCustom(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Output, const TCHAR* Code, int32 X, int32 Y)
	{
		auto* Custom = Cast<UMaterialExpressionCustom>(WetLookNode(Material, UMaterialExpressionCustom::StaticClass(), X, Y));
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
		WetLookInputNode(Custom, TEXT("P"), WetLookNode(Material, UMaterialExpressionWorldPosition::StaticClass(), -1300, Y));
		WetLookInputNode(Custom, TEXT("N"), WetLookNode(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -1300, Y + 60));
		if (Mode != EWetLookMode::World)
		{
			return;
		}
		auto* Map = Cast<UMaterialExpressionCollectionParameter>(WetLookNode(Material, UMaterialExpressionCollectionParameter::StaticClass(), -1300, Y + 120));
		Map->Collection = Collection;
		Map->ParameterName = WetnessShared::CoverMapParam;
		Map->ParameterId = Collection->GetParameterId(WetnessShared::CoverMapParam);
		WetLookInputNode(Custom, TEXT("Map"), Map);
		auto* Tex = Cast<UMaterialExpressionTextureObject>(WetLookNode(Material, UMaterialExpressionTextureObject::StaticClass(), -1300, Y + 180));
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
			auto* Wet = Cast<UMaterialExpressionCollectionParameter>(WetLookNode(Material, UMaterialExpressionCollectionParameter::StaticClass(), -1300, 1600));
			Wet->Collection = Collection;
			Wet->ParameterName = WetnessShared::WetnessParam;
			Wet->ParameterId = Collection->GetParameterId(WetnessShared::WetnessParam);
			WetLookInputNode(Amount, TEXT("Wet"), Wet);
		}
		else
		{
			auto* Wet = Cast<UMaterialExpressionScalarParameter>(WetLookNode(Material, UMaterialExpressionScalarParameter::StaticClass(), -1300, 1600));
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
			auto* Constant = Cast<UMaterialExpressionConstant>(WetLookNode(Material, UMaterialExpressionConstant::StaticClass(), -700, 1500));
			Constant->R = Data->Roughness.UseConstant ? Data->Roughness.Constant : 0.5f;
			RoughIn.Connect(0, Constant);
		}

		auto* Colour = WetLookCustom(Material, TEXT("Colour"), CMOT_Float3, WetLookColourCode, -150, -700);
		if (Mode == EWetLookMode::World)
		{
			// Cracks under the wet: a damaged building's colour first, then the water on it.
			auto* Damage = WetLookCustom(Material, TEXT("Damage"), CMOT_Float3, WetLookDamageCode, -450, -900);
			WetLookInput(Damage, TEXT("Base"), Data->BaseColor);
			WetLookInputNode(Damage, TEXT("P"), WetLookNode(Material, UMaterialExpressionWorldPosition::StaticClass(), -900, -900));
			WetLookInputNode(Damage, TEXT("N"), WetLookNode(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -900, -840));
			auto* D = Cast<UMaterialExpressionPerInstanceCustomData>(WetLookNode(Material, UMaterialExpressionPerInstanceCustomData::StaticClass(), -900, -780));
			D->DataIndex = WetnessShared::DamageDataIndex;
			D->ConstDefaultValue = 0.0f;
			WetLookInputNode(Damage, TEXT("D"), D);
			WetLookInputNode(Colour, TEXT("Base"), Damage);
		}
		else
		{
			WetLookInput(Colour, TEXT("Base"), Data->BaseColor);
		}
		WetLookInput(Colour, TEXT("Rough"), RoughIn);
		WetLookInputNode(Colour, TEXT("A"), Amount);

		auto* Rough = WetLookCustom(Material, TEXT("Roughness"), CMOT_Float1, WetLookRoughCode, -150, 1500);
		WetLookInput(Rough, TEXT("Rough"), RoughIn);
		WetLookInputNode(Rough, TEXT("A"), Amount);

		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		return true;
	}

	UMaterialExpressionCustom* WetLookFind(const UMaterial* Material, const TCHAR* Description)
	{
		const FString Wanted = FString(WetLookTag) + Description;
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression.Get());
			if (Custom && Custom->Description == Wanted)
			{
				return Custom;
			}
		}
		return nullptr;
	}

	const FExpressionInput* WetLookInputOf(const UMaterialExpressionCustom* Custom, const TCHAR* Name)
	{
		for (const FCustomInput& Input : Custom->Inputs)
		{
			if (Input.InputName == Name)
			{
				return &Input.Input;
			}
		}
		return nullptr;
	}

	/** Takes a wrap (WetLookWrap, this or an older run) back out: the base colour and roughness as they were before it. */
	bool WetLookUnwrap(UMaterial* Material)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		UMaterialExpressionCustom* Colour = WetLookFind(Material, TEXT("Colour"));
		if (!Data || !Colour)
		{
			return false;
		}
		const FExpressionInput* Base = WetLookInputOf(Colour, TEXT("Base"));
		const FExpressionInput* Rough = WetLookInputOf(Colour, TEXT("Rough"));
		if (!Base || !Rough)
		{
			return false;
		}
		const UMaterialExpressionCustom* Damage = Cast<UMaterialExpressionCustom>(Base->Expression);
		if (Damage && Damage->Description.StartsWith(WetLookTag))
		{
			Base = WetLookInputOf(Damage, TEXT("Base"));
			if (!Base)
			{
				return false;
			}
		}
		const FExpressionInput OldBase = *Base;
		const FExpressionInput OldRough = *Rough;
		// Everything the wrap added: its own nodes and the inputs only they used (older runs didn't mark those).
		TSet<UMaterialExpression*> Remove;
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression.Get());
			if (!Custom || !Custom->Description.StartsWith(WetLookTag) || Custom->Description == TEXT("WetLookCover"))
			{
				continue;
			}
			Remove.Add(Custom);
			for (const FCustomInput& Input : Custom->Inputs)
			{
				UMaterialExpression* From = Input.Input.Expression;
				const bool bAdded = From && (From->IsA<UMaterialExpressionWorldPosition>() || From->IsA<UMaterialExpressionVertexNormalWS>() ||
					From->IsA<UMaterialExpressionCollectionParameter>() || From->IsA<UMaterialExpressionTextureObject>() ||
					From->IsA<UMaterialExpressionPerInstanceCustomData>() ||
					(From->IsA<UMaterialExpressionScalarParameter>() && Cast<UMaterialExpressionScalarParameter>(From)->bUseCustomPrimitiveData &&
						Cast<UMaterialExpressionScalarParameter>(From)->PrimitiveDataIndex == WetnessShared::WornDataIndex));
				if (bAdded)
				{
					Remove.Add(From);
				}
			}
		}
		Remove.Remove(OldBase.Expression);
		Remove.Remove(OldRough.Expression);
		Data->BaseColor.Connect(OldBase.OutputIndex, OldBase.Expression);
		Data->Roughness.Connect(OldRough.OutputIndex, OldRough.Expression);
		for (UMaterialExpression* Expression : Remove)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}
		return true;
	}

	/** Leftover wrap nodes with nothing of the wrap on the material's base colour or roughness (a material its own
	 *  commandlet just built again, like GunArt's M_GunPBR, PC 2026-11-22): the leftovers are deleted so it is wrapped
	 *  afresh. False if the wrap still feeds a property (that one needs WetLookUnwrap). */
	bool WetLookClearLeftovers(UMaterial* Material)
	{
		UMaterialEditorOnlyData* Data = Material->GetEditorOnlyData();
		if (!Data)
		{
			return false;
		}
		const auto Tagged = [](const UMaterialExpression* Expression)
		{
			const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression);
			return (Custom && Custom->Description.StartsWith(WetLookTag)) || (Expression && Expression->Desc.StartsWith(WetLookTag));
		};
		if (Tagged(Data->BaseColor.Expression) || Tagged(Data->Roughness.Expression))
		{
			return false;
		}
		TArray<UMaterialExpression*> Gone;
		for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
		{
			if (Tagged(Expression.Get()))
			{
				Gone.Add(Expression.Get());
			}
		}
		for (UMaterialExpression* Expression : Gone)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
		}
		UE_LOG(LogWetLook, Display, TEXT("%s: %d leftover wrap nodes cleared, wrapped afresh"), *Material->GetPathName(), Gone.Num());
		return true;
	}

	/** People's own materials (bodies, hair, the cloth they wear) take the wearer's wetness, not the weather's. */
	EWetLookMode WetLookModeFor(const FString& PackageName)
	{
		const FString Asset = FPackageName::GetShortName(PackageName);
		return PackageName.Contains(TEXT("/Clothes/")) || Asset.StartsWith(TEXT("M_Pawn")) ? EWetLookMode::Worn : EWetLookMode::World;
	}

	/** True when a wrapped material is wrapped as wanted: the right mode, and in the world the damage look too. */
	bool WetLookWrapCurrent(const UMaterial* Material, EWetLookMode Mode)
	{
		const UMaterialExpressionCustom* Amount = WetLookFind(Material, TEXT("Amount"));
		if (!Amount)
		{
			return true;  // ground-style (own wetness masked by roofs): nothing to bring up to date
		}
		const bool bWorn = Amount->Code == WetLookWornCode;
		if (bWorn != (Mode == EWetLookMode::Worn))
		{
			return false;
		}
		return bWorn || WetLookFind(Material, TEXT("Damage")) != nullptr;
	}

	/** M_WetSolid: the engine's VertexColorMaterial made again as our own (a mesh's vertex colours, lit, roughness 0.5),
	 * so the plain colour buildings drawn with it get wet and show damage like the textured ones. */
	UMaterial* WetLookSolidMaterial(bool& bMade)
	{
		bMade = false;
		if (UMaterial* Existing = LoadObject<UMaterial>(nullptr, WetnessShared::SolidPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		const FString Folder = WetnessShared::Folder;
		UPackage* Package = CreatePackage(*(Folder / WetnessShared::SolidName));
		UMaterial* Material = NewObject<UMaterial>(Package, WetnessShared::SolidName, RF_Public | RF_Standalone);
		Material->bUsedWithInstancedStaticMeshes = true;
		Material->bUsedWithNanite = true;
		UMaterialExpression* Colour = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexColor::StaticClass(), -1000, 0);
		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);
		bMade = true;
		return Material;
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
	int32 Redone = 0;
	int32 Failed = 0;

	// The plain colour material (in the weather folder, which the scan below leaves alone).
	bool bSolidMade = false;
	if (UMaterial* Solid = bListOnly ? nullptr : WetLookSolidMaterial(bSolidMade))
	{
		const bool bCurrent = !bSolidMade && WetLookAlreadyDone(Solid) && WetLookWrapCurrent(Solid, EWetLookMode::World);
		if (!bCurrent)
		{
			if (!bSolidMade && WetLookAlreadyDone(Solid))
			{
				WetLookUnwrap(Solid);
			}
			WetLookWrap(Solid, EWetLookMode::World, Collection, Cover);
			UMaterialEditingLibrary::RecompileMaterial(Solid);
			Failed += WetLookSave(Solid) ? 0 : 1;
			UE_LOG(LogWetLook, Display, TEXT("%s: %s, wet by the weather"), WetnessShared::SolidPath, bSolidMade ? TEXT("made") : TEXT("brought up to date"));
		}
	}
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
		const EWetLookMode Mode = WetLookModeFor(Name);
		if (WetLookAlreadyDone(Material))
		{
			if (WetLookWrapCurrent(Material, Mode))
			{
				++Done;
				continue;
			}
			// Wrapped by an older run, or in the wrong mode (people's materials were once wet by the weather): redone.
			UE_LOG(LogWetLook, Display, TEXT("%s: wrapped before, done again"), *Name);
			if (bListOnly)
			{
				++Redone;
				continue;
			}
			if (!WetLookUnwrap(Material) && !WetLookClearLeftovers(Material))
			{
				UE_LOG(LogWetLook, Warning, TEXT("%s: could not take the old wrap out; left as it was"), *Name);
				++Failed;
				continue;
			}
			++Redone;
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
	UE_LOG(LogWetLook, Display, TEXT("WetLook%s: %d by the weather, %d by the wearer, %d redone, %d ground-style masked, %d done before, %d skipped, %d failed"),
		bListOnly ? TEXT(" (list only)") : TEXT(""), ByWeather, ByWearer, Redone, Masked, Done, Skipped, Failed);
	return Failed == 0 ? 0 : 1;
}
