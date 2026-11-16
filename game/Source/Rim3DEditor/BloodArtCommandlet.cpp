#include "BloodArtCommandlet.h"

#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionDecalColor.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "BloodSplatAtlas.h"

DEFINE_LOG_CATEGORY_STATIC(LogBloodArt, Log, All);

namespace
{
	const TCHAR* const BloodArtFolder = TEXT("/Game/Rim3D/Generated/Blood");
	constexpr int32 BloodArtAtlasSize = 2048;

	void BloodArtConnect(UMaterialExpression* From, const TCHAR* FromPin, UMaterialExpression* To, const TCHAR* ToPin)
	{
		if (!From || !To || !UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromPin, To, ToPin))
		{
			UE_LOG(LogBloodArt, Error, TEXT("Could not connect %s to %s pin '%s'"), From ? *From->GetName() : TEXT("null"), To ? *To->GetName() : TEXT("null"), ToPin);
		}
	}

	template <typename T>
	T* BloodArtNode(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	bool BloodArtSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogBloodArt, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	/** True if the asset should be made: it is not there, or -force (then its file is deleted first). */
	bool BloodArtWanted(const TCHAR* Name, bool bForce)
	{
		const FString Package = FString(BloodArtFolder) / Name;
		if (!FPackageName::DoesPackageExist(Package))
		{
			return true;
		}
		if (!bForce)
		{
			UE_LOG(LogBloodArt, Display, TEXT("%s is there already (-force makes it again)"), *Package);
			return false;
		}
		const FString File = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*File, false, true);
		return true;
	}

	UTexture2D* BloodArtTexture(const TCHAR* Name, const TArray<uint8>& Bgra, int32 Size, bool bNormal)
	{
		UPackage* Package = CreatePackage(*(FString(BloodArtFolder) / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, Name, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Bgra.GetData());
		Texture->SRGB = false;
		Texture->CompressionSettings = bNormal ? TC_Normalmap : TC_Masks;
		Texture->LODGroup = bNormal ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->PostEditChange();
		return BloodArtSave(Texture) ? Texture : nullptr;
	}

	/** The atlas and its normal map, drawn from shapes. */
	void BloodArtMakeTextures(UTexture2D*& OutAtlas, UTexture2D*& OutNormal, bool bForce, int32& Failures)
	{
		const FString AtlasPath = FString(BloodArtFolder) / TEXT("T_BloodAtlas.T_BloodAtlas");
		const FString NormalPath = FString(BloodArtFolder) / TEXT("T_BloodAtlasN.T_BloodAtlasN");
		const bool bAtlas = BloodArtWanted(TEXT("T_BloodAtlas"), bForce);
		const bool bNormal = BloodArtWanted(TEXT("T_BloodAtlasN"), bForce);
		if (!bAtlas && !bNormal)
		{
			OutAtlas = LoadObject<UTexture2D>(nullptr, *AtlasPath);
			OutNormal = LoadObject<UTexture2D>(nullptr, *NormalPath);
			return;
		}
		const bloodatlas::Image Image = bloodatlas::MakeAtlas(BloodArtAtlasSize);
		const int32 Count = BloodArtAtlasSize * BloodArtAtlasSize;
		if (bAtlas)
		{
			TArray<uint8> Bgra;
			Bgra.SetNumUninitialized(Count * 4);
			for (int32 I = 0; I < Count; ++I)
			{
				const bloodatlas::Pixel& P = Image.Pixels[I];
				Bgra[I * 4 + 0] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.Core * 255.0f), 0, 255));
				Bgra[I * 4 + 1] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.Thickness * 255.0f), 0, 255));
				Bgra[I * 4 + 2] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(P.Coverage * 255.0f), 0, 255));
				Bgra[I * 4 + 3] = 255;
			}
			OutAtlas = BloodArtTexture(TEXT("T_BloodAtlas"), Bgra, BloodArtAtlasSize, false);
			Failures += OutAtlas ? 0 : 1;
		}
		else
		{
			OutAtlas = LoadObject<UTexture2D>(nullptr, *AtlasPath);
		}
		if (bNormal)
		{
			TArray<uint8> Bgra;
			Bgra.SetNumUninitialized(Count * 4);
			const float Strength = BloodArtAtlasSize / 512.0f * 3.0f;
			for (int32 Y = 0; Y < BloodArtAtlasSize; ++Y)
			{
				for (int32 X = 0; X < BloodArtAtlasSize; ++X)
				{
					float Nx = 0.5f;
					float Ny = 0.5f;
					float Nz = 1.0f;
					bloodatlas::NormalAt(Image, X, Y, Strength, Nx, Ny, Nz);
					const int32 I = Y * BloodArtAtlasSize + X;
					Bgra[I * 4 + 0] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Nz * 255.0f), 0, 255));
					Bgra[I * 4 + 1] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Ny * 255.0f), 0, 255));
					Bgra[I * 4 + 2] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Nx * 255.0f), 0, 255));
					Bgra[I * 4 + 3] = 255;
				}
			}
			OutNormal = BloodArtTexture(TEXT("T_BloodAtlasN"), Bgra, BloodArtAtlasSize, true);
			Failures += OutNormal ? 0 : 1;
		}
		else
		{
			OutNormal = LoadObject<UTexture2D>(nullptr, *NormalPath);
		}
	}

	UMaterial* BloodArtNewMaterial(IAssetTools& Tools, const TCHAR* Name, int32& Failures)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, BloodArtFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			UE_LOG(LogBloodArt, Error, TEXT("Could not create %s/%s"), BloodArtFolder, Name);
			++Failures;
		}
		return Material;
	}

	UMaterialExpressionScalarParameter* BloodArtScalar(UMaterial* Material, const TCHAR* Name, float Default, int32 X, int32 Y)
	{
		auto* Node = BloodArtNode<UMaterialExpressionScalarParameter>(Material, X, Y);
		Node->ParameterName = Name;
		Node->DefaultValue = Default;
		return Node;
	}

	UMaterialExpressionLinearInterpolate* BloodArtLerp(UMaterial* Material, int32 X, int32 Y, UMaterialExpression* A, const TCHAR* APin, UMaterialExpression* B,
		const TCHAR* BPin, UMaterialExpression* Alpha, const TCHAR* AlphaPin)
	{
		auto* Node = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, X, Y);
		if (A) BloodArtConnect(A, APin, Node, TEXT("A"));
		if (B) BloodArtConnect(B, BPin, Node, TEXT("B"));
		if (Alpha) BloodArtConnect(Alpha, AlphaPin, Node, TEXT("Alpha"));
		return Node;
	}

	UMaterialExpressionComponentMask* BloodArtMask(UMaterial* Material, int32 X, int32 Y, UMaterialExpression* From, const TCHAR* Pin, bool R, bool G, bool B, bool A)
	{
		auto* Node = BloodArtNode<UMaterialExpressionComponentMask>(Material, X, Y);
		Node->R = R ? 1 : 0;
		Node->G = G ? 1 : 0;
		Node->B = B ? 1 : 0;
		Node->A = A ? 1 : 0;
		BloodArtConnect(From, Pin, Node, TEXT(""));
		return Node;
	}

	/**
	 * M_BloodSplatter, the stain decal. UV: the decal's, turned and mirrored by SwapUV / FlipU / FlipV (0 or 1), then into
	 * the atlas cell (CellX, CellY). Base colour: the decal colour, darker where thick and much darker in a wound's core.
	 * Wetness (decal alpha's floor / 62) makes it glossy and bumpy; opacity (alpha's frac) times the coverage.
	 */
	void BloodArtMakeSplatter(IAssetTools& Tools, UTexture2D* Atlas, UTexture2D* Normal, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_BloodSplatter"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_DeferredDecal;
		Material->BlendMode = BLEND_Translucent;
		Material->DecalBlendMode = DBM_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);

		// UV.
		auto* UV = BloodArtNode<UMaterialExpressionTextureCoordinate>(Material, -2600, 0);
		auto* U = BloodArtMask(Material, -2400, -60, UV, TEXT(""), true, false, false, false);
		auto* V = BloodArtMask(Material, -2400, 60, UV, TEXT(""), false, true, false, false);
		auto* Swapped = BloodArtNode<UMaterialExpressionAppendVector>(Material, -2250, 0);
		BloodArtConnect(V, TEXT(""), Swapped, TEXT("A"));
		BloodArtConnect(U, TEXT(""), Swapped, TEXT("B"));
		auto* Swap = BloodArtScalar(Material, TEXT("SwapUV"), 0.0f, -2250, 150);
		auto* Turned = BloodArtLerp(Material, -2100, 0, UV, TEXT(""), Swapped, TEXT(""), Swap, TEXT(""));
		auto* FlipU = BloodArtScalar(Material, TEXT("FlipU"), 0.0f, -2100, 200);
		auto* FlipV = BloodArtScalar(Material, TEXT("FlipV"), 0.0f, -2100, 300);
		auto* Flips = BloodArtNode<UMaterialExpressionAppendVector>(Material, -1950, 250);
		BloodArtConnect(FlipU, TEXT(""), Flips, TEXT("A"));
		BloodArtConnect(FlipV, TEXT(""), Flips, TEXT("B"));
		auto* Mirrored = BloodArtNode<UMaterialExpressionOneMinus>(Material, -1950, 100);
		BloodArtConnect(Turned, TEXT(""), Mirrored, TEXT(""));
		auto* Facing = BloodArtLerp(Material, -1800, 0, Turned, TEXT(""), Mirrored, TEXT(""), Flips, TEXT(""));
		// Into the cell, keeping a sliver of margin so mips do not bleed in from the next picture.
		auto* Shrunk = BloodArtNode<UMaterialExpressionMultiply>(Material, -1650, 0);
		BloodArtConnect(Facing, TEXT(""), Shrunk, TEXT("A"));
		Shrunk->ConstB = 0.25f * 0.98f;
		auto* CellX = BloodArtScalar(Material, TEXT("CellX"), 0.0f, -1800, 250);
		auto* CellY = BloodArtScalar(Material, TEXT("CellY"), 0.0f, -1800, 350);
		auto* CellXY = BloodArtNode<UMaterialExpressionAppendVector>(Material, -1650, 300);
		BloodArtConnect(CellX, TEXT(""), CellXY, TEXT("A"));
		BloodArtConnect(CellY, TEXT(""), CellXY, TEXT("B"));
		auto* CellOrigin = BloodArtNode<UMaterialExpressionMultiply>(Material, -1500, 300);
		BloodArtConnect(CellXY, TEXT(""), CellOrigin, TEXT("A"));
		CellOrigin->ConstB = 0.25f;
		auto* Margin = BloodArtNode<UMaterialExpressionAdd>(Material, -1350, 300);
		BloodArtConnect(CellOrigin, TEXT(""), Margin, TEXT("A"));
		Margin->ConstB = 0.25f * 0.01f;
		auto* AtlasUV = BloodArtNode<UMaterialExpressionAdd>(Material, -1350, 0);
		BloodArtConnect(Shrunk, TEXT(""), AtlasUV, TEXT("A"));
		BloodArtConnect(Margin, TEXT(""), AtlasUV, TEXT("B"));

		auto* Sample = BloodArtNode<UMaterialExpressionTextureSample>(Material, -1150, -100);
		Sample->Texture = Atlas;
		Sample->SamplerType = SAMPLERTYPE_Masks;
		BloodArtConnect(AtlasUV, TEXT(""), Sample, TEXT("UVs"));
		auto* NormalSample = BloodArtNode<UMaterialExpressionTextureSample>(Material, -1150, 250);
		NormalSample->Texture = Normal;
		NormalSample->SamplerType = SAMPLERTYPE_Normal;
		BloodArtConnect(AtlasUV, TEXT(""), NormalSample, TEXT("UVs"));

		// The decal colour: RGB, and alpha = 2 x round(wet x 31) + opacity.
		auto* DecalColor = BloodArtNode<UMaterialExpressionDecalColor>(Material, -1150, -450);
		auto* Rgb = BloodArtMask(Material, -950, -500, DecalColor, TEXT(""), true, true, true, false);
		auto* Alpha = BloodArtMask(Material, -950, -400, DecalColor, TEXT(""), false, false, false, true);
		auto* Opacity = BloodArtNode<UMaterialExpressionFrac>(Material, -800, -350);
		BloodArtConnect(Alpha, TEXT(""), Opacity, TEXT(""));
		auto* WetSteps = BloodArtNode<UMaterialExpressionFloor>(Material, -800, -450);
		BloodArtConnect(Alpha, TEXT(""), WetSteps, TEXT(""));
		auto* Wet = BloodArtNode<UMaterialExpressionMultiply>(Material, -650, -450);
		BloodArtConnect(WetSteps, TEXT(""), Wet, TEXT("A"));
		Wet->ConstB = 1.0f / 62.0f;

		// Base colour: thick blood darker, a wound's core near black.
		auto* ThickShade = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -800, -200);
		ThickShade->ConstA = 1.25f;
		ThickShade->ConstB = 0.6f;
		BloodArtConnect(Sample, TEXT("G"), ThickShade, TEXT("Alpha"));
		auto* CoreShade = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -800, -100);
		CoreShade->ConstA = 1.0f;
		CoreShade->ConstB = 0.22f;
		BloodArtConnect(Sample, TEXT("B"), CoreShade, TEXT("Alpha"));
		auto* Shade = BloodArtNode<UMaterialExpressionMultiply>(Material, -650, -150);
		BloodArtConnect(ThickShade, TEXT(""), Shade, TEXT("A"));
		BloodArtConnect(CoreShade, TEXT(""), Shade, TEXT("B"));
		auto* Base = BloodArtNode<UMaterialExpressionMultiply>(Material, -500, -300);
		BloodArtConnect(Rgb, TEXT(""), Base, TEXT("A"));
		BloodArtConnect(Shade, TEXT(""), Base, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);

		// Roughness: matte when dry; wet blood is a mirror where it is thick.
		auto* WetRough = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 0);
		WetRough->ConstA = 0.22f;
		WetRough->ConstB = 0.04f;
		BloodArtConnect(Sample, TEXT("G"), WetRough, TEXT("Alpha"));
		auto* DryRough = BloodArtNode<UMaterialExpressionConstant>(Material, -650, 100);
		DryRough->R = 0.72f;
		auto* Roughness = BloodArtLerp(Material, -450, 50, DryRough, TEXT(""), WetRough, TEXT(""), Wet, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		auto* Specular = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -450, 150);
		Specular->ConstA = 0.3f;
		Specular->ConstB = 0.65f;
		BloodArtConnect(Wet, TEXT(""), Specular, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);

		// Normal: the bumps of the blood, flatter once dry.
		auto* Flat = BloodArtNode<UMaterialExpressionConstant3Vector>(Material, -800, 250);
		Flat->Constant = FLinearColor(0.0f, 0.0f, 1.0f);
		auto* Bump = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 350);
		Bump->ConstA = 0.35f;
		Bump->ConstB = 1.0f;
		BloodArtConnect(Wet, TEXT(""), Bump, TEXT("Alpha"));
		auto* NormalOut = BloodArtLerp(Material, -450, 300, Flat, TEXT(""), NormalSample, TEXT("RGB"), Bump, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalOut, TEXT(""), MP_Normal);

		auto* Covered = BloodArtNode<UMaterialExpressionMultiply>(Material, -450, 450);
		BloodArtConnect(Sample, TEXT("R"), Covered, TEXT("A"));
		BloodArtConnect(Opacity, TEXT(""), Covered, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Covered, TEXT(""), MP_Opacity);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	/** RGB from per-instance custom data 0, 1, 2. */
	UMaterialExpression* BloodArtInstanceColor(UMaterial* Material, int32 X, int32 Y)
	{
		UMaterialExpressionPerInstanceCustomData* Channels[3];
		for (int32 I = 0; I < 3; ++I)
		{
			Channels[I] = BloodArtNode<UMaterialExpressionPerInstanceCustomData>(Material, X, Y + I * 100);
			Channels[I]->DataIndex = I;
			Channels[I]->ConstDefaultValue = I == 0 ? 0.3f : 0.01f;
		}
		auto* RG = BloodArtNode<UMaterialExpressionAppendVector>(Material, X + 200, Y + 50);
		BloodArtConnect(Channels[0], TEXT(""), RG, TEXT("A"));
		BloodArtConnect(Channels[1], TEXT(""), RG, TEXT("B"));
		auto* RGB = BloodArtNode<UMaterialExpressionAppendVector>(Material, X + 350, Y + 100);
		BloodArtConnect(RG, TEXT(""), RGB, TEXT("A"));
		BloodArtConnect(Channels[2], TEXT(""), RGB, TEXT("B"));
		return RGB;
	}

	/** M_BloodDrop: an opaque, glossy wet drop in its instance's colour. */
	void BloodArtMakeDrop(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_BloodDrop"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Opaque;
		Material->SetShadingModel(MSM_DefaultLit);
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
		UMaterialExpression* Color = BloodArtInstanceColor(Material, -900, -200);
		UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT(""), MP_BaseColor);
		auto* Roughness = BloodArtNode<UMaterialExpressionConstant>(Material, -400, 100);
		Roughness->R = 0.06f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		auto* Specular = BloodArtNode<UMaterialExpressionConstant>(Material, -400, 200);
		Specular->R = 0.7f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	/** M_BloodMist: a soft round puff in its instance's colour; opacity from instance data 3, faded where it meets a surface. */
	void BloodArtMakeMist(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_BloodMist"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TwoSided = true;
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
		UMaterialExpression* Color = BloodArtInstanceColor(Material, -1100, -300);
		UMaterialEditingLibrary::ConnectMaterialProperty(Color, TEXT(""), MP_BaseColor);
		auto* Roughness = BloodArtNode<UMaterialExpressionConstant>(Material, -400, -100);
		Roughness->R = 0.6f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);

		// Round and soft: (1 - 2 x distance from the middle) ^ 1.6.
		auto* UV = BloodArtNode<UMaterialExpressionTextureCoordinate>(Material, -1300, 200);
		auto* Middle = BloodArtNode<UMaterialExpressionConstant2Vector>(Material, -1300, 320);
		Middle->R = 0.5f;
		Middle->G = 0.5f;
		auto* Distance = BloodArtNode<UMaterialExpressionDistance>(Material, -1150, 250);
		BloodArtConnect(UV, TEXT(""), Distance, TEXT("A"));
		BloodArtConnect(Middle, TEXT(""), Distance, TEXT("B"));
		auto* Wide = BloodArtNode<UMaterialExpressionMultiply>(Material, -1000, 250);
		BloodArtConnect(Distance, TEXT(""), Wide, TEXT("A"));
		Wide->ConstB = 2.0f;
		auto* Inside = BloodArtNode<UMaterialExpressionOneMinus>(Material, -850, 250);
		BloodArtConnect(Wide, TEXT(""), Inside, TEXT(""));
		auto* Clamped = BloodArtNode<UMaterialExpressionSaturate>(Material, -700, 250);
		BloodArtConnect(Inside, TEXT(""), Clamped, TEXT(""));
		auto* Soft = BloodArtNode<UMaterialExpressionPower>(Material, -550, 250);
		BloodArtConnect(Clamped, TEXT(""), Soft, TEXT("Base"));
		Soft->ConstExponent = 1.6f;
		auto* InstanceOpacity = BloodArtNode<UMaterialExpressionPerInstanceCustomData>(Material, -700, 400);
		InstanceOpacity->DataIndex = 3;
		InstanceOpacity->ConstDefaultValue = 0.5f;
		auto* Opacity = BloodArtNode<UMaterialExpressionMultiply>(Material, -400, 300);
		BloodArtConnect(Soft, TEXT(""), Opacity, TEXT("A"));
		BloodArtConnect(InstanceOpacity, TEXT(""), Opacity, TEXT("B"));
		auto* Fade = BloodArtNode<UMaterialExpressionDepthFade>(Material, -250, 300);
		Fade->FadeDistanceDefault = 20.0f;
		BloodArtConnect(Opacity, TEXT(""), Fade, TEXT("Opacity"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Fade, TEXT(""), MP_Opacity);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Failures += BloodArtSave(Material) ? 0 : 1;
	}
}

UBloodArtCommandlet::UBloodArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBloodArtCommandlet::Main(const FString& Params)
{
	const bool bForce = FParse::Param(*Params, TEXT("force"));
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Failures = 0;
	UTexture2D* Atlas = nullptr;
	UTexture2D* Normal = nullptr;
	BloodArtMakeTextures(Atlas, Normal, bForce, Failures);
	if (!Atlas || !Normal)
	{
		UE_LOG(LogBloodArt, Error, TEXT("The blood atlas could not be made or loaded"));
		return 1;
	}
	if (BloodArtWanted(TEXT("M_BloodSplatter"), bForce))
	{
		BloodArtMakeSplatter(Tools, Atlas, Normal, Failures);
	}
	if (BloodArtWanted(TEXT("M_BloodDrop"), bForce))
	{
		BloodArtMakeDrop(Tools, Failures);
	}
	if (BloodArtWanted(TEXT("M_BloodMist"), bForce))
	{
		BloodArtMakeMist(Tools, Failures);
	}
	UE_LOG(LogBloodArt, Display, TEXT("Blood art done, %d failures"), Failures);
	return Failures == 0 ? 0 : 1;
}
