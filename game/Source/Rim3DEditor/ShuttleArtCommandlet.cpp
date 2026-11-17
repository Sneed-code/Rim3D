#include "ShuttleArtCommandlet.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogShuttleArt, Log, All);

namespace
{
	const TCHAR* const ShuttleArtFolder = TEXT("/Game/Rim3D/Generated/Vehicles");

	bool ShuttleArtSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogShuttleArt, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	void ShuttleArtInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 Output = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(Output, From);
	}

	/** A fresh material (an older one of that name is emptied), for instanced meshes. */
	UMaterial* ShuttleArtMaterial(IAssetTools& Tools, const TCHAR* Name)
	{
		const FString Path = FString(ShuttleArtFolder) / Name;
		UMaterial* Material = LoadObject<UMaterial>(nullptr, *(Path + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (Material)
		{
			UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);
		}
		else
		{
			Material = Cast<UMaterial>(Tools.CreateAsset(Name, ShuttleArtFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		}
		if (Material)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes, bRecompile);
			Material->TwoSided = true;
		}
		return Material;
	}

	/** A custom node fed the vertex colour (C), per-instance custom data 0 (I), the normal (N) and the camera vector (V). */
	UMaterialExpressionCustom* ShuttleArtNode(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Output, const TCHAR* Code)
	{
		auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, 0));
		Custom->Description = Description;
		Custom->OutputType = Output;
		Custom->Code = Code;
		Custom->Inputs.Reset();
		ShuttleArtInput(Custom, TEXT("C"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexColor::StaticClass(), -800, 0));
		auto* Data = Cast<UMaterialExpressionPerInstanceCustomData>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionPerInstanceCustomData::StaticClass(), -800, 150));
		Data->DataIndex = 0;
		ShuttleArtInput(Custom, TEXT("I"), Data);
		ShuttleArtInput(Custom, TEXT("N"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -800, 300));
		ShuttleArtInput(Custom, TEXT("V"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCameraVectorWS::StaticClass(), -800, 400));
		return Custom;
	}
}

UShuttleArtCommandlet::UShuttleArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UShuttleArtCommandlet::Main(const FString& Params)
{
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	bool bOk = true;

	// The flame: glows, brightest where the cone faces the camera, gone at its silhouette.
	if (UMaterial* Flame = ShuttleArtMaterial(Tools, TEXT("M_JetFlame")))
	{
		Flame->SetShadingModel(MSM_Unlit);
		Flame->BlendMode = BLEND_Additive;
		auto* Glow = ShuttleArtNode(Flame, TEXT("JetFlame"), CMOT_Float3, TEXT(
			"float facing = saturate(abs(dot(normalize(N), normalize(V))));\n"
			"return C.rgb * max(I, 0.0) * Brightness * facing * facing;\n"));
		auto* Brightness = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Flame, UMaterialExpressionScalarParameter::StaticClass(), -800, 500));
		Brightness->ParameterName = TEXT("Brightness");
		Brightness->DefaultValue = 12.0f;
		ShuttleArtInput(Glow, TEXT("Brightness"), Brightness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Glow, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::RecompileMaterial(Flame);
		bOk = ShuttleArtSave(Flame) && bOk;
	}
	else
	{
		bOk = false;
	}

	// The dust: a soft, lit puff whose opacity the game sets per puff.
	if (UMaterial* Dust = ShuttleArtMaterial(Tools, TEXT("M_JetDust")))
	{
		Dust->SetShadingModel(MSM_DefaultLit);
		Dust->BlendMode = BLEND_Translucent;
		Dust->TranslucencyLightingMode = TLM_VolumetricNonDirectional;
		auto* Colour = ShuttleArtNode(Dust, TEXT("JetDustColour"), CMOT_Float3, TEXT("return C.rgb;\n"));
		auto* Opacity = ShuttleArtNode(Dust, TEXT("JetDustOpacity"), CMOT_Float1, TEXT(
			"float facing = saturate(abs(dot(normalize(N), normalize(V))));\n"
			"return saturate(I) * facing * facing;\n"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Colour, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
		UMaterialEditingLibrary::RecompileMaterial(Dust);
		bOk = ShuttleArtSave(Dust) && bOk;
	}
	else
	{
		bOk = false;
	}
	UE_LOG(LogShuttleArt, Display, TEXT("ShuttleArt %s"), bOk ? TEXT("done") : TEXT("FAILED"));
	return bOk ? 0 : 1;
}
