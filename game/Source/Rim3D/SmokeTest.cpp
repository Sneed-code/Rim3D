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
#include "GameFramework/CharacterMovementComponent.h"
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

#include "SimSubsystem.h"

#include "ColonySim/Map.h"
#include "ColonySim/Random.h"
#include "ColonySim/Replay.h"
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
	constexpr int32 MapSize = 1024;
	constexpr int32 Chunk = 32;
	const colony::Map Map(colony::MapSettings{MapSize, MapSize, colony::splitMix64(42 ^ 0x6D6170ULL)});

	AActor* Terrain = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), SmokeOrigin, FRotator::ZeroRotator);
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Terrain, TEXT("Terrain"));
	Mesh->bUseAsyncCooking = true;
	Terrain->SetRootComponent(Mesh);
	Mesh->SetWorldLocation(SmokeOrigin);
	Mesh->RegisterComponent();
	Spawned.Add(Terrain);

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
	const auto HeightAt = [&Map](int32 X, int32 Y)
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
	if (!bScreenshotTaken && PhaseTime >= Phase.WarmupSeconds + 1.0f)
	{
		bScreenshotTaken = true;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("Smoke") / TestName / (Phase.Name + TEXT(".png"));
		// The sim test shows the on-screen sim timings, which count as UI.
		FScreenshotRequest::RequestScreenshot(Path, TestName == TEXT("sim"), false);
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
