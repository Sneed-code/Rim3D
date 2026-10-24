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
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
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
		if (!GetWorld()->GetSubsystem<USimSubsystem>())
		{
			Fail(TEXT("USimSubsystem did not start"));
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
	// The lineup shows fixed examples first (heavy, thin, Impid), then seeded ones.
	colony::Rng Rng(static_cast<uint64_t>(Seed));
	float Height = 0.92f + Rng.unit() * 0.16f;
	float Width = 0.9f + Rng.unit() * 0.25f;
	bool bImpid = Rng.below(8) == 0;
	const int32 Slot = Seed % 1000;
	if (Seed < 2000 && Slot == 0) { Width = 1.3f; Height = 1.0f; bImpid = false; }
	if (Seed < 2000 && Slot == 1) { Width = 0.8f; Height = 1.05f; bImpid = false; }
	if (Seed < 2000 && Slot == 2) { Width = 1.0f; Height = 0.95f; bImpid = true; }

	// Width scales the body sideways and front to back; a stand-in for real body types.
	MetaHuman->SetActorRelativeScale3D(FVector(Width, Width, Height));
	OutDescription = FString::Printf(TEXT("height %.2f, width %.2f%s"), Height, Width, bImpid ? TEXT(", Impid") : TEXT(""));
	if (!bImpid)
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
		FScreenshotRequest::RequestScreenshot(Path, false, false);
	}
	if (PhaseTime >= Phase.WarmupSeconds + Phase.MeasureSeconds)
	{
		FinishPhase();
	}
}

void USmokeTestSubsystem::FinishPhase()
{
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
