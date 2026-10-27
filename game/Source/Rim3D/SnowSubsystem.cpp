#include "SnowSubsystem.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Weather.h"

DEFINE_LOG_CATEGORY_STATIC(LogSnow, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GSnowEnable(
		TEXT("Snow.Enable"), 1, TEXT("1 draws the snow lying on the ground, 0 draws none."), ECVF_Default);
	TAutoConsoleVariable<int32> GSnowChunksPerFrame(
		TEXT("Snow.ChunksPerFrame"), 24, TEXT("How many chunks a frame are checked against the sim's snow after it changed (and at most a quarter of that remade)."), ECVF_Default);

	const TCHAR* const SnowMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");

	// Nothing is drawn under this depth (percent).
	constexpr float SnowMinDepth = 8.0f;
	// The skin's thickness in cm: at least this much above the ground (so it never fights the ground for the pixel), and
	// this much more for every percent of depth, averaged over the four cells round a corner.
	constexpr float SnowBaseCm = 1.5f;
	constexpr float SnowCmPerPercent = 0.07f;
	const FLinearColor SnowColor(0.80f, 0.84f, 0.90f);

	int32 SnowChunksFor(int32 Cells)
	{
		return (Cells + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	}

	uint32 SnowHash(int32 X, int32 Y)
	{
		uint32 H = static_cast<uint32>(X) * 73856093u ^ static_cast<uint32>(Y) * 19349663u;
		H ^= H >> 13;
		H *= 0x5bd1e995u;
		H ^= H >> 15;
		return H;
	}
}

bool USnowSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USnowSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USnowSubsystem, STATGROUP_Tickables);
}

void USnowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void USnowSubsystem::Deinitialize()
{
	Reset();
	Super::Deinitialize();
}

void USnowSubsystem::Reset()
{
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
	Chunks.Reset();
	BuiltSim = nullptr;
	BuiltGeneration = -1;
	SeenChanges = ~0ULL;
	Cursor = 0;
	bChecking = false;
	ChunksWithSnow = 0;
	CellsDrawn = 0;
	Vertices = 0;
	Rebuilds = 0;
	SlowestRebuildMs = 0.0;
	DeepestDrawn = 0.0f;
}

void USnowSubsystem::StartChecking(const colony::Sim& Sim)
{
	const colony::Map& Map = Sim.map();
	ChunksX = SnowChunksFor(Map.width());
	ChunksY = SnowChunksFor(Map.height());
	if (Chunks.Num() != ChunksX * ChunksY)
	{
		Chunks.Reset();
		Chunks.SetNum(ChunksX * ChunksY);
	}
	Cursor = 0;
	bChecking = true;
}

bool USnowSubsystem::CheckChunk(const colony::Sim& Sim, int32 ChunkX, int32 ChunkY)
{
	const colony::Map& Map = Sim.map();
	const colony::Weather& Weather = Sim.weather();
	const int32 MapW = Map.width();
	const int32 MapH = Map.height();
	const int32 X0 = ChunkX * TerrainDraw::ChunkCells;
	const int32 Y0 = ChunkY * TerrainDraw::ChunkCells;
	const int32 Nx = FMath::Min(TerrainDraw::ChunkCells, MapW - X0);
	const int32 Ny = FMath::Min(TerrainDraw::ChunkCells, MapH - Y0);
	FChunk& Chunk = Chunks[ChunkIndex(ChunkX, ChunkY)];

	// The depths of the chunk's cells and the ring of cells round it (a corner is lifted by the four cells that meet in it).
	const int32 Side = Nx + 2;
	TArray<float> Depth;
	Depth.SetNumUninitialized(Side * (Ny + 2));
	uint64 Hash = 1469598103934665603ULL;
	bool bAny = false;
	for (int32 DY = 0; DY < Ny + 2; ++DY)
	{
		for (int32 DX = 0; DX < Side; ++DX)
		{
			const int32 CX = X0 + DX - 1;
			const int32 CY = Y0 + DY - 1;
			float D = 0.0f;
			if (CX >= 0 && CY >= 0 && CX < MapW && CY < MapH)
			{
				// A dusting is not drawn but still counts toward a corner's lift next to deeper snow.
				D = Weather.snowDepth(colony::CellPos{CX, CY, 0});
			}
			Depth[DY * Side + DX] = D;
			bAny = bAny || D >= SnowMinDepth;
			Hash = (Hash ^ static_cast<uint64>(FMath::RoundToInt(D * 4.0f))) * 1099511628211ULL;
		}
	}
	if (!bAny)
	{
		Hash = 0;
	}
	if (Hash == Chunk.Hash)
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	Chunk.Hash = Hash;

	if (!bAny)
	{
		if (Chunk.Mesh)
		{
			Chunk.Mesh->ClearMeshSection(0);
		}
		if (Chunk.Cells > 0)
		{
			--ChunksWithSnow;
		}
		CellsDrawn -= Chunk.Cells;
		Vertices -= Chunk.Vertices;
		Chunk.Cells = 0;
		Chunk.Vertices = 0;
		++Rebuilds;
		return true;
	}

	const auto Height = [&Map, MapW, MapH](int32 X, int32 Y)
	{
		return static_cast<double>(Map.groundHeight(FMath::Clamp(X, 0, MapW - 1), FMath::Clamp(Y, 0, MapH - 1))) * 100.0;
	};
	// The depth in the (cell) grid with the ring: cell (CX, CY) of the map is at (CX - X0 + 1, CY - Y0 + 1).
	const auto DepthAt = [&](int32 CX, int32 CY)
	{
		return Depth[(CY - Y0 + 1) * Side + (CX - X0 + 1)];
	};
	// Where the skin is at a cell corner: the ground there and a lift by the depth round it.
	const auto Corner = [&](int32 VX, int32 VY)
	{
		const float Mean = (DepthAt(VX - 1, VY - 1) + DepthAt(VX, VY - 1) + DepthAt(VX - 1, VY) + DepthAt(VX, VY)) * 0.25f;
		return FVector(VX * 100.0, VY * 100.0, Height(VX, VY) + SnowBaseCm + SnowCmPerPercent * Mean);
	};
	const auto Shade = [](int32 VX, int32 VY)
	{
		const float Variation = static_cast<float>(SnowHash(VX, VY) & 255u) / 255.0f;
		return (SnowColor * (0.93f + 0.07f * Variation)).ToFColor(false);
	};

	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colors;
	TArray<int32> Triangles;
	int32 Cells = 0;
	float Deepest = 0.0f;
	for (int32 CY = Y0; CY < Y0 + Ny; ++CY)
	{
		for (int32 CX = X0; CX < X0 + Nx; ++CX)
		{
			const float D = DepthAt(CX, CY);
			if (D < SnowMinDepth || Map.terrain(CX, CY) == colony::Terrain::Rock || Map.terrain(CX, CY) == colony::Terrain::Water)
			{
				continue;
			}
			++Cells;
			Deepest = FMath::Max(Deepest, D);
			const FVector A = Corner(CX, CY);
			const FVector B = Corner(CX + 1, CY);
			const FVector C = Corner(CX + 1, CY + 1);
			const FVector E = Corner(CX, CY + 1);
			const FColor Ca = Shade(CX, CY);
			const FColor Cb = Shade(CX + 1, CY);
			const FColor Cc = Shade(CX + 1, CY + 1);
			const FColor Ce = Shade(CX, CY + 1);
			// The ground's diagonal runs from (x + 1, y) to (x, y + 1); the skin's follows it.
			const auto AddTriangle = [&](const FVector& P, const FVector& Q, const FVector& R, const FColor& CP, const FColor& CQ, const FColor& CR)
			{
				FVector Normal = FVector::CrossProduct(R - P, Q - P).GetSafeNormal();
				if (Normal.Z < 0.0)
				{
					Normal = -Normal;
				}
				const int32 First = Positions.Num();
				Positions.Add(P);
				Positions.Add(Q);
				Positions.Add(R);
				Normals.Add(Normal);
				Normals.Add(Normal);
				Normals.Add(Normal);
				UVs.Add(FVector2D(P.X * 0.01, P.Y * 0.01));
				UVs.Add(FVector2D(Q.X * 0.01, Q.Y * 0.01));
				UVs.Add(FVector2D(R.X * 0.01, R.Y * 0.01));
				Colors.Add(CP);
				Colors.Add(CQ);
				Colors.Add(CR);
				Triangles.Append({First, First + 1, First + 2});
			};
			AddTriangle(A, E, B, Ca, Ce, Cb);
			AddTriangle(B, E, C, Cb, Ce, Cc);
		}
	}

	if (!Chunk.Mesh)
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
	if (Positions.IsEmpty())
	{
		Chunk.Mesh->ClearMeshSection(0);
	}
	else
	{
		Chunk.Mesh->CreateMeshSection(0, Positions, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), false);
		if (Material)
		{
			Chunk.Mesh->SetMaterial(0, Material);
		}
	}
	if ((Chunk.Cells > 0) != (Cells > 0))
	{
		ChunksWithSnow += Cells > 0 ? 1 : -1;
	}
	CellsDrawn += Cells - Chunk.Cells;
	Vertices += Positions.Num() - Chunk.Vertices;
	Chunk.Cells = Cells;
	Chunk.Vertices = Positions.Num();
	DeepestDrawn = FMath::Max(DeepestDrawn, Deepest);
	++Rebuilds;
	SlowestRebuildMs = FMath::Max(SlowestRebuildMs, (FPlatformTime::Seconds() - Start) * 1000.0);
	return true;
}

void USnowSubsystem::SyncAll()
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || !Holder)
	{
		return;
	}
	StartChecking(*Source->GetSim());
	for (int32 Y = 0; Y < ChunksY; ++Y)
	{
		for (int32 X = 0; X < ChunksX; ++X)
		{
			CheckChunk(*Source->GetSim(), X, Y);
		}
	}
	bChecking = false;
}

void USnowSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim() || GSnowEnable.GetValueOnGameThread() == 0)
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
		Reset();
		if (!GetWorld())
		{
			return;
		}
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		BuiltOrigin = Source->GetWorldOrigin();
		Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), BuiltOrigin, FRotator::ZeroRotator, Params);
		USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("SnowRoot"));
		Holder->SetRootComponent(Root);
		Root->RegisterComponent();
		Root->SetWorldLocation(BuiltOrigin);
#if WITH_EDITOR
		Holder->SetActorLabel(TEXT("Snow"));
#endif
		Material = LoadObject<UMaterialInterface>(nullptr, SnowMaterialPath);
		if (!Material)
		{
			UE_LOG(LogSnow, Error, TEXT("Could not load %s; snow is drawn with the default material"), SnowMaterialPath);
		}
		BuiltSim = &Sim;
		BuiltGeneration = Source->GetSimGeneration();
		SeenChanges = ~0ULL;
	}
	if (Source->GetWorldOrigin() != BuiltOrigin && Holder)
	{
		BuiltOrigin = Source->GetWorldOrigin();
		Holder->SetActorLocation(BuiltOrigin);
	}

	const uint64 Changes = Source->GetCurrentSnapshot().weather.snowChanges;
	if (Changes != SeenChanges)
	{
		SeenChanges = Changes;
		StartChecking(Sim);
	}
	if (!bChecking)
	{
		return;
	}
	const int32 Total = ChunksX * ChunksY;
	const int32 Checks = FMath::Max(1, GSnowChunksPerFrame.GetValueOnGameThread());
	const int32 MaxRebuilds = FMath::Max(1, Checks / 4);
	int32 Done = 0;
	int32 Made = 0;
	while (Cursor < Total && Done < Checks && Made < MaxRebuilds)
	{
		if (CheckChunk(Sim, Cursor % ChunksX, Cursor / ChunksX))
		{
			++Made;
		}
		++Cursor;
		++Done;
	}
	if (Cursor >= Total)
	{
		bChecking = false;
	}
}
