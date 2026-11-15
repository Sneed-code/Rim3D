#include "SkyArtCommandlet.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogSkyArt, Log, All);

namespace
{
	const TCHAR* const SkyFolder = TEXT("/Game/Rim3D/Generated/Sky");

	// V is the camera vector (from the pixel toward the eye), so the view direction is -V. Unreal axes here: x north,
	// y east, z up (SkySubsystem). The celestial pole stands Lat above the north horizon; the hour angle is measured
	// west from the meridian, and right ascension is the local sidereal angle (Turn) minus the hour angle. NASA's map is
	// plate carree with 0h in the middle and right ascension growing to the left, as the sky looks from inside.
	const TCHAR* const SkyStarCode = TEXT(
		"float3 d = -normalize(V);\n"
		"float sl = sin(Lat); float cl = cos(Lat);\n"
		"float zc = d.x * cl + d.z * sl;\n"
		"float xh = d.z * cl - d.x * sl;\n"
		"float yh = -d.y;\n"
		"float ha = atan2(yh, xh);\n"
		"float ra = Turn - ha;\n"
		"float dec = asin(clamp(zc, -1.0, 1.0));\n"
		"float2 uv = float2(frac(0.5 - ra / 6.2831853), 0.5 - dec / 3.1415927);\n"
		// Mip 0 only: the map's left and right edges meet in the sky, where a gradient-picked mip would draw a seam.
		"float3 c = Texture2DSampleLevel(Stars, StarsSampler, uv, 0.0).rgb;\n"
		// Under the horizon there is ground; the fade also hides the stars in the haze low down.
		"return c * Brightness * smoothstep(-0.02, 0.12, d.z);\n");

	bool SkySave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	UTexture2D* SkyImportStars()
	{
		const FString File = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/Sky/starmap_2020_8k.exr"));
		if (!FPaths::FileExists(File))
		{
			UE_LOG(LogSkyArt, Error, TEXT("%s is missing: run python tools/sky_textures.py"), *File);
			return nullptr;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = SkyFolder;
		Task->DestinationName = TEXT("T_StarMap");
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Tools.ImportAssetTasks({Task});
		for (UObject* Object : Task->GetObjects())
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Object))
			{
				// BC6H: 8192 x 4096 HDR in 32 MB. The sky is one texture, drawn at mip 0.
				Texture->CompressionSettings = TC_HDR_Compressed;
				Texture->LODGroup = TEXTUREGROUP_Skybox;
				Texture->SRGB = false;
				Texture->MipGenSettings = TMGS_NoMipmaps;
				Texture->NeverStream = true;
				Texture->AddressX = TA_Wrap;
				Texture->AddressY = TA_Clamp;
				Texture->PostEditChange();
				SkySave(Texture);
				return Texture;
			}
		}
		UE_LOG(LogSkyArt, Error, TEXT("Importing %s made no texture"), *File);
		return nullptr;
	}

	UMaterialExpressionScalarParameter* SkyScalar(UMaterial* Material, const TCHAR* Name, float Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -900, Y));
		Node->ParameterName = Name;
		Node->DefaultValue = Value;
		return Node;
	}

	void SkyInput(UMaterialExpressionCustom* Custom, const TCHAR* Name, UMaterialExpression* From)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = Name;
		Input.Input.Connect(0, From);
	}

	UMaterial* SkyMakeMaterial(UTexture2D* Stars)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_StarSky"), SkyFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		// Light added on top of the sky atmosphere, seen from inside the sphere, never fogged (the fog would hide it).
		Material->BlendMode = BLEND_Additive;
		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		Material->bUseTranslucencyVertexFog = false;
		Material->bComputeFogPerPixel = false;

		auto* Custom = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -300, 0));
		Custom->Description = TEXT("StarSky");
		Custom->OutputType = CMOT_Float3;
		Custom->Code = SkyStarCode;
		Custom->Inputs.Reset();
		SkyInput(Custom, TEXT("V"), UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCameraVectorWS::StaticClass(), -900, -100));
		auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -900, 0));
		Texture->ParameterName = TEXT("Stars");
		Texture->Texture = Stars;
		Texture->SamplerType = SAMPLERTYPE_LinearColor;
		SkyInput(Custom, TEXT("Stars"), Texture);
		SkyInput(Custom, TEXT("Lat"), SkyScalar(Material, TEXT("StarLatitude"), 0.1f, 150));
		SkyInput(Custom, TEXT("Turn"), SkyScalar(Material, TEXT("StarTurn"), 0.0f, 250));
		SkyInput(Custom, TEXT("Brightness"), SkyScalar(Material, TEXT("StarBrightness"), 1.0f, 350));
		UMaterialEditingLibrary::ConnectMaterialProperty(Custom, TEXT(""), MP_EmissiveColor);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}
}

USkyArtCommandlet::USkyArtCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 USkyArtCommandlet::Main(const FString& Params)
{
	// Start clean: saving over the last run's files fails while they exist.
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(SkyFolder), false, true);
	UTexture2D* Stars = SkyImportStars();
	UMaterial* Material = Stars ? SkyMakeMaterial(Stars) : nullptr;
	if (!Material)
	{
		UE_LOG(LogSkyArt, Error, TEXT("Could not make the star sky"));
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
			UE_LOG(LogSkyArt, Error, TEXT("Material: %s"), *Error);
			bErrors = true;
		}
	}
	const bool bSaved = SkySave(Material);
	UE_LOG(LogSkyArt, Display, TEXT("%s %s/M_StarSky%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), SkyFolder, bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
