#include "WaterArtCommandlet.h"

#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "ObjectTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogWaterArt, Log, All);

namespace
{
	const TCHAR* const WaterArtFolder = TEXT("/Game/Rim3D/Generated/Water");
	// The terrain's non-virtual copy of the Megascans swamp water normal (TerrainMaterials commandlet).
	const TCHAR* const WaterArtNormalPath = TEXT("/Game/Rim3D/Generated/Terrain/Textures/T_water_normal.T_water_normal");
	// The default of the wave textures until the game gives its render targets: flat.
	const TCHAR* const WaterArtBlankPath = TEXT("/Engine/EngineResources/Black.Black");
	constexpr int32 WaterArtPushes = 16;

	// What the water mesh gives each vertex (WaterSubsystem.cpp): UV0 = the map position in metres, UV1 = the flow in
	// metres per real second, UV2.x = the water's depth over the ground in metres (below 0 on the bank above the surface).
	// The two-phase flow map: each layer is pushed along the flow for 4 s, then fades out while a copy half a cycle
	// behind fades in, so no ripple is ever stretched far. Still water drifts with a slow wind.
	const TCHAR* const WaterArtCommon = TEXT(
		"float2 flow = Flow;\n"
		"float speed = length(flow);\n"
		"float2 f = flow + WindDrift;\n"
		"float ph0 = frac(T / Cycle);\n"
		"float ph1 = frac(T / Cycle + 0.5);\n"
		"float wA = 1.0 - abs(2.0 * ph0 - 1.0);\n"
		"float wB = 1.0 - wA;\n"
		"float2 offA = f * (ph0 * Cycle);\n"
		"float2 offB = f * (ph1 * Cycle);\n");

	const TCHAR* const WaterArtNormalCode = TEXT(
		"float2 pA = UV - offA; float2 pB = UV - offB;\n"
		"float2 a1 = Texture2DSample(Tex, TexSampler, pA / Tile1).xy * 2.0 - 1.0;\n"
		"float2 b1 = Texture2DSample(Tex, TexSampler, pB / Tile1 + 0.37).xy * 2.0 - 1.0;\n"
		// The second, finer layer turned a quarter round, so the two never line up.
		"float2 a2 = Texture2DSample(Tex, TexSampler, float2(pA.y, -pA.x) / Tile2 + 0.11).xy * 2.0 - 1.0;\n"
		"float2 b2 = Texture2DSample(Tex, TexSampler, float2(pB.y, -pB.x) / Tile2 + 0.61).xy * 2.0 - 1.0;\n"
		"a2 = float2(-a2.y, a2.x); b2 = float2(-b2.y, b2.x);\n"
		// A third, fine layer (cat's paws, the sun's sparkle) turned the other way.
		"float2 a3 = Texture2DSample(Tex, TexSampler, float2(-pA.y, pA.x) / Tile3 + 0.29).xy * 2.0 - 1.0;\n"
		"float2 b3 = Texture2DSample(Tex, TexSampler, float2(-pB.y, pB.x) / Tile3 + 0.83).xy * 2.0 - 1.0;\n"
		"a3 = float2(a3.y, -a3.x); b3 = float2(b3.y, -b3.x);\n"
		"float keep = rsqrt(max(wA * wA + wB * wB, 0.0001));\n"
		"float2 n = ((a1 * wA + b1 * wB) * 0.5 + (a2 * wA + b2 * wB) * 0.35 + (a3 * wA + b3 * wB) * 0.3) * keep;\n"
		"float strength = Ripple * (0.7 + 1.3 * saturate(speed));\n"
		// Calmer in the last few centimetres at the shore, so the water line is soft.
		"strength *= lerp(0.35, 1.0, saturate(Depth / 0.3));\n"
		// W2: the wave simulation round the camera (USimWaterSubsystem), its slope added to the ripples. WaveOrigin is
		// its corner in map metres (xy) and its size (z); outside it, or with the default black texture, nothing.
		"float2 wuv = (UV - WaveOrigin.xy) / max(WaveOrigin.z, 0.001);\n"
		"float inside = saturate(min(min(wuv.x, wuv.y), min(1.0 - wuv.x, 1.0 - wuv.y)) * 12.0) * saturate(Depth / 0.05 + 0.5);\n"
		"float2 wave = 0;\n"
		"if (inside > 0.0)\n"
		"{\n"
		"    float tx = 1.0 / WaveRes;\n"
		// A height is kept as (above 0, below 0) in red and green (see the step below).
		"    float2 sL = Texture2DSampleLevel(WaveTex, WaveTexSampler, wuv - float2(tx, 0), 0).rg; float hL = sL.x - sL.y;\n"
		"    float2 sR = Texture2DSampleLevel(WaveTex, WaveTexSampler, wuv + float2(tx, 0), 0).rg; float hR = sR.x - sR.y;\n"
		"    float2 sD = Texture2DSampleLevel(WaveTex, WaveTexSampler, wuv - float2(0, tx), 0).rg; float hD = sD.x - sD.y;\n"
		"    float2 sU = Texture2DSampleLevel(WaveTex, WaveTexSampler, wuv + float2(0, tx), 0).rg; float hU = sU.x - sU.y;\n"
		"    wave = -float2(hR - hL, hU - hD) / (2.0 * tx * WaveOrigin.z) * WaveStrength * inside;\n"
		"}\n"
		"return normalize(float3(n * strength + wave, 1.0));\n");

	// W2's wave simulation step, drawn into a 256 x 256 half-float render target that follows the camera (USimWaterSubsystem):
	// the wave equation with damping, the area shifted by whole texels when the camera moves (Shift, in UV), waves only
	// over open water (WaveMask, the map's open water cells), and the bodies' pushes (Imp0..Imp15: map metres x, y,
	// radius in metres, push), each a soft round dip.
	const TCHAR* const WaterArtWaveStepCode = TEXT(
		"float2 src = UV + Shift.xy;\n"
		"float t = 1.0 / Res;\n"
		// Unreal keeps a material's emissive at 0 or more, so a height is kept as (above 0, below 0) in red and green.
		"float2 s0 = Texture2DSampleLevel(Cur, CurSampler, src, 0).rg; float c = s0.x - s0.y;\n"
		"float2 s1 = Texture2DSampleLevel(Prev, PrevSampler, src, 0).rg; float p = s1.x - s1.y;\n"
		"float2 s2 = Texture2DSampleLevel(Cur, CurSampler, src - float2(t, 0), 0).rg;\n"
		"float2 s3 = Texture2DSampleLevel(Cur, CurSampler, src + float2(t, 0), 0).rg;\n"
		"float2 s4 = Texture2DSampleLevel(Cur, CurSampler, src - float2(0, t), 0).rg;\n"
		"float2 s5 = Texture2DSampleLevel(Cur, CurSampler, src + float2(0, t), 0).rg;\n"
		"float l = (s2.x - s2.y) + (s3.x - s3.y) + (s4.x - s4.y) + (s5.x - s5.y) - 4.0 * c;\n"
		"float h = (2.0 * c - p + K * l) * Damp;\n"
		"if (src.x < 0.0 || src.y < 0.0 || src.x > 1.0 || src.y > 1.0) { h = 0.0; }\n"
		"float2 world = Origin.xy + UV * Origin.z;\n"
		"float m = Texture2DSampleLevel(Mask, MaskSampler, world / max(MaskSize.xy, 1.0), 0).r;\n"
		"h *= saturate(m * 1.5 - 0.25);\n"
		"float edge = saturate(min(min(UV.x, UV.y), min(1.0 - UV.x, 1.0 - UV.y)) * Res / 4.0);\n"
		"h *= edge;\n"
		"float push = 0.0;\n"
		"PUSHES\n"
		"h += push * m;\n"
		"h = clamp(h, -4.0, 4.0);\n"
		"return float3(max(h, 0.0), max(-h, 0.0), 0.0);\n");

	const TCHAR* const WaterArtFoamCode = TEXT(
		"float depth = Depth;\n"
		"float nA = Texture2DSample(Tex, TexSampler, (UV - offA) / FoamTile).x;\n"
		"float nB = Texture2DSample(Tex, TexSampler, (UV - offB) / FoamTile + 0.5).x;\n"
		"float noise = nA * wA + nB * wB;\n"
		// Depth is the true depth (it runs smoothly through the bank's line), so the shore band follows the bank. It is a
		// broken froth just inside the water line that fades out on both sides, never a solid white line.
		"float shore = smoothstep(-0.01, 0.03, depth) * (1.0 - smoothstep(0.03, ShoreFoamDepth, depth));\n"
		"float fast = smoothstep(FastFoamFrom, FastFoamFrom + 0.5, speed);\n"
		"float amount = saturate(shore * 0.6 + fast * 0.5);\n"
		"float foam = amount * smoothstep(0.58, 0.86, noise);\n"
		"foam = saturate(foam * FoamStrength);\n"
		"return float4(foam, lerp(Roughness, 0.55, foam), 0.0, 0.0);\n");

	bool WaterArtSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	UMaterialExpression* WaterArtNode(UMaterial* Material, UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	}

	UMaterialExpressionScalarParameter* WaterArtScalar(UMaterial* Material, const TCHAR* Name, float Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionScalarParameter>(WaterArtNode(Material, UMaterialExpressionScalarParameter::StaticClass(), -1400, Y));
		Node->ParameterName = Name;
		Node->DefaultValue = Value;
		return Node;
	}

	UMaterialExpressionVectorParameter* WaterArtVector(UMaterial* Material, const TCHAR* Name, const FLinearColor& Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionVectorParameter>(WaterArtNode(Material, UMaterialExpressionVectorParameter::StaticClass(), -600, Y));
		Node->ParameterName = Name;
		Node->DefaultValue = Value;
		return Node;
	}

	void WaterArtInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 OutputIndex = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(OutputIndex, From);
	}

	UMaterialExpressionComponentMask* WaterArtMask(UMaterial* Material, UMaterialExpression* From, bool R, bool G, int32 Y)
	{
		auto* Mask = Cast<UMaterialExpressionComponentMask>(WaterArtNode(Material, UMaterialExpressionComponentMask::StaticClass(), 100, Y));
		Mask->R = R ? 1 : 0;
		Mask->G = G ? 1 : 0;
		Mask->B = 0;
		Mask->A = 0;
		Mask->Input.Connect(0, From);
		return Mask;
	}

	/** The RGB of a vector parameter (its own output is four wide). */
	UMaterialExpressionComponentMask* WaterArtRgb(UMaterial* Material, UMaterialExpression* From, int32 Y)
	{
		UMaterialExpressionComponentMask* Mask = WaterArtMask(Material, From, true, true, Y);
		Mask->B = 1;
		return Mask;
	}

	UMaterial* WaterArtMakeMaster()
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		// Made again: an older copy is deleted first (CreateAsset will not overwrite it).
		if (UMaterial* Old = LoadObject<UMaterial>(nullptr, *(FString(WaterArtFolder) / TEXT("M_Water.M_Water")), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			ObjectTools::ForceDeleteObjects({Old}, false);
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_Water"), WaterArtFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		Material->SetShadingModel(MSM_SingleLayerWater);
		Material->BlendMode = BLEND_Opaque;
		Material->TwoSided = false;
		// The surface is flat and its UVs run along the world's X and Y, so the ripples are made as a world normal.
		Material->bTangentSpaceNormal = false;

		auto* Position = Cast<UMaterialExpressionTextureCoordinate>(WaterArtNode(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 0));
		Position->CoordinateIndex = 0;
		auto* Flow = Cast<UMaterialExpressionTextureCoordinate>(WaterArtNode(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 80));
		Flow->CoordinateIndex = 1;
		auto* DepthUV = Cast<UMaterialExpressionTextureCoordinate>(WaterArtNode(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 160));
		DepthUV->CoordinateIndex = 2;
		auto* DepthX = WaterArtMask(Material, DepthUV, true, false, 160);
		auto* Time = WaterArtNode(Material, UMaterialExpressionTime::StaticClass(), -1400, 240);
		auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(WaterArtNode(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1100, 320));
		Texture->ParameterName = TEXT("RippleNormal");
		UTexture* Ripples = LoadObject<UTexture>(nullptr, WaterArtNormalPath);
		if (!Ripples)
		{
			UE_LOG(LogWaterArt, Warning, TEXT("%s not found (run the TerrainMaterials commandlet first); the water is flat"), WaterArtNormalPath);
			Ripples = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));
		}
		Texture->Texture = Ripples;
		Texture->SamplerType = SAMPLERTYPE_Normal;

		auto* Cycle = WaterArtScalar(Material, TEXT("FlowCycle"), 4.0f, 400);
		auto* Wind = WaterArtVector(Material, TEXT("WindDrift"), FLinearColor(0.05f, 0.03f, 0.0f, 0.0f), 460);
		auto* WindXY = WaterArtMask(Material, Wind, true, true, 460);

		const auto MakeCustom = [&](const TCHAR* Description, ECustomMaterialOutputType Output, const TCHAR* Body, int32 AtY)
		{
			auto* Custom = Cast<UMaterialExpressionCustom>(WaterArtNode(Material, UMaterialExpressionCustom::StaticClass(), -300, AtY));
			Custom->Description = Description;
			Custom->OutputType = Output;
			Custom->Code = FString(WaterArtCommon) + Body;
			Custom->Inputs.Reset();
			WaterArtInput(Custom, TEXT("UV"), Position);
			WaterArtInput(Custom, TEXT("Flow"), Flow);
			WaterArtInput(Custom, TEXT("T"), Time);
			WaterArtInput(Custom, TEXT("Tex"), Texture);
			WaterArtInput(Custom, TEXT("Cycle"), Cycle);
			WaterArtInput(Custom, TEXT("WindDrift"), WindXY);
			return Custom;
		};
		auto* NormalNode = MakeCustom(TEXT("WaterNormal"), CMOT_Float3, WaterArtNormalCode, 0);
		WaterArtInput(NormalNode, TEXT("Tile1"), WaterArtScalar(Material, TEXT("RippleTile1"), 3.2f, 520));
		WaterArtInput(NormalNode, TEXT("Tile2"), WaterArtScalar(Material, TEXT("RippleTile2"), 1.3f, 580));
		WaterArtInput(NormalNode, TEXT("Tile3"), WaterArtScalar(Material, TEXT("RippleTile3"), 0.45f, 600));
		WaterArtInput(NormalNode, TEXT("Ripple"), WaterArtScalar(Material, TEXT("RippleStrength"), 1.2f, 640));
		WaterArtInput(NormalNode, TEXT("Depth"), DepthX);
		auto* WaveTex = Cast<UMaterialExpressionTextureObjectParameter>(WaterArtNode(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1100, 1400));
		WaveTex->ParameterName = TEXT("WaveTex");
		WaveTex->Texture = LoadObject<UTexture>(nullptr, WaterArtBlankPath);
		WaveTex->SamplerType = SAMPLERTYPE_Color;
		WaterArtInput(NormalNode, TEXT("WaveTex"), WaveTex);
		WaterArtInput(NormalNode, TEXT("WaveOrigin"), WaterArtVector(Material, TEXT("WaveOrigin"), FLinearColor(0.0f, 0.0f, 1.0f, 0.0f), 1460));
		WaterArtInput(NormalNode, TEXT("WaveRes"), WaterArtScalar(Material, TEXT("WaveRes"), 256.0f, 1520));
		WaterArtInput(NormalNode, TEXT("WaveStrength"), WaterArtScalar(Material, TEXT("WaveStrength"), 1.0f, 1580));

		auto* FoamNode = MakeCustom(TEXT("WaterFoam"), CMOT_Float4, WaterArtFoamCode, 500);
		WaterArtInput(FoamNode, TEXT("Depth"), DepthX);
		WaterArtInput(FoamNode, TEXT("FoamTile"), WaterArtScalar(Material, TEXT("FoamTile"), 1.7f, 700));
		WaterArtInput(FoamNode, TEXT("ShoreFoamDepth"), WaterArtScalar(Material, TEXT("ShoreFoamDepth"), 0.15f, 760));
		WaterArtInput(FoamNode, TEXT("FastFoamFrom"), WaterArtScalar(Material, TEXT("FastFoamFrom"), 0.7f, 820));
		WaterArtInput(FoamNode, TEXT("FoamStrength"), WaterArtScalar(Material, TEXT("FoamStrength"), 0.5f, 880));
		WaterArtInput(FoamNode, TEXT("Roughness"), WaterArtScalar(Material, TEXT("Roughness"), 0.02f, 940));

		auto* Foam = WaterArtMask(Material, FoamNode, true, false, 600);
		auto* Rough = WaterArtMask(Material, FoamNode, false, true, 700);
		auto* FoamColor = WaterArtVector(Material, TEXT("FoamColor"), FLinearColor(0.75f, 0.74f, 0.68f), 800);
		UMaterialEditingLibrary::ConnectMaterialProperty(WaterArtRgb(Material, FoamColor, 800), TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Foam, TEXT(""), MP_Opacity);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalNode, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(WaterArtScalar(Material, TEXT("Specular"), 0.255f, 1000), TEXT(""), MP_Specular);
		UMaterialEditingLibrary::ConnectMaterialProperty(WaterArtScalar(Material, TEXT("RefractionIndex"), 1.33f, 1060), TEXT(""), MP_Refraction);

		// Murky lowland tropical water: per metre it takes out blue most, then red, and green least, and scatters a
		// little green-brown back, so the bed shows clearly through the first 30 to 50 cm and is gone by about 1.5 m.
		auto* Out = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(WaterArtNode(Material, UMaterialExpressionSingleLayerWaterMaterialOutput::StaticClass(), 300, 900));
		// The parameters are per metre; Unreal's water shading measures the depth in centimetres, so they are scaled by 0.01
		// (W1 as first built missed this: the water was a hundred times too thick and looked opaque).
		const auto PerCm = [&](UMaterialExpression* From, int32 AtY)
		{
			auto* Mul = Cast<UMaterialExpressionMultiply>(WaterArtNode(Material, UMaterialExpressionMultiply::StaticClass(), 200, AtY));
			Mul->A.Connect(0, From);
			Mul->ConstB = 0.01f;
			return Mul;
		};
		Out->AbsorptionCoefficients.Connect(0, PerCm(WaterArtRgb(Material, WaterArtVector(Material, TEXT("Absorption"), FLinearColor(1.1f, 0.8f, 2.0f), 1100), 1100), 1100));
		Out->ScatteringCoefficients.Connect(0, PerCm(WaterArtRgb(Material, WaterArtVector(Material, TEXT("Scattering"), FLinearColor(0.08f, 0.1f, 0.06f), 1160), 1160), 1160));
		Out->PhaseG.Connect(0, WaterArtScalar(Material, TEXT("PhaseG"), 0.2f, 1220));
		Out->ColorScaleBehindWater.Connect(0, WaterArtScalar(Material, TEXT("ColorScaleBehindWater"), 1.0f, 1280));

		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	/** M_WaterWaveStep: one step of the W2 wave simulation, drawn into a render target (unlit, the height as emissive). */
	UMaterial* WaterArtMakeWaveStep()
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		if (UMaterial* Old = LoadObject<UMaterial>(nullptr, *(FString(WaterArtFolder) / TEXT("M_WaterWaveStep.M_WaterWaveStep")), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			ObjectTools::ForceDeleteObjects({Old}, false);
		}
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_WaterWaveStep"), WaterArtFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		Material->SetShadingModel(MSM_Unlit);
		Material->BlendMode = BLEND_Opaque;

		auto* Custom = Cast<UMaterialExpressionCustom>(WaterArtNode(Material, UMaterialExpressionCustom::StaticClass(), -300, 0));
		Custom->Description = TEXT("WaveStep");
		Custom->OutputType = CMOT_Float3;
		FString Pushes;
		for (int32 I = 0; I < WaterArtPushes; ++I)
		{
			Pushes += FString::Printf(TEXT("{ float2 d = world - Imp%d.xy; float r = max(Imp%d.z, 0.05); push += Imp%d.w * exp(-dot(d, d) / (r * r)); }\n"), I, I, I);
		}
		Custom->Code = FString(WaterArtWaveStepCode).Replace(TEXT("PUSHES"), *Pushes);
		Custom->Inputs.Reset();

		auto* UV = Cast<UMaterialExpressionTextureCoordinate>(WaterArtNode(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 0));
		UV->CoordinateIndex = 0;
		WaterArtInput(Custom, TEXT("UV"), UV);
		int32 Y = 80;
		for (const TCHAR* Name : {TEXT("Cur"), TEXT("Prev"), TEXT("Mask")})
		{
			auto* Tex = Cast<UMaterialExpressionTextureObjectParameter>(WaterArtNode(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1100, Y));
			Tex->ParameterName = FName(FString(TEXT("Wave")) + Name);
			Tex->Texture = LoadObject<UTexture>(nullptr, WaterArtBlankPath);
			Tex->SamplerType = SAMPLERTYPE_Color;
			WaterArtInput(Custom, Name, Tex);
			Y += 80;
		}
		WaterArtInput(Custom, TEXT("Res"), WaterArtScalar(Material, TEXT("Res"), 256.0f, Y));
		WaterArtInput(Custom, TEXT("K"), WaterArtScalar(Material, TEXT("K"), 0.0f, Y + 60));
		WaterArtInput(Custom, TEXT("Damp"), WaterArtScalar(Material, TEXT("Damp"), 0.98f, Y + 120));
		WaterArtInput(Custom, TEXT("Shift"), WaterArtVector(Material, TEXT("Shift"), FLinearColor::Transparent, Y + 180));
		WaterArtInput(Custom, TEXT("Origin"), WaterArtVector(Material, TEXT("Origin"), FLinearColor(0.0f, 0.0f, 64.0f, 0.0f), Y + 240));
		WaterArtInput(Custom, TEXT("MaskSize"), WaterArtVector(Material, TEXT("MaskSize"), FLinearColor(1.0f, 1.0f, 0.0f, 0.0f), Y + 300));
		for (int32 I = 0; I < WaterArtPushes; ++I)
		{
			const FString Name = FString::Printf(TEXT("Imp%d"), I);
			// Output 5 is the parameter's RGBA (output 0 is only RGB).
			WaterArtInput(Custom, *Name, WaterArtVector(Material, *Name, FLinearColor::Transparent, Y + 360 + I * 60), 5);
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	bool WaterArtReportErrors(UMaterial* Material)
	{
		bool bErrors = false;
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			for (const FString& Error : Resource->GetCompileErrors())
			{
				UE_LOG(LogWaterArt, Error, TEXT("%s: %s"), *Material->GetName(), *Error);
				bErrors = true;
			}
		}
		return bErrors;
	}
}

UWaterArtCommandlet::UWaterArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UWaterArtCommandlet::Main(const FString& Params)
{
	UMaterial* Master = WaterArtMakeMaster();
	UMaterial* WaveStep = WaterArtMakeWaveStep();
	if (!Master || !WaveStep)
	{
		UE_LOG(LogWaterArt, Error, TEXT("Could not make the water materials"));
		return 1;
	}
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	const bool bErrors = WaterArtReportErrors(Master) | WaterArtReportErrors(WaveStep);
	const bool bSaved = WaterArtSave(Master) && WaterArtSave(WaveStep);
	UE_LOG(LogWaterArt, Display, TEXT("%s %s/M_Water and M_WaterWaveStep%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), WaterArtFolder, bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
