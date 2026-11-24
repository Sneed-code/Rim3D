#include "BloodArtCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
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
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDecalColor.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionDistance.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNoise.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionPreSkinnedPosition.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "BloodSplatAtlas.h"
#include "MakeHumanArt.h"
#include "SkinBloodArt.h"
#include "SkinBloodMaterial.h"

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
		// UE 5.8: the node's first pin is "RGB" (3 channels); alpha has its own "A" pin.
		auto* Rgb = BloodArtMask(Material, -950, -500, DecalColor, TEXT("RGB"), true, true, true, false);
		auto* Alpha = BloodArtMask(Material, -950, -400, DecalColor, TEXT("A"), true, false, false, false);
		auto* Opacity = BloodArtNode<UMaterialExpressionFrac>(Material, -800, -350);
		BloodArtConnect(Alpha, TEXT(""), Opacity, TEXT(""));
		auto* WetSteps = BloodArtNode<UMaterialExpressionFloor>(Material, -800, -450);
		BloodArtConnect(Alpha, TEXT(""), WetSteps, TEXT(""));
		auto* Wet = BloodArtNode<UMaterialExpressionMultiply>(Material, -650, -450);
		BloodArtConnect(WetSteps, TEXT(""), Wet, TEXT("A"));
		Wet->ConstB = 1.0f / 62.0f;

		// Base colour: thick blood darker, a wound's core near black.
		auto* ThickShade = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -800, -200);
		ThickShade->ConstA = 0.95f;  // (PC look, 2026-11-18: 1.25 to 0.6 read as bright paint, 0.95 to 0.4 as black on skin)
		ThickShade->ConstB = 0.5f;
		BloodArtConnect(Sample, TEXT("G"), ThickShade, TEXT("Alpha"));
		auto* CoreShade = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -800, -100);
		CoreShade->ConstA = 1.0f;
		CoreShade->ConstB = 0.22f;
		BloodArtConnect(Sample, TEXT("B"), CoreShade, TEXT("Alpha"));
		auto* Shade = BloodArtNode<UMaterialExpressionMultiply>(Material, -650, -150);
		BloodArtConnect(ThickShade, TEXT(""), Shade, TEXT("A"));
		BloodArtConnect(CoreShade, TEXT(""), Shade, TEXT("B"));
		auto* Shaded = BloodArtNode<UMaterialExpressionMultiply>(Material, -500, -300);
		BloodArtConnect(Rgb, TEXT(""), Shaded, TEXT("A"));
		BloodArtConnect(Shade, TEXT(""), Shaded, TEXT("B"));
		// WorldTint: white on bodies; on the ground and walls UBloodSubsystem sets it darker and redder (Blood.GroundShade),
		// since the shared colour keeps a little green and blue for shaded faces, and in sun that read as flat pink paint
		// (PC look, 2026-11-20).
		auto* WorldTint = BloodArtNode<UMaterialExpressionVectorParameter>(Material, -650, -550);
		WorldTint->ParameterName = TEXT("WorldTint");
		WorldTint->DefaultValue = FLinearColor::White;
		auto* WorldRgb = BloodArtMask(Material, -500, -550, WorldTint, TEXT(""), true, true, true, false);
		auto* Base = BloodArtNode<UMaterialExpressionMultiply>(Material, -350, -300);
		BloodArtConnect(Shaded, TEXT(""), Base, TEXT("A"));
		BloodArtConnect(WorldRgb, TEXT(""), Base, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);

		// Roughness: matte when dry; wet blood shines where it is thick, but soaks into the ground: not a mirror
		// (PC look, 2026-11-18: 0.04 made pools a glossy sheet of paint).
		auto* WetRough = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 0);
		WetRough->ConstA = 0.62f;  // (close-ups of pools, 2026-11-18: 0.5 to 0.22 still shone like plastic)
		WetRough->ConstB = 0.42f;
		BloodArtConnect(Sample, TEXT("G"), WetRough, TEXT("Alpha"));
		auto* DryRough = BloodArtNode<UMaterialExpressionConstant>(Material, -650, 100);
		DryRough->R = 0.72f;
		auto* Roughness = BloodArtLerp(Material, -450, 50, DryRough, TEXT(""), WetRough, TEXT(""), Wet, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		auto* Specular = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -450, 150);
		Specular->ConstA = 0.3f;
		Specular->ConstB = 0.38f;
		BloodArtConnect(Wet, TEXT(""), Specular, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);

		// Normal: the bumps of the blood, flatter once dry.
		auto* Flat = BloodArtNode<UMaterialExpressionConstant3Vector>(Material, -800, 250);
		Flat->Constant = FLinearColor(0.0f, 0.0f, 1.0f);
		auto* Bump = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 350);
		Bump->ConstA = 0.2f;
		Bump->ConstB = 0.35f;  // (at 1 round drops read as raised beads)
		BloodArtConnect(Wet, TEXT(""), Bump, TEXT("Alpha"));
		auto* NormalOut = BloodArtLerp(Material, -450, 300, Flat, TEXT(""), NormalSample, TEXT("RGB"), Bump, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalOut, TEXT(""), MP_Normal);

		auto* Covered = BloodArtNode<UMaterialExpressionMultiply>(Material, -450, 450);
		// Thin edges and fine spatter count for more: at their own strength they vanished into brown soil from a few metres.
		// The faintest part is cut first: boosted too, it drew the picture's square edge.
		auto* Cut = BloodArtNode<UMaterialExpressionAdd>(Material, -900, 450);
		BloodArtConnect(Sample, TEXT("R"), Cut, TEXT("A"));
		Cut->ConstB = -0.06f;
		auto* Boosted = BloodArtNode<UMaterialExpressionMultiply>(Material, -750, 450);
		BloodArtConnect(Cut, TEXT(""), Boosted, TEXT("A"));
		Boosted->ConstB = 1.8f;
		auto* Solid = BloodArtNode<UMaterialExpressionSaturate>(Material, -600, 450);
		BloodArtConnect(Boosted, TEXT(""), Solid, TEXT(""));
		BloodArtConnect(Solid, TEXT(""), Covered, TEXT("A"));
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

	/**
	 * M_Gore, the cut flesh of a stump (GoreStump.cpp, docs/design/body-health.md "Gore"): colour from the mesh's vertex
	 * colours (skin rim, fat, muscle, bone, marrow), wetness from their alpha (0 wet and glossy, 1 dry bone), and a fine
	 * noise bump so muscle doesn't read as plastic.
	 */
	void BloodArtMakeGore(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_Gore"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Opaque;
		Material->SetShadingModel(MSM_DefaultLit);
		auto* Vertex = BloodArtNode<UMaterialExpressionVertexColor>(Material, -1000, -200);
		UMaterialEditingLibrary::ConnectMaterialProperty(Vertex, TEXT(""), MP_BaseColor);
		auto* Wet = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 50);
		Wet->R = 0.32f;
		auto* Dry = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 150);
		Dry->R = 0.6f;
		auto* Rough = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -500, 100);
		BloodArtConnect(Wet, TEXT(""), Rough, TEXT("A"));
		BloodArtConnect(Dry, TEXT(""), Rough, TEXT("B"));
		BloodArtConnect(Vertex, TEXT("A"), Rough, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		auto* Specular = BloodArtNode<UMaterialExpressionConstant>(Material, -400, 250);
		Specular->R = 0.35f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);
		// A fine bump: the slope of a small noise in the mesh's own space (local position, cm).
		auto* Bump = BloodArtNode<UMaterialExpressionCustom>(Material, -500, 400);
		Bump->OutputType = CMOT_Float3;
		Bump->Description = TEXT("GoreBump");
		Bump->Code = TEXT("float3 p = LocalPos * 1.7;\n"
			"float3 i = floor(p); float3 f = frac(p);\n"
			"float a = frac(sin(dot(i, float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"float b = frac(sin(dot(i + float3(1, 0, 0), float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"float c = frac(sin(dot(i + float3(0, 1, 0), float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"float2 g = float2(lerp(a, b, f.x) - a, lerp(a, c, f.y) - a) * 0.35 * (1.0 - Dryness);\n"
			"return normalize(float3(g, 1.0));");
		Bump->Inputs.SetNum(2);
		Bump->Inputs[0].InputName = TEXT("LocalPos");
		Bump->Inputs[1].InputName = TEXT("Dryness");
		auto* Local = BloodArtNode<UMaterialExpressionLocalPosition>(Material, -800, 400);
		Bump->Inputs[0].Input.Connect(0, Local);
		Bump->Inputs[1].Input.Connect(4, Vertex);
		UMaterialEditingLibrary::ConnectMaterialProperty(Bump, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	/**
	 * M_Organ, the organs and guts of a burst trunk (GoreOrgans.cpp): colour from the vertex colours like M_Gore, under a
	 * wet clear coat (the glistening membrane over a liver or a loop of gut), its gloss from the alpha (0 wet), and a soft
	 * bump so the surface isn't plastic-smooth.
	 */
	void BloodArtMakeOrgan(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_Organ"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Opaque;
		Material->SetShadingModel(MSM_ClearCoat);
		auto* Vertex = BloodArtNode<UMaterialExpressionVertexColor>(Material, -1000, -200);
		UMaterialEditingLibrary::ConnectMaterialProperty(Vertex, TEXT(""), MP_BaseColor);
		// Under the coat the tissue is soft and fairly rough; the coat is glass-smooth where wet.
		auto* Wet = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 50);
		Wet->R = 0.42f;
		auto* Dry = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 150);
		Dry->R = 0.7f;
		auto* Rough = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -500, 100);
		BloodArtConnect(Wet, TEXT(""), Rough, TEXT("A"));
		BloodArtConnect(Dry, TEXT(""), Rough, TEXT("B"));
		BloodArtConnect(Vertex, TEXT("A"), Rough, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		auto* Specular = BloodArtNode<UMaterialExpressionConstant>(Material, -400, 250);
		Specular->R = 0.45f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);
		auto* CoatWet = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 300);
		CoatWet->R = 1.0f;
		auto* CoatDry = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 380);
		CoatDry->R = 0.15f;
		auto* Coat = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -500, 320);
		BloodArtConnect(CoatWet, TEXT(""), Coat, TEXT("A"));
		BloodArtConnect(CoatDry, TEXT(""), Coat, TEXT("B"));
		BloodArtConnect(Vertex, TEXT("A"), Coat, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Coat, TEXT(""), MP_CustomData0);
		auto* GlossWet = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 460);
		GlossWet->R = 0.06f;
		auto* GlossDry = BloodArtNode<UMaterialExpressionConstant>(Material, -800, 540);
		GlossDry->R = 0.45f;
		auto* Gloss = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -500, 480);
		BloodArtConnect(GlossWet, TEXT(""), Gloss, TEXT("A"));
		BloodArtConnect(GlossDry, TEXT(""), Gloss, TEXT("B"));
		BloodArtConnect(Vertex, TEXT("A"), Gloss, TEXT("Alpha"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Gloss, TEXT(""), MP_CustomData1);
		// A soft bump: two scales of smooth noise in the mesh's own space (cm).
		auto* Bump = BloodArtNode<UMaterialExpressionCustom>(Material, -500, 700);
		Bump->OutputType = CMOT_Float3;
		Bump->Description = TEXT("OrganBump");
		Bump->Code = TEXT("float2 g = 0;\n"
			"for (int o = 0; o < 2; ++o) {\n"
			"  float3 p = LocalPos * (o == 0 ? 0.6 : 2.3);\n"
			"  float3 i = floor(p); float3 f = frac(p); f = f * f * (3.0 - 2.0 * f);\n"
			"  float a = frac(sin(dot(i, float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"  float b = frac(sin(dot(i + float3(1, 0, 0), float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"  float c = frac(sin(dot(i + float3(0, 1, 0), float3(12.9898, 78.233, 37.719))) * 43758.5453);\n"
			"  g += float2(lerp(a, b, f.x) - a, lerp(a, c, f.y) - a) * (o == 0 ? 0.22 : 0.12);\n"
			"}\n"
			"return normalize(float3(g, 1.0));");
		Bump->Inputs.SetNum(1);
		Bump->Inputs[0].InputName = TEXT("LocalPos");
		auto* Local = BloodArtNode<UMaterialExpressionLocalPosition>(Material, -800, 700);
		Bump->Inputs[0].Input.Connect(0, Local);
		UMaterialEditingLibrary::ConnectMaterialProperty(Bump, TEXT(""), MP_Normal);
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
		Fade->FadeDistanceDefault = 4.0f;  // (20 hid every puff made at a wound, right on the skin: sneed saw no spray)
		BloodArtConnect(Opacity, TEXT(""), Fade, TEXT("Opacity"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Fade, TEXT(""), MP_Opacity);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	/** Patchy noise 0 to 1 at a position scaled by `Scale` and shifted by `Offset` (cm). */
	UMaterialExpression* GrimeArtNoise(UMaterial* Material, int32 X, int32 Y, UMaterialExpression* Position, UMaterialExpression* Scale, float Extra, float Offset)
	{
		auto* Scaled = BloodArtNode<UMaterialExpressionMultiply>(Material, X - 300, Y);
		BloodArtConnect(Position, TEXT(""), Scaled, TEXT("A"));
		BloodArtConnect(Scale, TEXT(""), Scaled, TEXT("B"));
		auto* Stretched = BloodArtNode<UMaterialExpressionMultiply>(Material, X - 200, Y);
		BloodArtConnect(Scaled, TEXT(""), Stretched, TEXT("A"));
		Stretched->ConstB = Extra;
		auto* Shifted = BloodArtNode<UMaterialExpressionAdd>(Material, X - 100, Y);
		BloodArtConnect(Stretched, TEXT(""), Shifted, TEXT("A"));
		Shifted->ConstB = Offset;
		auto* Noise = BloodArtNode<UMaterialExpressionNoise>(Material, X, Y);
		Noise->Scale = 1.0f;
		Noise->Levels = 4;
		Noise->OutputMin = 0.0f;
		Noise->OutputMax = 1.0f;
		Noise->bTurbulence = false;
		BloodArtConnect(Shifted, TEXT(""), Noise, TEXT(""));  // the first input is Position (UE 5.8 will not connect it by name)
		return Noise;
	}

	/** saturate((Amount x Gain - Noise) x Sharp): patches that grow from the noise's low spots as Amount rises. */
	UMaterialExpression* GrimeArtCover(UMaterial* Material, int32 X, int32 Y, UMaterialExpression* Amount, float Gain, UMaterialExpression* Noise, float Sharp)
	{
		auto* Gained = BloodArtNode<UMaterialExpressionMultiply>(Material, X - 300, Y);
		BloodArtConnect(Amount, TEXT(""), Gained, TEXT("A"));
		Gained->ConstB = Gain;
		auto* Less = BloodArtNode<UMaterialExpressionSubtract>(Material, X - 200, Y);
		BloodArtConnect(Gained, TEXT(""), Less, TEXT("A"));
		BloodArtConnect(Noise, TEXT(""), Less, TEXT("B"));
		auto* Sharpened = BloodArtNode<UMaterialExpressionMultiply>(Material, X - 100, Y);
		BloodArtConnect(Less, TEXT(""), Sharpened, TEXT("A"));
		Sharpened->ConstB = Sharp;
		auto* Clamped = BloodArtNode<UMaterialExpressionSaturate>(Material, X, Y);
		BloodArtConnect(Sharpened, TEXT(""), Clamped, TEXT(""));
		return Clamped;
	}

	/**
	 * M_GrimeOverlay (docs/design/grime.md): drawn over a whole pawn mesh (skin and clothes) or a weapon as its overlay
	 * material. Patches stick to the body as it moves (pre-skinned local position). Height up the body
	 * h = (local z - LocalMinZ) / LocalHeight; legs dirt fades out by h 0.52; body dirt everywhere; blood its own patches. The gains keep
	 * even level 1 patchy (blood about half covered, as Grime.Strength 0.4 looked with the first gains on the PC): never a solid coat.
	 * Parameters: DirtLegs, DirtBody, Blood, BloodWet (0 to 1), BloodColor, DirtColor, LocalMinZ, LocalHeight (cm),
	 * NoiseScale (per cm).
	 */
	void BloodArtMakeGrimeOverlay(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_GrimeOverlay"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_SkeletalMesh);

		// Pre-skinned position only exists in the vertex shader: carry it to the pixels through an interpolator.
		auto* RawPosition = BloodArtNode<UMaterialExpressionPreSkinnedPosition>(Material, -2800, 0);
		auto* Position = BloodArtNode<UMaterialExpressionVertexInterpolator>(Material, -2600, 0);
		BloodArtConnect(RawPosition, TEXT(""), Position, TEXT(""));
		auto* NoiseScale = BloodArtScalar(Material, TEXT("NoiseScale"), 0.06f, -2600, 150);

		// Height up the body.
		auto* Z = BloodArtMask(Material, -2400, -300, Position, TEXT(""), false, false, true, false);
		auto* MinZ = BloodArtScalar(Material, TEXT("LocalMinZ"), 0.0f, -2400, -200);
		auto* Height = BloodArtScalar(Material, TEXT("LocalHeight"), 180.0f, -2400, -100);
		auto* Above = BloodArtNode<UMaterialExpressionSubtract>(Material, -2250, -300);
		BloodArtConnect(Z, TEXT(""), Above, TEXT("A"));
		BloodArtConnect(MinZ, TEXT(""), Above, TEXT("B"));
		auto* H = BloodArtNode<UMaterialExpressionDivide>(Material, -2100, -300);
		BloodArtConnect(Above, TEXT(""), H, TEXT("A"));
		BloodArtConnect(Height, TEXT(""), H, TEXT("B"));
		// Dirt (sneed 2026-11-20: fine spray over the broad smudges, like blood): one node draws three layers and returns
		// (cover, darkness).
		//  - Broad smudges: two octaves of noise patches growing from their low spots as the level rises, the legs' top
		//    edge ragged (fading out by about h 0.52), gritty inside (a 1 cm grain thins them in places).
		//  - Spray: drops of mud flicked up by the feet or thrown by work, one jittered drop per 3D cell at three sizes
		//    (0.35, 0.8 and 2.4 cm cells); on the legs they reach higher than the smudges (to the waist), stretched a
		//    little upwards, and they crowd round the edges of each smudge so a smudge trails off into speckles.
		//  - Drops smaller than about 3 pixels on screen fade into an even tint of the same average cover (no sparkle
		//    at range).
		//  Darkness: drops and smudge cores are wet dark mud, smudge edges dry pale dust.
		auto* DirtLegs = BloodArtScalar(Material, TEXT("DirtLegs"), 0.0f, -1650, -450);
		auto* DirtBody = BloodArtScalar(Material, TEXT("DirtBody"), 0.0f, -1500, -450);
		auto* Dirt = BloodArtNode<UMaterialExpressionCustom>(Material, -1100, -250);
		Dirt->Description = TEXT("GrimeDirt");
		Dirt->OutputType = CMOT_Float2;
		Dirt->Code = TEXT(R"HLSL(
struct GrimeDirtFn
{
	float H1(float3 p) { p = frac(p * 0.3183099 + 0.1); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }
	float3 H3(float3 p) { return float3(H1(p), H1(p + 19.19), H1(p + 47.31)); }
	float N(float3 x)
	{
		float3 i = floor(x);
		float3 f = frac(x);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(lerp(H1(i), H1(i + float3(1, 0, 0)), f.x), lerp(H1(i + float3(0, 1, 0)), H1(i + float3(1, 1, 0)), f.x), f.y),
			lerp(lerp(H1(i + float3(0, 0, 1)), H1(i + float3(1, 0, 1)), f.x), lerp(H1(i + float3(0, 1, 1)), H1(i + float3(1, 1, 1)), f.x), f.y), f.z);
	}
	float Fbm(float3 x) { return N(x) * 0.55 + N(x * 2.03 + 7.7) * 0.3 + N(x * 4.11 + 3.1) * 0.15; }
	// One drop per cell of `cell` cm (z stretched by 1/stretch), kept where its hash is under `density`; x: cover with a
	// pixel-wide soft rim, y: how visible the size is on screen (0 below about 2 pixels a cell).
	float2 Drops(float3 p, float cell, float density, float rMin, float rMax, float stretch, float fwCm, float seed)
	{
		float3 q = p / cell;
		q.z *= stretch;
		float3 i = floor(q) + seed;
		float3 f = frac(q);
		float keep = step(H1(i + 5.31), density);
		float h = H1(i + 2.17);
		float r = lerp(rMin, rMax, h * h);
		float3 c = r + H3(i + 11.1) * (1.0 - 2.0 * r);
		float aa = max(fwCm / cell, 0.01);
		float d = length(f - c);
		float vis = saturate((cell / max(fwCm, 1e-4) - 2.0) * 0.5);
		return float2(keep * (1.0 - smoothstep(r - aa, r + aa, d)), vis);
	}
};
GrimeDirtFn g;
if (Legs <= 0.001 && Body <= 0.001) return float2(0.0, 0.0);
float3 p = Pre;
float fwCm = max(length(fwidth(Pre)), 1e-4);
// Smudges.
float ragged = (g.N(p * 0.05 + 3.7) - 0.5) * 0.14;
float legs = saturate(1.35 - 2.6 * (H + ragged));
float amount = saturate(Legs * legs + Body);
float broad = g.Fbm(p * Scale * 0.8);
float mid = g.N(p * Scale * 3.0 + 17.0);
float field = amount * 0.78 - broad * 0.74 - mid * 0.3 + 0.02;
float smudge = saturate(field * 5.0);
float grainVis = saturate((1.0 / fwCm - 2.0) * 0.5);
float grain = lerp(0.6, g.N(p * 1.0 + 3.3), grainVis);
smudge *= lerp(0.62, 1.0, grain);
float core = saturate((field - 0.2) * 3.0);
float nearEdge = saturate((field + 0.22) * 3.0);
// Spray.
float sprayLegs = saturate(1.25 - 1.8 * (H + ragged * 0.5));
float spray = saturate(Legs * sprayLegs * 1.1 + Body * 0.55);
float fineDensity = spray * (0.25 + 0.75 * nearEdge) * 0.6;
float bigDensity = spray * nearEdge * nearEdge * 0.3;
float stretch = lerp(1.0, 0.7, saturate(Legs * sprayLegs));
float2 mist = g.Drops(p, 0.35, fineDensity * 0.9, 0.12, 0.32, stretch, fwCm, 0.0);
float2 fine = g.Drops(p, 0.8, fineDensity, 0.08, 0.3, stretch, fwCm, 31.0);
float2 big = g.Drops(p, 2.4, bigDensity, 0.07, 0.24, stretch, fwCm, 67.0);
float drops = max(max(mist.x * mist.y, fine.x * fine.y), big.x * big.y);
// What the eye can't resolve as drops: their average cover (about density x pi r^2).
float tint = saturate((1.0 - mist.y) * fineDensity * 0.9 * 0.18 + (1.0 - fine.y) * fineDensity * 0.15 + (1.0 - big.y) * bigDensity * 0.12);
float cover = 1.0 - (1.0 - smudge * 0.85) * (1.0 - drops * 0.95) * (1.0 - tint);
float dark = saturate(max(drops, core * (0.55 + 0.45 * mid)) + tint * 0.5);
return float2(saturate(cover), dark);
)HLSL");
		Dirt->Inputs.Reset();
		const auto DirtIn = [&](const TCHAR* Name, UMaterialExpression* From)
		{
			FCustomInput& In = Dirt->Inputs.AddDefaulted_GetRef();
			In.InputName = Name;
			In.Input.Connect(0, From);
		};
		DirtIn(TEXT("Pre"), Position);
		DirtIn(TEXT("H"), H);
		DirtIn(TEXT("Legs"), DirtLegs);
		DirtIn(TEXT("Body"), DirtBody);
		DirtIn(TEXT("Scale"), NoiseScale);
		UMaterialExpression* DirtCover = BloodArtMask(Material, -900, -300, Dirt, TEXT(""), true, false, false, false);
		UMaterialExpression* DirtDark = BloodArtMask(Material, -900, -200, Dirt, TEXT(""), false, true, false, false);
		auto* BloodAmount = BloodArtScalar(Material, TEXT("Blood"), 0.0f, -1200, 150);
		UMaterialExpression* BloodNoise = GrimeArtNoise(Material, -1200, 300, Position, NoiseScale, 5.0f, 37.1f);
		UMaterialExpression* BloodCover = GrimeArtCover(Material, -800, 200, BloodAmount, 0.34f, BloodNoise, 9.0f);  // small flecks, never a coat: the wound marks carry the look
		// Blood soaked into clothes over a wound (USkinBlood's cloth spots, set on clothes only): up to 8 bind-pose spheres
		// (Spot0-7: centre, radius in cm; 0 is off) with how much is left of each (SpotAmountA/B), an uneven blot round each
		// that moves with the cloth. Clothes share the body's bind pose, so the pre-skinned position lines up.
		{
			auto* Cloth = BloodArtNode<UMaterialExpressionCustom>(Material, -1000, 900);
			Cloth->Description = TEXT("ClothBlood");
			Cloth->OutputType = CMOT_Float1;
			Cloth->Code = TEXT(R"HLSL(
struct ClothBloodNoise
{
	float H(float3 p) { p = frac(p * 0.3183099 + 0.1); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }
	float N(float3 x)
	{
		float3 i = floor(x);
		float3 f = frac(x);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(lerp(H(i), H(i + float3(1, 0, 0)), f.x), lerp(H(i + float3(0, 1, 0)), H(i + float3(1, 1, 0)), f.x), f.y),
			lerp(lerp(H(i + float3(0, 0, 1)), H(i + float3(1, 0, 1)), f.x), lerp(H(i + float3(0, 1, 1)), H(i + float3(1, 1, 1)), f.x), f.y), f.z);
	}
};
float4 S[8] = { Spot0, Spot1, Spot2, Spot3, Spot4, Spot5, Spot6, Spot7 };
float A[8] = { AmountA.x, AmountA.y, AmountA.z, AmountA.w, AmountB.x, AmountB.y, AmountB.z, AmountB.w };
if (S[0].w <= 0.01) return 0.0;
ClothBloodNoise b;
float n = b.N(Pre * 0.22) * 0.65 + b.N(Pre * 0.7 + 13.1) * 0.35;
float cover = 0.0;
for (int i = 0; i < 8; i++)
{
	if (S[i].w <= 0.01) continue;
	float d = length(Pre - S[i].xyz);
	float R = S[i].w * (0.55 + 0.8 * n);
	cover = max(cover, (1.0 - smoothstep(R * 0.75, R, d)) * saturate(A[i]));
}
return cover;
)HLSL");
			Cloth->Inputs.Reset();
			FCustomInput& PreIn = Cloth->Inputs.AddDefaulted_GetRef();
			PreIn.InputName = TEXT("Pre");
			PreIn.Input.Connect(0, Position);
			int32 Y = 1000;
			for (int32 I = 0; I < 8; ++I)
			{
				auto* Spot = BloodArtNode<UMaterialExpressionVectorParameter>(Material, -1300, Y);
				Spot->ParameterName = *FString::Printf(TEXT("Spot%d"), I);
				Spot->DefaultValue = FLinearColor::Transparent;
				FCustomInput& In = Cloth->Inputs.AddDefaulted_GetRef();
				In.InputName = *FString::Printf(TEXT("Spot%d"), I);
				In.Input.Connect(5, Spot);  // RGBA
				Y += 80;
			}
			for (const TCHAR* Name : {TEXT("AmountA"), TEXT("AmountB")})
			{
				auto* Amount = BloodArtNode<UMaterialExpressionVectorParameter>(Material, -1300, Y);
				Amount->ParameterName = FName(FString(TEXT("Spot")) + Name);
				Amount->DefaultValue = FLinearColor::Transparent;
				FCustomInput& In = Cloth->Inputs.AddDefaulted_GetRef();
				In.InputName = Name;
				In.Input.Connect(5, Amount);
				Y += 80;
			}
			auto* Both = BloodArtNode<UMaterialExpressionMax>(Material, -700, 300);
			BloodArtConnect(BloodCover, TEXT(""), Both, TEXT("A"));
			BloodArtConnect(Cloth, TEXT(""), Both, TEXT("B"));
			BloodCover = Both;
		}

		// Colour.
		auto* DirtColor = BloodArtNode<UMaterialExpressionVectorParameter>(Material, -800, -500);
		DirtColor->ParameterName = TEXT("DirtColor");
		DirtColor->DefaultValue = FLinearColor(0.11f, 0.085f, 0.06f);
		auto* BloodColor = BloodArtNode<UMaterialExpressionVectorParameter>(Material, -800, -400);
		BloodColor->ParameterName = TEXT("BloodColor");
		BloodColor->DefaultValue = FLinearColor(0.2f, 0.008f, 0.007f);
		// Dry dust at the smudges' edges, dark wet mud in the drops and cores.
		auto* DirtPale = BloodArtNode<UMaterialExpressionMultiply>(Material, -650, -560);
		BloodArtConnect(DirtColor, TEXT(""), DirtPale, TEXT("A"));
		DirtPale->ConstB = 1.35f;
		auto* DirtDeep = BloodArtNode<UMaterialExpressionMultiply>(Material, -650, -500);
		BloodArtConnect(DirtColor, TEXT(""), DirtDeep, TEXT("A"));
		DirtDeep->ConstB = 0.55f;
		auto* DirtShade = BloodArtLerp(Material, -550, -530, DirtPale, TEXT(""), DirtDeep, TEXT(""), DirtDark, TEXT(""));
		auto* Base = BloodArtLerp(Material, -500, -450, DirtShade, TEXT(""), BloodColor, TEXT(""), BloodCover, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);

		// Wet blood is glossy, dry blood and dirt matte.
		auto* Wet = BloodArtScalar(Material, TEXT("BloodWet"), 0.0f, -800, 400);
		auto* BloodRough = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 400);
		BloodRough->ConstA = 0.75f;
		BloodRough->ConstB = 0.3f;
		BloodArtConnect(Wet, TEXT(""), BloodRough, TEXT("Alpha"));
		auto* DirtRough = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 500);  // wet mud a little less matte
		DirtRough->ConstA = 0.95f;
		DirtRough->ConstB = 0.78f;
		BloodArtConnect(DirtDark, TEXT(""), DirtRough, TEXT("Alpha"));
		auto* Roughness = BloodArtLerp(Material, -450, 450, DirtRough, TEXT(""), BloodRough, TEXT(""), BloodCover, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		auto* BloodSpec = BloodArtNode<UMaterialExpressionLinearInterpolate>(Material, -650, 600);
		BloodSpec->ConstA = 0.35f;
		BloodSpec->ConstB = 0.7f;
		BloodArtConnect(Wet, TEXT(""), BloodSpec, TEXT("Alpha"));
		auto* DirtSpec = BloodArtNode<UMaterialExpressionConstant>(Material, -650, 700);
		DirtSpec->R = 0.2f;
		auto* Specular = BloodArtLerp(Material, -450, 650, DirtSpec, TEXT(""), BloodSpec, TEXT(""), BloodCover, TEXT(""));
		UMaterialEditingLibrary::ConnectMaterialProperty(Specular, TEXT(""), MP_Specular);

		// Opacity: the stronger of the two.
		auto* DirtOpacity = BloodArtNode<UMaterialExpressionMultiply>(Material, -500, -150);
		BloodArtConnect(DirtCover, TEXT(""), DirtOpacity, TEXT("A"));
		DirtOpacity->ConstB = 0.9f;
		auto* BloodOpacity = BloodArtNode<UMaterialExpressionMultiply>(Material, -500, 150);
		BloodArtConnect(BloodCover, TEXT(""), BloodOpacity, TEXT("A"));
		BloodOpacity->ConstB = 0.92f;
		auto* Opacity = BloodArtNode<UMaterialExpressionMax>(Material, -300, 0);
		BloodArtConnect(DirtOpacity, TEXT(""), Opacity, TEXT("A"));
		BloodArtConnect(BloodOpacity, TEXT(""), Opacity, TEXT("B"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Blood in the skin (docs/design/blood.md): each MakeHuman body base's bind-pose position map, the paint material
	// that draws wounds into a pawn's mask with it, and the skin material reading the mask.

	/** Draws every skin triangle of a body into its UV layout, each texel holding where on the body (bind pose) it is. */
	bool SkinArtBakePositions(USkeletalMesh* Mesh, const FString& Base, const FString& TextureName)
	{
		const FSkeletalMeshModel* Model = Mesh->GetImportedModel();
		if (!Model || Model->LODModels.Num() == 0)
		{
			UE_LOG(LogBloodArt, Error, TEXT("%s has no imported model"), *Mesh->GetName());
			return false;
		}
		const FSkeletalMeshLODModel& Lod = Model->LODModels[0];
		TArray<FSoftSkinVertex> Vertices;
		Lod.GetVertices(Vertices);
		const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
		const FString SkinName = MakeHumanSkinName(Base);
		constexpr int32 Res = SkinBloodResolution;
		TArray<FVector4f> Texels;
		Texels.SetNumZeroed(Res * Res);
		// Texels that more than one piece of skin is laid on (sneed's groin square, 2026-11-20): the tail, ears and other
		// extras made with the skin material have their own small UV patches, laid over the body's own skin (the tail's
		// over the hip). Each texel keeps the piece with the most texels per cm2 (the body's real skin, not a stretched
		// extra), so a wound by the hips no longer lights up the tail's patch as a square on the hip.
		TArray<float> Density;
		Density.SetNumZeroed(Res * Res);
		int32 Contested = 0;
		int32 Triangles = 0;
		// Every skin triangle first, so the stretched ones can be told apart from the rest.
		struct FSkinArtTriangle
		{
			const FSoftSkinVertex* V[3];
			float Stretch;  // longest UV edge (texels) per longest edge on the body (cm)
		};
		TArray<FSkinArtTriangle> Skin;
		for (const FSkelMeshSection& Section : Lod.Sections)
		{
			const UMaterialInterface* Material = Materials.IsValidIndex(Section.MaterialIndex) ? Materials[Section.MaterialIndex].MaterialInterface.Get() : nullptr;
			const FString Slot = Materials.IsValidIndex(Section.MaterialIndex) ? Materials[Section.MaterialIndex].MaterialSlotName.ToString() : FString();
			if (!Material || !Material->GetName().StartsWith(SkinName))
			{
				continue;
			}
			// Indices into the whole vertex list, or (older imports) into the section's own.
			bool bGlobal = true;
			for (uint32 I = 0; I < Section.NumTriangles * 3; ++I)
			{
				const uint32 Index = Lod.IndexBuffer[Section.BaseIndex + I];
				if (Index < Section.BaseVertexIndex || Index >= Section.BaseVertexIndex + static_cast<uint32>(Section.SoftVertices.Num()))
				{
					bGlobal = false;
					break;
				}
			}
			const auto Vertex = [&](uint32 Index) -> const FSoftSkinVertex*
			{
				return bGlobal ? &Vertices[Index] : &Section.SoftVertices[FMath::Min<int32>(Index, Section.SoftVertices.Num() - 1)];
			};
			for (uint32 T = 0; T < Section.NumTriangles; ++T)
			{
				FSkinArtTriangle& Tri = Skin.AddDefaulted_GetRef();
				float LongestUV = 0.0f;
				float LongestBody = 0.0f;
				for (int32 K = 0; K < 3; ++K)
				{
					Tri.V[K] = Vertex(Lod.IndexBuffer[Section.BaseIndex + T * 3 + K]);
				}
				for (int32 K = 0; K < 3; ++K)
				{
					const FSoftSkinVertex& P = *Tri.V[K];
					const FSoftSkinVertex& Q = *Tri.V[(K + 1) % 3];
					LongestUV = FMath::Max(LongestUV, FVector2f::Distance(P.UVs[0], Q.UVs[0]) * Res);
					LongestBody = FMath::Max(LongestBody, FVector3f::Dist(P.Position, Q.Position));
				}
				Tri.Stretch = LongestUV / FMath::Max(LongestBody, 0.001f);
			}
			UE_LOG(LogBloodArt, Display, TEXT("  %s: skin section %s, %u triangles (%s indices)"), *Base, *Slot, Section.NumTriangles, bGlobal ? TEXT("global") : TEXT("section"));
		}
		// The stocking band (sneed, 2026-11-20 to 11-22): the woman's anatomy body joins its vulva to the body with ~47
		// triangles a few mm across on the body but hundreds of texels long in UV, laid right across the top of both
		// thighs, the belly and an arm. Drawn into the position map they won every texel they crossed (densest in UV),
		// so those texels said "groin" and any groin or hip wound painted a band round the thighs and lines up the body.
		// A triangle whose UV edge is over 8 times longer per cm than the median skin's is such a stitch (the man's body
		// has none; the next most stretched skin is under 5 times): it is left out and the real skin keeps its texels.
		TArray<float> Stretches;
		Stretches.Reserve(Skin.Num());
		for (const FSkinArtTriangle& Tri : Skin)
		{
			Stretches.Add(Tri.Stretch);
		}
		Stretches.Sort();
		const float MedianStretch = Stretches.Num() > 0 ? Stretches[Stretches.Num() / 2] : 0.0f;
		const float MaxStretch = MedianStretch * 8.0f;
		int32 Stitches = 0;
		for (const FSkinArtTriangle& Tri : Skin)
		{
			const FSoftSkinVertex& A = *Tri.V[0];
			const FSoftSkinVertex& B = *Tri.V[1];
			const FSoftSkinVertex& C = *Tri.V[2];
			if (MedianStretch > 0.0f && Tri.Stretch > MaxStretch)
			{
				++Stitches;
				continue;
			}
			const FVector2f Pa = A.UVs[0] * Res;
			const FVector2f Pb = B.UVs[0] * Res;
			const FVector2f Pc = C.UVs[0] * Res;
			const float Area = (Pb.X - Pa.X) * (Pc.Y - Pa.Y) - (Pc.X - Pa.X) * (Pb.Y - Pa.Y);
			if (FMath::Abs(Area) < 1e-8f)
			{
				continue;
			}
			const float WorldArea = 0.5f * FVector3f::CrossProduct(B.Position - A.Position, C.Position - A.Position).Size();
			const float TexelsPerArea = 0.5f * FMath::Abs(Area) / FMath::Max(WorldArea, 1e-4f);
			const int32 X0 = FMath::Clamp(FMath::FloorToInt(FMath::Min3(Pa.X, Pb.X, Pc.X)), 0, Res - 1);
			const int32 X1 = FMath::Clamp(FMath::CeilToInt(FMath::Max3(Pa.X, Pb.X, Pc.X)), 0, Res - 1);
			const int32 Y0 = FMath::Clamp(FMath::FloorToInt(FMath::Min3(Pa.Y, Pb.Y, Pc.Y)), 0, Res - 1);
			const int32 Y1 = FMath::Clamp(FMath::CeilToInt(FMath::Max3(Pa.Y, Pb.Y, Pc.Y)), 0, Res - 1);
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					const FVector2f P(X + 0.5f, Y + 0.5f);
					const float Wa = ((Pb.X - P.X) * (Pc.Y - P.Y) - (Pc.X - P.X) * (Pb.Y - P.Y)) / Area;
					const float Wb = ((Pc.X - P.X) * (Pa.Y - P.Y) - (Pa.X - P.X) * (Pc.Y - P.Y)) / Area;
					const float Wc = 1.0f - Wa - Wb;
					constexpr float Edge = -0.02f;
					if (Wa < Edge || Wb < Edge || Wc < Edge)
					{
						continue;
					}
					const FVector3f Position = A.Position * Wa + B.Position * Wb + C.Position * Wc;
					FVector4f& Texel = Texels[Y * Res + X];
					if (Texel.W > 0.5f && FVector3f::Dist(FVector3f(Texel.X, Texel.Y, Texel.Z) - FVector3f(SkinBloodPositionOffset), Position) > 3.0f)
					{
						++Contested;
						if (TexelsPerArea < Density[Y * Res + X] * 1.5f)
						{
							continue;  // a sparser piece laid over skin already there
						}
					}
					Density[Y * Res + X] = FMath::Max(Density[Y * Res + X], TexelsPerArea);
					Texel = FVector4f(Position.X + SkinBloodPositionOffset, Position.Y + SkinBloodPositionOffset, Position.Z + SkinBloodPositionOffset, 1.0f);
				}
			}
			++Triangles;
		}
		UE_LOG(LogBloodArt, Display, TEXT("  %s: %d stretched stitch triangles left out (UV over %.0f texels per cm, 8x the median)"), *Base, Stitches, MaxStretch);
		if (Triangles == 0)
		{
			UE_LOG(LogBloodArt, Error, TEXT("%s: no skin sections (material %s*)"), *Mesh->GetName(), *SkinName);
			return false;
		}
		// Grow each island a few texels so the skin's filtered lookups across a UV seam find blood on both sides. A texel
		// between two islands takes one island's skin only (neighbours more than 3 cm from the first one found are not
		// averaged in): a mix of two far-apart places is a point inside the body that a wound near neither could paint.
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			TArray<FVector4f> Grown = Texels;
			for (int32 Y = 0; Y < Res; ++Y)
			{
				for (int32 X = 0; X < Res; ++X)
				{
					if (Texels[Y * Res + X].W > 0.5f)
					{
						continue;
					}
					FVector4f Sum(0, 0, 0, 0);
					for (int32 Dy = -1; Dy <= 1; ++Dy)
					{
						for (int32 Dx = -1; Dx <= 1; ++Dx)
						{
							const int32 Nx = X + Dx;
							const int32 Ny = Y + Dy;
							if (Nx >= 0 && Ny >= 0 && Nx < Res && Ny < Res && Texels[Ny * Res + Nx].W > 0.5f)
							{
								const FVector4f& Near = Texels[Ny * Res + Nx];
								if (Sum.W < 0.5f || FVector3f::Dist(FVector3f(Near.X, Near.Y, Near.Z), FVector3f(Sum.X, Sum.Y, Sum.Z) / Sum.W) <= 3.0f)
								{
									Sum += Near;
								}
							}
						}
					}
					if (Sum.W > 0.5f)
					{
						Grown[Y * Res + X] = FVector4f(Sum.X / Sum.W, Sum.Y / Sum.W, Sum.Z / Sum.W, 1.0f);
					}
				}
			}
			Texels = MoveTemp(Grown);
		}
		TArray<FFloat16Color> Half;
		Half.SetNumUninitialized(Res * Res);
		int32 Filled = 0;
		for (int32 I = 0; I < Res * Res; ++I)
		{
			Half[I] = FFloat16Color(FLinearColor(Texels[I].X, Texels[I].Y, Texels[I].Z, Texels[I].W));
			Filled += Texels[I].W > 0.5f ? 1 : 0;
		}
		UPackage* Package = CreatePackage(*(FString(SkinBloodFolder) / TextureName));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, *TextureName, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Res, Res, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(Half.GetData()));
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_HDR;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->Filter = TF_Nearest;
		Texture->NeverStream = true;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->LODGroup = TEXTUREGROUP_Pixels2D;
		Texture->PostEditChange();
		UE_LOG(LogBloodArt, Display, TEXT("%s: %d skin triangles, %.1f%% of the texture is skin, %d texels laid on twice (the denser skin kept)"), *TextureName, Triangles,
			100.0f * Filled / (Res * Res), Contested);
		return BloodArtSave(Texture);
	}

	/** A position map for every MakeHuman body base there is. The first one made or found (for the paint material's default). */
	UTexture2D* SkinArtMakePositions(bool bForce, int32& Failures)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		Registry.ScanPathsSynchronous({FString(MakeHumanFolder)}, true);
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(MakeHumanFolder), Assets, false);
		UTexture2D* First = nullptr;
		for (const FAssetData& Asset : Assets)
		{
			const FString Name = Asset.AssetName.ToString();
			if (!Name.StartsWith(TEXT("SK_MH_")) || Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName())
			{
				continue;
			}
			const FString Base = Name.RightChop(6);
			const FString TextureName = SkinBloodPositionName(Base);
			if (BloodArtWanted(*TextureName, bForce))
			{
				USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
				if (!Mesh || !SkinArtBakePositions(Mesh, Base, TextureName))
				{
					++Failures;
					continue;
				}
			}
			if (!First)
			{
				First = LoadObject<UTexture2D>(nullptr, *MakeHumanAssetPath(SkinBloodFolder, TextureName));
			}
		}
		if (!First)
		{
			UE_LOG(LogBloodArt, Warning, TEXT("No MakeHuman bodies in %s: no skin blood"), MakeHumanFolder);
		}
		return First;
	}

	// The paint step. Every texel of the mask looks up where on the body it is (Pos, bind pose) and adds the wound's
	// marks there: the wound itself (Shape.x radius), blood soaked round it (Shape.y), spatter (Shape.z) and runs down
	// the body from it (Shape.w long), all in cm and broken up by noise so no two look alike. Only skin near the shot
	// line (Dir.w deep) takes it, not the far side of the body. Hit.w: how dark the open core is; Down.w: strength;
	// Hit2: the far end of a gash (= Hit for a hole), w its seed. Mode 1 fades instead (Decay: amount, wetness, core).
	const TCHAR* const SkinArtPaintCode = TEXT(R"HLSL(
float4 prev = Texture2DSampleLevel(Prev, PrevSampler, UV, 0);
if (Mode > 0.5)
{
	return float3(max(0.0, prev.r - Decay.x), max(0.0, prev.g - Decay.y), max(0.0, prev.b - Decay.z));
}
float4 pm = Texture2DSampleLevel(Pos, PosSampler, UV, 0);
if (pm.a < 0.5) return max(prev.rgb, 0.0);
float3 P = pm.xyz - Offset;
struct SkinBloodNoise
{
	float H(float3 p) { p = frac(p * 0.3183099 + 0.1); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }
	float N(float3 x)
	{
		float3 i = floor(x);
		float3 f = frac(x);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(lerp(H(i), H(i + float3(1, 0, 0)), f.x), lerp(H(i + float3(0, 1, 0)), H(i + float3(1, 1, 0)), f.x), f.y),
			lerp(lerp(H(i + float3(0, 0, 1)), H(i + float3(1, 0, 1)), f.x), lerp(H(i + float3(0, 1, 1)), H(i + float3(1, 1, 1)), f.x), f.y), f.z);
	}
};
SkinBloodNoise b;
float seed = Hit2.w;
float3 A = Hit.xyz;
float3 AB = Hit2.xyz - A;
float t = saturate(dot(P - A, AB) / max(dot(AB, AB), 0.0001));
float3 C = A + AB * t;
float3 D = normalize(Dir.xyz + float3(0.0, 0.0, 0.00001));
float3 d = P - C;
float along = dot(d, D);
float r = length(d - D * along);
float facing = 1.0 - smoothstep(Dir.w, Dir.w * 1.6, abs(along));
float rr = r * (0.7 + 0.6 * b.N(P * 0.45 + seed));
float core = 1.0 - smoothstep(Shape.x * 0.6, Shape.x, rr);
float sn = b.N(P * 0.35 + seed * 1.7) * 0.6 + b.N(P * 1.1 + seed * 2.3) * 0.4;
float soakR = Shape.y * (0.45 + 0.75 * sn);
float soak = Shape.y > 0.01 ? 0.8 * (1.0 - smoothstep(soakR * 0.8, soakR, r)) : 0.0;
float fall = saturate(1.0 - r / max(Shape.z, 0.01));
float spk = b.N(P * 2.4 + seed * 3.1);
float sp = Shape.z > 0.01 ? smoothstep(0.84 - 0.3 * fall, 0.88 - 0.3 * fall, spk) * step(0.0001, fall) * 0.9 : 0.0;
float wide = Dir.w + Shape.z * 0.4;
float nearWide = 1.0 - smoothstep(wide, wide * 1.6, abs(along));
float runs = 0.0;
if (Shape.w > 0.01)
{
	float3 Dn = normalize(Down.xyz + float3(0.0, 0.0, -0.00001));
	float3 q = P - C;
	float h = dot(q, Dn);
	float3 E1 = normalize(cross(Dn, D) + float3(0.00001, 0.0, 0.0));
	float x = dot(q, E1);
	float depth = dot(q - Dn * h, D);
	float width = Shape.x * 1.4 + Shape.y * 0.35;
	if (h > -Shape.x && abs(x) < width && abs(depth) < Dir.w * 1.5)
	{
		float lane = x * 1.1 + seed * 7.0;
		float pick = b.N(float3(lane, seed * 3.0, 0.5));
		float len = Shape.w * (0.25 + 0.95 * b.N(float3(lane * 0.7, seed * 5.0, 9.5)));
		float streak = smoothstep(0.52, 0.6, pick) * (1.0 - smoothstep(len * 0.75, len, h));
		float mainRun = (1.0 - smoothstep(Shape.x * 0.35, Shape.x * 0.8, abs(x) * (1.0 + 1.5 * saturate(h / max(Shape.w, 1.0))))) * (1.0 - smoothstep(Shape.w * 0.6, Shape.w, h));
		runs = max(streak, mainRun) * 0.9 * (1.0 - smoothstep(width * 0.8, width, abs(x)));
	}
}
float s = Down.w;
float paint = max(max(core, soak) * facing, max(sp * nearWide, runs)) * s;
float hole = Hit.w * (1.0 - smoothstep(Shape.x * 0.22, Shape.x * 0.38, rr)) * facing * s;
return max(float3(max(prev.r, paint), max(prev.g, saturate(paint * 4.0)), max(prev.b, hole)), 0.0);
)HLSL");

	void SkinArtInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 Output = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(Output, From);
	}

	bool SkinArtReportErrors(UMaterial* Material)
	{
		bool bErrors = false;
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			for (const FString& Error : Resource->GetCompileErrors())
			{
				UE_LOG(LogBloodArt, Error, TEXT("%s: %s"), *Material->GetName(), *Error);
				bErrors = true;
			}
		}
		return bErrors;
	}

	void SkinArtMakePaint(IAssetTools& Tools, UTexture2D* SamplePositions, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, SkinBloodPaintName, Failures);
		if (!Material)
		{
			return;
		}
		Material->SetShadingModel(MSM_Unlit);
		Material->BlendMode = BLEND_Opaque;
		auto* Custom = BloodArtNode<UMaterialExpressionCustom>(Material, -300, 0);
		Custom->Description = TEXT("SkinBloodPaint");
		Custom->OutputType = CMOT_Float3;
		Custom->Code = SkinArtPaintCode;
		Custom->Inputs.Reset();
		auto* UV = BloodArtNode<UMaterialExpressionTextureCoordinate>(Material, -1400, 0);
		SkinArtInput(Custom, TEXT("UV"), UV);
		auto* Prev = BloodArtNode<UMaterialExpressionTextureObjectParameter>(Material, -1100, 80);
		Prev->ParameterName = TEXT("Prev");
		Prev->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
		Prev->SamplerType = SAMPLERTYPE_Color;
		SkinArtInput(Custom, TEXT("Prev"), Prev);
		auto* Pos = BloodArtNode<UMaterialExpressionTextureObjectParameter>(Material, -1100, 180);
		Pos->ParameterName = TEXT("Pos");
		Pos->Texture = SamplePositions ? static_cast<UTexture*>(SamplePositions) : LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
		Pos->SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Pos->Texture);
		SkinArtInput(Custom, TEXT("Pos"), Pos);
		SkinArtInput(Custom, TEXT("Offset"), BloodArtScalar(Material, TEXT("Offset"), SkinBloodPositionOffset, -1100, 280));
		SkinArtInput(Custom, TEXT("Mode"), BloodArtScalar(Material, TEXT("Mode"), 0.0f, -1100, 340));
		int32 Y = 420;
		for (const TCHAR* Name : {TEXT("Hit"), TEXT("Hit2"), TEXT("Dir"), TEXT("Down"), TEXT("Shape"), TEXT("Decay")})
		{
			auto* Vector = BloodArtNode<UMaterialExpressionVectorParameter>(Material, -1100, Y);
			Vector->ParameterName = Name;
			Vector->DefaultValue = FLinearColor::Transparent;
			SkinArtInput(Custom, Name, Vector, 5);  // output 5: RGBA
			Y += 120;
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		Failures += SkinArtReportErrors(Material) ? 1 : 0;
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	// A gush of blood in the air (sneed 2026-11-20): a ragged streak, thick at its head (+U) and thinning to a tail, with
	// drops flying off it, different for every seed (instance data 4); instance colour (0-2) and opacity (3).
	const TCHAR* const BloodArtSprayCode = TEXT(R"HLSL(
struct BloodSprayNoise
{
	float H(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
	float N(float2 x)
	{
		float2 i = floor(x);
		float2 f = frac(x);
		f = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(H(i), H(i + float2(1, 0)), f.x), lerp(H(i + float2(0, 1)), H(i + float2(1, 1)), f.x), f.y);
	}
};
BloodSprayNoise b;
float2 p = UV * 2.0 - 1.0;
float head = saturate(p.x * 0.5 + 0.5);
float w = lerp(0.18, 0.85, head * head);
float ragged = (b.N(float2(p.x * 5.0 + Seed, p.y * 5.0 - Seed)) - 0.5) * 0.55 + (b.N(float2(p.x * 13.0 - Seed, p.y * 13.0 + Seed * 0.5)) - 0.5) * 0.25;
float edge = abs(p.y) / w + max(0.0, p.x) * max(0.0, p.x) * 0.9 + max(0.0, -p.x - 0.75) * 3.0;
float body = 1.0 - smoothstep(0.7, 0.9, edge + ragged);
float drops = 0.0;
[unroll] for (int i = 0; i < 7; ++i)
{
	float2 c = float2(b.H(float2(Seed, i)) * 1.7 - 0.85, (b.H(float2(i, Seed)) * 2.0 - 1.0) * 0.75);
	float r = 0.04 + 0.08 * b.H(float2(Seed + i, 3.7));
	drops = max(drops, 1.0 - smoothstep(r * 0.65, r, length(p - c)));
}
return saturate(max(body, drops)) * Opacity;
)HLSL");

	void BloodArtMakeSpray(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BloodArtNewMaterial(Tools, TEXT("M_BloodSpray"), Failures);
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
		Roughness->R = 0.25f;  // wet
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness, TEXT(""), MP_Roughness);
		auto* Custom = BloodArtNode<UMaterialExpressionCustom>(Material, -500, 250);
		Custom->Description = TEXT("BloodSpray");
		Custom->OutputType = CMOT_Float1;
		Custom->Code = BloodArtSprayCode;
		Custom->Inputs.Reset();
		SkinArtInput(Custom, TEXT("UV"), BloodArtNode<UMaterialExpressionTextureCoordinate>(Material, -900, 200));
		auto* Seed = BloodArtNode<UMaterialExpressionPerInstanceCustomData>(Material, -900, 300);
		Seed->DataIndex = 4;
		Seed->ConstDefaultValue = 0.0f;
		SkinArtInput(Custom, TEXT("Seed"), Seed);
		auto* Opacity = BloodArtNode<UMaterialExpressionPerInstanceCustomData>(Material, -900, 400);
		Opacity->DataIndex = 3;
		Opacity->ConstDefaultValue = 0.9f;
		SkinArtInput(Custom, TEXT("Opacity"), Opacity);
		auto* Fade = BloodArtNode<UMaterialExpressionDepthFade>(Material, -250, 300);
		Fade->FadeDistanceDefault = 3.0f;
		BloodArtConnect(Custom, TEXT(""), Fade, TEXT("Opacity"));
		UMaterialEditingLibrary::ConnectMaterialProperty(Fade, TEXT(""), MP_Opacity);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		Failures += SkinArtReportErrors(Material) ? 1 : 0;
		Failures += BloodArtSave(Material) ? 0 : 1;
	}

	/** Adds the blood mask to the MakeHuman skin material (again with -force). */
	void SkinArtPatchSkin(bool bForce, int32& Failures)
	{
		const FString Path = MakeHumanAssetPath(MakeHumanFolder, SkinBloodSkinMaterialName);
		UMaterial* Material = LoadObject<UMaterial>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Material)
		{
			UE_LOG(LogBloodArt, Warning, TEXT("No %s yet (run -run=MakeHumanArt first): skin blood not added"), *Path);
			return;
		}
		if (SkinBloodMaterial::Has(Material))
		{
			if (!bForce)
			{
				UE_LOG(LogBloodArt, Display, TEXT("%s reads the blood mask already (-force adds it again)"), *Path);
				return;
			}
			SkinBloodMaterial::Strip(Material);
		}
		if (!SkinBloodMaterial::Add(Material))
		{
			UE_LOG(LogBloodArt, Error, TEXT("%s: base colour, roughness or normal not connected"), *Path);
			++Failures;
			return;
		}
		Material->PostEditChange();
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		Failures += SkinArtReportErrors(Material) ? 1 : 0;
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
	if (BloodArtWanted(TEXT("M_Gore"), bForce))
	{
		BloodArtMakeGore(Tools, Failures);
	}
	if (BloodArtWanted(TEXT("M_Organ"), bForce))
	{
		BloodArtMakeOrgan(Tools, Failures);
	}
	if (BloodArtWanted(TEXT("M_BloodMist"), bForce))
	{
		BloodArtMakeMist(Tools, Failures);
	}
	if (BloodArtWanted(TEXT("M_BloodSpray"), bForce))
	{
		BloodArtMakeSpray(Tools, Failures);
	}
	if (BloodArtWanted(TEXT("M_GrimeOverlay"), bForce))
	{
		BloodArtMakeGrimeOverlay(Tools, Failures);
	}
	UTexture2D* SkinPositions = SkinArtMakePositions(bForce, Failures);
	if (BloodArtWanted(SkinBloodPaintName, bForce))
	{
		SkinArtMakePaint(Tools, SkinPositions, Failures);
	}
	SkinArtPatchSkin(bForce, Failures);
	UE_LOG(LogBloodArt, Display, TEXT("Blood art done, %d failures"), Failures);
	return Failures == 0 ? 0 : 1;
}
