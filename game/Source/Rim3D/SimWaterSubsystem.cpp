#include "SimWaterSubsystem.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"

#include "AnimalActor.h"
#include "AnimalSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

#include <cmath>

DEFINE_LOG_CATEGORY_STATIC(LogSimWater, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GWaterEnable(
		TEXT("Water.Enable"), 1, TEXT("1 draws water as a real surface (M_Water), 0 draws the old opaque water colour on the ground."), ECVF_Default);
	TAutoConsoleVariable<int32> GWaterChunksPerFrame(
		TEXT("Water.ChunksPerFrame"), 4, TEXT("How many water chunks a frame are checked for changed water (a moisture pump drying a cell)."), ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs GWaterParamCommand(
		TEXT("Water.Param"), TEXT("Water.Param Name V, or Water.Param Name R G B: sets a parameter of the water material (RippleStrength, Absorption, ...)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			USimWaterSubsystem* Water = World ? World->GetSubsystem<USimWaterSubsystem>() : nullptr;
			if (!Water || Args.Num() < 2)
			{
				return;
			}
			if (Args.Num() >= 4)
			{
				Water->SetWaterVector(FName(*Args[0]), FLinearColor(FCString::Atof(*Args[1]), FCString::Atof(*Args[2]), FCString::Atof(*Args[3]), 0.0f));
			}
			else
			{
				Water->SetWaterParam(FName(*Args[0]), FCString::Atof(*Args[1]));
			}
		}));

	const TCHAR* const WaterDrawMaterialPath = TEXT("/Game/Rim3D/Generated/Water/M_Water.M_Water");
	const TCHAR* const WaterWaveStepPath = TEXT("/Game/Rim3D/Generated/Water/M_WaterWaveStep.M_WaterWaveStep");

	TAutoConsoleVariable<int32> GWaterRipples(
		TEXT("Water.Ripples"), 1, TEXT("1 runs the wave simulation round the camera (bodies leave wakes and rings), 0 turns it off."), ECVF_Default);
	TAutoConsoleVariable<int32> GWaterRippleMask(
		TEXT("Water.RippleMask"), 1, TEXT("1 keeps waves to open water (the map's water cells), 0 lets them run everywhere (a test)."), ECVF_Default);
	TAutoConsoleVariable<float> GWaterRipplePush(
		TEXT("Water.RipplePush"), 1.0f, TEXT("How hard bodies push the wave simulation (1 normal)."), ECVF_Default);

	// The wave area: 256 x 256 texels over 64 m (25 cm a texel) round the camera, moved in whole texels so it never swims.
	constexpr int32 WaterWaveRes = 256;
	constexpr float WaterWaveTexelM = 0.25f;
	// Speed of the simulated ripples (m/s) and how long they take to fade to a third (s). Ours: small ripples on a river.
	constexpr float WaterWaveSpeed = 0.8f;
	constexpr float WaterWaveFade = 1.2f;
	constexpr int32 WaterWavePushes = 16;
	// Bodies in water shallower than this push nothing (a film of marsh water).
	constexpr float WaterWaveMinDepthM = 0.05f;

	// Cells of margin round open water: the surface runs on under the bank and the ground's own height cuts the shoreline.
	constexpr int32 WaterDrawMargin = 2;
	// The margin's outer edge is sunk this far under the ground, so the surface never ends in the air where a bank is low.
	constexpr float WaterDrawSkirtM = 0.15f;

	int32 WaterDrawChunksFor(int32 Cells)
	{
		return (Cells + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	}

	/** Open water the surface is drawn over: Water and ShallowWater with a surface. */
	bool WaterDrawIsOpen(const colony::Map& Map, int32 X, int32 Y)
	{
		if (X < 0 || Y < 0 || X >= Map.width() || Y >= Map.height())
		{
			return false;
		}
		const colony::Terrain T = Map.terrain(X, Y);
		return (T == colony::Terrain::Water || T == colony::Terrain::ShallowWater) && !std::isnan(Map.waterSurface(colony::CellPos{X, Y, 0}));
	}

	/** The surface height (m) drawn over a cell: its own for open water, a neighbour's for the one-cell margin, NaN otherwise. */
	float WaterDrawSurface(const colony::Map& Map, int32 X, int32 Y)
	{
		if (WaterDrawIsOpen(Map, X, Y))
		{
			return Map.waterSurface(colony::CellPos{X, Y, 0});
		}
		if (X < 0 || Y < 0 || X >= Map.width() || Y >= Map.height())
		{
			return std::nanf("");
		}
		float Best = std::nanf("");
		for (int32 DY = -WaterDrawMargin; DY <= WaterDrawMargin; ++DY)
		{
			for (int32 DX = -WaterDrawMargin; DX <= WaterDrawMargin; ++DX)
			{
				if ((DX || DY) && WaterDrawIsOpen(Map, X + DX, Y + DY))
				{
					const float S = Map.waterSurface(colony::CellPos{X + DX, Y + DY, 0});
					Best = std::isnan(Best) ? S : FMath::Max(Best, S);
				}
			}
		}
		return Best;
	}

	uint64 WaterDrawMix(uint64 H, uint64 V)
	{
		H ^= V + 0x9E3779B97F4A7C15ULL + (H << 6) + (H >> 2);
		return H;
	}
}

bool USimWaterSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USimWaterSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USimWaterSubsystem, STATGROUP_Tickables);
}

bool USimWaterSubsystem::WantsSurface()
{
	if (GWaterEnable.GetValueOnGameThread() == 0)
	{
		return false;
	}
	static TWeakObjectPtr<UMaterialInterface> Cached;
	static bool bLooked = false;
	if (!bLooked || !Cached.IsValid())
	{
		bLooked = true;
		Cached = LoadObject<UMaterialInterface>(nullptr, WaterDrawMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	return Cached.IsValid();
}

void USimWaterSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void USimWaterSubsystem::Deinitialize()
{
	Reset();
	Super::Deinitialize();
}

void USimWaterSubsystem::Reset()
{
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
	Chunks.Reset();
	StopWaves();
	WaterMID = nullptr;
	BuiltSim = nullptr;
	BuiltGeneration = -1;
	Cursor = 0;
	Stats = FWaterStats();
}

void USimWaterSubsystem::SetWaterParam(FName Name, float Value)
{
	ScalarOverrides.Add(Name, Value);
	if (WaterMID)
	{
		WaterMID->SetScalarParameterValue(Name, Value);
	}
}

void USimWaterSubsystem::SetWaterVector(FName Name, const FLinearColor& Value)
{
	VectorOverrides.Add(Name, Value);
	if (WaterMID)
	{
		WaterMID->SetVectorParameterValue(Name, Value);
	}
}

UProceduralMeshComponent* USimWaterSubsystem::GetChunkMesh(int32 ChunkX, int32 ChunkY) const
{
	if (ChunkX < 0 || ChunkY < 0 || ChunkX >= ChunksX || ChunkY >= ChunksY || Chunks.Num() != ChunksX * ChunksY)
	{
		return nullptr;
	}
	return Chunks[ChunkIndex(ChunkX, ChunkY)].Mesh;
}

uint64 USimWaterSubsystem::HashChunk(const colony::Map& Map, int32 ChunkX, int32 ChunkY) const
{
	// The open-water cells of the chunk and a two-cell ring round it (the margin of a neighbour's water reaches in).
	const int32 X0 = ChunkX * TerrainDraw::ChunkCells - WaterDrawMargin - 1;
	const int32 Y0 = ChunkY * TerrainDraw::ChunkCells - WaterDrawMargin - 1;
	const int32 X1 = FMath::Min((ChunkX + 1) * TerrainDraw::ChunkCells, Map.width()) + WaterDrawMargin + 1;
	const int32 Y1 = FMath::Min((ChunkY + 1) * TerrainDraw::ChunkCells, Map.height()) + WaterDrawMargin + 1;
	uint64 H = 1469598103934665603ULL;
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		uint64 Row = 0;
		for (int32 X = X0; X < X1; ++X)
		{
			Row = (Row << 1) ^ (Row >> 63) ^ (WaterDrawIsOpen(Map, X, Y) ? 1ULL : 0ULL);
		}
		H = WaterDrawMix(H, Row ^ (static_cast<uint64>(Y) << 40));
	}
	return H;
}

void USimWaterSubsystem::BuildChunk(const colony::Map& Map, int32 ChunkX, int32 ChunkY)
{
	const double Start = FPlatformTime::Seconds();
	FChunk& Chunk = Chunks[ChunkIndex(ChunkX, ChunkY)];
	const int32 MapW = Map.width();
	const int32 MapH = Map.height();
	const int32 X0 = ChunkX * TerrainDraw::ChunkCells;
	const int32 Y0 = ChunkY * TerrainDraw::ChunkCells;
	const int32 X1 = FMath::Min(X0 + TerrainDraw::ChunkCells, MapW);
	const int32 Y1 = FMath::Min(Y0 + TerrainDraw::ChunkCells, MapH);
	const int32 Nx = X1 - X0;
	const int32 Ny = Y1 - Y0;
	const int32 Side = Nx + 1;

	// The surface over each cell of the chunk and a cell round it (corners on the chunk's edge need their outer cells).
	const int32 SW = Nx + 2;
	TArray<float> Surface;
	Surface.SetNumUninitialized(SW * (Ny + 2));
	for (int32 Y = -1; Y <= Ny; ++Y)
	{
		for (int32 X = -1; X <= Nx; ++X)
		{
			Surface[(Y + 1) * SW + (X + 1)] = WaterDrawSurface(Map, X0 + X, Y0 + Y);
		}
	}
	const auto SurfaceOf = [&Surface, SW](int32 LX, int32 LY) { return Surface[(LY + 1) * SW + (LX + 1)]; };

	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0, UV1, UV2;
	TArray<FColor> Colors;
	TArray<int32> Triangles;
	TArray<int32> VertexOf;
	VertexOf.Init(-1, Side * (Ny + 1));

	const auto Vertex = [&](int32 VX, int32 VY) -> int32
	{
		int32& Slot = VertexOf[VY * Side + VX];
		if (Slot >= 0)
		{
			return Slot;
		}
		// A corner's surface is the highest of the drawn cells round it, its flow the mean of the open cells' flow.
		float Height = std::nanf("");
		bool bOuter = false;
		float Fx = 0.0f, Fy = 0.0f;
		int32 Flows = 0;
		for (int32 DY = -1; DY <= 0; ++DY)
		{
			for (int32 DX = -1; DX <= 0; ++DX)
			{
				const float S = SurfaceOf(VX + DX, VY + DY);
				if (!std::isnan(S))
				{
					Height = std::isnan(Height) ? S : FMath::Max(Height, S);
				}
				else
				{
					bOuter = true;
				}
				const int32 CX = X0 + VX + DX;
				const int32 CY = Y0 + VY + DY;
				if (WaterDrawIsOpen(Map, CX, CY))
				{
					float Vx = 0.0f, Vy = 0.0f;
					Map.flowAt(colony::CellPos{CX, CY, 0}, Vx, Vy);
					Fx += Vx;
					Fy += Vy;
					++Flows;
				}
			}
		}
		const int32 MX = X0 + VX;
		const int32 MY = Y0 + VY;
		// The ground under the corner, as the terrain mesh draws it (the map's last corner reuses the last cell's).
		const float Ground = Map.groundHeight(FMath::Clamp(MX, 0, MapW - 1), FMath::Clamp(MY, 0, MapH - 1));
		// The water's true depth here (negative under the bank), before the skirt: it runs smoothly through the real
		// shoreline, so the shore foam and fade follow the bank's line and not the cells' edges.
		const float TrueDepth = Height - Ground;
		// On the edge of the drawn water the surface dives under the ground (a skirt), so it is always cut by the bank.
		if (bOuter)
		{
			Height = FMath::Min(Height, Ground - WaterDrawSkirtM);
		}
		if (Flows > 0)
		{
			Fx /= Flows;
			Fy /= Flows;
		}
		Slot = Positions.Add(FVector(MX * 100.0, MY * 100.0, static_cast<double>(Height) * 100.0));
		Normals.Add(FVector::UpVector);
		UV0.Add(FVector2D(MX, MY));
		UV1.Add(FVector2D(Fx, Fy));
		UV2.Add(FVector2D(TrueDepth, 0.0f));
		Colors.Add(FColor::White);
		return Slot;
	};

	for (int32 Y = 0; Y < Ny; ++Y)
	{
		for (int32 X = 0; X < Nx; ++X)
		{
			if (std::isnan(SurfaceOf(X, Y)))
			{
				continue;
			}
			const int32 A = Vertex(X, Y);
			const int32 B = Vertex(X, Y + 1);
			const int32 C = Vertex(X + 1, Y);
			const int32 D = Vertex(X + 1, Y + 1);
			Triangles.Append({A, B, C, C, B, D});
		}
	}

	if (!Chunk.Mesh && !Positions.IsEmpty())
	{
		Chunk.Mesh = NewObject<UProceduralMeshComponent>(Holder);
		Chunk.Mesh->bUseAsyncCooking = false;
		Chunk.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Chunk.Mesh->SetCanEverAffectNavigation(false);
		Chunk.Mesh->SetCastShadow(false);
		Chunk.Mesh->bReceivesDecals = false;
		Chunk.Mesh->SetupAttachment(Holder->GetRootComponent());
		Chunk.Mesh->RegisterComponent();
	}
	if (Chunk.Mesh)
	{
		if (Positions.IsEmpty())
		{
			Chunk.Mesh->ClearMeshSection(0);
		}
		else
		{
			Chunk.Mesh->CreateMeshSection(0, Positions, Triangles, Normals, UV0, UV1, UV2, TArray<FVector2D>(), Colors, TArray<FProcMeshTangent>(), false);
			if (WaterMID)
			{
				Chunk.Mesh->SetMaterial(0, WaterMID);
			}
			else if (Material)
			{
				Chunk.Mesh->SetMaterial(0, Material);
			}
		}
	}
	const int32 NewTriangles = Triangles.Num() / 3;
	if ((Chunk.Triangles > 0) != (NewTriangles > 0))
	{
		Stats.Chunks += NewTriangles > 0 ? 1 : -1;
	}
	Stats.Triangles += NewTriangles - Chunk.Triangles;
	Stats.Vertices += Positions.Num() - Chunk.Vertices;
	Chunk.Triangles = NewTriangles;
	Chunk.Vertices = Positions.Num();
	++Stats.Rebuilds;
	Stats.LastRebuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	Stats.SlowestRebuildMs = FMath::Max(Stats.SlowestRebuildMs, Stats.LastRebuildMs);
}

void USimWaterSubsystem::RebuildAll()
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !GetWorld())
	{
		return;
	}
	Reset();
	const colony::Sim& Sim = *Source->GetSim();
	const double Start = FPlatformTime::Seconds();
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	BuiltOrigin = Source->GetWorldOrigin();
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), BuiltOrigin, FRotator::ZeroRotator, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("WaterRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(BuiltOrigin);
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("Water"));
#endif
	Material = LoadObject<UMaterialInterface>(nullptr, WaterDrawMaterialPath);
	WaterMID = Material ? UMaterialInstanceDynamic::Create(Material, Holder) : nullptr;
	if (WaterMID)
	{
		for (const TPair<FName, float>& P : ScalarOverrides)
		{
			WaterMID->SetScalarParameterValue(P.Key, P.Value);
		}
		for (const TPair<FName, FLinearColor>& P : VectorOverrides)
		{
			WaterMID->SetVectorParameterValue(P.Key, P.Value);
		}
	}
	BuiltSim = &Sim;
	BuiltGeneration = Source->GetSimGeneration();

	const colony::Map& Map = Sim.map();
	ChunksX = WaterDrawChunksFor(Map.width());
	ChunksY = WaterDrawChunksFor(Map.height());
	Chunks.SetNum(ChunksX * ChunksY);
	for (int32 Y = 0; Y < ChunksY; ++Y)
	{
		for (int32 X = 0; X < ChunksX; ++X)
		{
			Chunks[ChunkIndex(X, Y)].Hash = HashChunk(Map, X, Y);
			BuildChunk(Map, X, Y);
		}
	}
	Stats.FullBuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	UE_LOG(LogSimWater, Log, TEXT("Water: %d chunks, %lld triangles in %.1f ms"), Stats.Chunks, Stats.Triangles, Stats.FullBuildMs);
}

void USimWaterSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !WantsSurface())
	{
		if (Holder)
		{
			Reset();
		}
		return;
	}
	const colony::Sim& Sim = *Source->GetSim();
	if (!Holder || BuiltGeneration != Source->GetSimGeneration() || BuiltSim != &Sim)
	{
		RebuildAll();
		return;
	}
	if (Source->GetWorldOrigin() != BuiltOrigin)
	{
		BuiltOrigin = Source->GetWorldOrigin();
		Holder->SetActorLocation(BuiltOrigin);
	}
	// A few chunks a frame: remade when their open water changed.
	const colony::Map& Map = Sim.map();
	const int32 Total = ChunksX * ChunksY;
	const int32 Checks = FMath::Clamp(GWaterChunksPerFrame.GetValueOnGameThread(), 0, Total);
	for (int32 I = 0; I < Checks; ++I)
	{
		Cursor = (Cursor + 1) % Total;
		const int32 X = Cursor % ChunksX;
		const int32 Y = Cursor / ChunksX;
		const uint64 Hash = HashChunk(Map, X, Y);
		if (Hash != Chunks[Cursor].Hash)
		{
			Chunks[Cursor].Hash = Hash;
			BuildChunk(Map, X, Y);
			bWaveMaskDirty = true;
		}
	}
	TickWaves(DeltaTime);
}

UTextureRenderTarget2D* USimWaterSubsystem::GetWaveTarget() const
{
	return WaveTargets[0] ? WaveTargets[(WaveTurn + 1) % 3].Get() : nullptr;
}

void USimWaterSubsystem::StopWaves()
{
	if (WaterMID)
	{
		WaterMID->SetScalarParameterValue(TEXT("WaveStrength"), 0.0f);
	}
	for (int32 I = 0; I < 3; ++I)
	{
		WaveTargets[I] = nullptr;
		WaveStepMIDs[I] = nullptr;
	}
	WaveCorner = FIntPoint(MAX_int32, MAX_int32);
	WaveBodies.Reset();
	WavePushes = 0;
}

void USimWaterSubsystem::BuildWaveMask(const colony::Map& Map)
{
	const int32 W = Map.width();
	const int32 H = Map.height();
	if (!WaveMask || WaveMask->GetSizeX() != W || WaveMask->GetSizeY() != H)
	{
		WaveMask = UTexture2D::CreateTransient(W, H, PF_G8);
		if (!WaveMask)
		{
			return;
		}
		WaveMask->SRGB = false;
		WaveMask->Filter = TF_Bilinear;
		WaveMask->AddressX = TA_Clamp;
		WaveMask->AddressY = TA_Clamp;
	}
	FTexture2DMipMap& Mip = WaveMask->GetPlatformData()->Mips[0];
	uint8* Bytes = static_cast<uint8*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
	for (int32 Y = 0; Y < H; ++Y)
	{
		for (int32 X = 0; X < W; ++X)
		{
			Bytes[Y * W + X] = WaterDrawIsOpen(Map, X, Y) ? 255 : 0;
		}
	}
	Mip.BulkData.Unlock();
	WaveMask->UpdateResource();
	bWaveMaskDirty = false;
}

void USimWaterSubsystem::TickWaves(float DeltaTime)
{
	USimSubsystem* Source = Bridge.Get();
	UWorld* World = GetWorld();
	if (!Source || !Source->GetSim() || !WaterMID || !World || GWaterRipples.GetValueOnGameThread() == 0 || DeltaTime <= 0.0f)
	{
		if (WaveTargets[0])
		{
			StopWaves();
		}
		return;
	}
	const colony::Sim& Sim = *Source->GetSim();
	const colony::Map& Map = Sim.map();
	if (!WaveTargets[0])
	{
		UMaterialInterface* StepMaterial = LoadObject<UMaterialInterface>(nullptr, WaterWaveStepPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!StepMaterial)
		{
			return;
		}
		for (int32 I = 0; I < 3; ++I)
		{
			WaveTargets[I] = UKismetRenderingLibrary::CreateRenderTarget2D(this, WaterWaveRes, WaterWaveRes, RTF_RGBA16f, FLinearColor::Black, false);
			WaveTargets[I]->AddressX = TA_Clamp;
			WaveTargets[I]->AddressY = TA_Clamp;
			WaveTargets[I]->UpdateResourceImmediate(true);
		}
		// Turn I reads the field before (I) and now (I + 1) and writes the next (I + 2).
		for (int32 I = 0; I < 3; ++I)
		{
			WaveStepMIDs[I] = UMaterialInstanceDynamic::Create(StepMaterial, this);
			WaveStepMIDs[I]->SetTextureParameterValue(TEXT("WavePrev"), WaveTargets[I]);
			WaveStepMIDs[I]->SetTextureParameterValue(TEXT("WaveCur"), WaveTargets[(I + 1) % 3]);
			WaveStepMIDs[I]->SetScalarParameterValue(TEXT("Res"), WaterWaveRes);
		}
		WaveTurn = 0;
		bWaveMaskDirty = true;
	}
	if (bWaveMaskDirty || !WaveMask)
	{
		BuildWaveMask(Map);
	}
	++WaveFrame;
	const float Dt = FMath::Clamp(DeltaTime, 1.0f / 240.0f, 1.0f / 30.0f);
	WaveTime += Dt;

	// The area follows the camera in whole texels.
	FVector Camera = BuiltOrigin + FVector(Map.width() * 50.0, Map.height() * 50.0, 0.0);
	if (APlayerController* PC = World->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
	{
		Camera = PC->PlayerCameraManager->GetCameraLocation();
	}
	const FVector2D CamM((Camera.X - BuiltOrigin.X) / 100.0, (Camera.Y - BuiltOrigin.Y) / 100.0);
	const FIntPoint Corner(FMath::FloorToInt32(CamM.X / WaterWaveTexelM) - WaterWaveRes / 2, FMath::FloorToInt32(CamM.Y / WaterWaveTexelM) - WaterWaveRes / 2);
	FIntPoint Shift = Corner - WaveCorner;
	if (WaveCorner.X == MAX_int32 || FMath::Abs(Shift.X) >= WaterWaveRes || FMath::Abs(Shift.Y) >= WaterWaveRes)
	{
		for (int32 I = 0; I < 3; ++I)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(this, WaveTargets[I], FLinearColor::Black);
		}
		Shift = FIntPoint::ZeroValue;
	}
	WaveCorner = Corner;
	const FVector2D CornerM(Corner.X * WaterWaveTexelM, Corner.Y * WaterWaveTexelM);
	const float SizeM = WaterWaveRes * WaterWaveTexelM;

	// The bodies in water: each pushes a soft dip, by its size, harder the faster it moves; one standing still only
	// bobs (faint rings); coming into the water gives one strong push (a ring).
	struct FPush
	{
		FLinearColor Value;
		double DistSq;
	};
	TArray<FPush, TInlineAllocator<64>> Pushes;
	const USimPawnSubsystem* PawnDraw = World->GetSubsystem<USimPawnSubsystem>();
	const USimAnimalSubsystem* AnimalDraw = World->GetSubsystem<USimAnimalSubsystem>();
	const float PushScale = GWaterRipplePush.GetValueOnGameThread();
	for (const colony::ThingView& View : Source->GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.level != 0)
		{
			continue;
		}
		const uint32 Key = static_cast<uint32>(entt::to_integral(View.id));
		FWaveBody* Body = WaveBodies.Find(Key);
		if (View.waterDepth < WaterWaveMinDepthM)
		{
			if (Body)
			{
				Body->bIn = false;
			}
			continue;
		}
		// Where it is drawn (smooth between sim steps), else where the sim has it.
		FVector2D Pos(View.x, View.y);
		const AActor* Actor = PawnDraw ? static_cast<const AActor*>(PawnDraw->FindPawn(View.id)) : nullptr;
		if (!Actor && AnimalDraw)
		{
			Actor = AnimalDraw->FindAnimal(View.id);
		}
		if (Actor)
		{
			const FVector L = Actor->GetActorLocation();
			Pos = FVector2D((L.X - BuiltOrigin.X) / 100.0, (L.Y - BuiltOrigin.Y) / 100.0);
		}
		if (!Body)
		{
			Body = &WaveBodies.Add(Key);
			Body->Last = Pos;
		}
		const float Speed = static_cast<float>(FVector2D::Distance(Pos, Body->Last)) / Dt;
		Body->Last = Pos;
		Body->SeenFrame = WaveFrame;
		if (!Body->bIn)
		{
			Body->bIn = true;
			Body->Entry = 0.25f;
			++WaveEntries;
		}
		Body->Entry = FMath::Max(0.0f, Body->Entry - Dt);
		const float HeightM = Sim.bodyHeightM(View.id);
		// A body's width in the water: a person's shoulders or hips, an animal by its height.
		const float Radius = FMath::Clamp(0.17f * HeightM, 0.12f, 0.6f);
		const float Size = FMath::Clamp(HeightM / 1.8f, 0.3f, 2.0f);
		const float Moving = FMath::Min(Speed, 2.5f) * 0.012f;
		const float Bob = 0.0025f * FMath::Sin(static_cast<float>(WaveTime) * 3.3f + (Key % 17));
		const float Enter = Body->Entry > 0.0f ? 0.06f : 0.0f;
		const float Push = -(Moving + Bob + Enter) * Size * PushScale * (Dt * 60.0f);
		const double DistSq = FVector2D::DistSquared(Pos, CamM);
		if (Pos.X < CornerM.X || Pos.Y < CornerM.Y || Pos.X > CornerM.X + SizeM || Pos.Y > CornerM.Y + SizeM)
		{
			continue;
		}
		Pushes.Add({FLinearColor(Pos.X, Pos.Y, Radius, Push), DistSq});
	}
	Pushes.Sort([](const FPush& A, const FPush& B) { return A.DistSq < B.DistSq; });
	for (auto It = WaveBodies.CreateIterator(); It; ++It)
	{
		if (It.Value().SeenFrame + 300 < WaveFrame)
		{
			It.RemoveCurrent();
		}
	}

	UMaterialInstanceDynamic* Step = WaveStepMIDs[WaveTurn];
	const float Courant = WaterWaveSpeed * Dt / WaterWaveTexelM;
	Step->SetScalarParameterValue(TEXT("K"), FMath::Min(Courant * Courant, 0.45f));
	Step->SetScalarParameterValue(TEXT("Damp"), FMath::Exp(-Dt / WaterWaveFade));
	Step->SetVectorParameterValue(TEXT("Shift"), FLinearColor(static_cast<float>(Shift.X) / WaterWaveRes, static_cast<float>(Shift.Y) / WaterWaveRes, 0.0f, 0.0f));
	Step->SetVectorParameterValue(TEXT("Origin"), FLinearColor(CornerM.X, CornerM.Y, SizeM, 0.0f));
	Step->SetVectorParameterValue(TEXT("MaskSize"), FLinearColor(Map.width(), Map.height(), 0.0f, 0.0f));
	UTexture* MaskTex = WaveMask;
	if (GWaterRippleMask.GetValueOnGameThread() == 0)
	{
		MaskTex = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	Step->SetTextureParameterValue(TEXT("WaveMask"), MaskTex);
	WavePushes = FMath::Min(Pushes.Num(), WaterWavePushes);
	for (int32 I = 0; I < WaterWavePushes; ++I)
	{
		Step->SetVectorParameterValue(*FString::Printf(TEXT("Imp%d"), I), I < WavePushes ? Pushes[I].Value : FLinearColor::Transparent);
	}
	UTextureRenderTarget2D* Next = WaveTargets[(WaveTurn + 2) % 3];
	UKismetRenderingLibrary::DrawMaterialToRenderTarget(this, Next, Step);
	WaveTurn = (WaveTurn + 1) % 3;
	++WaveSteps;

	WaterMID->SetTextureParameterValue(TEXT("WaveTex"), Next);
	WaterMID->SetVectorParameterValue(TEXT("WaveOrigin"), FLinearColor(CornerM.X, CornerM.Y, SizeM, 0.0f));
	WaterMID->SetScalarParameterValue(TEXT("WaveRes"), WaterWaveRes);
	const float* Strength = ScalarOverrides.Find(TEXT("WaveStrength"));
	WaterMID->SetScalarParameterValue(TEXT("WaveStrength"), Strength ? *Strength : 3.0f);
}
