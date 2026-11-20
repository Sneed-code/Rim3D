#include "BuildingMaterialsCommandlet.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionEyeAdaptationInverse.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogBuildingMaterials, Log, All);

namespace
{
	const TCHAR* const BuildingMaterialsFolder = TEXT("/Game/Rim3D/Generated/Buildings");
	const TCHAR* const GhostName = TEXT("M_BuildingGhost");
	const TCHAR* const GlassName = TEXT("M_WindowGlass");
	const TCHAR* const DustName = TEXT("M_RoomDust");

	bool BldMatSave(UMaterial* Material)
	{
		UPackage* Package = Material->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Material, *File, Args);
		UE_LOG(LogBuildingMaterials, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	/**
	 * Window glass (docs/design/windows.md): lit, see-through, smooth and two-sided, for instanced meshes. Lit per pixel so
	 * the sky and the sun show on it; the panes cast no shadow (BuildingSubsystem), so the light comes in.
	 */
	bool BldMatMakeGlass(IAssetTools& Tools)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(GlassName, BuildingMaterialsFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			UE_LOG(LogBuildingMaterials, Error, TEXT("Could not create %s/%s"), BuildingMaterialsFolder, GlassName);
			return false;
		}
		Material->SetShadingModel(MSM_DefaultLit);
		Material->BlendMode = BLEND_Translucent;
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		Material->TwoSided = true;
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes, bRecompile);
		const auto Scalar = [Material](const TCHAR* Name, float Value, int32 Y, EMaterialProperty Property)
		{
			auto* Param = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, Y));
			Param->ParameterName = Name;
			Param->DefaultValue = Value;
			UMaterialEditingLibrary::ConnectMaterialProperty(Param, TEXT(""), Property);
		};
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -400, -200));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor(0.78f, 0.86f, 0.88f, 1.0f);
		UMaterialEditingLibrary::ConnectMaterialProperty(Tint, TEXT(""), MP_BaseColor);
		Scalar(TEXT("Metallic"), 0.0f, 0, MP_Metallic);
		Scalar(TEXT("Specular"), 0.5f, 100, MP_Specular);
		Scalar(TEXT("Roughness"), 0.04f, 200, MP_Roughness);
		Scalar(TEXT("Opacity"), 0.16f, 300, MP_Opacity);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return BldMatSave(Material);
	}

	/**
	 * Dust in a room's air (docs/design/windows.md "Drawing"): a volume material for a box that fills a room, so the volumetric
	 * fog has a little haze there and only there. The sun through a window lights it into a shaft; outdoors stays clear. In the
	 * volume domain the base colour is the albedo and the subsurface colour pin is the extinction (per metre of fog).
	 */
	bool BldMatMakeDust(IAssetTools& Tools)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(DustName, BuildingMaterialsFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			UE_LOG(LogBuildingMaterials, Error, TEXT("Could not create %s/%s"), BuildingMaterialsFolder, DustName);
			return false;
		}
		Material->MaterialDomain = MD_Volume;
		Material->BlendMode = BLEND_Additive;
		Material->SetShadingModel(MSM_Unlit);
		auto* Albedo = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -400, -100));
		Albedo->ParameterName = TEXT("Albedo");
		Albedo->DefaultValue = FLinearColor(0.85f, 0.82f, 0.76f, 1.0f);
		UMaterialEditingLibrary::ConnectMaterialProperty(Albedo, TEXT(""), MP_BaseColor);
		auto* Extinction = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, 100));
		Extinction->ParameterName = TEXT("Extinction");
		Extinction->DefaultValue = 0.25f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Extinction, TEXT(""), MP_SubsurfaceColor);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return BldMatSave(Material);
	}
}

UBuildingMaterialsCommandlet::UBuildingMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBuildingMaterialsCommandlet::Main(const FString& Params)
{
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	// Each material is made only if it isn't there yet, so running this again after a new one is added (window glass, room dust) keeps the rest.
	const bool bHaveGlass = LoadObject<UMaterial>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), BuildingMaterialsFolder, GlassName, GlassName)) != nullptr;
	const bool bHaveDust = LoadObject<UMaterial>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), BuildingMaterialsFolder, DustName, DustName)) != nullptr;
	const bool bGlassSaved = (bHaveGlass || BldMatMakeGlass(Tools)) && (bHaveDust || BldMatMakeDust(Tools));
	if (LoadObject<UMaterial>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), BuildingMaterialsFolder, GhostName, GhostName)) != nullptr)
	{
		UE_LOG(LogBuildingMaterials, Display, TEXT("%s/%s is there already"), BuildingMaterialsFolder, GhostName);
		return bGlassSaved ? 0 : 1;
	}
	UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(GhostName, BuildingMaterialsFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
	if (!Material)
	{
		UE_LOG(LogBuildingMaterials, Error, TEXT("Could not create %s/%s"), BuildingMaterialsFolder, GhostName);
		return 1;
	}
	Material->SetShadingModel(MSM_Unlit);
	Material->BlendMode = BLEND_Translucent;
	Material->TwoSided = true;
	bool bRecompile = false;
	UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes, bRecompile);

	auto* Color = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -500, 0));
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(0.18f, 0.42f, 0.95f, 1.0f);
	// The colour goes through the inverse of the camera's exposure, so a ghost looks the same at noon (EV 15) as at night (L1).
	auto* Unexposed = Cast<UMaterialExpressionEyeAdaptationInverse>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionEyeAdaptationInverse::StaticClass(), -250, 0));
	if (!UMaterialEditingLibrary::ConnectMaterialExpressions(Color, TEXT("RGB"), Unexposed, TEXT("LightValue")) &&
		!UMaterialEditingLibrary::ConnectMaterialExpressions(Color, TEXT("RGB"), Unexposed, TEXT("LightValueInput")))
	{
		Unexposed->LightValueInput.Connect(0, Color);
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Unexposed, TEXT(""), MP_EmissiveColor);
	auto* Opacity = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -500, 200));
	Opacity->ParameterName = TEXT("Opacity");
	Opacity->DefaultValue = 0.38f;
	UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
	UMaterialEditingLibrary::RecompileMaterial(Material);
	const bool bGhostSaved = BldMatSave(Material);
	return bGhostSaved && bGlassSaved ? 0 : 1;
}
