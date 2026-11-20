#include "BlastArtCommandlet.h"

#include "Async/ParallelFor.h"
#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionEyeAdaptationInverse.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "BlastFlipbooks.h"
#include "BlastFxData.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlastArt, Log, All);

namespace
{
	constexpr int32 BlastArtFlipbookSize = 2048;  // 256 px a frame
	constexpr int32 BlastArtScorchSize = 1024;

	template <typename T>
	T* BlastArtNode(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	bool BlastArtSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *File, Args);
		UE_LOG(LogBlastArt, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *File);
		return bSaved;
	}

	/** True if the asset should be made: it is not there, or -force (then its file is deleted first). */
	bool BlastArtWanted(const TCHAR* Name, bool bForce)
	{
		const FString Package = FString(BlastFxData::Folder) / Name;
		if (!FPackageName::DoesPackageExist(Package))
		{
			return true;
		}
		if (!bForce)
		{
			UE_LOG(LogBlastArt, Display, TEXT("%s is there already (-force makes it again)"), *Package);
			return false;
		}
		const FString File = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*File, false, true);
		return true;
	}

	UTexture2D* BlastArtLoadTexture(const TCHAR* Name)
	{
		return LoadObject<UTexture2D>(nullptr, *(FString(BlastFxData::Folder) / Name + TEXT(".") + Name));
	}

	/** Saves a sheet as a texture: B = depth, G = light, R = heat, A = opacity (BGRA bytes). */
	UTexture2D* BlastArtTexture(const TCHAR* Name, const blastfx::Image& Image)
	{
		const int32 Size = Image.Size;
		TArray<uint8> Bgra;
		Bgra.SetNumUninitialized(Size * Size * 4);
		const auto Byte = [](float Value) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.0f), 0, 255)); };
		for (int32 I = 0; I < Size * Size; ++I)
		{
			const blastfx::Pixel& P = Image.Pixels[I];
			Bgra[I * 4 + 0] = Byte(P.Depth);
			Bgra[I * 4 + 1] = Byte(P.Light);
			Bgra[I * 4 + 2] = Byte(P.Heat);
			Bgra[I * 4 + 3] = Byte(P.Opacity);
		}
		UPackage* Package = CreatePackage(*(FString(BlastFxData::Folder) / Name));
		UTexture2D* Texture = NewObject<UTexture2D>(Package, Name, RF_Public | RF_Standalone);
		Texture->PreEditChange(nullptr);
		Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Bgra.GetData());
		Texture->SRGB = false;
		Texture->CompressionSettings = TC_BC7;
		Texture->LODGroup = TEXTUREGROUP_Effects;
		Texture->MipGenSettings = TMGS_FromTextureGroup;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->PostEditChange();
		return BlastArtSave(Texture) ? Texture : nullptr;
	}

	UTexture2D* BlastArtFlipbook(const TCHAR* Name, blastfx::Kind What, uint32 Seed, bool bForce, int32& Failures)
	{
		if (!BlastArtWanted(Name, bForce))
		{
			return BlastArtLoadTexture(Name);
		}
		blastfx::Image Image;
		Image.Size = BlastArtFlipbookSize;
		Image.Pixels.assign(static_cast<size_t>(BlastArtFlipbookSize) * BlastArtFlipbookSize, blastfx::Pixel{});
		UE_LOG(LogBlastArt, Display, TEXT("Drawing %s (64 frames)..."), Name);
		// Each frame writes only its own cell.
		ParallelFor(blastfx::kFrames * blastfx::kFrames, [&Image, What, Seed](int32 Frame) { blastfx::RenderFrame(Image, What, Frame, Seed); });
		UTexture2D* Texture = BlastArtTexture(Name, Image);
		Failures += Texture ? 0 : 1;
		return Texture;
	}

	UMaterial* BlastArtNewMaterial(IAssetTools& Tools, const TCHAR* Name, int32& Failures)
	{
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(Name, BlastFxData::Folder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			UE_LOG(LogBlastArt, Error, TEXT("Could not create %s/%s"), BlastFxData::Folder, Name);
			++Failures;
		}
		return Material;
	}

	UMaterialExpressionCustom* BlastArtCustom(UMaterial* Material, const TCHAR* Description, ECustomMaterialOutputType Type, const TCHAR* Code, int32 X, int32 Y)
	{
		auto* Custom = BlastArtNode<UMaterialExpressionCustom>(Material, X, Y);
		Custom->Description = Description;
		Custom->OutputType = Type;
		Custom->Code = Code;
		Custom->Inputs.Reset();
		return Custom;
	}

	void BlastArtInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 Output = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(Output, From);
	}

	UMaterialExpression* BlastArtData(UMaterial* Material, int32 Index, float Default, int32 X, int32 Y)
	{
		auto* Node = BlastArtNode<UMaterialExpressionPerInstanceCustomData>(Material, X, Y);
		Node->DataIndex = Index;
		Node->ConstDefaultValue = Default;
		return Node;
	}

	UMaterialExpression* BlastArtTextureObject(UMaterial* Material, UTexture2D* Texture, int32 X, int32 Y)
	{
		auto* Node = BlastArtNode<UMaterialExpressionTextureObject>(Material, X, Y);
		Node->Texture = Texture;
		Node->SamplerType = SAMPLERTYPE_LinearColor;
		return Node;
	}

	/**
	 * Emissive through the inverse of the camera's exposure, so a glow of 1 reads as bright white whether the camera is set
	 * for noon (EV 15, where an emissive of 40 was invisible: PC round 2) or night. As M_BuildingGhost does it.
	 */
	void BlastArtUnexposedEmissive(UMaterial* Material, UMaterialExpression* Glow, int32 X, int32 Y)
	{
		auto* Unexposed = BlastArtNode<UMaterialExpressionEyeAdaptationInverse>(Material, X, Y);
		if (!UMaterialEditingLibrary::ConnectMaterialExpressions(Glow, TEXT(""), Unexposed, TEXT("LightValue")) &&
			!UMaterialEditingLibrary::ConnectMaterialExpressions(Glow, TEXT(""), Unexposed, TEXT("LightValueInput")))
		{
			Unexposed->LightValueInput.Connect(0, Glow);
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Unexposed, TEXT(""), MP_EmissiveColor);
	}

	bool BlastArtFinish(UMaterial* Material)
	{
		UMaterialEditingLibrary::RecompileMaterial(Material);
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		bool bErrors = false;
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			for (const FString& Error : Resource->GetCompileErrors())
			{
				UE_LOG(LogBlastArt, Error, TEXT("%s: %s"), *Material->GetName(), *Error);
				bErrors = true;
			}
		}
		return BlastArtSave(Material) && !bErrors;
	}

	// The flipbook frame at Frame, blended with the next; Flip mirrors the picture (bit 1 left-right, bit 2 up-down).
	const TCHAR* const BlastArtSampleCode = TEXT(
		"float2 uv = UV;\n"
		"float fl = round(Flip);\n"
		"if (fmod(fl, 2.0) >= 1.0) { uv.x = 1.0 - uv.x; }\n"
		"if (fl >= 2.0) { uv.y = 1.0 - uv.y; }\n"
		"float f = clamp(Frame, 0.0, 62.999);\n"
		"float i0 = floor(f);\n"
		"float i1 = i0 + 1.0;\n"
		"float2 c0 = float2(fmod(i0, 8.0), floor(i0 / 8.0));\n"
		"float2 c1 = float2(fmod(i1, 8.0), floor(i1 / 8.0));\n"
		"float4 a = Texture2DSample(Tex, TexSampler, (c0 + uv) / 8.0);\n"
		"float4 b = Texture2DSample(Tex, TexSampler, (c1 + uv) / 8.0);\n"
		"return lerp(a, b, f - i0);\n");

	// Smoke's colour: its tint, shaded by the baked light (G) and front noise (B); none where the fire glows (R).
	const TCHAR* const BlastArtBaseCode = TEXT(
		"float lit = 0.4 + 0.6 * S.g;\n"
		"float shade = 0.85 + 0.3 * S.b;\n"
		"return float3(TintR, TintG, TintB) * lit * shade * (1.0 - saturate(S.r * 2.0));\n");

	// Fire glows as a black body from its heat (R): dark red, orange, yellow, white; smoke over a fire glows orange from below.
	const TCHAR* const BlastArtEmissiveCode = TEXT(
		"float h = saturate(S.r);\n"
		"float3 c = lerp(float3(0.0, 0.0, 0.0), float3(0.35, 0.03, 0.0), saturate(h / 0.15));\n"
		"c = lerp(c, float3(1.0, 0.25, 0.02), saturate((h - 0.15) / 0.25));\n"
		"c = lerp(c, float3(1.0, 0.62, 0.15), saturate((h - 0.4) / 0.3));\n"
		"c = lerp(c, float3(1.0, 0.95, 0.75), saturate((h - 0.7) / 0.3));\n"
		"float3 fire = c * pow(h, 1.5) * Gain;\n"
		"float3 glow = float3(1.0, 0.42, 0.12) * Glow * (1.0 - 0.6 * S.g);\n"
		"return fire + glow;\n");

	/** M_BlastFire or M_BlastSmoke: a camera-facing flipbook sprite, lit translucent, per-instance data as BlastFxData. */
	void BlastArtMakeSprite(IAssetTools& Tools, const TCHAR* Name, UTexture2D* Texture, int32& Failures)
	{
		UMaterial* Material = BlastArtNewMaterial(Tools, Name, Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TranslucencyLightingMode = TLM_VolumetricNonDirectional;
		Material->TwoSided = true;
		Material->bEnableResponsiveAA = true;
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);

		auto* UV = BlastArtNode<UMaterialExpressionTextureCoordinate>(Material, -1400, -200);
		UMaterialExpression* Tex = BlastArtTextureObject(Material, Texture, -1400, -80);
		UMaterialExpression* Frame = BlastArtData(Material, BlastFxData::DataFrame, 0.0f, -1400, 40);
		UMaterialExpression* Flip = BlastArtData(Material, BlastFxData::DataFlip, 0.0f, -1400, 140);
		auto* Sample = BlastArtCustom(Material, TEXT("BlastSample"), CMOT_Float4, BlastArtSampleCode, -1100, -100);
		BlastArtInput(Sample, TEXT("UV"), UV);
		BlastArtInput(Sample, TEXT("Tex"), Tex);
		BlastArtInput(Sample, TEXT("Frame"), Frame);
		BlastArtInput(Sample, TEXT("Flip"), Flip);

		UMaterialExpression* TintR = BlastArtData(Material, BlastFxData::DataTintR, 0.1f, -1100, 200);
		UMaterialExpression* TintG = BlastArtData(Material, BlastFxData::DataTintG, 0.1f, -1100, 300);
		UMaterialExpression* TintB = BlastArtData(Material, BlastFxData::DataTintB, 0.1f, -1100, 400);
		auto* Base = BlastArtCustom(Material, TEXT("BlastBase"), CMOT_Float3, BlastArtBaseCode, -700, -300);
		BlastArtInput(Base, TEXT("S"), Sample);
		BlastArtInput(Base, TEXT("TintR"), TintR);
		BlastArtInput(Base, TEXT("TintG"), TintG);
		BlastArtInput(Base, TEXT("TintB"), TintB);
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);

		auto* Emissive = BlastArtCustom(Material, TEXT("BlastEmissive"), CMOT_Float3, BlastArtEmissiveCode, -700, -100);
		BlastArtInput(Emissive, TEXT("S"), Sample);
		BlastArtInput(Emissive, TEXT("Gain"), BlastArtData(Material, BlastFxData::DataFireGain, 0.0f, -1100, 500));
		BlastArtInput(Emissive, TEXT("Glow"), BlastArtData(Material, BlastFxData::DataGlow, 0.0f, -1100, 600));
		BlastArtUnexposedEmissive(Material, Emissive, -450, -100);

		auto* Rough = BlastArtNode<UMaterialExpressionConstant>(Material, -400, 0);
		Rough->R = 1.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		auto* Spec = BlastArtNode<UMaterialExpressionConstant>(Material, -400, 80);
		Spec->R = 0.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Spec, TEXT(""), MP_Specular);

		auto* Opacity = BlastArtCustom(Material, TEXT("BlastOpacity"), CMOT_Float1, TEXT("return saturate(S.a * Opacity);\n"), -700, 150);
		BlastArtInput(Opacity, TEXT("S"), Sample);
		BlastArtInput(Opacity, TEXT("Opacity"), BlastArtData(Material, BlastFxData::DataOpacity, 1.0f, -1100, 700));
		// Soft where the sprite cuts into the ground or a wall, instead of a hard line.
		auto* Fade = BlastArtNode<UMaterialExpressionDepthFade>(Material, -400, 200);
		Fade->FadeDistanceDefault = 60.0f;
		Fade->InOpacity.Connect(0, Opacity);
		UMaterialEditingLibrary::ConnectMaterialProperty(Fade, TEXT(""), MP_Opacity);
		Failures += BlastArtFinish(Material) ? 0 : 1;
	}

	/** M_BlastSpark: a hot streak along the quad's U, white in the middle, orange at the edge; additive. */
	void BlastArtMakeSpark(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BlastArtNewMaterial(Tools, TEXT("M_BlastSpark"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Additive;
		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		Material->bEnableResponsiveAA = true;
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
		auto* UV = BlastArtNode<UMaterialExpressionTextureCoordinate>(Material, -900, -100);
		auto* Glow = BlastArtCustom(Material, TEXT("BlastSpark"), CMOT_Float3, TEXT(
			"float across = abs(UV.y - 0.5) * 2.0;\n"
			"float along = sin(3.14159 * saturate(UV.x));\n"
			"float core = exp(-across * across * 10.0) * pow(along, 0.6);\n"
			"float3 c = lerp(float3(1.0, 0.45, 0.1), float3(1.0, 0.9, 0.7), pow(core, 4.0));\n"
			"return c * core * Bright * Opacity;\n"), -500, -100);
		BlastArtInput(Glow, TEXT("UV"), UV);
		BlastArtInput(Glow, TEXT("Bright"), BlastArtData(Material, 0, 4.0f, -900, 50));
		BlastArtInput(Glow, TEXT("Opacity"), BlastArtData(Material, 1, 1.0f, -900, 150));
		BlastArtUnexposedEmissive(Material, Glow, -250, -100);
		Failures += BlastArtFinish(Material) ? 0 : 1;
	}

	/** M_BlastHaze: a ring of heat shimmer on a camera-facing quad (refraction only, nothing drawn). */
	void BlastArtMakeHaze(IAssetTools& Tools, int32& Failures)
	{
		UMaterial* Material = BlastArtNewMaterial(Tools, TEXT("M_BlastHaze"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TranslucencyLightingMode = TLM_VolumetricNonDirectional;
		Material->TwoSided = true;
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
		auto* UV = BlastArtNode<UMaterialExpressionTextureCoordinate>(Material, -900, -100);
		auto* Normal = BlastArtCustom(Material, TEXT("BlastHaze"), CMOT_Float3, TEXT(
			"float2 d = UV - 0.5;\n"
			"float r = length(d) * 2.0;\n"
			"float ring = exp(-pow((r - 0.72) / 0.14, 2.0)) * (1.0 - smoothstep(0.9, 1.0, r));\n"
			"float2 n = d / max(length(d), 0.0001) * ring * Strength;\n"
			"return normalize(float3(n, 1.0));\n"), -500, -100);
		BlastArtInput(Normal, TEXT("UV"), UV);
		BlastArtInput(Normal, TEXT("Strength"), BlastArtData(Material, 0, 0.0f, -900, 50));
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT(""), MP_Normal);
		auto* Black = BlastArtNode<UMaterialExpressionConstant>(Material, -400, 100);
		Black->R = 0.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Black, TEXT(""), MP_BaseColor);
		auto* None = BlastArtNode<UMaterialExpressionConstant>(Material, -400, 180);
		None->R = 0.0f;
		UMaterialEditingLibrary::ConnectMaterialProperty(None, TEXT(""), MP_Opacity);
		auto* Index = BlastArtNode<UMaterialExpressionConstant>(Material, -400, 260);
		Index->R = 1.12f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Index, TEXT(""), MP_Refraction);
		Failures += BlastArtFinish(Material) ? 0 : 1;
	}

	/**
	 * M_BlastScorch: the scorch, on a flat square laid on the ground (instanced; data 0 its opacity, 1 its age): a charred core
	 * (mottled soot, grey ash, embers glowing for the first half minute) and rays of soot, pale dust between them, lit translucent. (A deferred decal first, but on the PC it never showed on the ground in rounds 3 and 4.)
	 */
	void BlastArtMakeScorch(IAssetTools& Tools, UTexture2D* Texture, int32& Failures)
	{
		UMaterial* Material = BlastArtNewMaterial(Tools, TEXT("M_BlastScorch"), Failures);
		if (!Material)
		{
			return;
		}
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_DefaultLit);
		Material->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		// Seen from either side, so a square laid the wrong way up still shows.
		Material->TwoSided = true;
		bool bNeedsRecompile = false;
		Material->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
		auto* UV = BlastArtNode<UMaterialExpressionTextureCoordinate>(Material, -1100, -200);
		UMaterialExpression* Tex = BlastArtTextureObject(Material, Texture, -1100, -80);
		auto* Sample = BlastArtCustom(Material, TEXT("BlastScorchSample"), CMOT_Float4, TEXT("return Texture2DSample(Tex, TexSampler, UV);\n"), -800, -150);
		BlastArtInput(Sample, TEXT("UV"), UV);
		BlastArtInput(Sample, TEXT("Tex"), Tex);
		auto* Base = BlastArtCustom(Material, TEXT("BlastScorchColour"), CMOT_Float3, TEXT(
			"float3 soot = float3(0.012, 0.011, 0.01);\n"
			"float3 dust = float3(0.34, 0.31, 0.27);\n"
			"float3 ash = float3(0.17, 0.165, 0.155);\n"
			"float ember = smoothstep(0.85, 0.95, S.b);\n"
			"float3 c = lerp(lerp(dust, soot, saturate(S.r * 1.3)), dust, S.g * (1.0 - S.r));\n"
			"return lerp(c, ash, saturate(S.b / 0.8) * (1.0 - ember));\n"), -500, -250);
		BlastArtInput(Base, TEXT("S"), Sample);
		UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
		// Charred soot catches a little sheen; ash and dust are dull.
		auto* Rough = BlastArtCustom(Material, TEXT("BlastScorchRough"), CMOT_Float1, TEXT(
			"return 0.95 - 0.3 * saturate(S.r * 1.3) * (1.0 - saturate(S.b / 0.8));\n"), -500, -50);
		BlastArtInput(Rough, TEXT("S"), Sample);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		auto* Spec = BlastArtNode<UMaterialExpressionConstant>(Material, -400, 30);
		Spec->R = 0.3f;
		UMaterialEditingLibrary::ConnectMaterialProperty(Spec, TEXT(""), MP_Specular);
		// Embers in the core glow orange and flicker, dying over the first half minute (data 1: the mark's age, seconds).
		auto* Ember = BlastArtCustom(Material, TEXT("BlastScorchEmber"), CMOT_Float3, TEXT(
			"float ember = smoothstep(0.85, 0.95, S.b);\n"
			"float flicker = 0.65 + 0.35 * sin(Age * 6.0 + UV.x * 53.0 + UV.y * 37.0);\n"
			"return float3(1.0, 0.33, 0.06) * ember * flicker * exp(-Age / 9.0) * 1.5;\n"), -500, 150);
		BlastArtInput(Ember, TEXT("S"), Sample);
		BlastArtInput(Ember, TEXT("UV"), UV);
		BlastArtInput(Ember, TEXT("Age"), BlastArtData(Material, 1, 100.0f, -800, 200));
		BlastArtUnexposedEmissive(Material, Ember, -250, 150);
		auto* Opacity = BlastArtCustom(Material, TEXT("BlastScorchOpacity"), CMOT_Float1, TEXT("return saturate(S.a * Fade * 1.2);\n"), -500, 50);
		BlastArtInput(Opacity, TEXT("S"), Sample);
		BlastArtInput(Opacity, TEXT("Fade"), BlastArtData(Material, 0, 1.0f, -800, 100));
		UMaterialEditingLibrary::ConnectMaterialProperty(Opacity, TEXT(""), MP_Opacity);
		Failures += BlastArtFinish(Material) ? 0 : 1;
	}
}

UBlastArtCommandlet::UBlastArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UBlastArtCommandlet::Main(const FString& Params)
{
	const bool bForce = FParse::Param(*Params, TEXT("force"));
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	int32 Failures = 0;
	UTexture2D* Fire = BlastArtFlipbook(TEXT("T_BlastFire"), blastfx::Kind::Fireball, 1, bForce, Failures);
	UTexture2D* Smoke = BlastArtFlipbook(TEXT("T_BlastSmoke"), blastfx::Kind::Smoke, 2, bForce, Failures);
	UTexture2D* Scorch = nullptr;
	if (BlastArtWanted(TEXT("T_BlastScorch"), bForce))
	{
		Scorch = BlastArtTexture(TEXT("T_BlastScorch"), blastfx::MakeScorch(BlastArtScorchSize, 3));
		Failures += Scorch ? 0 : 1;
	}
	else
	{
		Scorch = BlastArtLoadTexture(TEXT("T_BlastScorch"));
	}
	if (!Fire || !Smoke || !Scorch)
	{
		UE_LOG(LogBlastArt, Error, TEXT("The blast textures could not be made or loaded"));
		return 1;
	}
	if (BlastArtWanted(TEXT("M_BlastFire"), bForce))
	{
		BlastArtMakeSprite(Tools, TEXT("M_BlastFire"), Fire, Failures);
	}
	if (BlastArtWanted(TEXT("M_BlastSmoke"), bForce))
	{
		BlastArtMakeSprite(Tools, TEXT("M_BlastSmoke"), Smoke, Failures);
	}
	if (BlastArtWanted(TEXT("M_BlastSpark"), bForce))
	{
		BlastArtMakeSpark(Tools, Failures);
	}
	if (BlastArtWanted(TEXT("M_BlastHaze"), bForce))
	{
		BlastArtMakeHaze(Tools, Failures);
	}
	if (BlastArtWanted(TEXT("M_BlastScorch"), bForce))
	{
		BlastArtMakeScorch(Tools, Scorch, Failures);
	}
	UE_LOG(LogBlastArt, Display, TEXT("Blast art done, %d failures"), Failures);
	return Failures == 0 ? 0 : 1;
}
