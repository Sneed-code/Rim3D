#include "SmokeTest.h"

#include "AIController.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Character.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProceduralMeshComponent.h"
#include "RHI.h"
#include "RenderCore.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"

DEFINE_LOG_CATEGORY_STATIC(LogSmoke, Log, All);

namespace
{
	// Far from the sample level's own geometry, so the tests start on empty ground.
	const FVector SmokeOrigin(300000.0, 0.0, 0.0);

	const TCHAR* const RunnerClassPath = TEXT("/Game/Blueprints/SandboxCharacter_CMC.SandboxCharacter_CMC_C");
	const TCHAR* const MetaHumanClassPath = TEXT("/Game/MetaHumans/Kellan/BP_Kellan.BP_Kellan_C");

	double SmokePercentile(TArray<float> Values, double P)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		return Values[FMath::Clamp(static_cast<int32>(P * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}
}

bool USmokeTestSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game;
}

TStatId USmokeTestSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USmokeTestSubsystem, STATGROUP_Tickables);
}

void USmokeTestSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (!FParse::Value(FCommandLine::Get(), TEXT("SmokeTest="), TestName))
	{
		return;
	}
	UE_LOG(LogSmoke, Display, TEXT("Smoke test '%s' starting"), *TestName);
	StartSeconds = FPlatformTime::Seconds();
	FParse::Value(FCommandLine::Get(), TEXT("SmokeTimeout="), TimeoutSeconds);

	FActorSpawnParameters Params;
	Camera = InWorld.SpawnActor<ACameraActor>(SmokeOrigin, FRotator::ZeroRotator, Params);
	Camera->GetCameraComponent()->SetFieldOfView(70.0f);
	Camera->GetCameraComponent()->bConstrainAspectRatio = false;

	if (TestName == TEXT("load"))
	{
		BuildLoadPhases();
	}
	else if (TestName == TEXT("crowd"))
	{
		BuildCrowdPhases();
	}
	else if (TestName == TEXT("terrain"))
	{
		BuildTerrainPhases();
	}
	else if (TestName == TEXT("sim"))
	{
		BuildSimPhases();
	}
	else if (TestName == TEXT("overseer"))
	{
		BuildOverseerPhases();
	}
	else if (TestName == TEXT("pawns"))
	{
		BuildPawnsPhases();
	}
	else if (TestName == TEXT("saves"))
	{
		BuildSavesPhases();
	}
	else if (TestName == TEXT("protagonist"))
	{
		BuildProtagonistPhases();
	}
	else if (TestName == TEXT("looks"))
	{
		BuildLooksPhases();
	}
	else if (TestName == TEXT("faces"))
	{
		BuildFacesPhases();
	}
	else if (TestName == TEXT("world"))
	{
		BuildWorldPhases();
	}
	else if (TestName == TEXT("buildings"))
	{
		BuildBuildingsPhases();
	}
	else if (TestName == TEXT("orders"))
	{
		BuildOrdersPhases();
	}
	else if (TestName == TEXT("panels"))
	{
		BuildPanelsPhases();
	}
	else if (TestName == TEXT("worldmap"))
	{
		BuildWorldMapPhases();
	}
	else if (TestName == TEXT("fights"))
	{
		BuildFightsPhases();
	}
	else if (TestName == TEXT("handson"))
	{
		BuildHandsOnPhases();
	}
	else if (TestName == TEXT("animals"))
	{
		BuildAnimalsPhases();
	}
	else if (TestName == TEXT("sky"))
	{
		BuildSkyPhases();
	}
	else if (TestName == TEXT("melee"))
	{
		BuildMeleePhases();
	}
	else if (TestName == TEXT("levels"))
	{
		BuildLevelsPhases();
	}
	else if (TestName == TEXT("clothes"))
	{
		BuildClothesPhases();
	}
	else if (TestName == TEXT("feel"))
	{
		BuildFeelPhases();
	}
	else if (TestName == TEXT("fire"))
	{
		BuildFirePhases();
	}
	else
	{
		Fail(FString::Printf(TEXT("Unknown smoke test '%s'"), *TestName));
		WriteResultsAndQuit();
		return;
	}
	StartPhase(0);
}

void USmokeTestSubsystem::ViewFrom(const FVector& Location, const FRotator& Rotation)
{
	Camera->SetActorLocationAndRotation(Location, Rotation);
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		PC->SetViewTarget(Camera);
		if (APawn* PlayerPawn = PC->GetPawn())
		{
			PlayerPawn->DisableInput(PC);
		}
	}
}

void USmokeTestSubsystem::ClearSpawned()
{
	for (AActor* Actor : Spawned)
	{
		if (IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	Spawned.Reset();
	RunnerRadius.Reset();
}

// ---------------------------------------------------------------------------
// Load: the level opens, the sim subsystem is running and frames are produced.

void USmokeTestSubsystem::BuildLoadPhases()
{
	Phases.Add({TEXT("load_default_level"), [this]()
	{
		Notes.Add(FString::Printf(TEXT("Level: %s"), *GetWorld()->GetMapName()));
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge)
		{
			Fail(TEXT("USimSubsystem did not start"));
		}
		else
		{
			Notes.Add(FString::Printf(TEXT("Sim data folder %s: %d defs"), *Bridge->GetDataRoot(), Bridge->GetDefCount()));
			if (Bridge->GetDefCount() == 0)
			{
				Fail(TEXT("The sim loaded no defs from the data folder"));
			}
		}
		// No ViewFrom: the screenshot shows the level as a player sees it.
	}, 3.0f, 3.0f});
}

// ---------------------------------------------------------------------------
// Crowd: seeded MetaHuman variety, then 100 animated characters.

void USmokeTestSubsystem::BuildCrowdPhases()
{
	CrowdCentre = SmokeOrigin;

	// Ground: a 400 m slab with collision so characters can run on it.
	if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
	{
		AStaticMeshActor* Ground = GetWorld()->SpawnActor<AStaticMeshActor>(SmokeOrigin - FVector(0, 0, 5), FRotator::ZeroRotator);
		Ground->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Ground->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Ground->SetActorScale3D(FVector(400.0, 400.0, 0.1));
	}

	const auto SpawnLineup = [this](bool bMetaHuman)
	{
		// Ten seeded pawns standing in a row, facing the camera.
		for (int32 i = 0; i < 10; ++i)
		{
			const FVector Location = CrowdCentre + FVector(0.0, (i - 4.5) * 120.0, 100.0);
			ACharacter* Runner = SpawnRunner(Location, bMetaHuman, 1000 + i);
			if (Runner)
			{
				Runner->SetActorRotation(FRotator(0.0, 180.0, 0.0));
				RunnerRadius.Add(0.0f);  // standing still
			}
		}
	};

	const auto SpawnCrowd = [this](bool bMetaHuman, int32 Count)
	{
		colony::Rng Rng(7);
		for (int32 i = 0; i < Count; ++i)
		{
			const float Radius = 300.0f + Rng.unit() * 2200.0f;  // 3 to 25 m from the centre
			const float Angle = Rng.unit() * 2.0f * PI;
			const FVector Location = CrowdCentre + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 100.0);
			if (SpawnRunner(Location, bMetaHuman, 2000 + i))
			{
				RunnerRadius.Add((i % 2 == 0) ? Radius : -Radius);  // sign picks the direction
			}
		}
	};

	const FVector CloseCam = CrowdCentre + FVector(650.0, 0.0, 170.0);
	const FRotator CloseRot(-5.0, 180.0, 0.0);
	// Roughly the overseer camera: 25 m up, looking down at the crowd.
	const FVector OverseerCam = CrowdCentre + FVector(3200.0, 0.0, 2500.0);
	const FRotator OverseerRot(-38.0, 180.0, 0.0);

	Phases.Add({TEXT("variety_10_metahumans"), [=, this]() { SpawnLineup(true); ViewFrom(CloseCam, CloseRot); }, 6.0f, 4.0f});
	Phases.Add({TEXT("crowd_100_metahumans"), [=, this]() { SpawnCrowd(true, 100); ViewFrom(OverseerCam, OverseerRot); }, 8.0f, 15.0f});
	Phases.Add({TEXT("crowd_100_mannequins"), [=, this]() { SpawnCrowd(false, 100); ViewFrom(OverseerCam, OverseerRot); }, 8.0f, 15.0f});
	Phases.Add({TEXT("crowd_200_mannequins"), [=, this]() { SpawnCrowd(false, 200); ViewFrom(OverseerCam, OverseerRot); }, 8.0f, 15.0f});
}

ACharacter* USmokeTestSubsystem::SpawnRunner(const FVector& Location, bool bMetaHuman, int32 Seed)
{
	UClass* RunnerClass = LoadClass<ACharacter>(nullptr, RunnerClassPath);
	if (!RunnerClass)
	{
		Fail(FString::Printf(TEXT("Could not load %s"), RunnerClassPath));
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	ACharacter* Runner = GetWorld()->SpawnActor<ACharacter>(RunnerClass, Location, FRotator::ZeroRotator, Params);
	if (!Runner)
	{
		return nullptr;
	}
	Runner->AIControllerClass = AAIController::StaticClass();
	Runner->SpawnDefaultController();
	Spawned.Add(Runner);

	if (bMetaHuman)
	{
		UClass* MetaHumanClass = LoadClass<AActor>(nullptr, MetaHumanClassPath);
		AActor* MetaHuman = MetaHumanClass ? GetWorld()->SpawnActor<AActor>(MetaHumanClass, Location, FRotator::ZeroRotator, Params) : nullptr;
		if (!MetaHuman)
		{
			Fail(FString::Printf(TEXT("Could not load %s"), MetaHumanClassPath));
			return Runner;
		}
		Spawned.Add(MetaHuman);

		// The hidden sample character keeps animating; the MetaHuman body copies its pose.
		USkeletalMeshComponent* Driver = Runner->GetMesh();
		Driver->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Driver->SetVisibility(false, false);
		MetaHuman->AttachToComponent(Driver, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		TArray<USkeletalMeshComponent*> Meshes;
		MetaHuman->GetComponents(Meshes);
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Mesh->GetName() == TEXT("Body"))
			{
				Mesh->SetLeaderPoseComponent(Driver);
			}
		}
		FString Look;
		ApplySeededLook(MetaHuman, Seed, Look);
		if (Seed < 2000)
		{
			Notes.Add(FString::Printf(TEXT("Pawn %d: %s"), Seed - 1000, *Look));
		}
	}
	return Runner;
}

void USmokeTestSubsystem::ApplySeededLook(AActor* MetaHuman, int32 Seed, FString& OutDescription)
{
	// The lineup shows fixed examples first (heavy, thin, Emberkin), then seeded ones.
	colony::Rng Rng(static_cast<uint64_t>(Seed));
	float Height = 0.92f + Rng.unit() * 0.16f;
	float Width = 0.9f + Rng.unit() * 0.25f;
	bool bEmberkin = Rng.below(8) == 0;
	const int32 Slot = Seed % 1000;
	if (Seed < 2000 && Slot == 0) { Width = 1.3f; Height = 1.0f; bEmberkin = false; }
	if (Seed < 2000 && Slot == 1) { Width = 0.8f; Height = 1.05f; bEmberkin = false; }
	if (Seed < 2000 && Slot == 2) { Width = 1.0f; Height = 0.95f; bEmberkin = true; }

	// Width scales the body sideways and front to back; a stand-in for real body types.
	MetaHuman->SetActorRelativeScale3D(FVector(Width, Width, Height));
	OutDescription = FString::Printf(TEXT("height %.2f, width %.2f%s"), Height, Width, bEmberkin ? TEXT(", Emberkin") : TEXT(""));
	if (!bEmberkin)
	{
		return;
	}

	// Red skin: tint any skin colour parameter on the body and face materials.
	TArray<USkeletalMeshComponent*> Meshes;
	MetaHuman->GetComponents(Meshes);
	USkeletalMeshComponent* Face = nullptr;
	TArray<FString> TintedParams;
	for (USkeletalMeshComponent* Mesh : Meshes)
	{
		const FString Name = Mesh->GetName();
		if (Name == TEXT("Face"))
		{
			Face = Mesh;
		}
		if (Name != TEXT("Face") && Name != TEXT("Body"))
		{
			continue;
		}
		for (int32 m = 0; m < Mesh->GetNumMaterials(); ++m)
		{
			UMaterialInterface* Material = Mesh->GetMaterial(m);
			if (!Material)
			{
				continue;
			}
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Material->GetAllVectorParameterInfo(Infos, Ids);
			UMaterialInstanceDynamic* Mid = nullptr;
			for (const FMaterialParameterInfo& Info : Infos)
			{
				const FString Param = Info.Name.ToString();
				if (Param.Contains(TEXT("Skin")) && (Param.Contains(TEXT("Tint")) || Param.Contains(TEXT("Color"))))
				{
					Mid = Mid ? Mid : Mesh->CreateDynamicMaterialInstance(m);
					Mid->SetVectorParameterValue(Info.Name, FLinearColor(0.75f, 0.08f, 0.06f));
					TintedParams.AddUnique(Param);
				}
			}
		}
	}
	if (TintedParams.IsEmpty())
	{
		Notes.AddUnique(TEXT("No skin tint parameter found on the MetaHuman materials; red skin needs a material change"));
	}
	else
	{
		Notes.AddUnique(FString::Printf(TEXT("Red skin via material parameters: %s"), *FString::Join(TintedParams, TEXT(", "))));
	}

	// Horns: two cones on the head bone.
	UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	USkeletalMeshComponent* HeadMesh = Face ? Face : (Meshes.IsEmpty() ? nullptr : Meshes[0]);
	if (!Cone || !HeadMesh)
	{
		return;
	}
	for (int32 Side = -1; Side <= 1; Side += 2)
	{
		UStaticMeshComponent* Horn = NewObject<UStaticMeshComponent>(MetaHuman);
		Horn->SetStaticMesh(Cone);
		Horn->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Horn->SetupAttachment(HeadMesh, TEXT("head"));
		Horn->SetRelativeLocation(FVector(12.0, 6.0 * Side, 4.0));
		Horn->SetRelativeRotation(FRotator(0.0, 0.0, -90.0 + 15.0 * Side));
		Horn->SetRelativeScale3D(FVector(0.05, 0.05, 0.12));
		Horn->RegisterComponent();
		if (UMaterialInstanceDynamic* Mid = Horn->CreateDynamicMaterialInstance(0))
		{
			Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.1f, 0.08f, 0.07f));
		}
	}
}

// ---------------------------------------------------------------------------
// Terrain: the sim's 1 km height map as a runtime mesh.

void USmokeTestSubsystem::BuildTerrainPhases()
{
	const FVector Middle = SmokeOrigin + FVector(51200.0, 51200.0, 0.0);
	Phases.Add({TEXT("terrain_overseer_view"), [=, this]() { BuildTerrain(); ViewFrom(Middle + FVector(-6000.0, 0.0, 6000.0), FRotator(-40.0, 0.0, 0.0)); }, 8.0f, 10.0f});
	Phases.Add({TEXT("terrain_low_view"), [=, this]() { ViewFrom(Middle + FVector(-20000.0, -8000.0, 4500.0), FRotator(-8.0, 20.0, 0.0)); }, 5.0f, 10.0f});
	Phases.Add({TEXT("terrain_whole_map"), [=, this]() { ViewFrom(Middle + FVector(-75000.0, 0.0, 70000.0), FRotator(-42.0, 0.0, 0.0)); }, 5.0f, 10.0f});
}

void USmokeTestSubsystem::BuildTerrain()
{
	const double BuildStart = FPlatformTime::Seconds();
	const colony::Map Map(colony::MapSettings{1024, 1024, colony::splitMix64(42 ^ 0x6D6170ULL)});
	BuildTerrainMesh(Map, BuildStart);
}

// A runtime mesh of the map's ground at SmokeOrigin, in chunks of 32 cells (the map size must be a multiple of 32).
void USmokeTestSubsystem::BuildTerrainMesh(const colony::Map& Map, double BuildStart)
{
	const int32 MapSize = Map.width();
	constexpr int32 Chunk = 32;

	AActor* Terrain = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), SmokeOrigin, FRotator::ZeroRotator);
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Terrain, TEXT("Terrain"));
	Mesh->bUseAsyncCooking = true;
	Terrain->SetRootComponent(Mesh);
	Mesh->SetWorldLocation(SmokeOrigin);
	Mesh->RegisterComponent();
	Spawned.Add(Terrain);

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
	const auto HeightAt = [&Map, MapSize](int32 X, int32 Y)
	{
		X = FMath::Clamp(X, 0, MapSize - 1);
		Y = FMath::Clamp(Y, 0, MapSize - 1);
		return Map.groundHeight(X, Y) * 100.0f;
	};

	int32 Section = 0;
	int64 Vertices = 0;
	for (int32 CY = 0; CY < MapSize; CY += Chunk)
	{
		for (int32 CX = 0; CX < MapSize; CX += Chunk)
		{
			TArray<FVector> Positions;
			TArray<FVector> Normals;
			TArray<FColor> Colors;
			TArray<FVector2D> UVs;
			TArray<FProcMeshTangent> Tangents;
			TArray<int32> Triangles;
			const int32 Side = Chunk + 1;
			for (int32 Y = 0; Y < Side; ++Y)
			{
				for (int32 X = 0; X < Side; ++X)
				{
					const int32 MX = CX + X;
					const int32 MY = CY + Y;
					Positions.Add(FVector(MX * 100.0, MY * 100.0, HeightAt(MX, MY)));
					const FVector Normal = FVector(HeightAt(MX - 1, MY) - HeightAt(MX + 1, MY), HeightAt(MX, MY - 1) - HeightAt(MX, MY + 1), 200.0).GetSafeNormal();
					Normals.Add(Normal);
					UVs.Add(FVector2D(MX, MY));
					FColor Color(90, 130, 60);
					switch (Map.terrain(FMath::Min(MX, MapSize - 1), FMath::Min(MY, MapSize - 1)))
					{
						case colony::Terrain::Water: Color = FColor(40, 90, 170); break;
						case colony::Terrain::Sand: Color = FColor(210, 195, 140); break;
						case colony::Terrain::Soil: Color = FColor(90, 130, 60); break;
						case colony::Terrain::Rock: Color = FColor(120, 115, 110); break;
						case colony::Terrain::RoughStone: Color = FColor(150, 140, 125); break;
						case colony::Terrain::Air:
						case colony::Terrain::UpperFloor: break;
					}
					Colors.Add(Color);
				}
			}
			for (int32 Y = 0; Y < Chunk; ++Y)
			{
				for (int32 X = 0; X < Chunk; ++X)
				{
					const int32 I = Y * Side + X;
					Triangles.Append({I, I + Side, I + 1, I + 1, I + Side, I + Side + 1});
				}
			}
			Vertices += Positions.Num();
			Mesh->CreateMeshSection(Section, Positions, Triangles, Normals, UVs, Colors, Tangents, false);
			if (Material)
			{
				Mesh->SetMaterial(Section, Material);
			}
			++Section;
		}
	}
	const double BuildMs = (FPlatformTime::Seconds() - BuildStart) * 1000.0;
	Notes.Add(FString::Printf(TEXT("Terrain: %d chunks, %lld vertices, built in %.0f ms (sim map generation included)"), Section, Vertices, BuildMs));
	Notes.AddUnique(TEXT("Terrain uses flat vertex colours; Megascans materials need the packs downloaded from Fab first"));
}

// ---------------------------------------------------------------------------
// Sim: the bridge between Unreal and the sim (defs, commands, events, snapshots, speed keys, replay).

namespace
{
	struct FSimSmokeState
	{
		int32 SpawnedEvents = 0;
		int32 FailedEvents = 0;
		int32 PlacedCommands = 0;
		int32 WallsPlaced = 0;
		FDelegateHandle EventHandle;
		TArray<colony::Entity> Pawns;
		TArray<FVector> LastLocation;
		TArray<FVector> FirstLocation;
		int32 Frames = 0;
		int32 MovedFrames = 0;
		float MaxTravelCm = 0.0f;
		int64 StartSeconds = 0;
		double StartRealSeconds = 0.0;
		bool bSlateKeysWork = true;
	};

	// Presses a key through Slate, as the keyboard would. If the bridge did not react (`Expect` is
	// false) it hands the key to the bridge directly and remembers that the Slate path failed.
	bool SimSmokePressKey(USimSubsystem& Bridge, const FKey& Key, FSimSmokeState& State, TFunctionRef<bool()> Expect)
	{
		if (FSlateApplication::IsInitialized())
		{
			const FKeyEvent Event(Key, FModifierKeysState(), static_cast<uint32>(0), false, 0, 0);
			FSlateApplication::Get().ProcessKeyDownEvent(Event);
		}
		if (Expect())
		{
			return true;
		}
		State.bSlateKeysWork = false;
		Bridge.HandleKey(Key);
		return Expect();
	}
}

void USmokeTestSubsystem::BuildSimPhases()
{
	const TSharedRef<FSimSmokeState> State = MakeShared<FSimSmokeState>();

	// Phase 1 (1x): load defs, start a sim with pawns and items, send commands (one of them bad),
	// and check the events and the snapshot show them.
	Phases.Add({TEXT("sim_commands_and_events"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge)
		{
			Fail(TEXT("USimSubsystem did not start"));
			return;
		}
		Notes.Add(FString::Printf(TEXT("Default data folder %s: %d defs"), *Bridge->GetDataRoot(), Bridge->GetDefCount()));
		if (Bridge->GetDefCount() == 0)
		{
			Fail(TEXT("The sim loaded no defs from the project's data folder"));
		}

		// The project's data has no Things defs until the RimWorld import (D3) runs, so this test
		// plays on the test fixture's, with pawns that haul (they move, which shows the blending).
		FSimStartOptions Options;
		Options.DataRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Plugins/ColonySim/Tests/data"));
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.Pawns = 10;
		Options.Items = 40;
		Bridge->SetWorldOrigin(SmokeOrigin);
		if (!Bridge->StartSim(Options))
		{
			Fail(TEXT("The test defs did not load"));
			return;
		}
		Bridge->SetSpeed(1);
		State->EventHandle = Bridge->OnSimEvent.AddLambda([State](const colony::Event& Event)
		{
			if (Event.type == colony::EventType::ThingSpawned)
			{
				++State->SpawnedEvents;
			}
			else if (Event.type == colony::EventType::CommandFailed)
			{
				++State->FailedEvents;
			}
		});

		const colony::Sim* Sim = Bridge->GetSim();
		const colony::Snapshot& Opening = Bridge->GetCurrentSnapshot();
		Notes.Add(FString::Printf(TEXT("Opening snapshot: %d things on a %d m map"), static_cast<int32>(Opening.things.size()), Opening.mapWidth));
		if (Opening.things.empty())
		{
			Fail(TEXT("The opening snapshot has no things"));
		}

		// A row of walls on walkable ground just north of the middle, a bed, and one bad command.
		const int32 Middle = Options.MapSize / 2;
		for (int32 X = Middle - 12; X < Middle + 12 && State->WallsPlaced < 6; ++X)
		{
			if (Sim->map().walkable(colony::CellPos{X, Middle + 8, 0}))
			{
				Bridge->Submit(colony::SpawnThingCommand{"Wall", colony::CellPos{X, Middle + 8, 0}});
				++State->WallsPlaced;
				++State->PlacedCommands;
			}
		}
		for (int32 X = Middle + 3; X < Middle + 20; ++X)
		{
			if (Sim->map().walkable(colony::CellPos{X, Middle - 8, 0}))
			{
				Bridge->Submit(colony::SpawnThingCommand{"Steel", colony::CellPos{X, Middle - 8, 0}, 25});
				++State->PlacedCommands;
				break;
			}
		}
		Bridge->Submit(colony::SpawnThingCommand{"NoSuchThing", colony::CellPos{Middle, Middle, 0}});

		IConsoleVariable* DebugDraw = IConsoleManager::Get().FindConsoleVariable(TEXT("Sim.DebugDraw"));
		if (DebugDraw)
		{
			DebugDraw->Set(1);
		}
		const FVector Centre = Bridge->SimToWorld(Middle, Middle, 0);
		ViewFrom(Centre + FVector(-3200.0, 0.0, 3200.0), FRotator(-42.0, 0.0, 0.0));
	}, 2.0f, 4.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		Notes.Add(FString::Printf(TEXT("Commands: %d placed, %d ThingSpawned events, %d CommandFailed events"),
			State->PlacedCommands, State->SpawnedEvents, State->FailedEvents));
		// Hauling pawns put items down too, and each of those is a ThingSpawned event as well.
		if (State->SpawnedEvents < State->PlacedCommands)
		{
			Fail(FString::Printf(TEXT("Expected at least %d ThingSpawned events for the commands, got %d"), State->PlacedCommands, State->SpawnedEvents));
		}
		if (State->FailedEvents != 1)
		{
			Fail(FString::Printf(TEXT("Expected exactly 1 CommandFailed event (the bad def), got %d"), State->FailedEvents));
		}
		if (State->WallsPlaced < 3)
		{
			Fail(TEXT("Could not find walkable ground to place the test walls"));
		}

		// The snapshot shows the walls the commands made.
		int32 WallsInSnapshot = 0;
		const colony::DefDatabase* Defs = Bridge->GetDefs().get();
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			if (Defs && Defs->get(View.def).defName() == "Wall")
			{
				++WallsInSnapshot;
			}
		}
		if (WallsInSnapshot != State->WallsPlaced)
		{
			Fail(FString::Printf(TEXT("The snapshot has %d walls, the commands made %d"), WallsInSnapshot, State->WallsPlaced));
		}
	}});

	// Phase 2 (3x via the 3 key): game time runs at three times the speed, and a hauling pawn's
	// blended position moves on every frame even when a frame has no sim step in it.
	Phases.Add({TEXT("sim_speed_3x_blending"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		if (!SimSmokePressKey(*Bridge, EKeys::Three, *State, [Bridge]() { return Bridge->GetSpeed() == 3; }))
		{
			Fail(TEXT("The 3 key did not set 3x speed"));
		}
		State->StartSeconds = Bridge->GetGameSeconds();
		State->StartRealSeconds = FPlatformTime::Seconds();
		State->Frames = State->MovedFrames = 0;
		State->MaxTravelCm = 0.0f;
		State->Pawns.Reset();
		State->LastLocation.Reset();
		State->FirstLocation.Reset();
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			FVector Location;
			if (View.category == colony::ThingCategory::Pawn && State->Pawns.Num() < 10 && Bridge->GetBlendedLocation(View.id, Location))
			{
				State->Pawns.Add(View.id);
				State->LastLocation.Add(Location);
				State->FirstLocation.Add(Location);
			}
		}
		if (State->Pawns.IsEmpty())
		{
			Fail(TEXT("No pawns in the snapshot to follow"));
		}
	}, 1.0f, 8.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		const double RealSeconds = FPlatformTime::Seconds() - State->StartRealSeconds;
		const int64 GameSeconds = Bridge->GetGameSeconds() - State->StartSeconds;
		const double Expected = RealSeconds * colony::kStepsPerRealSecondAt1x * 3.0;
		const FSimTimings& Timings = Bridge->GetTimings();
		Notes.Add(FString::Printf(TEXT("3x: %lld game seconds in %.1f real seconds (full speed would be %.0f); sim cost %.2f ms per frame avg, %.2f max; %.1f steps per frame"),
			static_cast<long long>(GameSeconds), RealSeconds, Expected, Timings.AvgFrameMs, Timings.MaxFrameMs, Timings.StepsPerFrame));
		// The sim catches up after hitches only up to a cap, so allow some loss on a busy PC.
		if (static_cast<double>(GameSeconds) < Expected * 0.4)
		{
			Fail(FString::Printf(TEXT("At 3x only %lld of about %.0f game seconds passed"), static_cast<long long>(GameSeconds), Expected));
		}
		if (Timings.AvgFrameMs <= 0.0)
		{
			Fail(TEXT("The sim timings stayed at zero"));
		}
		const float MovedShare = State->Frames > 0 ? static_cast<float>(State->MovedFrames) / State->Frames : 0.0f;
		Notes.Add(FString::Printf(TEXT("Blending: pawns moved on screen in %d of %d frames (%.0f%%); furthest travel %.1f m"),
			State->MovedFrames, State->Frames, MovedShare * 100.0f, State->MaxTravelCm / 100.0f));
		// Only judge the blend if the pawns really travelled, otherwise they were just idle.
		if (State->MaxTravelCm > 100.0f && State->Frames >= 30 && MovedShare < 0.8f)
		{
			Fail(FString::Printf(TEXT("Pawns moved on screen in only %.0f%% of frames; the blend between snapshots is not smoothing them"), MovedShare * 100.0f));
		}
	}, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim() || State->Pawns.IsEmpty())
		{
			return;
		}
		bool bAnyMoved = false;
		for (int32 Index = 0; Index < State->Pawns.Num(); ++Index)
		{
			FVector Location;
			if (!Bridge->GetBlendedLocation(State->Pawns[Index], Location))
			{
				continue;
			}
			if (!Location.Equals(State->LastLocation[Index], 0.01))
			{
				bAnyMoved = true;
			}
			State->LastLocation[Index] = Location;
			State->MaxTravelCm = FMath::Max(State->MaxTravelCm, static_cast<float>(FVector::Dist(Location, State->FirstLocation[Index])));
		}
		++State->Frames;
		State->MovedFrames += bAnyMoved ? 1 : 0;
	}});

	// Phase 3: Space pauses and resumes, the period key steps once, F7 writes a replay, and that
	// replay plays back in a fresh sim to exactly the same state.
	Phases.Add({TEXT("sim_pause_and_replay"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		if (!SimSmokePressKey(*Bridge, EKeys::SpaceBar, *State, [Bridge]() { return Bridge->GetSpeed() == 0; }))
		{
			Fail(TEXT("Space did not pause"));
		}
		State->StartSeconds = Bridge->GetGameSeconds();
	}, 1.0f, 3.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		if (Bridge->GetGameSeconds() != State->StartSeconds)
		{
			Fail(FString::Printf(TEXT("The sim ran while paused (%lld game seconds)"), static_cast<long long>(Bridge->GetGameSeconds() - State->StartSeconds)));
		}
		const int64 Before = Bridge->GetGameSeconds();
		SimSmokePressKey(*Bridge, EKeys::Period, *State, [Bridge, Before]() { return Bridge->GetGameSeconds() == Before + 1; });
		if (Bridge->GetGameSeconds() != Before + 1)
		{
			Fail(TEXT("The period key did not step once while paused"));
		}
		if (!SimSmokePressKey(*Bridge, EKeys::SpaceBar, *State, [Bridge]() { return Bridge->GetSpeed() == 3; }))
		{
			Fail(TEXT("Space did not resume at the speed it had before the pause (3x)"));
		}
		// Pause again so the replay is written and replayed at one exact moment.
		Bridge->SetSpeed(0);

		const FString Path = Bridge->SaveReplay();
		if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
		{
			Fail(TEXT("The replay file was not written"));
		}
		else
		{
			colony::Replay Replay;
			std::string Error;
			if (!colony::loadReplayFile(std::string(TCHAR_TO_UTF8(*Path)), Replay, Error))
			{
				Fail(FString::Printf(TEXT("The replay file did not load: %s"), UTF8_TO_TCHAR(Error.c_str())));
			}
			else
			{
				colony::Sim Fresh(Replay.settings, Bridge->GetDefs());
				colony::playReplay(Replay, Fresh, Replay.endStep);
				const colony::ReplayCheck Check = colony::checkReplay(Replay, Fresh);
				const bool bSameEnd = Replay.hasEndHash && Fresh.stateHash() == Replay.endHash && Fresh.time().seconds == Bridge->GetGameSeconds();
				Notes.Add(FString::Printf(TEXT("Replay %s: %d commands, %lld steps; played again in a fresh sim: %d checkpoints compared, final hash %s"),
					*FPaths::GetCleanFilename(Path), static_cast<int32>(Replay.commands.size()), static_cast<long long>(Replay.endStep), Check.compared, bSameEnd ? TEXT("same") : TEXT("DIFFERENT")));
				if (!Check.matched() || !bSameEnd)
				{
					Fail(TEXT("Playing the saved replay in a fresh sim gave a different game"));
				}
			}
		}

		if (!State->bSlateKeysWork)
		{
			Notes.Add(TEXT("Keys sent through Slate did not reach the bridge in this run; the bridge's key handler was checked directly instead"));
		}
		else
		{
			Notes.Add(TEXT("Keys 3, Space and period worked through Slate input"));
		}

		// Leave the world as other tests expect it.
		if (State->EventHandle.IsValid())
		{
			Bridge->OnSimEvent.Remove(State->EventHandle);
		}
		if (IConsoleVariable* DebugDraw = IConsoleManager::Get().FindConsoleVariable(TEXT("Sim.DebugDraw")))
		{
			DebugDraw->Set(0);
		}
	}});
}

namespace
{
	struct FSavesSmokeState
	{
		FSimSmokeState Keys;
		int64 SavedSeconds = 0;
		uint64 SavedHash = 0;
		int32 SavedThings = 0;
		int32 SavedPawns = 0;
		int32 AutosavesBefore = 0;
	};

	int32 SavesSmokeCountPawns(const colony::Snapshot& Snapshot)
	{
		int32 Count = 0;
		for (const colony::ThingView& View : Snapshot.things)
		{
			Count += View.category == colony::ThingCategory::Pawn ? 1 : 0;
		}
		return Count;
	}
}

void USmokeTestSubsystem::BuildSavesPhases()
{
	const TSharedRef<FSavesSmokeState> State = MakeShared<FSavesSmokeState>();

	// Phase 1 (3x): a sim with hauling pawns runs while the autosave interval is 120 game seconds,
	// so autosaves must appear; then F5 writes the quicksave at a paused moment.
	Phases.Add({TEXT("saves_autosave_and_quicksave"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge)
		{
			Fail(TEXT("USimSubsystem did not start"));
			return;
		}
		IFileManager::Get().DeleteDirectory(*USimSubsystem::GetSaveDirectory(), false, true);
		FSimStartOptions Options;
		Options.DataRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Plugins/ColonySim/Tests/data"));
		Options.Seed = 7;
		Options.MapSize = 128;
		Options.Pawns = 10;
		Options.Items = 40;
		Bridge->SetWorldOrigin(SmokeOrigin);
		if (!Bridge->StartSim(Options))
		{
			Fail(TEXT("The test defs did not load"));
			return;
		}
		Bridge->SetAutosaveInterval(120);
		Bridge->SetSpeed(3);
		const FVector Centre = Bridge->SimToWorld(Options.MapSize / 2, Options.MapSize / 2, 0);
		ViewFrom(Centre + FVector(-3200.0, 0.0, 3200.0), FRotator(-42.0, 0.0, 0.0));
	}, 1.0f, 6.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		const int32 Autosaves = Bridge->GetAutosaveCount();
		Notes.Add(FString::Printf(TEXT("%d autosaves in %lld game seconds (interval 120)"), Autosaves, static_cast<long long>(Bridge->GetGameSeconds())));
		if (Autosaves < 2)
		{
			Fail(FString::Printf(TEXT("Expected at least 2 autosaves, got %d"), Autosaves));
		}
		const FString Directory = USimSubsystem::GetSaveDirectory();
		if (!IFileManager::Get().FileExists(*(Directory / TEXT("autosave-1.save"))) && !IFileManager::Get().FileExists(*(Directory / TEXT("autosave-2.save"))))
		{
			Fail(TEXT("No autosave file in Saved/Saves/"));
		}
		State->AutosavesBefore = Autosaves;

		// Pause so F5 captures one exact moment, then press it as the keyboard would.
		Bridge->SetSpeed(0);
		State->SavedSeconds = Bridge->GetGameSeconds();
		State->SavedHash = Bridge->GetSim()->stateHash();
		State->SavedThings = static_cast<int32>(Bridge->GetCurrentSnapshot().things.size());
		State->SavedPawns = SavesSmokeCountPawns(Bridge->GetCurrentSnapshot());
		const FString Quick = Directory / TEXT("quicksave.save");
		if (!SimSmokePressKey(*Bridge, EKeys::F5, State->Keys, [&Quick]() { return IFileManager::Get().FileExists(*Quick); }))
		{
			Fail(TEXT("F5 did not write the quicksave"));
		}
		Notes.Add(FString::Printf(TEXT("Quicksave at game second %lld: %lld bytes"), static_cast<long long>(State->SavedSeconds), IFileManager::Get().FileSize(*Quick)));
		Bridge->SetSpeed(3);
	}});

	// Phase 2 (3x): the game moves on, then F9 puts it back exactly, and running on from there
	// gives the same game as a second sim loaded from the same file.
	Phases.Add({TEXT("saves_quickload"), [this]() {}, 1.0f, 3.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		Bridge->SetSpeed(0);
		if (Bridge->GetGameSeconds() <= State->SavedSeconds)
		{
			Fail(TEXT("The game did not move on after the quicksave, so the quickload proves nothing"));
		}
		const int64 SecondsBefore = Bridge->GetGameSeconds();
		const int64 Target = State->SavedSeconds;
		if (!SimSmokePressKey(*Bridge, EKeys::F9, State->Keys, [Bridge, Target]() { return Bridge->GetGameSeconds() == Target; }))
		{
			Fail(TEXT("F9 did not load the quicksave"));
			return;
		}
		const uint64 LoadedHash = Bridge->GetSim()->stateHash();
		Notes.Add(FString::Printf(TEXT("Quickload went from game second %lld back to %lld; hash %s"),
			static_cast<long long>(SecondsBefore), static_cast<long long>(Bridge->GetGameSeconds()), LoadedHash == State->SavedHash ? TEXT("same") : TEXT("DIFFERENT")));
		if (LoadedHash != State->SavedHash)
		{
			Fail(TEXT("The loaded game has a different state hash than the one saved"));
		}
		if (static_cast<int32>(Bridge->GetCurrentSnapshot().things.size()) != State->SavedThings || SavesSmokeCountPawns(Bridge->GetCurrentSnapshot()) != State->SavedPawns)
		{
			Fail(TEXT("The snapshot after loading does not show the same things as when saved"));
		}

		// A second sim from the same file must run on exactly like the one the bridge loaded.
		std::vector<uint8_t> Bytes;
		std::string Error;
		if (!colony::readSaveFile(std::string(TCHAR_TO_UTF8(*(USimSubsystem::GetSaveDirectory() / TEXT("quicksave.save")))), Bytes, Error))
		{
			Fail(FString::Printf(TEXT("Could not read the quicksave: %s"), UTF8_TO_TCHAR(Error.c_str())));
			return;
		}
		colony::SaveLoadResult Other = colony::loadSim(Bytes, Bridge->GetDefs());
		if (!Other.sim)
		{
			Fail(FString::Printf(TEXT("A second load of the quicksave failed: %s"), UTF8_TO_TCHAR(Other.error.c_str())));
			return;
		}
		constexpr int32 RunOn = 300;
		for (int32 Step = 0; Step < RunOn; ++Step)
		{
			Bridge->StepOnce();
			Other.sim->step();
		}
		const bool bSame = Bridge->GetSim()->stateHash() == Other.sim->stateHash() && Bridge->GetGameSeconds() == Other.sim->time().seconds;
		Notes.Add(FString::Printf(TEXT("Ran %d steps on after the load; the bridge and a second sim loaded from the file are %s"), RunOn, bSame ? TEXT("the same") : TEXT("DIFFERENT")));
		if (!bSame)
		{
			Fail(TEXT("Running on after F9 gave a different game than running on from the same file"));
		}

		// A damaged file is refused and the running game stays as it is.
		const FString BadPath = USimSubsystem::GetSaveDirectory() / TEXT("broken.save");
		FFileHelper::SaveStringToFile(TEXT("this is not a save"), *BadPath);
		const int64 Now = Bridge->GetGameSeconds();
		const uint64 NowHash = Bridge->GetSim()->stateHash();
		if (Bridge->LoadGame(TEXT("broken")) || Bridge->GetGameSeconds() != Now || Bridge->GetSim()->stateHash() != NowHash)
		{
			Fail(TEXT("A damaged save was not refused cleanly"));
		}
		if (Bridge->LoadGame(TEXT("no-such-slot")))
		{
			Fail(TEXT("Loading a save that does not exist said it worked"));
		}
		Notes.Add(TEXT("A damaged save and a missing save were refused and the running game was untouched"));
		Bridge->SetSpeed(3);
	}});

	// Phase 3: the pawn actors followed the loaded sim, and F8 writes a bug report whose replay
	// plays back to the same state.
	Phases.Add({TEXT("saves_bug_report"), [this]() {}, 2.0f, 2.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!Bridge || !Bridge->GetSim())
		{
			return;
		}
		if (Pawns && Pawns->AreAssetsLoaded())
		{
			const int32 SimPawns = SavesSmokeCountPawns(Bridge->GetCurrentSnapshot());
			const int32 Actors = Pawns->GetPawns().Num();
			Notes.Add(FString::Printf(TEXT("%d pawn actors for %d sim pawns after the quickload"), Actors, SimPawns));
			if (Actors != SimPawns)
			{
				Fail(TEXT("The pawn actors do not match the sim's pawns after loading"));
			}
		}
		else
		{
			Notes.Add(TEXT("Pawn assets are not loaded in this run, so the pawn actors were not checked"));
		}

		Bridge->SetSpeed(0);
		FString Folder;
		const FString Reports = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("BugReports"));
		TArray<FString> Before;
		IFileManager::Get().FindFiles(Before, *(Reports / TEXT("*")), false, true);
		if (!SimSmokePressKey(*Bridge, EKeys::F8, State->Keys, [&]()
		{
			TArray<FString> After;
			IFileManager::Get().FindFiles(After, *(Reports / TEXT("*")), false, true);
			return After.Num() > Before.Num();
		}))
		{
			Fail(TEXT("F8 did not write a bug report folder"));
			return;
		}
		TArray<FString> After;
		IFileManager::Get().FindFiles(After, *(Reports / TEXT("*")), false, true);
		After.Sort();
		Folder = Reports / After.Last();
		for (const FString& Name : TArray<FString>{TEXT("replay.json"), TEXT("state.save"), TEXT("state.json"), TEXT("notes.txt")})
		{
			if (!IFileManager::Get().FileExists(*(Folder / Name)))
			{
				Fail(FString::Printf(TEXT("The bug report has no %s"), *Name));
			}
		}

		colony::Replay Replay;
		std::string Error;
		if (!colony::loadReplayFile(std::string(TCHAR_TO_UTF8(*(Folder / TEXT("replay.json")))), Replay, Error))
		{
			Fail(FString::Printf(TEXT("The bug report's replay did not load: %s"), UTF8_TO_TCHAR(Error.c_str())));
			return;
		}
		// The session was loaded from a save halfway, but the command log came with it, so the
		// replay from step 0 must still end in the same state.
		colony::Sim Fresh(Replay.settings, Bridge->GetDefs());
		colony::playReplay(Replay, Fresh, Replay.endStep);
		const bool bSameEnd = Replay.hasEndHash && Fresh.stateHash() == Replay.endHash && Fresh.time().seconds == Bridge->GetGameSeconds();
		Notes.Add(FString::Printf(TEXT("Bug report %s: replay of %d commands to step %lld played again in a fresh sim: %s"),
			*FPaths::GetCleanFilename(Folder), static_cast<int32>(Replay.commands.size()), static_cast<long long>(Replay.endStep), bSameEnd ? TEXT("same final hash") : TEXT("DIFFERENT")));
		if (!bSameEnd)
		{
			Fail(TEXT("The bug report's replay did not give the same game"));
		}
		if (!State->Keys.bSlateKeysWork)
		{
			Notes.Add(TEXT("Keys sent through Slate did not reach the bridge in this run; the bridge's key handler was used directly instead"));
		}
		else
		{
			Notes.Add(TEXT("F5, F9 and F8 worked through Slate input"));
		}
	}});
}

// ---------------------------------------------------------------------------
// Phases and measurement.

void USmokeTestSubsystem::StartPhase(int32 Index)
{
	PhaseIndex = Index;
	PhaseTime = 0.0f;
	bScreenshotTaken = false;
	Samples.Reset();
	if (TestName == TEXT("crowd"))
	{
		ClearSpawned();
	}
	UE_LOG(LogSmoke, Display, TEXT("Phase %s"), *Phases[Index].Name);
	Phases[Index].Setup();
}

void USmokeTestSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (bFinished)
	{
		return;
	}
	if (FPlatformTime::Seconds() - StartSeconds > TimeoutSeconds)
	{
		Fail(FString::Printf(TEXT("Timed out after %.0f s in phase %d"), TimeoutSeconds, PhaseIndex));
		WriteResultsAndQuit();
		return;
	}
	if (PhaseIndex < 0 || PhaseIndex >= Phases.Num())
	{
		return;
	}
	PhaseTime += DeltaTime;

	// Runners jog in circles around the crowd centre.
	int32 RunnerIndex = 0;
	for (AActor* Actor : Spawned)
	{
		ACharacter* Runner = Cast<ACharacter>(Actor);
		if (!Runner || !RunnerRadius.IsValidIndex(RunnerIndex))
		{
			continue;
		}
		const float Radius = RunnerRadius[RunnerIndex++];
		if (Radius == 0.0f)
		{
			continue;
		}
		const FVector ToRunner = (Runner->GetActorLocation() - CrowdCentre).GetSafeNormal2D();
		const FVector Tangent = FVector::CrossProduct(FVector::UpVector, ToRunner) * FMath::Sign(Radius);
		const float Drift = (FMath::Abs(Radius) - FVector::Dist2D(Runner->GetActorLocation(), CrowdCentre)) / 300.0f;
		Runner->AddMovementInput((Tangent + ToRunner * FMath::Clamp(Drift, -1.0f, 1.0f)).GetSafeNormal());
	}

	const FPhase& Phase = Phases[PhaseIndex];
	if (Phase.OnFrame)
	{
		Phase.OnFrame();
	}
	if (PhaseTime < Phase.WarmupSeconds)
	{
		return;
	}
	Samples.Add({DeltaTime * 1000.0f, static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime)),
		static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime)), static_cast<float>(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()))});
	// A second into the measuring, or earlier in a phase measured for less than that (the file is written a frame or two later).
	if (!bScreenshotTaken && PhaseTime >= Phase.WarmupSeconds + FMath::Min(1.0f, Phase.MeasureSeconds * 0.6f))
	{
		bScreenshotTaken = true;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("Smoke") / TestName / (Phase.Name + TEXT(".png"));
		// The sim tests show the on-screen sim timings and the overseer its ImGui panels, which count as UI.
		FScreenshotRequest::RequestScreenshot(Path, TestName == TEXT("sim") || TestName == TEXT("overseer") || TestName == TEXT("saves") || TestName == TEXT("protagonist") || TestName == TEXT("orders") || TestName == TEXT("panels") || TestName == TEXT("worldmap") || TestName == TEXT("fights") || TestName == TEXT("handson") || TestName == TEXT("sky") || TestName == TEXT("melee") || TestName == TEXT("levels"), false);
	}
	if (PhaseTime >= Phase.WarmupSeconds + Phase.MeasureSeconds)
	{
		FinishPhase();
	}
}

void USmokeTestSubsystem::FinishPhase()
{
	if (Phases[PhaseIndex].Verify)
	{
		Phases[PhaseIndex].Verify();
	}
	TArray<float> Frame, Game, Render, Gpu;
	for (const FFrameSample& S : Samples)
	{
		Frame.Add(S.FrameMs);
		Game.Add(S.GameMs);
		Render.Add(S.RenderMs);
		Gpu.Add(S.GpuMs);
	}
	const double AvgFrame = Frame.IsEmpty() ? 0.0 : [&Frame]() { double Sum = 0; for (float F : Frame) { Sum += F; } return Sum / Frame.Num(); }();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("phase"), Phases[PhaseIndex].Name);
	Result->SetNumberField(TEXT("frames"), Frame.Num());
	Result->SetNumberField(TEXT("avgFps"), AvgFrame > 0.0 ? 1000.0 / AvgFrame : 0.0);
	Result->SetNumberField(TEXT("p95FrameMs"), SmokePercentile(Frame, 0.95));
	Result->SetNumberField(TEXT("medianGameThreadMs"), SmokePercentile(Game, 0.5));
	Result->SetNumberField(TEXT("medianRenderThreadMs"), SmokePercentile(Render, 0.5));
	Result->SetNumberField(TEXT("medianGpuMs"), SmokePercentile(Gpu, 0.5));
	Result->SetNumberField(TEXT("actors"), Spawned.Num());
	Result->SetStringField(TEXT("screenshot"), TestName / (Phases[PhaseIndex].Name + TEXT(".png")));
	if (Frame.IsEmpty())
	{
		Fail(FString::Printf(TEXT("Phase %s produced no frames"), *Phases[PhaseIndex].Name));
	}
	Results.Add(MakeShared<FJsonValueObject>(Result));
	UE_LOG(LogSmoke, Display, TEXT("Phase %s: %.1f fps, GPU %.1f ms, game %.1f ms"), *Phases[PhaseIndex].Name,
		AvgFrame > 0.0 ? 1000.0 / AvgFrame : 0.0, SmokePercentile(Gpu, 0.5), SmokePercentile(Game, 0.5));

	if (PhaseIndex + 1 < Phases.Num())
	{
		StartPhase(PhaseIndex + 1);
	}
	else
	{
		WriteResultsAndQuit();
	}
}

void USmokeTestSubsystem::WriteResultsAndQuit()
{
	bFinished = true;
	PhaseIndex = Phases.Num();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("test"), TestName);
	Root->SetStringField(TEXT("status"), Errors.IsEmpty() ? TEXT("pass") : TEXT("fail"));
	Root->SetNumberField(TEXT("durationSeconds"), FPlatformTime::Seconds() - StartSeconds);
	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	Root->SetArrayField(TEXT("errors"), ErrorValues);
	Root->SetStringField(TEXT("gpu"), GRHIAdapterName);
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		Root->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%dx%d"), Size.X, Size.Y));
	}
	Root->SetArrayField(TEXT("phases"), Results);
	TArray<TSharedPtr<FJsonValue>> NoteValues;
	for (const FString& Note : Notes)
	{
		NoteValues.Add(MakeShared<FJsonValueString>(Note));
	}
	Root->SetArrayField(TEXT("notes"), NoteValues);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	const FString Path = FPaths::ProjectSavedDir() / TEXT("Smoke") / (TestName + TEXT(".json"));
	FFileHelper::SaveStringToFile(Json, *Path);
	UE_LOG(LogSmoke, Display, TEXT("Smoke test %s: %s, results written to %s"), *TestName, Errors.IsEmpty() ? TEXT("pass") : TEXT("FAIL"), *Path);

	FPlatformMisc::RequestExitWithStatus(false, Errors.IsEmpty() ? 0 : 1);
}

void USmokeTestSubsystem::Fail(const FString& Message)
{
	UE_LOG(LogSmoke, Error, TEXT("%s"), *Message);
	Errors.AddUnique(Message);
}

// ---------------------------------------------------------------------------
// Overseer: the free camera, picking against the sim, selection and the ImGui panels (U6).

namespace
{
	struct FOverseerSmokeState
	{
		AOverseerPawn* Pawn = nullptr;
		int32 Middle = 64;
		colony::CellPos ExpectedCell;
		colony::Entity ExpectedPawn = colony::kNoEntity;
		bool bCameraLimitsOk = true;
		FString CameraProblem;
	};

	// Where a world point lands on screen, in viewport pixels; false if it is behind the camera or off screen.
	bool OverseerSmokeProject(UWorld* World, const FVector& Point, FVector2D& OutScreen, float Margin = 0.0f)
	{
		APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
		if (!PC || !GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
		{
			return false;
		}
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		if (!PC->ProjectWorldLocationToScreen(Point, OutScreen, false))
		{
			return false;
		}
		return OutScreen.X >= Margin * Size.X && OutScreen.Y >= Margin * Size.Y && OutScreen.X < (1.0f - Margin) * Size.X && OutScreen.Y < (1.0f - Margin) * Size.Y;
	}

	// The Index-th cell (in a fixed scan) with open ground around it (no thing within 3 cells) that is visible on screen.
	// A click lands on what is drawn, so a rock cell is aimed at on top of its block, not at the ground under it.
	bool OverseerSmokeFindEmptyCell(UWorld* World, const USimSubsystem& Bridge, int32 Middle, int32 Index, colony::CellPos& OutCell, FVector2D& OutScreen)
	{
		int32 Found = 0;
		for (int32 Y = Middle - 40; Y <= Middle + 40; Y += 7)
		{
			for (int32 X = Middle - 40; X <= Middle + 40; X += 7)
			{
				const colony::CellPos Cell{X, Y, 0};
				if (!Bridge.GetSim()->map().inBounds(Cell))
				{
					continue;
				}
				bool bClear = true;
				for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
				{
					if (FMath::Abs(View.x - (X + 0.5f)) < 3.0f && FMath::Abs(View.y - (Y + 0.5f)) < 3.0f)
					{
						bClear = false;
						break;
					}
				}
				if (bClear && OverseerSmokeProject(World, Bridge.SimToWorld(X + 0.5f, Y + 0.5f, 0) + FVector(0.0, 0.0, TerrainDraw::SurfaceBonusCm(Bridge.GetSim()->map(), X + 0.5, Y + 0.5)), OutScreen, 0.1f) && Found++ == Index)
				{
					OutCell = Cell;
					return true;
				}
			}
		}
		return false;
	}
}

void USmokeTestSubsystem::BuildOverseerPhases()
{
	const TSharedRef<FOverseerSmokeState> State = MakeShared<FOverseerSmokeState>();

	// Phase 1: start a sim with pawns, items and a row of walls on a visible ground mesh, take the
	// overseer camera, and click (by ray) on the middle of every pawn, item and wall and on empty ground.
	Phases.Add({TEXT("overseer_picking"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
		if (!Bridge || !Overseer)
		{
			Fail(TEXT("USimSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.DataRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Plugins/ColonySim/Tests/data"));
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.Pawns = 12;
		Options.Items = 40;
		Bridge->SetWorldOrigin(SmokeOrigin);
		if (!Bridge->StartSim(Options))
		{
			Fail(TEXT("The test defs did not load"));
			return;
		}
		Bridge->SetSpeed(1);
		State->Middle = Options.MapSize / 2;
		int32 Walls = 0;
		for (int32 X = State->Middle - 12; X < State->Middle + 12 && Walls < 6; ++X)
		{
			if (Bridge->GetSim()->map().walkable(colony::CellPos{X, State->Middle + 8, 0}))
			{
				Bridge->Submit(colony::SpawnThingCommand{"Wall", colony::CellPos{X, State->Middle + 8, 0}});
				++Walls;
			}
		}
		if (IConsoleVariable* DebugDraw = IConsoleManager::Get().FindConsoleVariable(TEXT("Sim.DebugDraw")))
		{
			DebugDraw->Set(1);
		}

		State->Pawn = Overseer->EnterOverseerMode();
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		if (!State->Pawn || !PC || PC->GetPawn() != State->Pawn)
		{
			Fail(TEXT("The overseer camera pawn did not take over the player controller"));
			return;
		}
		// Zoomed far out so the whole map is on screen.
		State->Pawn->FocusOn(Bridge->SimToWorld(State->Middle, State->Middle, 0));
		State->Pawn->Zoom(-8.0f);
		State->Pawn->SnapCamera();
	}, 2.0f, 2.0f, [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !Bridge->GetSim() || !State->Pawn)
		{
			return;
		}
		int32 Tested = 0;
		int32 Exact = 0;
		int32 Missed = 0;
		int32 PawnsTested = 0;
		int32 WallsTested = 0;
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			FBox Box;
			FVector2D Screen;
			if (!SimPicking::ThingBox(*Bridge, View, Box) || !OverseerSmokeProject(GetWorld(), Box.GetCenter(), Screen, 0.02f))
			{
				continue;
			}
			++Tested;
			PawnsTested += View.category == colony::ThingCategory::Pawn ? 1 : 0;
			WallsTested += View.category == colony::ThingCategory::Building ? 1 : 0;
			const FSimPick Pick = State->Pawn->PickAtScreenPosition(Screen);
			if (Pick.Kind == ESimPickKind::Thing && Pick.Thing == View.id)
			{
				++Exact;
			}
			else if (Pick.Kind != ESimPickKind::Thing)
			{
				++Missed;  // a click on the middle of a thing found only ground or sky
			}
			// else: another thing stands in front of it, which is fine
		}
		Notes.Add(FString::Printf(TEXT("Picking: %d things on screen (%d pawns, %d walls), %d clicks picked exactly that thing, %d found no thing"), Tested, PawnsTested, WallsTested, Exact, Missed));
		if (PawnsTested < 3 || WallsTested < 2)
		{
			Fail(FString::Printf(TEXT("Too few things on screen to test picking (%d pawns, %d walls)"), PawnsTested, WallsTested));
		}
		if (Missed > 0 || Exact * 10 < Tested * 8)
		{
			Fail(FString::Printf(TEXT("Clicking a thing's middle picked it %d of %d times (%d found nothing)"), Exact, Tested, Missed));
		}

		// Empty ground: the cell under the click is the cell asked for.
		int32 CellsTested = 0;
		int32 CellsRight = 0;
		colony::CellPos Cell;
		FVector2D Screen;
		for (int32 Index = 0; Index < 8 && OverseerSmokeFindEmptyCell(GetWorld(), *Bridge, State->Middle, Index, Cell, Screen); ++Index)
		{
			++CellsTested;
			const FSimPick Pick = State->Pawn->PickAtScreenPosition(Screen);
			CellsRight += (Pick.Kind == ESimPickKind::Cell && Pick.Cell == Cell) ? 1 : 0;
		}
		Notes.Add(FString::Printf(TEXT("Picking: %d of %d clicks on empty ground gave the right cell"), CellsRight, CellsTested));
		if (CellsTested < 3 || CellsRight != CellsTested)
		{
			Fail(FString::Printf(TEXT("Clicks on empty ground gave the right cell %d of %d times"), CellsRight, CellsTested));
		}

		// A ray into the sky finds nothing.
		if (SimPicking::PickFromRay(*Bridge, State->Pawn->GetActorLocation(), FVector::UpVector).IsValid())
		{
			Fail(TEXT("A ray pointing at the sky picked something"));
		}
	}});

	// Phase 2: the camera methods the keys call (pan, turn, tilt, zoom, limits), then a view to look at.
	Phases.Add({TEXT("overseer_camera_controls"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		AOverseerPawn* Pawn = State->Pawn;
		if (!Bridge || !Bridge->GetSim() || !Pawn)
		{
			return;
		}
		const auto Problem = [&State](const FString& Text)
		{
			State->bCameraLimitsOk = false;
			State->CameraProblem += Text + TEXT("; ");
		};
		Pawn->ResetView();
		Pawn->FocusOn(Bridge->SimToWorld(State->Middle, State->Middle, 0));
		Pawn->SnapCamera();

		const FVector Before = Pawn->GetFocus();
		Pawn->Pan(0.0, 1.0, 0.5f);
		const FVector Moved = Pawn->GetFocus() - Before;
		if (Moved.X < 100.0 || FMath::Abs(Moved.Y) > 1.0)
		{
			Problem(FString::Printf(TEXT("pan forward at yaw 0 moved (%.0f, %.0f) cm, expected along +X"), Moved.X, Moved.Y));
		}
		Pawn->Rotate(90.0f, 0.0f);
		const FVector BeforeSide = Pawn->GetFocus();
		Pawn->Pan(0.0, 1.0, 0.5f);
		const FVector MovedSide = Pawn->GetFocus() - BeforeSide;
		if (MovedSide.Y < 100.0 || FMath::Abs(MovedSide.X) > 1.0)
		{
			Problem(FString::Printf(TEXT("pan forward at yaw 90 moved (%.0f, %.0f) cm, expected along +Y"), MovedSide.X, MovedSide.Y));
		}
		Pawn->Rotate(-90.0f, 0.0f);

		Pawn->Rotate(0.0f, 500.0f);
		if (!FMath::IsNearlyEqual(Pawn->GetTilt(), AOverseerPawn::kMaxTilt))
		{
			Problem(FString::Printf(TEXT("tilt %.1f after a big tilt down, expected the limit %.0f"), Pawn->GetTilt(), AOverseerPawn::kMaxTilt));
		}
		Pawn->Rotate(0.0f, -500.0f);
		if (!FMath::IsNearlyEqual(Pawn->GetTilt(), AOverseerPawn::kMinTilt))
		{
			Problem(FString::Printf(TEXT("tilt %.1f after a big tilt up, expected the limit %.0f"), Pawn->GetTilt(), AOverseerPawn::kMinTilt));
		}
		Pawn->ResetView();
		Pawn->Zoom(1.0f);
		const float OneIn = Pawn->GetTargetDistance();
		Pawn->ResetView();
		Pawn->Zoom(-1.0f);
		if (OneIn >= Pawn->GetTargetDistance())
		{
			Problem(TEXT("zooming in did not bring the camera closer than zooming out"));
		}
		Pawn->Zoom(500.0f);
		if (!FMath::IsNearlyEqual(Pawn->GetTargetDistance(), AOverseerPawn::kMinDistance))
		{
			Problem(FString::Printf(TEXT("zoom in stopped at %.0f cm, expected %.0f"), Pawn->GetTargetDistance(), AOverseerPawn::kMinDistance));
		}
		Pawn->Zoom(-500.0f);
		if (!FMath::IsNearlyEqual(Pawn->GetTargetDistance(), AOverseerPawn::kMaxDistance))
		{
			Problem(FString::Printf(TEXT("zoom out stopped at %.0f cm, expected %.0f"), Pawn->GetTargetDistance(), AOverseerPawn::kMaxDistance));
		}

		// Panning far past the edge keeps the focus on the map.
		Pawn->Pan(1.0, 1.0, 1000.0f, true);
		const FVector Local = (Pawn->GetFocus() - Bridge->GetWorldOrigin()) / 100.0;
		const int32 Size = Bridge->GetSim()->map().width();
		if (Local.X < 0.0 || Local.Y < 0.0 || Local.X > Size || Local.Y > Size)
		{
			Problem(FString::Printf(TEXT("focus left the map: (%.0f, %.0f) m on a %d m map"), Local.X, Local.Y, Size));
		}

		// A view to look at: closer, turned and tilted.
		Pawn->ResetView();
		Pawn->FocusOn(Bridge->SimToWorld(State->Middle, State->Middle + 2, 0));
		Pawn->Zoom(2.0f);
		Pawn->Rotate(25.0f, -5.0f);
		Pawn->SnapCamera();
		Notes.Add(FString::Printf(TEXT("Camera: yaw %.0f, tilt %.0f, %.0f m from the focus"), Pawn->GetYaw(), Pawn->GetTilt(), Pawn->GetDistance() / 100.0f));
	}, 1.0f, 2.0f, [=, this]()
	{
		if (!State->bCameraLimitsOk)
		{
			Fail(FString::Printf(TEXT("Camera: %s"), *State->CameraProblem));
		}
	}});

	// Phase 3: select a pawn with a click on screen. The inspect panel and the highlight show.
	Phases.Add({TEXT("overseer_select_pawn"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !State->Pawn)
		{
			return;
		}
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			FBox Box;
			FVector2D Screen;
			if (View.category == colony::ThingCategory::Pawn && SimPicking::ThingBox(*Bridge, View, Box) && OverseerSmokeProject(GetWorld(), Box.GetCenter(), Screen, 0.25f))
			{
				State->Pawn->SelectAtScreenPosition(Screen);
				State->ExpectedPawn = View.id;
				break;
			}
		}
		if (State->ExpectedPawn == colony::kNoEntity)
		{
			Fail(TEXT("No pawn near the middle of the screen to click on"));
		}
	}, 2.0f, 3.0f, [=, this]()
	{
		const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
		if (!Overseer)
		{
			return;
		}
		const FSimPick& Selection = Overseer->GetSelection();
		if (State->ExpectedPawn != colony::kNoEntity && (Selection.Kind != ESimPickKind::Thing || Selection.Thing != State->ExpectedPawn))
		{
			Fail(TEXT("Clicking a pawn did not select it (or the selection was lost while it walked)"));
		}
		Notes.Add(FString::Printf(TEXT("ImGui panels drawn in %d frames"), Overseer->GetPanelFramesDrawn()));
		if (Overseer->GetPanelFramesDrawn() < 10)
		{
			Fail(TEXT("The ImGui panels were not drawn (ImGui context not usable)"));
		}
	}});

	// Phase 4: select an empty ground cell.
	Phases.Add({TEXT("overseer_select_cell"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !State->Pawn)
		{
			return;
		}
		FVector2D Screen;
		if (!OverseerSmokeFindEmptyCell(GetWorld(), *Bridge, State->Middle, 2, State->ExpectedCell, Screen))
		{
			Fail(TEXT("No empty ground cell on screen to click on"));
			return;
		}
		State->Pawn->SelectAtScreenPosition(Screen);
	}, 2.0f, 3.0f, [=, this]()
	{
		const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
		if (!Overseer)
		{
			return;
		}
		const FSimPick& Selection = Overseer->GetSelection();
		if (Selection.Kind != ESimPickKind::Cell || !(Selection.Cell == State->ExpectedCell))
		{
			Fail(TEXT("Clicking empty ground did not select that cell"));
		}
	}});
}

// ---------------------------------------------------------------------------
// Pawns (U5): an actor per sim pawn that follows the sim, animation detail by distance, motion
// matching up close, loops far away, action animations from sim events, then the same as MetaHumans.

namespace
{
	struct FPawnsSmokeState
	{
		int64 Frames = 0;
		int32 MostMissing = 0;
		int32 MovementComponents = 0;
		double WorstFollowCm = 0.0;
		double Travelled = 0.0;
		int32 DetailCountErrors = 0;
		int32 DetailOrderErrors = 0;
		int32 TickIntervalErrors = 0;
		int64 FullSamples = 0;
		int64 FullMatched = 0;
		int64 MovingFullSamples = 0;
		int64 MovingFullOnLocomotion = 0;
		int64 SimpleSamples = 0;
		int64 SimpleOnLoops = 0;
		int64 NearSamples = 0;
		int64 FarSamples = 0;
		int32 JumpsAtStart = 0;
		TSet<FName> AssetsSeen;
		int32 ActionEventsAtStart = 0;
		int32 ActionsStartedAtStart = 0;
		int64 GroomFullSamples = 0;
		int64 GroomFullVisible = 0;
		int64 GroomOtherVisible = 0;
		int32 MetaHumanMissing = 0;
		TMap<uint32, FVector> LastLocation;
		TMap<uint32, FVector> LastSimLocation;

		void Reset() { *this = FPawnsSmokeState(); }
	};

	void PawnsSmokeSetMetaHumans(bool bOn)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.MetaHumans")))
		{
			Var->Set(bOn ? 1 : 0);
		}
	}

	bool PawnsSmokeMetaHumansOn()
	{
		const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.MetaHumans"));
		return Var && Var->GetInt() != 0;
	}

	int32 PawnsSmokeTotalJumps(const USimPawnSubsystem& Pawns)
	{
		int32 Jumps = 0;
		for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns.GetPawns())
		{
			if (const USimPawnAnimInstance* Anim = Pair.Value ? Pair.Value->GetAnim() : nullptr)
			{
				Jumps += Anim->GetStats().Jumps;
			}
		}
		return Jumps;
	}
}

void USmokeTestSubsystem::BuildPawnsPhases()
{
	const TSharedRef<FPawnsSmokeState> State = MakeShared<FPawnsSmokeState>();
	constexpr float Warmup = 5.0f;

	// The camera high up, looking north over the stockpile, where most pawns come and go.
	const auto OverseerView = [this]()
	{
		const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		const colony::CellPos Cell = Bridge->GetSim()->stockpileCell().value_or(colony::CellPos{0, 0, 0});
		const FVector Stockpile = Bridge->SimToWorld(Cell.x + 0.5f, Cell.y + 0.5f, 0);
		ViewFrom(Stockpile + FVector(0.0, -2500.0, 3000.0), FRotator(-50.0, 90.0, 0.0));
	};
	// Close to the stockpile, where pawns put things down and turn back.
	const auto StockpileView = [this]()
	{
		const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		const colony::CellPos Cell = Bridge->GetSim()->stockpileCell().value_or(colony::CellPos{0, 0, 0});
		const FVector Target = Bridge->SimToWorld(Cell.x + 0.5f, Cell.y + 0.5f, 0) + FVector(0.0, 0.0, 90.0);
		const FVector From = Target + FVector(-650.0, -450.0, 250.0);
		ViewFrom(From, (Target - From).Rotation());
	};

	// Every frame after the warm-up: actors match the snapshot, detail goes by distance, and each
	// pawn plays what its detail should play.
	const auto CheckFrame = [this, State, Warmup]()
	{
		if (PhaseTime < Warmup)
		{
			return;
		}
		const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		const APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
		if (!Bridge || !Bridge->GetSim() || !Pawns || !CameraManager || !Pawns->AreAssetsLoaded())
		{
			return;
		}
		if (State->Frames++ == 0)
		{
			// Counted from the first frame checked: the MetaHuman phases start with new actors.
			State->JumpsAtStart = PawnsSmokeTotalJumps(*Pawns);
		}
		const FVector Camera = CameraManager->GetCameraLocation();
		const bool bMetaHumans = PawnsSmokeMetaHumansOn();
		int32 Missing = 0;
		int32 PawnCount = 0;
		double FarthestFull = 0.0;
		double NearestOther = TNumericLimits<double>::Max();
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn)
			{
				continue;
			}
			++PawnCount;
			const ASimPawnActor* Pawn = Pawns->FindPawn(View.id);
			if (!Pawn)
			{
				++Missing;
				continue;
			}
			const FVector Location = Pawn->GetActorLocation();
			const FVector SimLocation = Bridge->SimToWorld(View.x, View.y, View.level);
			const uint32 Key = static_cast<uint32>(entt::to_integral(View.id));
			// Actors are placed where the sim stood when the pawn subsystem last ticked, which is this frame's
			// or the last frame's (the subsystems tick in no fixed order). After a slow frame the sim has caught
			// up many steps at once, so a pawn one frame behind is as far as all those steps, not one: it is
			// on time if it is near either place.
			double Follow = FVector::Dist2D(Location, SimLocation);
			if (const FVector* LastSim = State->LastSimLocation.Find(Key))
			{
				Follow = FMath::Min(Follow, FVector::Dist2D(Location, *LastSim));
			}
			State->LastSimLocation.Add(Key, SimLocation);
			State->WorstFollowCm = FMath::Max(State->WorstFollowCm, Follow);
			if (const FVector* Last = State->LastLocation.Find(Key))
			{
				State->Travelled += FVector::Dist2D(*Last, Location);
			}
			State->LastLocation.Add(Key, Location);

			const double Distance = FVector::Dist(Location, Camera);
			const bool bFull = Pawn->GetDetail() == ESimPawnDetail::Full;
			const float Interval = Pawn->GetBody()->GetComponentTickInterval();
			if (bFull)
			{
				FarthestFull = FMath::Max(FarthestFull, Distance);
				State->TickIntervalErrors += Interval > 0.0f ? 1 : 0;
			}
			else
			{
				NearestOther = FMath::Min(NearestOther, Distance);
				const bool bFar = Interval > 0.1f;
				State->FarSamples += bFar ? 1 : 0;
				State->NearSamples += bFar ? 0 : 1;
				State->TickIntervalErrors += Interval <= 0.0f ? 1 : 0;
			}
			// The stats are from the last update; a pawn whose detail just changed is skipped.
			const USimPawnAnimInstance* Anim = Pawn->GetAnim();
			if (Anim && Anim->GetStats().Detail == Pawn->GetDetail())
			{
				const FSimPawnAnimStats& Stats = Anim->GetStats();
				State->AssetsSeen.Add(Stats.PlayingAsset);
				const float Speed = FVector2D(View.vx, View.vy).Size() * colony::kStepsPerRealSecondAt1x * 100.0f;
				if (bFull)
				{
					++State->FullSamples;
					State->FullMatched += Stats.Database.IsNone() ? 0 : 1;
					if (Speed > 150.0f)
					{
						++State->MovingFullSamples;
						State->MovingFullOnLocomotion += Stats.PlayingAsset.ToString().Contains(TEXT("Idle")) ? 0 : 1;
					}
				}
				else
				{
					++State->SimpleSamples;
					const FName Asset = Stats.PlayingAsset;
					State->SimpleOnLoops += (Asset == Pawns->GetIdleLoop()->GetFName() || Asset == Pawns->GetWalkLoop()->GetFName()
						|| Asset == Pawns->GetRunLoop()->GetFName()) ? 1 : 0;
				}
			}
			if (Pawn->GetMetaHuman())
			{
				const int32 Visible = Pawn->CountVisibleGrooms();
				if (bFull)
				{
					++State->GroomFullSamples;
					State->GroomFullVisible += Visible > 0 ? 1 : 0;
				}
				else
				{
					State->GroomOtherVisible += Visible;
				}
			}
			else if (bMetaHumans)
			{
				++State->MetaHumanMissing;
			}
		}
		State->MostMissing = FMath::Max(State->MostMissing, Missing + FMath::Max(0, Pawns->GetPawns().Num() - PawnCount));
		const FSimPawnDetailCounts& Counts = Pawns->GetDetailCounts();
		State->DetailCountErrors += Counts.Full == FMath::Min(20, PawnCount) ? 0 : 1;
		State->DetailOrderErrors += FarthestFull <= NearestOther + 1.0 ? 0 : 1;
	};

	const auto StartPhaseCounters = [this, State]()
	{
		State->Reset();
		if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
		{
			State->ActionEventsAtStart = Pawns->GetActionEvents();
			State->ActionsStartedAtStart = Pawns->GetActionsStarted();
		}
	};

	// What every phase checks when it ends, and its notes.
	const auto VerifyCommon = [this, State](const TCHAR* Phase, bool bMetaHumans)
	{
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!Pawns)
		{
			return;
		}
		if (!Pawns->AreAssetsLoaded())
		{
			Fail(TEXT("Pawn meshes, loops or motion matching databases did not load"));
			return;
		}
		for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Pair : Pawns->GetPawns())
		{
			if (Pair.Value && Pair.Value->FindComponentByClass<UMovementComponent>())
			{
				++State->MovementComponents;
			}
		}
		const int32 Jumps = PawnsSmokeTotalJumps(*Pawns) - State->JumpsAtStart;
		const int32 Actions = Pawns->GetActionEvents() - State->ActionEventsAtStart;
		const int32 ActionsStarted = Pawns->GetActionsStarted() - State->ActionsStartedAtStart;
		Notes.Add(FString::Printf(TEXT("%s: %d pawn actors over %lld frames; worst distance from the sim %.0f cm; walked %.0f m in all"),
			Phase, Pawns->GetPawns().Num(), State->Frames, State->WorstFollowCm, State->Travelled / 100.0));
		Notes.Add(FString::Printf(TEXT("%s: full detail %lld samples (%.0f%% motion matched, moving ones %.0f%% on walk or run animations), %d new animations chosen; simple %lld samples (%.0f%% on the loops; %lld near, %lld far); %d animations seen"),
			Phase, State->FullSamples, State->FullSamples ? 100.0 * State->FullMatched / State->FullSamples : 0.0,
			State->MovingFullSamples ? 100.0 * State->MovingFullOnLocomotion / State->MovingFullSamples : 0.0, Jumps,
			State->SimpleSamples, State->SimpleSamples ? 100.0 * State->SimpleOnLoops / State->SimpleSamples : 0.0,
			State->NearSamples, State->FarSamples, State->AssetsSeen.Num()));
		Notes.Add(FString::Printf(TEXT("%s: %d pick-up and put-down events, %d action animations started"), Phase, Actions, ActionsStarted));

		if (State->Frames < 10)
		{
			Fail(FString::Printf(TEXT("%s: too few frames checked (%lld)"), Phase, State->Frames));
			return;
		}
		if (State->MostMissing > 0)
		{
			Fail(FString::Printf(TEXT("%s: up to %d pawns had no actor (or an actor had no pawn) in a frame"), Phase, State->MostMissing));
		}
		if (State->MovementComponents > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d pawn actors have a movement component"), Phase, State->MovementComponents));
		}
		// One step at 1x is 15 cm of walking, plus a frame of blending.
		if (State->WorstFollowCm > 50.0)
		{
			Fail(FString::Printf(TEXT("%s: a pawn actor was %.0f cm from its sim position"), Phase, State->WorstFollowCm));
		}
		if (State->Travelled < 100.0 * 100.0)
		{
			Fail(FString::Printf(TEXT("%s: pawn actors hardly moved (%.0f m in all)"), Phase, State->Travelled / 100.0));
		}
		if (State->DetailCountErrors > 0 || State->DetailOrderErrors > 0 || State->TickIntervalErrors > 0)
		{
			Fail(FString::Printf(TEXT("%s: detail by distance wrong in %d frames (count), %d frames (a full-detail pawn farther than a simple one), %d pawn-frames (update rate)"),
				Phase, State->DetailCountErrors, State->DetailOrderErrors, State->TickIntervalErrors));
		}
		if (State->FullSamples == 0 || State->FullMatched * 10 < State->FullSamples * 9)
		{
			Fail(FString::Printf(TEXT("%s: motion matching found a pose in %lld of %lld full-detail samples"), Phase, State->FullMatched, State->FullSamples));
		}
		if (State->MovingFullSamples == 0 || State->MovingFullOnLocomotion * 10 < State->MovingFullSamples * 7)
		{
			Fail(FString::Printf(TEXT("%s: walking full-detail pawns played walk or run animations in %lld of %lld samples"), Phase, State->MovingFullOnLocomotion, State->MovingFullSamples));
		}
		if (Jumps < 20)
		{
			Fail(FString::Printf(TEXT("%s: motion matching chose only %d new animations"), Phase, Jumps));
		}
		if (State->SimpleSamples == 0 || State->SimpleOnLoops * 20 < State->SimpleSamples * 19)
		{
			Fail(FString::Printf(TEXT("%s: simple-detail pawns played the loops in %lld of %lld samples"), Phase, State->SimpleOnLoops, State->SimpleSamples));
		}
		if (Actions < 3 || ActionsStarted * 10 < Actions * 9)
		{
			Fail(FString::Printf(TEXT("%s: %d action events, %d action animations started"), Phase, Actions, ActionsStarted));
		}
		if (bMetaHumans)
		{
			Notes.Add(FString::Printf(TEXT("%s: strand hair visible in %lld of %lld full-detail samples, %lld groom-frames visible on other pawns"),
				Phase, State->GroomFullVisible, State->GroomFullSamples, State->GroomOtherVisible));
			if (State->MetaHumanMissing > 0)
			{
				Fail(FString::Printf(TEXT("%s: %d pawn-frames had no MetaHuman"), Phase, State->MetaHumanMissing));
			}
			if (State->GroomOtherVisible > 0)
			{
				Fail(FString::Printf(TEXT("%s: strand hair showed on pawns below full detail"), Phase));
			}
			if (State->GroomFullSamples == 0 || State->GroomFullVisible < State->GroomFullSamples)
			{
				Fail(FString::Printf(TEXT("%s: strand hair missing on full-detail MetaHumans"), Phase));
			}
		}
	};

	// Phase 1: 100 hauling pawns as mannequins, seen from high up.
	Phases.Add({TEXT("pawns_overseer_100"), [=, this]()
	{
		USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
		if (!Bridge || !GetWorld()->GetSubsystem<USimPawnSubsystem>())
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		PawnsSmokeSetMetaHumans(false);
		FSimStartOptions Options;
		Options.DataRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Plugins/ColonySim/Tests/data"));
		Options.Seed = 7;
		Options.MapSize = 128;
		Options.Pawns = 100;
		Options.Items = 600;
		Bridge->SetWorldOrigin(SmokeOrigin);
		if (!Bridge->StartSim(Options))
		{
			Fail(TEXT("The test defs did not load"));
			return;
		}
		Bridge->SetSpeed(1);
		OverseerView();
		StartPhaseCounters();
	}, Warmup, 10.0f, [=, this]()
	{
		VerifyCommon(TEXT("Mannequins, overseer view"), false);
		if (State->FarSamples == 0 || State->NearSamples == 0)
		{
			Fail(TEXT("The overseer view should have pawns at all three detail levels"));
		}
	}, CheckFrame});

	// Phase 2: close up at the stockpile.
	Phases.Add({TEXT("pawns_close_up"), [=, this]()
	{
		StockpileView();
		StartPhaseCounters();
	}, Warmup, 8.0f, [=, this]()
	{
		VerifyCommon(TEXT("Mannequins, close up"), false);
	}, CheckFrame});

	// Phase 3: the same pawns as MetaHumans, strand hair only at full detail.
	Phases.Add({TEXT("pawns_metahumans_overseer_100"), [=, this]()
	{
		PawnsSmokeSetMetaHumans(true);
		OverseerView();
		StartPhaseCounters();
	}, Warmup, 10.0f, [=, this]()
	{
		VerifyCommon(TEXT("MetaHumans, overseer view"), true);
	}, CheckFrame});

	// Phase 4: MetaHumans close up.
	Phases.Add({TEXT("pawns_metahumans_close_up"), [=, this]()
	{
		StockpileView();
		StartPhaseCounters();
	}, Warmup, 8.0f, [=, this]()
	{
		VerifyCommon(TEXT("MetaHumans, close up"), true);
		PawnsSmokeSetMetaHumans(false);
	}, CheckFrame});
}
