#include "FightMaterialsCommandlet.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogFightMaterials, Log, All);

namespace
{
	const TCHAR* const FightMaterialsFolder = TEXT("/Game/Rim3D/Generated/Fights");
	const TCHAR* const WoundDecalName = TEXT("M_WoundDecal");

	void FightConnect(UMaterialExpression* From, const TCHAR* FromPin, UMaterialExpression* To, const TCHAR* ToPin)
	{
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromPin, To, ToPin))
		{
			UE_LOG(LogFightMaterials, Error, TEXT("Could not connect %s to %s pin '%s'"), *From->GetName(), *To->GetName(), ToPin);
		}
	}

	template <typename T>
	T* FightNode(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}
}

UFightMaterialsCommandlet::UFightMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UFightMaterialsCommandlet::Main(const FString& Params)
{
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(WoundDecalName, FightMaterialsFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
	if (!Material)
	{
		UE_LOG(LogFightMaterials, Error, TEXT("Could not create %s/%s"), FightMaterialsFolder, WoundDecalName);
		return 1;
	}
	Material->MaterialDomain = MD_DeferredDecal;
	Material->BlendMode = BLEND_Translucent;
	Material->DecalBlendMode = DBM_Translucent;
	Material->SetShadingModel(MSM_DefaultLit);

	// Colour: a vector parameter.
	auto* Color = FightNode<UMaterialExpressionVectorParameter>(Material, -900, -200);
	Color->ParameterName = TEXT("Color");
	Color->DefaultValue = FLinearColor(0.2f, 0.008f, 0.008f, 1.0f);
	UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT("RGB"), MP_BaseColor);

	auto* Roughness = FightNode<UMaterialExpressionConstant>(Material, -900, 0);
	Roughness->R = 0.3f;
	UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);

	// Opacity: 1 in the middle of the decal's picture, 0 at the edge, 1 - 2.1 x the distance from the middle, softened.
	auto* UV = FightNode<UMaterialExpressionTextureCoordinate>(Material, -1100, 200);
	auto* Middle = FightNode<UMaterialExpressionConstant2Vector>(Material, -1100, 350);
	Middle->R = 0.5f;
	Middle->G = 0.5f;
	auto* Distance = FightNode<UMaterialExpressionDistance>(Material, -900, 250);
	FightConnect(UV, TEXT(""), Distance, TEXT("A"));
	FightConnect(Middle, TEXT(""), Distance, TEXT("B"));
	auto* Widen = FightNode<UMaterialExpressionConstant>(Material, -900, 400);
	Widen->R = 2.1f;
	auto* Scaled = FightNode<UMaterialExpressionMultiply>(Material, -700, 250);
	FightConnect(Distance, TEXT(""), Scaled, TEXT("A"));
	FightConnect(Widen, TEXT(""), Scaled, TEXT("B"));
	auto* Inverse = FightNode<UMaterialExpressionOneMinus>(Material, -550, 250);
	FightConnect(Scaled, TEXT(""), Inverse, TEXT(""));
	auto* Clamp = FightNode<UMaterialExpressionSaturate>(Material, -400, 250);
	FightConnect(Inverse, TEXT(""), Clamp, TEXT(""));
	auto* Blob = FightNode<UMaterialExpressionPower>(Material, -250, 250);
	FightConnect(Clamp, TEXT(""), Blob, TEXT("Base"));
	Blob->ConstExponent = 0.4f;
	auto* Strength = FightNode<UMaterialExpressionScalarParameter>(Material, -250, 400);
	Strength->ParameterName = TEXT("Opacity");
	Strength->DefaultValue = 1.0f;
	auto* Opacity = FightNode<UMaterialExpressionMultiply>(Material, -100, 300);
	FightConnect(Blob, TEXT(""), Opacity, TEXT("A"));
	FightConnect(Strength, TEXT(""), Opacity, TEXT("B"));
	UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);

	UMaterialEditingLibrary::RecompileMaterial(Material);

	UPackage* Package = Material->GetOutermost();
	Package->MarkPackageDirty();
	const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Material, *File, Args);
	UE_LOG(LogFightMaterials, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
	return bSaved ? 0 : 1;
}
