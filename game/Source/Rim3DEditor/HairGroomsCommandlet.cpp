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

	/** Sets part of one hair group's physics by text (only the fields named change), and logs what it became. */
	bool GroomsPhysicsSet(FHairGroupsPhysics& Physics, const TCHAR* Field, const TCHAR* Text)
	{
		FProperty* Prop = FHairGroupsPhysics::StaticStruct()->FindPropertyByName(Field);
		if (!Prop)
		{
			UE_LOG(LogHairGrooms, Warning, TEXT("  hair physics has no field %s"), Field);
			return false;
		}
		const TCHAR* End = Prop->ImportText_InContainer(Text, &Physics, nullptr, PPF_None);
		FString Now;
		Prop->ExportTextItem_InContainer(Now, &Physics, nullptr, nullptr, PPF_None);
		UE_LOG(LogHairGrooms, Display, TEXT("  physics %s = %s%s, now %s"), Field, Text, End ? TEXT("") : TEXT(" (NOT taken)"), *Now);
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

	bool GroomsMake(const FString& File, const FString& Name, UMaterialInterface* HairMaterial, bool bSimulate, int32 FileGuides)
	{
		UGroomAsset* Groom = GroomsImport(File, Name);
		if (!Groom)
		{
			return false;
		}
		// Guides for the physics: grow_hair.py writes them into the file (groom_guide 1), the innermost strand of each patch
		// of hair, so every drawn strand lies on or over the guide it follows and none is pushed under the skin when its
		// guide is pressed onto the body (sneed 2026-11-25). Each strand follows its one nearest guide: blended between
		// guides, a strand whose guides lay either side of the body (one over the shoulder, one down the back) was drawn
		// straight through the chest (PC 2026-11-22, #450). A file with none (an older grow, a still style): made from the
		// strands, one in 20.
		GroomsSet(Groom, TEXT("HairGroupsInterpolation"), FileGuides > 0
			? TEXT("((InterpolationSettings=(GuideType=Imported,bUseUniqueGuide=True)))")
			: TEXT("((InterpolationSettings=(GuideType=Generated,HairToGuideDensity=0.05,bUseUniqueGuide=True)))"));
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
			// Twice Unreal's default steps and iterations: at 5 and 5, long hair on bare skin could pass through the
			// collision shell while the body turned on the spot mid-walk (PC, 2026-11-22) and end up in front or inside.
			Physics.SolverSettings.SubSteps = 10;
			Physics.SolverSettings.IterationCount = 10;
			// 32 points along each guide (Unreal's default is 8): the solver only keeps its points out of the collision
			// shapes, and on waist-long hair 8 points are 10 cm apart, so the straight pieces between them cut into the
			// back as it turns (PC 2026-11-22, #450). 32 makes them about 2.5 cm. Set by text so a renamed field is a
			// log line, not a broken build.
			GroomsPhysicsSet(Physics, TEXT("StrandsParameters"), TEXT("(StrandsSize=Size32)"));
			// The grown shape is the rest shape, held against gravity (sneed 2026-11-25: hair sank onto and into the scalp,
			// showing it, on every style but the bob, which had its physics off and so was stiff). Gravity preloading makes
			// the solver work out the pull that holds the rest pose up under gravity, so hair stands where it was grown and
			// swings from there; the same for every style, so a new style needs nothing of its own. (Unreal's Groom Springs
			// solver, the default; set by text so a renamed field is a log line, not a broken build.)
			GroomsPhysicsSet(Physics, TEXT("SolverSettings"), TEXT("(GravityPreloading=1.0)"));
			// Stiff near the scalp, as real hair is where it leaves the skin, softer towards the tips (5 times Unreal's
			// default bend stiffness at the root, 3/4 of it at the tips), so the roots keep their lie as the head turns
			// and stops while the lengths still swing.
			// Each guide is StrandHairGuideRadiusCm thick to the collision, the whole of its length: Unreal's default tapers from
			// 1 mm at the root to 0.1 mm at the tip, so long hair's lower half lay on the shapes themselves. The game makes
			// its shapes that much smaller than grow_hair.py's barrier (StrandHair.cpp), so guide plus shape meet the barrier.
			FHairCollisionConstraint& Collide = Physics.MaterialConstraints.CollisionConstraint;
			Collide.CollisionRadius = StrandHairGuideRadiusCm;
			Collide.SolveCollision = true;
			Collide.ProjectCollision = true;
			if (FRichCurve* Curve = Collide.RadiusScale.GetRichCurve())
			{
				Curve->Reset();
				Curve->AddKey(0.0f, 1.0f);
				Curve->AddKey(1.0f, 1.0f);
			}
			UE_LOG(LogHairGrooms, Display, TEXT("  physics collision thickness %.2f cm root to tip, guides %s"), Collide.CollisionRadius,
				FileGuides > 0 ? *FString::Printf(TEXT("from the file (%d)"), FileGuides) : TEXT("made from the strands"));
			FHairBendConstraint& Bend = Physics.MaterialConstraints.BendConstraint;
			Bend.BendStiffness = 0.05f;
			if (FRichCurve* Curve = Bend.BendScale.GetRichCurve())
			{
				Curve->Reset();
				Curve->AddKey(0.0f, 1.0f);
				Curve->AddKey(0.15f, 0.6f);
				Curve->AddKey(0.4f, 0.25f);
				Curve->AddKey(1.0f, 0.15f);
			}
			UE_LOG(LogHairGrooms, Display, TEXT("  physics bend stiffness %.3f, scale 1.0 at the root to 0.15 at the tips"), Bend.BendStiffness);
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
			// Each style, its under-a-hat version (<Style>Hat; grow_hair.py makes one for every style but Buzz) and its
			// undercoat (<Style>Under: short strands flat on the scalp, worn under the style, never simulated).
			for (const FString& Style : {StyleValue->AsString(), StyleValue->AsString() + TEXT("Hat"), StyleValue->AsString() + TEXT("Under")})
			{
				const bool bHat = Style.EndsWith(TEXT("Hat"));
				const bool bUnder = Style.EndsWith(TEXT("Under"));
				const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Grooms") / Base->AsString() / (Style + TEXT(".abc")));
				if (!IFileManager::Get().FileExists(*File))
				{
					UE_LOG(LogHairGrooms, Display, TEXT("No %s: grow it with tools/makehuman/grow_hair.py (%s)"), *File,
						bHat ? TEXT("under a hat the plain style shows") : (bUnder ? TEXT("the style is worn with no undercoat") : TEXT("that style keeps its wig")));
					continue;
				}
				// How many guides grow_hair.py wrote into the file (grooms.json), so the import uses them.
				int32 FileGuides = 0;
				{
					FString Manifest;
					TSharedPtr<FJsonObject> Grown;
					const TSharedPtr<FJsonObject>* GrownStyles = nullptr;
					const TSharedPtr<FJsonObject>* Entry = nullptr;
					if (FFileHelper::LoadFileToString(Manifest, *(FPaths::GetPath(File) / TEXT("grooms.json"))) &&
						FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manifest), Grown) && Grown.IsValid() &&
						Grown->TryGetObjectField(TEXT("styles"), GrownStyles) && (*GrownStyles)->TryGetObjectField(Style, Entry))
					{
						(*Entry)->TryGetNumberField(TEXT("guides"), FileGuides);
					}
				}
				const bool bOk = GroomsMake(File, StrandHairGrownName(Base->AsString(), Style), HairMaterial, !bUnder && !Still.Contains(StyleValue->AsString()), FileGuides);
				Made += bOk ? 1 : 0;
				Failed += bOk ? 0 : 1;
			}
		}
	}
	UE_LOG(LogHairGrooms, Display, TEXT("HairGrooms: %d grooms made, %d failed"), Made, Failed);
	return Failed == 0 && Made > 0 ? 0 : 1;
}
