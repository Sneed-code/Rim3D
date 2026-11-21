#include "HairGroomsCommandlet.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Factories/Factory.h"
#include "GroomAsset.h"
#include "GroomAssetPhysics.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#include "StrandHair.h"

DEFINE_LOG_CATEGORY_STATIC(LogHairGrooms, Log, All);

namespace
{
	const TCHAR* const GroomsHairMaterial = TEXT("/Game/MetaHumans/Common/Materials/MI_Hair.MI_Hair");

	bool GroomsSave(UObject* Asset)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bSaved = UPackage::SavePackage(Package, Asset, *FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()), Args);
		UE_LOG(LogHairGrooms, Display, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("FAILED to save"), *Package->GetName());
		return bSaved;
	}

	/** A class by its short name in the groom plugin's modules (the importer's own headers are private). */
	UClass* GroomsFindClass(const TCHAR* Name)
	{
		for (const TCHAR* Module : {TEXT("HairStrandsEditor"), TEXT("HairStrandsCore")})
		{
			FModuleManager::Get().LoadModule(Module);
			if (UClass* Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/%s.%s"), Module, Name)))
			{
				return Class;
			}
		}
		UE_LOG(LogHairGrooms, Error, TEXT("No class %s in the HairStrands plugin"), Name);
		return nullptr;
	}

	/** Sets a property of Object from Unreal's text form, e.g. ("ConversionSettings", "(Scale=(X=1,Y=-1,Z=1))"): fields not
	 *  named keep their values. By reflection, so a renamed field only warns. */
	bool GroomsSet(UObject* Object, const TCHAR* Property, const TCHAR* Text)
	{
		FProperty* Prop = Object ? Object->GetClass()->FindPropertyByName(Property) : nullptr;
		if (!Prop)
		{
			UE_LOG(LogHairGrooms, Warning, TEXT("%s has no property %s"), Object ? *Object->GetClass()->GetName() : TEXT("null"), Property);
			return false;
		}
		const TCHAR* End = Prop->ImportText_InContainer(Text, Object, Object, PPF_None);
		UE_LOG(LogHairGrooms, Display, TEXT("  %s.%s = %s%s"), *Object->GetClass()->GetName(), Property, Text, End ? TEXT("") : TEXT(" (NOT taken)"));
		return End != nullptr;
	}

	UGroomAsset* GroomsImport(const FString& File, const FString& Name)
	{
		UClass* FactoryClass = GroomsFindClass(TEXT("HairStrandsFactory"));
		UClass* OptionsClass = GroomsFindClass(TEXT("GroomImportOptions"));
		if (!FactoryClass || !OptionsClass)
		{
			return nullptr;
		}
		UObject* Options = NewObject<UObject>(GetTransientPackage(), OptionsClass);
		// grow_hair.py writes Blender's axes in centimetres; Alembic is Y up. Unreal's default conversion (turn 90 about X,
		// flip Y) gives the body's own component axes, as the FBX import of the same body does.
		GroomsSet(Options, TEXT("ConversionSettings"), TEXT("(Rotation=(X=90.000000,Y=0.000000,Z=0.000000),Scale=(X=1.000000,Y=-1.000000,Z=1.000000))"));
		// The groom factory is called directly: AssetTools' ImportAssetTasks hit a null shared pointer in a commandlet
		// (PC, 2026-11-21). The task only carries the options and the automated flag the factory reads.
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = File;
		Task->DestinationPath = StrandHairGrownFolder;
		Task->DestinationName = Name;
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = false;
		Task->Options = Options;
		UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		Factory->AssetImportTask = Task;
		Task->Factory = Factory;
		// The groom factory loads its file translators (Alembic and so on) lazily, when asked for its file types; until
		// then it has none, cannot read anything and imports nothing (PC, 2026-11-21: every file failed).
		TArray<FString> Extensions;
		Factory->GetSupportedFileExtensions(Extensions);
		UE_LOG(LogHairGrooms, Display, TEXT("  groom importer reads: %s"), *FString::Join(Extensions, TEXT(", ")));
		if (!Factory->FactoryCanImport(File))
		{
			UE_LOG(LogHairGrooms, Error, TEXT("The groom importer cannot read %s (no hair curves in it?)"), *File);
			return nullptr;
		}
		UPackage* Package = CreatePackage(*(FString(StrandHairGrownFolder) / Name));
		Package->FullyLoad();
		bool bCancelled = false;
		UObject* Made = Factory->ImportObject(UGroomAsset::StaticClass(), Package, FName(*Name), RF_Public | RF_Standalone, File, nullptr, bCancelled);
		if (UGroomAsset* Groom = Cast<UGroomAsset>(Made))
		{
			FAssetRegistryModule::AssetCreated(Groom);
			return Groom;
		}
		// Some factories make the asset under their own name in the package: look there too.
		TArray<UObject*> Inside;
		GetObjectsWithOuter(Package, Inside, false);
		for (UObject* Object : Inside)
		{
			if (UGroomAsset* Groom = Cast<UGroomAsset>(Object))
			{
				FAssetRegistryModule::AssetCreated(Groom);
				return Groom;
			}
		}
		UE_LOG(LogHairGrooms, Error, TEXT("Importing %s made no groom"), *File);
		return nullptr;
	}

	bool GroomsMake(const FString& File, const FString& Name, UMaterialInterface* HairMaterial, bool bSimulate)
	{
		UGroomAsset* Groom = GroomsImport(File, Name);
		if (!Groom)
		{
			return false;
		}
		// Guides for the physics, made from the strands (the file has none): one strand in 40 drives its neighbours.
		GroomsSet(Groom, TEXT("HairGroupsInterpolation"), TEXT("((InterpolationSettings=(GuideType=Generated,HairToGuideDensity=0.025)))"));
		// Full strands near; further off a second LOD with a third of the strands, a little thicker so it stays as full.
		GroomsSet(Groom, TEXT("HairGroupsLOD"),
			TEXT("((LODs=((CurveDecimation=1.000000,VertexDecimation=1.000000,ScreenSize=1.000000,ThicknessScale=1.000000),")
			TEXT("(CurveDecimation=0.350000,VertexDecimation=0.500000,ScreenSize=0.400000,ThicknessScale=1.600000))))"));
		// The import makes no material slot (PC: "0 materials"): one slot, Hair, on the MetaHuman hair material, which
		// every group draws with (so the pawn's hair colour reaches it).
		if (HairMaterial)
		{
			GroomsSet(Groom, TEXT("HairGroupsMaterials"), *FString::Printf(TEXT("((Material=\"%s\",SlotName=\"Hair\"))"), *HairMaterial->GetPathName()));
			FString Rendering = TEXT("(");
			for (int32 Group = 0; Group < Groom->GetNumHairGroups(); ++Group)
			{
				Rendering += Group ? TEXT(",(MaterialSlotName=\"Hair\")") : TEXT("(MaterialSlotName=\"Hair\")");
			}
			GroomsSet(Groom, TEXT("HairGroupsRendering"), *(Rendering + TEXT(")")));
		}
		for (FHairGroupsMaterial& Material : Groom->GetHairGroupsMaterials())
		{
			Material.Material = HairMaterial;
		}
		for (FHairGroupsPhysics& Physics : Groom->GetHairGroupsPhysics())
		{
			Physics.SolverSettings.EnableSimulation = bSimulate;
		}
		Groom->PostEditChange();
		UE_LOG(LogHairGrooms, Display, TEXT("Groom %s from %s: %d groups, %d materials, physics %s"), *Groom->GetName(), *File, Groom->GetNumHairGroups(),
			Groom->GetHairGroupsMaterials().Num(), bSimulate ? TEXT("on") : TEXT("off"));
		return GroomsSave(Groom);
	}
}

UHairGroomsCommandlet::UHairGroomsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UHairGroomsCommandlet::Main(const FString& Params)
{
	FString Text;
	const FString ListFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("../assets/hair_grooms.json")));
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *ListFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogHairGrooms, Error, TEXT("Cannot read %s"), *ListFile);
		return 1;
	}
	UMaterialInterface* HairMaterial = LoadObject<UMaterialInterface>(nullptr, GroomsHairMaterial);
	if (!HairMaterial)
	{
		UE_LOG(LogHairGrooms, Warning, TEXT("No %s (the MetaHuman hair material): the grooms keep the default hair material and the pawn's hair colour will not show"),
			GroomsHairMaterial);
	}
	IFileManager::Get().DeleteDirectory(*FPackageName::LongPackageNameToFilename(StrandHairGrownFolder), false, true);
	TArray<FString> Still;  // too short to move: no physics
	const TArray<TSharedPtr<FJsonValue>>* StillValues = nullptr;
	if (Root->TryGetArrayField(TEXT("still"), StillValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *StillValues)
		{
			Still.Add(Value->AsString());
		}
	}
	int32 Made = 0;
	int32 Failed = 0;
	for (const TSharedPtr<FJsonValue>& Base : Root->GetArrayField(TEXT("bases")))
	{
		for (const TSharedPtr<FJsonValue>& StyleValue : Root->GetArrayField(TEXT("styles")))
		{
			// Each style and its under-a-hat version (<Style>Hat; grow_hair.py makes one for every style but Buzz).
			for (const FString& Style : {StyleValue->AsString(), StyleValue->AsString() + TEXT("Hat")})
			{
				const bool bHat = Style.EndsWith(TEXT("Hat"));
				const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Grooms") / Base->AsString() / (Style + TEXT(".abc")));
				if (!IFileManager::Get().FileExists(*File))
				{
					UE_LOG(LogHairGrooms, Display, TEXT("No %s: grow it with tools/makehuman/grow_hair.py (%s)"), *File,
						bHat ? TEXT("under a hat the plain style shows") : TEXT("that style keeps its wig"));
					continue;
				}
				const bool bOk = GroomsMake(File, StrandHairGrownName(Base->AsString(), Style), HairMaterial, !Still.Contains(StyleValue->AsString()));
				Made += bOk ? 1 : 0;
				Failed += bOk ? 0 : 1;
			}
		}
	}
	UE_LOG(LogHairGrooms, Display, TEXT("HairGrooms: %d grooms made, %d failed"), Made, Failed);
	return Failed == 0 && Made > 0 ? 0 : 1;
}
