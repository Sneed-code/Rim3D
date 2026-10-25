#include "BuildingMaterialsCommandlet.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
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
	UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("RGB"), MP_EmissiveColor);
	auto* Opacity = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -500, 200));
	Opacity->ParameterName = TEXT("Opacity");
	Opacity->DefaultValue = 0.38f;
	UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
	UMaterialEditingLibrary::RecompileMaterial(Material);

	UPackage* Package = Material->GetOutermost();
	Package->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Material, *File, Args);
	UE_LOG(LogBuildingMaterials, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
	return bSaved ? 0 : 1;
}
