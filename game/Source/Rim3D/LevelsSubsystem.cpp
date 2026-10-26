#include "LevelsSubsystem.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "TerrainSubsystem.h"

#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

namespace
{
	TAutoConsoleVariable<int32> GLevelsEnable(
		TEXT("Levels.Enable"), 1, TEXT("1 draws the upper floors and the roofs of upper rooms, 0 draws none."), ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs GLevelsViewCommand(
		TEXT("Levels.View"), TEXT("Levels.View N: the level the overseer looks at (0 the ground to 3); the levels above it are cut away."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (ULevelsSubsystem* Levels = World ? World->GetSubsystem<ULevelsSubsystem>() : nullptr; Levels && Args.Num() > 0)
			{
				Levels->SetViewLevel(FCString::Atoi(*Args[0]));
			}
		}));

	const TCHAR* const LvlSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");

	// sRGB, as picked in a paint program: boards on top (two shades, alternating by cell), plaster under, the cut edge.
	const FColor LvlFloorColor(150, 112, 74);
	const FColor LvlFloorColor2(140, 104, 68);
	const FColor LvlCeilingColor(196, 186, 170);
	const FColor LvlEdgeColor(104, 80, 56);
	// The same as the terrain's thin roofs.
	const FColor LvlRoofColor(132, 104, 78);
	const FColor LvlRoofUnderColor(92, 78, 64);

	// How thick a floor is drawn, and a roof, cm. The floor's top is a hair over the sim's floor height, so it covers the tops
	// of the 3 m walls under it instead of flickering with them.
	constexpr double LvlFloorThicknessCm = 20.0;
	constexpr double LvlFloorLiftCm = 2.0;
	constexpr double LvlRoofThicknessCm = 4.0;

	uint64 LvlMix(uint64 Value)
	{
		Value += 0x9E3779B97F4A7C15ULL;
		Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBULL;
		return Value ^ (Value >> 31);
	}

	/** Flat-shaded triangles with their own vertices and colours, for one section of a procedural mesh. */
	struct FLvlMesh
	{
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FColor> Colors;
		TArray<int32> Triangles;

		void AddTriangle(const FVector& A, FVector B, FVector C, const FVector& Facing, const FColor& Color)
		{
			FVector Normal = FVector::CrossProduct(C - A, B - A);
			if (Normal.IsNearlyZero(1.0e-6))
			{
				return;
			}
			if (FVector::DotProduct(Normal, Facing) < 0.0)
			{
				Swap(B, C);
				Normal = -Normal;
			}
			Normal.Normalize();
			// The material reads vertex colours as linear.
			const FColor Linear = FLinearColor::FromSRGBColor(Color).ToFColor(false);
			const int32 First = Positions.Num();
			for (const FVector& P : {A, B, C})
			{
				Positions.Add(P);
				Normals.Add(Normal);
				UVs.Add(FVector2D(P.X * 0.01, P.Y * 0.01));
				Colors.Add(Linear);
			}
			Triangles.Append({First, First + 1, First + 2});
		}

		void AddQuad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector& Facing, const FColor& Color)
		{
			AddTriangle(A, B, C, Facing, Color);
			AddTriangle(A, C, D, Facing, Color);
		}
	};

	void LvlApplySection(UProceduralMeshComponent* Mesh, int32 Section, const FLvlMesh& Data, UMaterialInterface* Material)
	{
		if (Data.Positions.IsEmpty())
		{
			Mesh->ClearMeshSection(Section);
			return;
		}
		Mesh->CreateMeshSection(Section, Data.Positions, Data.Triangles, Data.Normals, Data.UVs, Data.Colors, TArray<FProcMeshTangent>(), false);
		if (Material)
		{
			Mesh->SetMaterial(Section, Material);
		}
	}
}

bool ULevelsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId ULevelsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(ULevelsSubsystem, STATGROUP_Tickables);
}

void ULevelsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void ULevelsSubsystem::Deinitialize()
{
	DrawActor = nullptr;
	Chunks.Reset();
	Super::Deinitialize();
}

void ULevelsSubsystem::SetViewLevel(int32 Level)
{
	ViewLevel = FMath::Clamp(Level, 0, colony::Map::kLevels - 1);
}

int32 ULevelsSubsystem::GetTopShownLevel() const
{
	const UOverseerSubsystem* Overseer = GetWorld() ? GetWorld()->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive() ? ViewLevel : colony::Map::kLevels - 1;
}

int32 ULevelsSubsystem::TopShownLevel(const UWorld* World)
{
	const ULevelsSubsystem* Levels = World ? World->GetSubsystem<ULevelsSubsystem>() : nullptr;
	return Levels ? Levels->GetTopShownLevel() : colony::Map::kLevels - 1;
}

FString ULevelsSubsystem::DescribeLevel(int32 Level)
{
	return Level <= 0 ? FString(TEXT("Ground")) : FString::Printf(TEXT("Level %d"), Level);
}

int32 ULevelsSubsystem::GetFloorCells(int32 Level) const
{
	int32 Count = 0;
	if (bBuilt && Level >= 0 && Level < colony::Map::kLevels)
	{
		for (int32 I = Level * ChunksX * ChunksY; I < (Level + 1) * ChunksX * ChunksY; ++I)
		{
			Count += Chunks[I].FloorCells;
		}
	}
	return Count;
}

int32 ULevelsSubsystem::GetRoofCells(int32 Level) const
{
	int32 Count = 0;
	if (bBuilt && Level >= 0 && Level < colony::Map::kLevels)
	{
		for (int32 I = Level * ChunksX * ChunksY; I < (Level + 1) * ChunksX * ChunksY; ++I)
		{
			Count += Chunks[I].RoofCells;
		}
	}
	return Count;
}

UProceduralMeshComponent* ULevelsSubsystem::GetChunkMesh(int32 Level, int32 ChunkX, int32 ChunkY) const
{
	if (!bBuilt || Level < 0 || Level >= colony::Map::kLevels || ChunkX < 0 || ChunkY < 0 || ChunkX >= ChunksX || ChunkY >= ChunksY)
	{
		return nullptr;
	}
	return Chunks[ChunkIndex(Level, ChunkX, ChunkY)].Mesh;
}

void ULevelsSubsystem::Teardown()
{
	if (DrawActor)
	{
		DrawActor->Destroy();
	}
	DrawActor = nullptr;
	Chunks.Reset();
	bBuilt = false;
	BuiltSim = nullptr;
	BuiltGeneration = -1;
	LastFloorChanges = ~0ULL;
	LastRoofChanges = ~0ULL;
	LastCollapses = ~0ULL;
	LastLinks = ~0ULL;
	ShownTop = -1;
	const int32 FullBuilds = Stats.FullBuilds;
	const int32 CutChanges = Stats.CutChanges;
	Stats = FLevelsStats();
	Stats.FullBuilds = FullBuilds;
	Stats.CutChanges = CutChanges;
}

void ULevelsSubsystem::RebuildAll()
{
	USimSubsystem* Sim = Bridge.Get();
	Teardown();
	if (!Sim || !Sim->GetSim() || !GetWorld())
	{
		return;
	}
	const colony::Sim& Core = *Sim->GetSim();
	BuiltSim = &Core;
	BuiltGeneration = Sim->GetSimGeneration();
	BuiltOrigin = Sim->GetWorldOrigin();
	ChunksX = (Core.map().width() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	ChunksY = (Core.map().height() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	DrawActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), BuiltOrigin, FRotator::ZeroRotator, Params);
	USceneComponent* Root = NewObject<USceneComponent>(DrawActor, TEXT("LevelsRoot"));
	DrawActor->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(BuiltOrigin);
#if WITH_EDITOR
	DrawActor->SetActorLabel(TEXT("UpperLevels"));
#endif
	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, LvlSolidMaterialPath);
	Chunks.SetNum(colony::Map::kLevels * ChunksX * ChunksY);
	bBuilt = true;
	++Stats.FullBuilds;
	Refresh();
}

void ULevelsSubsystem::Refresh()
{
	USimSubsystem* Sim = Bridge.Get();
	if (!bBuilt || !Sim || !Sim->GetSim() || Sim->GetSim() != BuiltSim)
	{
		return;
	}
	const colony::Map& Map = BuiltSim->map();
	const colony::Snapshot& Snapshot = Sim->GetCurrentSnapshot();
	uint64 Links = Map.levelLinks().size();
	for (const colony::LevelLink& Link : Map.levelLinks())
	{
		Links = LvlMix(Links ^ static_cast<uint64>(static_cast<uint32>(Link.building)));
	}
	if (Map.upperFloorChanges() == LastFloorChanges && Snapshot.roofChanges == LastRoofChanges && Snapshot.roofCollapses == LastCollapses && Links == LastLinks)
	{
		return;
	}
	LastFloorChanges = Map.upperFloorChanges();
	LastRoofChanges = Snapshot.roofChanges;
	LastCollapses = Snapshot.roofCollapses;
	LastLinks = Links;

	// Each chunk's signature: the floors and the roofs in it.
	const int32 Cells = Map.width() * Map.height();
	TArray<uint64> Signatures;
	Signatures.Init(0, Chunks.Num());
	const auto Sign = [&](const colony::CellPos& Cell, uint64 Salt)
	{
		const int32 CX = Cell.x / TerrainDraw::ChunkCells;
		const int32 CY = Cell.y / TerrainDraw::ChunkCells;
		Signatures[ChunkIndex(Cell.level, CX, CY)] += LvlMix((static_cast<uint64>(Map.index(Cell)) << 2) + Salt);
	};
	for (const colony::CellPos& Cell : Map.upperFloorCells())
	{
		if (Map.inBounds(Cell))
		{
			Sign(Cell, 1);
		}
	}
	TArray<uint8> Flags;
	Flags.Init(0, Cells * colony::Map::kLevels);
	for (const colony::CellPos& Cell : Snapshot.roofCells)
	{
		// The ground's roofs are the terrain's.
		if (Cell.level > 0 && Map.inBounds(Cell))
		{
			Flags[Cell.level * Cells + Map.index(Cell)] |= 1;
			Sign(Cell, 2);
		}
	}
	// The stairwell: the cells over each staircase and ladder, one level up.
	for (const colony::LevelLink& Link : Map.levelLinks())
	{
		const colony::ThingView* Stairs = Snapshot.find(Link.building);
		if (!Stairs || Stairs->level + 1 >= colony::Map::kLevels)
		{
			continue;
		}
		const int32 X0 = FMath::RoundToInt32(Stairs->x - Stairs->sizeX * 0.5f);
		const int32 Y0 = FMath::RoundToInt32(Stairs->y - Stairs->sizeY * 0.5f);
		for (int32 Y = Y0; Y < Y0 + Stairs->sizeY; ++Y)
		{
			for (int32 X = X0; X < X0 + Stairs->sizeX; ++X)
			{
				const colony::CellPos Cell{X, Y, Stairs->level + 1};
				if (Map.inBounds(Cell) && (Flags[Cell.level * Cells + Map.index(Cell)] & 2) == 0)
				{
					Flags[Cell.level * Cells + Map.index(Cell)] |= 2;
					Sign(Cell, 3);
				}
			}
		}
	}
	for (int32 Level = 1; Level < colony::Map::kLevels; ++Level)
	{
		for (int32 CY = 0; CY < ChunksY; ++CY)
		{
			for (int32 CX = 0; CX < ChunksX; ++CX)
			{
				FChunk& Chunk = Chunks[ChunkIndex(Level, CX, CY)];
				const uint64 Signature = Signatures[ChunkIndex(Level, CX, CY)];
				if (Signature != Chunk.Signature)
				{
					RebuildChunk(Level, CX, CY, Flags);
					Chunk.Signature = Signature;
				}
			}
		}
	}
	ApplyVisibility(true);
}

void ULevelsSubsystem::RebuildChunk(int32 Level, int32 ChunkX, int32 ChunkY, const TArray<uint8>& Flags)
{
	const colony::Map& Map = BuiltSim->map();
	FChunk& Chunk = Chunks[ChunkIndex(Level, ChunkX, ChunkY)];
	const int32 X0 = ChunkX * TerrainDraw::ChunkCells;
	const int32 Y0 = ChunkY * TerrainDraw::ChunkCells;
	const int32 X1 = FMath::Min(X0 + TerrainDraw::ChunkCells, Map.width());
	const int32 Y1 = FMath::Min(Y0 + TerrainDraw::ChunkCells, Map.height());
	const int32 Cells = Map.width() * Map.height();
	// A floor's corner follows the ground under it (the sim's heightAt), 3 m a level up.
	const auto Corner = [&Map, Level](int32 X, int32 Y, double LiftCm)
	{
		const float Ground = Map.groundHeight(FMath::Clamp(X, 0, Map.width() - 1), FMath::Clamp(Y, 0, Map.height() - 1));
		return FVector(X * 100.0, Y * 100.0, (Ground + Level * colony::Map::kLevelHeightM) * 100.0 + LiftCm);
	};
	// A floor drawn: laid, and not over stairs (the stairwell).
	const auto Floor = [&Map, &Flags, Level, Cells](int32 X, int32 Y)
	{
		const colony::CellPos Cell{X, Y, Level};
		return Map.inBounds(Cell) && Map.upperFloor(Cell) && (Flags[Level * Cells + Map.index(Cell)] & 2) == 0;
	};

	FLvlMesh Floors;
	FLvlMesh Roofs;
	int32 FloorCells = 0;
	int32 RoofCells = 0;
	const FVector Up(0.0, 0.0, 1.0);
	const FVector Down(0.0, 0.0, -1.0);
	for (int32 Y = Y0; Y < Y1; ++Y)
	{
		for (int32 X = X0; X < X1; ++X)
		{
			if (Floor(X, Y))
			{
				++FloorCells;
				const double T = LvlFloorLiftCm;
				const double B = LvlFloorLiftCm - LvlFloorThicknessCm;
				Floors.AddQuad(Corner(X, Y, T), Corner(X, Y + 1, T), Corner(X + 1, Y + 1, T), Corner(X + 1, Y, T), Up, ((X + Y) & 1) ? LvlFloorColor2 : LvlFloorColor);
				Floors.AddQuad(Corner(X, Y, B), Corner(X, Y + 1, B), Corner(X + 1, Y + 1, B), Corner(X + 1, Y, B), Down, LvlCeilingColor);
				// The cut edge where the floor ends.
				if (!Floor(X - 1, Y))
				{
					Floors.AddQuad(Corner(X, Y, B), Corner(X, Y + 1, B), Corner(X, Y + 1, T), Corner(X, Y, T), FVector(-1.0, 0.0, 0.0), LvlEdgeColor);
				}
				if (!Floor(X + 1, Y))
				{
					Floors.AddQuad(Corner(X + 1, Y, B), Corner(X + 1, Y + 1, B), Corner(X + 1, Y + 1, T), Corner(X + 1, Y, T), FVector(1.0, 0.0, 0.0), LvlEdgeColor);
				}
				if (!Floor(X, Y - 1))
				{
					Floors.AddQuad(Corner(X, Y, B), Corner(X + 1, Y, B), Corner(X + 1, Y, T), Corner(X, Y, T), FVector(0.0, -1.0, 0.0), LvlEdgeColor);
				}
				if (!Floor(X, Y + 1))
				{
					Floors.AddQuad(Corner(X, Y + 1, B), Corner(X + 1, Y + 1, B), Corner(X + 1, Y + 1, T), Corner(X, Y + 1, T), FVector(0.0, 1.0, 0.0), LvlEdgeColor);
				}
			}
			if ((Flags[Level * Cells + Map.index(colony::CellPos{X, Y, Level})] & 1) != 0)
			{
				++RoofCells;
				const double T = TerrainDraw::RoofHeightM * 100.0;
				const double B = T - LvlRoofThicknessCm;
				Roofs.AddQuad(Corner(X, Y, T), Corner(X, Y + 1, T), Corner(X + 1, Y + 1, T), Corner(X + 1, Y, T), Up, LvlRoofColor);
				Roofs.AddQuad(Corner(X, Y, B), Corner(X, Y + 1, B), Corner(X + 1, Y + 1, B), Corner(X + 1, Y, B), Down, LvlRoofUnderColor);
			}
		}
	}

	Stats.FloorCells += FloorCells - Chunk.FloorCells;
	Stats.RoofCells += RoofCells - Chunk.RoofCells;
	Chunk.FloorCells = FloorCells;
	Chunk.RoofCells = RoofCells;
	++Stats.ChunkRebuilds;
	if (!Chunk.Mesh)
	{
		if (FloorCells == 0 && RoofCells == 0)
		{
			return;
		}
		Chunk.Mesh = NewObject<UProceduralMeshComponent>(DrawActor);
		Chunk.Mesh->bUseAsyncCooking = false;
		Chunk.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Chunk.Mesh->SetCanEverAffectNavigation(false);
		Chunk.Mesh->bAffectDistanceFieldLighting = false;
		Chunk.Mesh->SetupAttachment(DrawActor->GetRootComponent());
		Chunk.Mesh->RegisterComponent();
	}
	LvlApplySection(Chunk.Mesh, 0, Floors, SolidMaterial);
	LvlApplySection(Chunk.Mesh, 1, Roofs, SolidMaterial);
}

bool ULevelsSubsystem::WantRoofs() const
{
	if (const UTerrainSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainSubsystem>() : nullptr; Terrain && Terrain->IsBuilt())
	{
		return Terrain->AreRoofsShown();
	}
	const UOverseerSubsystem* Overseer = GetWorld() ? GetWorld()->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return !(Overseer && Overseer->IsOverseerActive());
}

void ULevelsSubsystem::ApplyVisibility(bool bForce)
{
	const int32 Top = GetTopShownLevel();
	const bool bRoofs = WantRoofs();
	if (!bForce && Top == ShownTop && bRoofs == bRoofsShown)
	{
		return;
	}
	if (Top != ShownTop)
	{
		++Stats.CutChanges;
	}
	ShownTop = Top;
	bRoofsShown = bRoofs;
	for (int32 Level = 1; Level < colony::Map::kLevels; ++Level)
	{
		for (int32 I = Level * ChunksX * ChunksY; I < (Level + 1) * ChunksX * ChunksY; ++I)
		{
			const FChunk& Chunk = Chunks[I];
			if (!Chunk.Mesh)
			{
				continue;
			}
			if (Chunk.FloorCells > 0)
			{
				Chunk.Mesh->SetMeshSectionVisible(0, Level <= Top);
			}
			if (Chunk.RoofCells > 0)
			{
				Chunk.Mesh->SetMeshSectionVisible(1, bRoofs && Level <= Top);
			}
		}
	}
}

void ULevelsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || !Sim->GetSim() || GLevelsEnable.GetValueOnGameThread() == 0)
	{
		if (bBuilt || DrawActor)
		{
			Teardown();
		}
		return;
	}
	if (!bBuilt || BuiltGeneration != Sim->GetSimGeneration() || BuiltSim != Sim->GetSim())
	{
		RebuildAll();
	}
	else
	{
		Refresh();
	}
	if (Sim->GetWorldOrigin() != BuiltOrigin && DrawActor)
	{
		BuiltOrigin = Sim->GetWorldOrigin();
		DrawActor->SetActorLocation(BuiltOrigin);
	}
	ApplyVisibility(false);
}
