#include "FightMaterialsCommandlet.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
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
	const TCHAR* const BloodSplatName = TEXT("M_BloodSplat");

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

	/** A new translucent deferred decal material, or null if it is there already (delete the .uasset to make it again). */
	UMaterial* FightNewDecalMaterial(IAssetTools& Tools, const TCHAR* Name, int32& Failures)
	{
		const FString Package = FString(FightMaterialsFolder) / Name;
		if (FPackageName::DoesPackageExist(Package))
		{
			UE_LOG(LogFightMaterials, Display, TEXT("%s is there already; delete it to make it again"), *Package);
			return nullptr;
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, FightMaterialsFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			UE_LOG(LogFightMaterials, Error, TEXT("Could not create %s"), *Package);
			++Failures;
			return nullptr;
		}
		Material->MaterialDomain = MD_DeferredDecal;
		Material->BlendMode = BLEND_Translucent;
		Material->DecalBlendMode = DBM_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);
		return Material;
	}

	bool FightSave(UMaterial* Material)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
		UPackage* Package = Material->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Material, *File, Args);
		UE_LOG(LogFightMaterials, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	/** M_WoundDecal: a round soft-edged blob of the `Color` parameter (wounds and pools from gunfire, docs/design/fights.md). */
	void FightMakeWoundDecal(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = FightNewDecalMaterial(Tools, WoundDecalName, Failures);
		if (!Material)
		{
			return;
		}
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
		Failures += FightSave(Material) ? 0 : 1;
	}

	/**
	 * M_BloodSplat (the creator's fight demo, docs/design/creator-weapons.md): a splash of blood with a ragged, sharp
	 * edge and small drops thrown off around it, different for every `Seed`. `Color` and `Roughness` are set from how
	 * dry it is (wet: redder and glossy; dry: near black and dull); `Opacity` multiplies it.
	 */
	void FightMakeBloodSplat(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = FightNewDecalMaterial(Tools, BloodSplatName, Failures);
		if (!Material)
		{
			return;
		}
		auto* UV = FightNode<UMaterialExpressionTextureCoordinate>(Material, -1600, 200);
		auto* Seed = FightNode<UMaterialExpressionScalarParameter>(Material, -1600, 400);
		Seed->ParameterName = TEXT("Seed");
		Seed->DefaultValue = 0.0f;

		// Two noises over the picture, moved through the noise's third axis by the seed: a coarse one for the edge, a
		// fine one for the drops.
		const auto MakeNoise = [&](float Frequency, int32 Y, int32 Levels)
		{
			auto* Scaled = FightNode<UMaterialExpressionMultiply>(Material, -1400, Y);
			FightConnect(UV, TEXT(""), Scaled, TEXT("A"));
			Scaled->ConstB = Frequency;
			auto* Position = FightNode<UMaterialExpressionAppendVector>(Material, -1250, Y);
			FightConnect(Scaled, TEXT(""), Position, TEXT("A"));
			FightConnect(Seed, TEXT(""), Position, TEXT("B"));
			auto* Noise = FightNode<UMaterialExpressionNoise>(Material, -1100, Y);
			Noise->Scale = 1.0f;
			Noise->Levels = Levels;
			Noise->OutputMin = 0.0f;
			Noise->OutputMax = 1.0f;
			Noise->bTurbulence = false;
			FightConnect(Position, TEXT(""), Noise, TEXT(""));  // the first input (Position)
			return Noise;
		};
		UMaterialExpressionNoise* Coarse = MakeNoise(5.0f, 100, 4);
		UMaterialExpressionNoise* Fine = MakeNoise(24.0f, 500, 2);

		// The splash: distance from the middle, pushed in and out by the coarse noise, cut sharp.
		auto* Middle = FightNode<UMaterialExpressionConstant2Vector>(Material, -1400, 0);
		Middle->R = 0.5f;
		Middle->G = 0.5f;
		auto* Distance = FightNode<UMaterialExpressionDistance>(Material, -1250, -50);
		FightConnect(UV, TEXT(""), Distance, TEXT("A"));
		FightConnect(Middle, TEXT(""), Distance, TEXT("B"));
		auto* Wide = FightNode<UMaterialExpressionMultiply>(Material, -1100, -50);
		FightConnect(Distance, TEXT(""), Wide, TEXT("A"));
		Wide->ConstB = 2.1f;
		auto* Wobble = FightNode<UMaterialExpressionSubtract>(Material, -950, 100);
		FightConnect(Coarse, TEXT(""), Wobble, TEXT("A"));
		Wobble->ConstB = 0.5f;
		auto* WobbleScaled = FightNode<UMaterialExpressionMultiply>(Material, -800, 100);
		FightConnect(Wobble, TEXT(""), WobbleScaled, TEXT("A"));
		WobbleScaled->ConstB = 0.6f;
		auto* Edge = FightNode<UMaterialExpressionSubtract>(Material, -650, 0);
		FightConnect(Wide, TEXT(""), Edge, TEXT("A"));
		FightConnect(WobbleScaled, TEXT(""), Edge, TEXT("B"));
		auto* Inside = FightNode<UMaterialExpressionOneMinus>(Material, -500, 0);
		FightConnect(Edge, TEXT(""), Inside, TEXT(""));
		auto* Sharp = FightNode<UMaterialExpressionMultiply>(Material, -350, 0);
		FightConnect(Inside, TEXT(""), Sharp, TEXT("A"));
		Sharp->ConstB = 8.0f;
		auto* Splash = FightNode<UMaterialExpressionSaturate>(Material, -200, 0);
		FightConnect(Sharp, TEXT(""), Splash, TEXT(""));

		// The drops: where the fine noise peaks, inside the decal's circle.
		auto* Peak = FightNode<UMaterialExpressionSubtract>(Material, -950, 500);
		FightConnect(Fine, TEXT(""), Peak, TEXT("A"));
		Peak->ConstB = 0.74f;
		auto* PeakSharp = FightNode<UMaterialExpressionMultiply>(Material, -800, 500);
		FightConnect(Peak, TEXT(""), PeakSharp, TEXT("A"));
		PeakSharp->ConstB = 14.0f;
		auto* Drops = FightNode<UMaterialExpressionSaturate>(Material, -650, 500);
		FightConnect(PeakSharp, TEXT(""), Drops, TEXT(""));
		auto* InCircle = FightNode<UMaterialExpressionSubtract>(Material, -950, 350);
		InCircle->ConstA = 0.48f;
		FightConnect(Distance, TEXT(""), InCircle, TEXT("B"));
		auto* InCircleSharp = FightNode<UMaterialExpressionMultiply>(Material, -800, 350);
		FightConnect(InCircle, TEXT(""), InCircleSharp, TEXT("A"));
		InCircleSharp->ConstB = 20.0f;
		auto* Circle = FightNode<UMaterialExpressionSaturate>(Material, -650, 350);
		FightConnect(InCircleSharp, TEXT(""), Circle, TEXT(""));
		auto* DropsIn = FightNode<UMaterialExpressionMultiply>(Material, -500, 450);
		FightConnect(Drops, TEXT(""), DropsIn, TEXT("A"));
		FightConnect(Circle, TEXT(""), DropsIn, TEXT("B"));

		auto* Shape = FightNode<UMaterialExpressionMax>(Material, -50, 200);
		FightConnect(Splash, TEXT(""), Shape, TEXT("A"));
		FightConnect(DropsIn, TEXT(""), Shape, TEXT("B"));
		auto* Strength = FightNode<UMaterialExpressionScalarParameter>(Material, -50, 400);
		Strength->ParameterName = TEXT("Opacity");
		Strength->DefaultValue = 1.0f;
		auto* Opacity = FightNode<UMaterialExpressionMultiply>(Material, 100, 300);
		FightConnect(Shape, TEXT(""), Opacity, TEXT("A"));
		FightConnect(Strength, TEXT(""), Opacity, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);

		// Colour: the parameter, a little uneven (thicker blood darker).
		auto* Color = FightNode<UMaterialExpressionVectorParameter>(Material, -500, -350);
		Color->ParameterName = TEXT("Color");
		Color->DefaultValue = FLinearColor(0.28f, 0.01f, 0.008f, 1.0f);
		auto* Shade = FightNode<UMaterialExpressionMultiply>(Material, -650, -200);
		FightConnect(Coarse, TEXT(""), Shade, TEXT("A"));
		Shade->ConstB = 0.35f;
		auto* ShadeOffset = FightNode<UMaterialExpressionAdd>(Material, -500, -200);
		FightConnect(Shade, TEXT(""), ShadeOffset, TEXT("A"));
		ShadeOffset->ConstB = 0.8f;
		auto* Tinted = FightNode<UMaterialExpressionMultiply>(Material, -300, -300);
		FightConnect(Color, TEXT("RGB"), Tinted, TEXT("A"));
		FightConnect(ShadeOffset, TEXT(""), Tinted, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Tinted, TEXT(""), MP_BaseColor);

		auto* Roughness = FightNode<UMaterialExpressionScalarParameter>(Material, -300, -100);
		Roughness->ParameterName = TEXT("Roughness");
		Roughness->DefaultValue = 0.15f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		Failures += FightSave(Material) ? 0 : 1;
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
	int32 Failures = 0;
	FightMakeWoundDecal(Tools, Failures);
	FightMakeBloodSplat(Tools, Failures);
	return Failures == 0 ? 0 : 1;
}
