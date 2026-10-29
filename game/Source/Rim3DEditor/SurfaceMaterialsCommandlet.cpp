#include "SurfaceMaterialsCommandlet.h"

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

DEFINE_LOG_CATEGORY_STATIC(LogSurfaceMaterials, Log, All);

namespace
{
	const TCHAR* const SurfFolder = TEXT("/Game/Rim3D/Generated/Surfaces");

	// The textures run along the world's axes, chosen by the surface's normal: walls side by side run on without a seam and
	// the generated building meshes need no UVs. Brick courses and boards lie level on the sides.
	const TCHAR* const SurfCommon = TEXT(
		"float3 n = normalize(N);\n"
		"float3 tw = pow(abs(n), 4.0); tw /= (tw.x + tw.y + tw.z);\n"
		"float s = 1.0 / (Tile * 100.0);\n"
		"float2 uvX = float2(P.y, -P.z) * s;\n"
		"float2 uvY = float2(P.x, -P.z) * s;\n"
		"float2 uvZ = P.xy * s;\n");

	const TCHAR* const SurfColorCode = TEXT(
		"float3 c = tw.x * Texture2DSample(Col, ColSampler, uvX).rgb + tw.y * Texture2DSample(Col, ColSampler, uvY).rgb + tw.z * Texture2DSample(Col, ColSampler, uvZ).rgb;\n"
		"float r = tw.x * Texture2DSample(Rough, RoughSampler, uvX).r + tw.y * Texture2DSample(Rough, RoughSampler, uvY).r + tw.z * Texture2DSample(Rough, RoughSampler, uvZ).r;\n"
		"float luma = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
		// The part's own shade (vertex colour) darkens a wall's base course and cap a little.
		"c = lerp(luma.xxx, c, Saturation) * Tint.rgb * float3(VCR, VCG, VCB);\n"
		"return float4(c, saturate(r * RoughScale));\n");

	// DirectX normal maps: green runs down the texture, which is down the wall on the sides.
	const TCHAR* const SurfNormalCode = TEXT(
		"float2 nX = Texture2DSample(Nrm, NrmSampler, uvX).xy * 2.0 - 1.0;\n"
		"float2 nY = Texture2DSample(Nrm, NrmSampler, uvY).xy * 2.0 - 1.0;\n"
		"float2 nZ = Texture2DSample(Nrm, NrmSampler, uvZ).xy * 2.0 - 1.0;\n"
		"float3 bump = tw.x * float3(0.0, nX.x, -nX.y) + tw.y * float3(nY.x, 0.0, -nY.y) + tw.z * float3(nZ.x, nZ.y, 0.0);\n"
		"return normalize(n + bump);\n");

	bool SurfSave(UObject* Object)
	{
		UPackage* Package = Object->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Package, Object, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
	}

	enum class ESurfKind : uint8
	{
		Color,
		Normal,
		Rough,
	};

	/** Imports <surface>_<kind>.jpg or .png from the downloaded textures and sets it up as a colour, normal or roughness map. */
	UTexture2D* SurfImportTexture(const FString& Surface, const TCHAR* Kind, ESurfKind Use)
	{
		FString File;
		for (const TCHAR* Extension : {TEXT("jpg"), TEXT("png")})
		{
			const FString Candidate = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("ThirdParty/StuffTextures"), FString::Printf(TEXT("%s_%s.%s"), *Surface, Kind, Extension));
			if (FPaths::FileExists(Candidate))
			{
				File = Candidate;
				break;
			}
		}
		if (File.IsEmpty())
		{
			UE_LOG(LogSurfaceMaterials, Error, TEXT("No %s_%s.jpg or .png in game/Content/ThirdParty/StuffTextures: run python tools/stuff_textures.py"), *Surface, Kind);
			return nullptr;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = FString(SurfFolder) / TEXT("Textures");
		Task->DestinationName = FString::Printf(TEXT("T_%s_%s"), *Surface, Kind);
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		Tools.ImportAssetTasks({Task});
		for (UObject* Object : Task->GetObjects())
		{
			if (UTexture2D* Texture = Cast<UTexture2D>(Object))
			{
				Texture->LODGroup = Use == ESurfKind::Normal ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
				Texture->CompressionSettings = Use == ESurfKind::Normal ? TC_Normalmap : (Use == ESurfKind::Rough ? TC_Masks : TC_Default);
				Texture->SRGB = Use == ESurfKind::Color;
				Texture->AddressX = TA_Wrap;
				Texture->AddressY = TA_Wrap;
				Texture->PostEditChange();
				SurfSave(Texture);
				return Texture;
			}
		}
		UE_LOG(LogSurfaceMaterials, Error, TEXT("Importing %s made no texture"), *File);
		return nullptr;
	}

	UMaterialExpression* SurfNode(UMaterial* Material, UClass* Class, int32 X, int32 Y)
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	}

	UMaterialExpressionScalarParameter* SurfScalar(UMaterial* Material, const TCHAR* Name, float Value, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionScalarParameter>(SurfNode(Material, UMaterialExpressionScalarParameter::StaticClass(), -1400, Y));
		Node->ParameterName = Name;
		Node->DefaultValue = Value;
		return Node;
	}

	UMaterialExpressionTextureObjectParameter* SurfTexture(UMaterial* Material, const TCHAR* Name, ESurfKind Use, int32 Y)
	{
		auto* Node = Cast<UMaterialExpressionTextureObjectParameter>(SurfNode(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -1100, Y));
		Node->ParameterName = Name;
		switch (Use)
		{
			case ESurfKind::Normal:
				Node->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal"));
				Node->SamplerType = SAMPLERTYPE_Normal;
				break;
			case ESurfKind::Rough:
				Node->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
				Node->SamplerType = SAMPLERTYPE_Masks;
				break;
			default:
				Node->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
				Node->SamplerType = SAMPLERTYPE_Color;
				break;
		}
		return Node;
	}

	void SurfInput(UMaterialExpressionCustom* Custom, const FString& Name, UMaterialExpression* From, int32 OutputIndex = 0)
	{
		FCustomInput& Input = Custom->Inputs.AddDefaulted_GetRef();
		Input.InputName = *Name;
		Input.Input.Connect(OutputIndex, From);
	}

	UMaterial* SurfMakeMaster()
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterial* Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_Surface"), SurfFolder, UMaterial::StaticClass(), NewObject<UMaterialFactoryNew>()));
		if (!Material)
		{
			return nullptr;
		}
		// The normal is made in world space from the surface's own and the textures' bumps, like the terrain's.
		Material->bTangentSpaceNormal = false;
		bool bRecompile = false;
		UMaterialEditingLibrary::SetMaterialUsage(Material, MATUSAGE_InstancedStaticMeshes, bRecompile);

		auto* Position = SurfNode(Material, UMaterialExpressionWorldPosition::StaticClass(), -1400, 0);
		auto* Normal = SurfNode(Material, UMaterialExpressionVertexNormalWS::StaticClass(), -1400, 100);
		auto* VertexColor = SurfNode(Material, UMaterialExpressionVertexColor::StaticClass(), -1400, 200);
		auto* Tile = SurfScalar(Material, TEXT("Tile"), 2.0f, 300);

		const auto MakeCustom = [&](const TCHAR* Description, ECustomMaterialOutputType Output, const TCHAR* Body, int32 AtY)
		{
			auto* Custom = Cast<UMaterialExpressionCustom>(SurfNode(Material, UMaterialExpressionCustom::StaticClass(), -300, AtY));
			Custom->Description = Description;
			Custom->OutputType = Output;
			Custom->Code = FString(SurfCommon) + Body;
			Custom->Inputs.Reset();
			SurfInput(Custom, TEXT("P"), Position);
			SurfInput(Custom, TEXT("N"), Normal);
			SurfInput(Custom, TEXT("Tile"), Tile);
			return Custom;
		};
		auto* ColorNode = MakeCustom(TEXT("SurfaceColor"), CMOT_Float4, SurfColorCode, 0);
		auto* NormalNode = MakeCustom(TEXT("SurfaceNormal"), CMOT_Float3, SurfNormalCode, 600);

		SurfInput(ColorNode, TEXT("VCR"), VertexColor, 1);
		SurfInput(ColorNode, TEXT("VCG"), VertexColor, 2);
		SurfInput(ColorNode, TEXT("VCB"), VertexColor, 3);
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(SurfNode(Material, UMaterialExpressionVectorParameter::StaticClass(), -1400, 400));
		Tint->ParameterName = TEXT("Tint");
		Tint->DefaultValue = FLinearColor::White;
		SurfInput(ColorNode, TEXT("Tint"), Tint, 0);
		SurfInput(ColorNode, TEXT("Saturation"), SurfScalar(Material, TEXT("Saturation"), 1.0f, 500));
		SurfInput(ColorNode, TEXT("RoughScale"), SurfScalar(Material, TEXT("RoughScale"), 1.0f, 560));
		SurfInput(ColorNode, TEXT("Col"), SurfTexture(Material, TEXT("ColorMap"), ESurfKind::Color, 700));
		SurfInput(ColorNode, TEXT("Rough"), SurfTexture(Material, TEXT("RoughMap"), ESurfKind::Rough, 800));
		SurfInput(NormalNode, TEXT("Nrm"), SurfTexture(Material, TEXT("NormalMap"), ESurfKind::Normal, 900));

		auto* Rgb = Cast<UMaterialExpressionComponentMask>(SurfNode(Material, UMaterialExpressionComponentMask::StaticClass(), 100, 0));
		Rgb->R = Rgb->G = Rgb->B = 1;
		Rgb->A = 0;
		Rgb->Input.Connect(0, ColorNode);
		auto* Alpha = Cast<UMaterialExpressionComponentMask>(SurfNode(Material, UMaterialExpressionComponentMask::StaticClass(), 100, 200));
		Alpha->R = Alpha->G = Alpha->B = 0;
		Alpha->A = 1;
		Alpha->Input.Connect(0, ColorNode);
		UMaterialEditingLibrary::ConnectMaterialProperty(Rgb, TEXT(""), MP_BaseColor);
		UMaterialEditingLibrary::ConnectMaterialProperty(Alpha, TEXT(""), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(NormalNode, TEXT(""), MP_Normal);
		UMaterialEditingLibrary::ConnectMaterialProperty(SurfScalar(Material, TEXT("Metallic"), 0.0f, 620), TEXT(""), MP_Metallic);
		UMaterialEditingLibrary::RecompileMaterial(Material);
		return Material;
	}

	float SurfNumber(const TSharedPtr<FJsonObject>& Info, const TCHAR* Field, double Default)
	{
		double Value = Default;
		Info->TryGetNumberField(Field, Value);
		return static_cast<float>(Value);
	}

	UMaterialInstanceConstant* SurfMakeInstance(UMaterial* Master, const FString& Surface, const TSharedPtr<FJsonObject>& Info, UTexture2D* Color, UTexture2D* Normal, UTexture2D* Rough)
	{
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
		Factory->InitialParent = Master;
		UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Tools.CreateAsset(FString::Printf(TEXT("MI_Surface_%s"), *Surface), SurfFolder, UMaterialInstanceConstant::StaticClass(), Factory));
		if (!Instance)
		{
			return nullptr;
		}
		// The tint is written as an sRGB colour, as picked in a paint program.
		FLinearColor Tint = FLinearColor::White;
		const TArray<TSharedPtr<FJsonValue>>* TintList = nullptr;
		if (Info->TryGetArrayField(TEXT("tint"), TintList) && TintList->Num() >= 3)
		{
			const auto Linear = [&](int32 I) { return FMath::Pow(static_cast<float>((*TintList)[I]->AsNumber()), 2.2f); };
			Tint = FLinearColor(Linear(0), Linear(1), Linear(2), 1.0f);
		}
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("ColorMap"), Color);
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("NormalMap"), Normal);
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("RoughMap"), Rough);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, TEXT("Tint"), Tint);
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Tile"), SurfNumber(Info, TEXT("tileMetres"), 2.0));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Saturation"), SurfNumber(Info, TEXT("saturation"), 1.0));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("RoughScale"), SurfNumber(Info, TEXT("roughness"), 1.0));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Metallic"), SurfNumber(Info, TEXT("metallic"), 0.0));
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		return Instance;
	}
}

USurfaceMaterialsCommandlet::USurfaceMaterialsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 USurfaceMaterialsCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/stuff_textures.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogSurfaceMaterials, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	// Start clean: saving over the last run's files fails while they exist.
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(SurfFolder), false, true);

	UMaterial* Master = SurfMakeMaster();
	if (!Master)
	{
		UE_LOG(LogSurfaceMaterials, Error, TEXT("Could not make %s/M_Surface"), SurfFolder);
		return 1;
	}
	TArray<UObject*> ToSave = {Master};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->GetObjectField(TEXT("surfaces"))->Values)
	{
		const TSharedPtr<FJsonObject> Info = Pair.Value->AsObject();
		UTexture2D* Color = SurfImportTexture(Pair.Key, TEXT("color"), ESurfKind::Color);
		UTexture2D* Normal = SurfImportTexture(Pair.Key, TEXT("normal"), ESurfKind::Normal);
		UTexture2D* Rough = SurfImportTexture(Pair.Key, TEXT("rough"), ESurfKind::Rough);
		UMaterialInstanceConstant* Instance = Color && Normal && Rough ? SurfMakeInstance(Master, Pair.Key, Info, Color, Normal, Rough) : nullptr;
		if (!Instance)
		{
			UE_LOG(LogSurfaceMaterials, Error, TEXT("Could not make the %s surface"), *Pair.Key);
			return 1;
		}
		ToSave.Add(Instance);
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
			UE_LOG(LogSurfaceMaterials, Error, TEXT("Material: %s"), *Error);
			bErrors = true;
		}
	}
	bool bSaved = true;
	for (UObject* Object : ToSave)
	{
		bSaved &= SurfSave(Object);
	}
	UE_LOG(LogSurfaceMaterials, Display, TEXT("%s M_Surface and %d surfaces in %s%s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), ToSave.Num() - 1, SurfFolder, bErrors ? TEXT(" (with errors)") : TEXT(""));
	return bSaved && !bErrors ? 0 : 1;
}
