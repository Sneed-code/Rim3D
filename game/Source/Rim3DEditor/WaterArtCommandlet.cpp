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
		"float keep = rsqrt(max(wA * wA + wB * wB, 0.0001));\n"
		"float2 n = ((a1 * wA + b1 * wB) * 0.6 + (a2 * wA + b2 * wB) * 0.4) * keep;\n"
		"float strength = Ripple * (0.6 + 1.4 * saturate(speed));\n"
		"return normalize(float3(n * strength, 1.0));\n");

	const TCHAR* const WaterArtFoamCode = TEXT(
		"float depth = Depth;\n"
		"float nA = Texture2DSample(Tex, TexSampler, (UV - offA) / FoamTile).x;\n"
		"float nB = Texture2DSample(Tex, TexSampler, (UV - offB) / FoamTile + 0.5).x;\n"
		"float noise = nA * wA + nB * wB;\n"
		"float shore = 1.0 - smoothstep(0.0, ShoreFoamDepth, depth);\n"
		"float fast = smoothstep(FastFoamFrom, FastFoamFrom + 0.5, speed);\n"
		"float amount = saturate(shore * 0.9 + fast * 0.5);\n"
		"float foam = amount * smoothstep(0.62 - 0.35 * amount, 0.8 - 0.3 * amount, noise);\n"
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
		WaterArtInput(NormalNode, TEXT("Ripple"), WaterArtScalar(Material, TEXT("RippleStrength"), 0.55f, 640));

		auto* FoamNode = MakeCustom(TEXT("WaterFoam"), CMOT_Float4, WaterArtFoamCode, 500);
		WaterArtInput(FoamNode, TEXT("Depth"), DepthX);
		WaterArtInput(FoamNode, TEXT("FoamTile"), WaterArtScalar(Material, TEXT("FoamTile"), 1.7f, 700));
		WaterArtInput(FoamNode, TEXT("ShoreFoamDepth"), WaterArtScalar(Material, TEXT("ShoreFoamDepth"), 0.2f, 760));
		WaterArtInput(FoamNode, TEXT("FastFoamFrom"), WaterArtScalar(Material, TEXT("FastFoamFrom"), 0.7f, 820));
		WaterArtInput(FoamNode, TEXT("FoamStrength"), WaterArtScalar(Material, TEXT("FoamStrength"), 0.7f, 880));
		WaterArtInput(FoamNode, TEXT("Roughness"), WaterArtScalar(Material, TEXT("Roughness"), 0.04f, 940));

		auto* Foam = WaterArtMask(Material, FoamNode, true, false, 600);
		auto* Rough = WaterArtMask(Material, FoamNode, false, true, 700);
		auto* FoamColor = WaterArtVector(Material, TEXT("FoamColor"), FLinearColor(0.75f, 0.74f, 0.68f), 800);
		UMaterialEditingLibrary::ConnectMaterialProperty(WaterArtRgb(Material, FoamColor, 800), TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Foam, TEXT(""), MP_Opacity);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalNode, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(WaterArtScalar(Material, TEXT("Specular"), 0.255f, 1000), TEXT(""), MP_Specular);
		UMaterialEditingLibrary::ConnectMaterialProperty(WaterArtScalar(Material, TEXT("RefractionIndex"), 1.33f, 1060), TEXT(""), MP_Refraction);

		// Murky lowland tropical water: per metre it takes out blue and red more than green, and scatters a little
		// green-brown back, so the first 20 to 30 cm are nearly clear and a metre down the bed is gone.
		auto* Out = Cast<UMaterialExpressionSingleLayerWaterMaterialOutput>(WaterArtNode(Material, UMaterialExpressionSingleLayerWaterMaterialOutput::StaticClass(), 300, 900));
		Out->AbsorptionCoefficients.Connect(0, WaterArtRgb(Material, WaterArtVector(Material, TEXT("Absorption"), FLinearColor(0.9f, 0.6f, 1.6f), 1100), 1100));
		Out->ScatteringCoefficients.Connect(0, WaterArtRgb(Material, WaterArtVector(Material, TEXT("Scattering"), FLinearColor(0.05f, 0.06f, 0.035f), 1160), 1160));
		Out->PhaseG.Connect(0, WaterArtScalar(Material, TEXT("PhaseG"), 0.2f, 1220));
		Out->ColorScaleBehindWater.Connect(0, WaterArtScalar(Material, TEXT("ColorScaleBehindWater"), 1.0f, 1280));

		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
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
	if (!Master)
	{
		UE_LOG(LogWaterArt, Error, TEXT("Could not make the water material"));
		return 1;
	}
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	bool bErrors = false;
	if (const FMaterialResource* Resource = Master->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		for (const FString& Error : Resource->GetCompileErrors())
		{
			UE_LOG(LogWaterArt, Error, TEXT("Material: %s"), *Error);
			bErrors = true;
		}
	}
	const bool bSaved = WaterArtSave(Master);
	UE_LOG(LogWaterArt, Display, TEXT("%s %s/M_Water%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), WaterArtFolder, bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
