#include "TerrainMaterialsCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogTerrainMaterials, Log, All);

namespace
{
	const TCHAR* const TerrFolder = TEXT("/Game/Rim3D/Generated/Terrain");
	const TCHAR* const TerrLayers[4] = {TEXT("grass"), TEXT("sand"), TEXT("gravel"), TEXT("rock")};
	const TCHAR* const TerrLayerNames[4] = {TEXT("Grass"), TEXT("Sand"), TEXT("Gravel"), TEXT("Rock")};

	// What the terrain mesh gives each vertex (TerrainSubsystem.cpp, FTerrMesh):
	//   vertex colour RGB = a tint (0.5 is 1.0), A = 1 where the colour is used as it is (roofs, and no texture)
	//   UV1 = (sand, rock) weights, UV2 = (gravel, water) weights; the rest is grass.
	// Steep ground turns to rock here, from the vertex normal.
	const TCHAR* const TerrCommon = TEXT(
		"float3 n = normalize(N);\n"
		"float sand = UV1.x; float rock = UV1.y; float gravel = UV2.x; float water = UV2.y;\n"
		"float grass = saturate(1.0 - sand - rock - gravel - water);\n"
		"float steep = (1.0 - smoothstep(SlopeFull, SlopeFrom, n.z)) * (1.0 - water);\n"
		"grass *= 1.0 - steep; sand *= 1.0 - steep; gravel *= 1.0 - steep; rock = rock * (1.0 - steep) + steep;\n"
		"float total = max(grass + sand + gravel + rock + water, 0.0001);\n"
		"grass /= total; sand /= total; gravel /= total; rock /= total; water /= total;\n"
		"float3 tw = pow(abs(n), 4.0); tw /= (tw.x + tw.y + tw.z);\n"
		"float2 uvG = P.xy / (TileGrass * 100.0);\n"
		"float2 uvS = P.xy / (TileSand * 100.0);\n"
		"float2 uvV = P.xy / (TileGravel * 100.0);\n"
		"float2 rockX = P.yz / (TileRock * 100.0);\n"
		"float2 rockY = P.xz / (TileRock * 100.0);\n"
		"float2 rockZ = P.xy / (TileRock * 100.0);\n");

	const TCHAR* const TerrColorCode = TEXT(
		"float3 land = grass * Texture2DSample(GrassC, GrassCSampler, uvG).rgb\n"
		"  + sand * Texture2DSample(SandC, SandCSampler, uvS).rgb\n"
		"  + gravel * Texture2DSample(GravelC, GravelCSampler, uvV).rgb\n"
		"  + rock * (tw.x * Texture2DSample(RockC, RockCSampler, rockX).rgb + tw.y * Texture2DSample(RockC, RockCSampler, rockY).rgb + tw.z * Texture2DSample(RockC, RockCSampler, rockZ).rgb);\n"
		// Big soft patches of lighter and darker, so a repeat of the texture does not show.
		"float2 q = P.xy / 1100.0; float2 qi = floor(q); float2 qf = frac(q); qf = qf * qf * (3.0 - 2.0 * qf);\n"
		"float h00 = frac(sin(dot(qi, float2(12.9898, 78.233))) * 43758.5453);\n"
		"float h10 = frac(sin(dot(qi + float2(1, 0), float2(12.9898, 78.233))) * 43758.5453);\n"
		"float h01 = frac(sin(dot(qi + float2(0, 1), float2(12.9898, 78.233))) * 43758.5453);\n"
		"float h11 = frac(sin(dot(qi + float2(1, 1), float2(12.9898, 78.233))) * 43758.5453);\n"
		"float patch = lerp(lerp(h00, h10, qf.x), lerp(h01, h11, qf.x), qf.y);\n"
		"land *= float3(VCR, VCG, VCB) * 2.0 * lerp(0.8, 1.2, patch);\n"
		// Wet ground (U13, from the rain): darker, and shinier than it is dry. Water keeps its own look.
		"float wet = saturate(Wetness) * (1.0 - water);\n"
		"land *= lerp(1.0, 0.55, wet);\n"
		"float rough = grass * RoughGrass + sand * RoughSand + gravel * RoughGravel + rock * RoughRock + water * WaterRough;\n"
		"rough *= lerp(1.0, 0.3, wet);\n"
		"float3 color = land + water * WaterColor;\n"
		"color = lerp(color, float3(VCR, VCG, VCB), VCA);\n"
		"rough = lerp(rough, 0.9, VCA);\n"
		"return float4(color, rough);\n");

	const TCHAR* const TerrNormalCode = TEXT(
		"float2 nGrass = Texture2DSample(GrassN, GrassNSampler, uvG).xy * 2.0 - 1.0;\n"
		"float2 nSand = Texture2DSample(SandN, SandNSampler, uvS).xy * 2.0 - 1.0;\n"
		"float2 nGravel = Texture2DSample(GravelN, GravelNSampler, uvV).xy * 2.0 - 1.0;\n"
		"float2 nRx = Texture2DSample(RockN, RockNSampler, rockX).xy * 2.0 - 1.0;\n"
		"float2 nRy = Texture2DSample(RockN, RockNSampler, rockY).xy * 2.0 - 1.0;\n"
		"float2 nRz = Texture2DSample(RockN, RockNSampler, rockZ).xy * 2.0 - 1.0;\n"
		// Each texture's bumps are added to the surface's own normal, along the directions its projection runs.
		"float3 bump = float3(grass * nGrass + sand * nSand + gravel * nGravel, 0.0);\n"
		"bump += rock * (tw.x * float3(0.0, nRx.x, nRx.y) + tw.y * float3(nRy.x, 0.0, nRy.y) + tw.z * float3(nRz.x, nRz.y, 0.0));\n"
		"bump *= (1.0 - water) * (1.0 - VCA);\n"
		"return normalize(n + bump);\n");

	bool TerrSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	/** Imports <layer>_<kind>.jpg or .png from the downloaded textures and sets it up as a colour or a normal map. */
	UTexture2D* TerrImportTexture(const FString& Layer, const TCHAR* Kind, bool bNormal)
	{
		FString File;
		for (const TCHAR* Extension : {TEXT("jpg"), TEXT("png")})
		{
			const FString Candidate = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/TerrainTextures"), FString::Printf(TEXT("%s_%s.%s"), *Layer, Kind, Extension));
			if (FPaths::FileExists(Candidate))
			{
				File = Candidate;
				break;
			}
		}
		if (File.IsEmpty())
		{
			UE_LOG(LogTerrainMaterials, Error, TEXT("No %s_%s.jpg or .png in game/Content/ThirdParty/TerrainTextures: run python tools/terrain_textures.py"), *Layer, Kind);
			return nullptr;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FString(TerrFolder) / TEXT("Textures");
		Task->DestinationName = FString::Printf(TEXT("T_%s_%s"), *Layer, Kind);
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Tools.ImportAssetTasks({Task});
		for (UObject* Object : Task->GetObjects())
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Object))
			{
				Texture->LODGroup = bNormal ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
				Texture->CompressionSettings = bNormal ? TC_Normalmap : TC_Default;
				Texture->SRGB = !bNormal;
				Texture->AddressX = TA_Wrap;
				Texture->AddressY = TA_Wrap;
				Texture->PostEditChange();
				TerrSave(Texture);
				return Texture;
			}
		}
		UE_LOG(LogTerrainMaterials, Error, TEXT("Importing %s made no texture"), *File);
		return nullptr;
	}

	UMaterialExpression* TerrNode(UMaterial* Material, UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	}

	UMaterialExpressionScalarParameter* TerrScalar(UMaterial* Material, const FString& Name, double Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionScalarParameter>(TerrNode(Material, UMaterialExpressionScalarParameter::StaticClass(), -1400, Y));
		Node->ParameterName = *Name;
		Node->DefaultValue = static_cast<float>(Value);
		return Node;
	}

	UMaterialExpressionTextureObjectParameter* TerrTexture(UMaterial* Material, const FString& Name, bool bNormal, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionTextureObjectParameter>(TerrNode(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1100, Y));
		Node->ParameterName = *Name;
		Node->Texture = LoadObject<UTexture>(nullptr, bNormal ? TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal") : TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
		Node->SamplerType = bNormal ? SAMPLERTYPE_Normal : SAMPLERTYPE_Color;
		return Node;
	}

	/** One named input of a custom node, wired from an expression's output. */
	void TerrInput(UMaterialExpressionCustom* Custom, const FString& Name, UMaterialExpression* From, int32 OutputIndex = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = *Name;
		Input.Input.Connect(OutputIndex, From);
	}

	UMaterial* TerrMakeMaster(const TSharedPtr<FJsonObject>& Root)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_Terrain"), TerrFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		// The ground and the rock blocks are seen from both sides (a tunnel's roof from below), and the normal
		// is made in world space from the surface's own and the textures' bumps.
		Material->TwoSided = true;
		Material->bTangentSpaceNormal = false;

		auto* Position = TerrNode(Material, UMaterialExpressionWorldPosition::StaticClass(), -1400, 0);
		auto* Normal = TerrNode(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -1400, 100);
		auto* Weights1 = Cast<UMaterialExpressionTextureCoordinate>(TerrNode(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 200));
		Weights1->CoordinateIndex = 1;
		auto* Weights2 = Cast<UMaterialExpressionTextureCoordinate>(TerrNode(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -1400, 300));
		Weights2->CoordinateIndex = 2;
		auto* VertexColor = TerrNode(Material, UMaterialExpressionVertexColor::StaticClass(), -1400, 400);

		const TSharedPtr<FJsonObject> Slope = Root->GetObjectField(TEXT("slope"));
		const TSharedPtr<FJsonObject> Water = Root->GetObjectField(TEXT("water"));
		const TArray<TSharedPtr<FJsonValue>>& WaterColor = Water->GetArrayField(TEXT("color"));
		const TSharedPtr<FJsonObject> Layers = Root->GetObjectField(TEXT("layers"));

		// Both custom nodes read the same weights, position, slope numbers and tile sizes.
		const auto MakeCustom = [&](const TCHAR* Description, ECustomMaterialOutputType Output, const TCHAR* Body, int32 X, int32 AtY)
		{
			auto* Custom = Cast<UMaterialExpressionCustom>(TerrNode(Material, UMaterialExpressionCustom::StaticClass(), X, AtY));
			Custom->Description = Description;
			Custom->OutputType = Output;
			Custom->Code = FString(TerrCommon) + Body;
			Custom->Inputs.Reset();
			TerrInput(Custom, TEXT("P"), Position);
			TerrInput(Custom, TEXT("N"), Normal);
			TerrInput(Custom, TEXT("UV1"), Weights1);
			TerrInput(Custom, TEXT("UV2"), Weights2);
			TerrInput(Custom, TEXT("VCA"), VertexColor, 4);
			return Custom;
		};
		auto* ColorNode = MakeCustom(TEXT("TerrainColor"), CMOT_Float4, TerrColorCode, -300, 0);
		auto* NormalNode = MakeCustom(TEXT("TerrainNormal"), CMOT_Float3, TerrNormalCode, -300, 600);

		const auto BothGet = [&](const FString& Name, double Value, int32 Y)
		{
			UMaterialExpressionScalarParameter* Node = TerrScalar(Material, Name, Value, Y);
			TerrInput(ColorNode, Name, Node);
			TerrInput(NormalNode, Name, Node);
		};
		BothGet(TEXT("SlopeFrom"), Slope->GetNumberField(TEXT("rockFrom")), 500);
		BothGet(TEXT("SlopeFull"), Slope->GetNumberField(TEXT("rockFull")), 560);
		int32 Y = 620;
		for (int32 I = 0; I < 4; ++I, Y += 60)
		{
			BothGet(FString::Printf(TEXT("Tile%s"), TerrLayerNames[I]), Layers->GetObjectField(TerrLayers[I])->GetNumberField(TEXT("tileMetres")), Y);
		}

		// Colour and roughness only: the tint, the roughness numbers and water.
		TerrInput(ColorNode, TEXT("VCR"), VertexColor, 1);
		TerrInput(ColorNode, TEXT("VCG"), VertexColor, 2);
		TerrInput(ColorNode, TEXT("VCB"), VertexColor, 3);
		auto* WaterColorNode = Cast<UMaterialExpressionVectorParameter>(TerrNode(Material, UMaterialExpressionVectorParameter::StaticClass(), -1400, 900));
		WaterColorNode->ParameterName = TEXT("WaterColor");
		WaterColorNode->DefaultValue = FLinearColor(static_cast<float>(WaterColor[0]->AsNumber()), static_cast<float>(WaterColor[1]->AsNumber()), static_cast<float>(WaterColor[2]->AsNumber()), 1.0f);
		TerrInput(ColorNode, TEXT("WaterColor"), WaterColorNode, 0);
		TerrInput(ColorNode, TEXT("WaterRough"), TerrScalar(Material, TEXT("WaterRough"), Water->GetNumberField(TEXT("roughness")), 960));
		TerrInput(ColorNode, TEXT("Wetness"), TerrScalar(Material, TEXT("Wetness"), 0.0, 990));
		Y = 1020;
		for (int32 I = 0; I < 4; ++I, Y += 60)
		{
			const FString Name = FString::Printf(TEXT("Rough%s"), TerrLayerNames[I]);
			TerrInput(ColorNode, Name, TerrScalar(Material, Name, Layers->GetObjectField(TerrLayers[I])->GetNumberField(TEXT("roughness")), Y));
		}
		Y = 1300;
		for (int32 I = 0; I < 4; ++I, Y += 160)
		{
			TerrInput(ColorNode, FString::Printf(TEXT("%sC"), TerrLayerNames[I]), TerrTexture(Material, FString::Printf(TEXT("%sColor"), TerrLayerNames[I]), false, Y));
			TerrInput(NormalNode, FString::Printf(TEXT("%sN"), TerrLayerNames[I]), TerrTexture(Material, FString::Printf(TEXT("%sNormal"), TerrLayerNames[I]), true, Y + 80));
		}

		auto* Rgb = Cast<UMaterialExpressionComponentMask>(TerrNode(Material, UMaterialExpressionComponentMask::StaticClass(), 100, 0));
		Rgb->R = Rgb->G = Rgb->B = 1;
		Rgb->A = 0;
		Rgb->Input.Connect(0, ColorNode);
		auto* Alpha = Cast<UMaterialExpressionComponentMask>(TerrNode(Material, UMaterialExpressionComponentMask::StaticClass(), 100, 200));
		Alpha->R = Alpha->G = Alpha->B = 0;
		Alpha->A = 1;
		Alpha->Input.Connect(0, ColorNode);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rgb, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Alpha, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalNode, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	UMaterialInstanceConstant* TerrMakeInstance(UMaterial* Master, const TSharedPtr<FJsonObject>& Root, const TMap<FString, UTexture2D*>& Textures)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Master;
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(TEXT("MI_Terrain"), TerrFolder, UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject> Layers = Root->GetObjectField(TEXT("layers"));
		for (int32 I = 0; I < 4; ++I)
		{
			const TSharedPtr<FJsonObject> Layer = Layers->GetObjectField(TerrLayers[I]);
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, *FString::Printf(TEXT("%sColor"), TerrLayerNames[I]), Textures.FindRef(FString::Printf(TEXT("%s_color"), TerrLayers[I])));
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, *FString::Printf(TEXT("%sNormal"), TerrLayerNames[I]), Textures.FindRef(FString::Printf(TEXT("%s_normal"), TerrLayers[I])));
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, *FString::Printf(TEXT("Tile%s"), TerrLayerNames[I]), static_cast<float>(Layer->GetNumberField(TEXT("tileMetres"))));
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, *FString::Printf(TEXT("Rough%s"), TerrLayerNames[I]), static_cast<float>(Layer->GetNumberField(TEXT("roughness"))));
		}
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		return Instance;
	}
}

UTerrainMaterialsCommandlet::UTerrainMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UTerrainMaterialsCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/terrain_textures.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogTerrainMaterials, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	// Start clean: saving over the last run's files fails while they exist.
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(TerrFolder), false, true);

	TMap<FString, UTexture2D*> Textures;
	for (const TCHAR* Layer : TerrLayers)
	{
		for (const bool bNormal : {false, true})
		{
			const TCHAR* Kind = bNormal ? TEXT("normal") : TEXT("color");
			UTexture2D* Texture = TerrImportTexture(Layer, Kind, bNormal);
			if (!Texture)
			{
				return 1;
			}
			Textures.Add(FString::Printf(TEXT("%s_%s"), Layer, Kind), Texture);
		}
	}
	UMaterial* Master = TerrMakeMaster(Root);
	UMaterialInstanceConstant* Instance = Master ? TerrMakeInstance(Master, Root, Textures) : nullptr;
	if (!Instance)
	{
		UE_LOG(LogTerrainMaterials, Error, TEXT("Could not make the terrain material"));
		return 1;
	}
	// Compile the shaders now, so mistakes in the material show here and not on the first frame in the game.
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	bool bErrors = false;
	if (const FMaterialResource* Resource = Master->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		for (const FString& Error : Resource->GetCompileErrors())
		{
			UE_LOG(LogTerrainMaterials, Error, TEXT("Material: %s"), *Error);
			bErrors = true;
		}
	}
	const bool bSaved = TerrSave(Master) && TerrSave(Instance);
	UE_LOG(LogTerrainMaterials, Display, TEXT("%s %s/MI_Terrain%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), TerrFolder, bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
