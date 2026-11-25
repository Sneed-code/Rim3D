#include "SeeThroughArtCommandlet.h"

#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogSeeThroughArt, Log, All);

namespace
{
	const TCHAR* const SeeThroughFolder = TEXT("/Game/Rim3D/Generated/Overseer");

	// Scene texture ids for SceneTextureLookup (MaterialSceneTextureId.h): 1 scene depth, 13 custom depth, 25 custom
	// stencil. A marked pixel is hidden when its custom depth lies more than Gap cm behind the scene's nearest surface.
	// The outline is drawn on unmarked pixels (or marked ones in front) next to a hidden marked pixel, in that thing's
	// colour; the hidden shape itself gets Fill of the colour.
	const TCHAR* const SeeThroughCode = TEXT(
		"float2 uv = GetDefaultSceneTextureUV(Parameters, 1);\n"
		"float2 px = View.BufferSizeAndInvSize.zw * Thickness;\n"
		"float sd = SceneTextureLookup(uv, 1, false).r;\n"
		"float cd = SceneTextureLookup(uv, 13, false).r;\n"
		"float st = floor(SceneTextureLookup(uv, 25, false).r + 0.5);\n"
		"bool hidden = st > 0.5 && cd < 1e6 && cd > sd + Gap;\n"
		"float edgeSt = 0.0;\n"
		"if (!hidden)\n"
		"{\n"
		"  float2 offs[8] = { float2(1,0), float2(-1,0), float2(0,1), float2(0,-1), float2(0.7,0.7), float2(-0.7,0.7), float2(0.7,-0.7), float2(-0.7,-0.7) };\n"
		"  for (int i = 0; i < 8; ++i)\n"
		"  {\n"
		"    float2 nuv = uv + offs[i] * px;\n"
		"    float nst = floor(SceneTextureLookup(nuv, 25, false).r + 0.5);\n"
		"    if (nst > 0.5 && nst != st)\n"
		"    {\n"
		"      float ncd = SceneTextureLookup(nuv, 13, false).r;\n"
		"      float nsd = SceneTextureLookup(nuv, 1, false).r;\n"
		"      if (ncd < 1e6 && ncd > nsd + Gap) { edgeSt = nst; }\n"
		"    }\n"
		"  }\n"
		"}\n"
		"float s = hidden ? st : edgeSt;\n"
		"float3 tint = s < 1.5 ? float3(0.40, 0.95, 0.72) : (s < 2.5 ? float3(1.0, 0.30, 0.25) : (s < 3.5 ? float3(1.0, 0.88, 0.45) : float3(0.70, 0.85, 1.0)));\n"
		"float a = hidden ? Fill : (edgeSt > 0.5 ? Edge : 0.0);\n"
		"return lerp(Scene, tint, a);\n");

	bool SeeThroughSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	void SeeThroughInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From, int32 Output = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(Output, From);
	}

	UMaterialExpression* SeeThroughScene(UMaterial* Material, ESceneTextureId Id, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionSceneTexture>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSceneTexture::StaticClass(), -900, Y));
		Node->SceneTextureId = Id;
		return Node;
	}

	UMaterialExpression* SeeThroughScalar(UMaterial* Material, const TCHAR* Name, float Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -900, Y));
		Node->ParameterName = Name;
		Node->DefaultValue = Value;
		return Node;
	}

	UMaterial* SeeThroughMakeMaterial()
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_SeeThrough"), SeeThroughFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		Material->MaterialDomain = MD_PostProcess;
		Material->BlendableLocation = BL_SceneColorAfterTonemapping;

		auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, 0));
		Custom->Description = TEXT("SeeThrough");
		Custom->OutputType = CMOT_Float3;
		Custom->Code = SeeThroughCode;
		Custom->Inputs.Reset();
		// The scene colour, and the other scene textures wired in so the shader binds them for SceneTextureLookup.
		SeeThroughInput(Custom, TEXT("Scene"), SeeThroughScene(Material, PPI_PostProcessInput0, -200));
		SeeThroughInput(Custom, TEXT("UnusedDepth"), SeeThroughScene(Material, PPI_SceneDepth, -100));
		SeeThroughInput(Custom, TEXT("UnusedCustomDepth"), SeeThroughScene(Material, PPI_CustomDepth, 0));
		SeeThroughInput(Custom, TEXT("UnusedStencil"), SeeThroughScene(Material, PPI_CustomStencil, 100));
		SeeThroughInput(Custom, TEXT("Thickness"), SeeThroughScalar(Material, TEXT("Thickness"), 2.0f, 200));
		SeeThroughInput(Custom, TEXT("Gap"), SeeThroughScalar(Material, TEXT("Gap"), 25.0f, 300));
		SeeThroughInput(Custom, TEXT("Fill"), SeeThroughScalar(Material, TEXT("Fill"), 0.18f, 400));
		SeeThroughInput(Custom, TEXT("Edge"), SeeThroughScalar(Material, TEXT("Edge"), 0.9f, 500));
		UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}
}

USeeThroughArtCommandlet::USeeThroughArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 USeeThroughArtCommandlet::Main(const FString& Params)
{
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(SeeThroughFolder), false, true);
	UMaterial* Material = SeeThroughMakeMaterial();
	if (!Material)
	{
		UE_LOG(LogSeeThroughArt, Error, TEXT("Could not make the see-through material"));
		return 1;
	}
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	bool bErrors = false;
	if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
	{
		for (const FString& Error : Resource->GetCompileErrors())
		{
			UE_LOG(LogSeeThroughArt, Error, TEXT("Material: %s"), *Error);
			bErrors = true;
		}
	}
	const bool bSaved = SeeThroughSave(Material);
	UE_LOG(LogSeeThroughArt, Display, TEXT("%s %s/M_SeeThrough%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), SeeThroughFolder, bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
