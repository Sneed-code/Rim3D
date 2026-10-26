#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include "TerrainSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UProceduralMeshComponent;
class USimSubsystem;
class UStaticMesh;
namespace colony { class Map; class Sim; }

/** What the terrain subsystem has built and how much work it did (smoke test `world`, docs/design/terrain.md). */
struct FTerrainStats
{
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	int64 GroundVertices = 0;
	int64 RoofVertices = 0;
	int64 PlantInstances = 0;
	/** Plant instance components that hold at least one instance. */
	int32 PlantComponents = 0;
	/** Plant kinds that have a mesh. */
	int32 PlantKinds = 0;
	/** Chunk meshes and chunk plant sets rebuilt since the last full build. */
	int64 ChunkRebuilds = 0;
	int64 PlantRebuilds = 0;
	/** The last full build, and the slowest single chunk rebuild since, in milliseconds. */
	double FullBuildMs = 0.0;
	double SlowestChunkMs = 0.0;
	int32 FullBuilds = 0;
};

/**
 * Draws the sim's ground and plants (docs/design/terrain.md). One actor holds a mesh component per
 * 32 m chunk of the map: the ground (a vertex per cell corner, rock cells as solid blocks, mined
 * rock as rough stone) and, in a second section, the roofs (the mountain's thick roof over mined-out
 * rock, the thin roof over built rooms). Plants are instanced meshes, one component per chunk and
 * plant kind, read straight from `sim.plants()`.
 *
 * Mining a cell rebuilds that chunk (and a neighbour if the cell is on the edge); the sim's
 * CellMined, PlantRemoved and PlantHarvested events say when. Plants that change in any other way
 * (growing, dying, built over) are caught by a rotating scan of the chunks' plant stages, a few
 * chunks a frame. The whole thing is rebuilt when the sim is replaced (a new start, a load).
 *
 * Roofs: while the overseer view is the one in use they are hidden, so the colony can be managed;
 * in any other view they are shown. Console: Terrain.Roofs (-1 automatic, 0 hide, 1 show),
 * Terrain.Enable (0 draws nothing), Terrain.PlantScan (chunks scanned a frame).
 */
UCLASS()
class RIM3D_API UTerrainSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Builds everything again from the sim as it is now. */
	void RebuildAll();

	/** Does the work waiting (dirty chunks) now instead of at the next tick. */
	void FlushDirty();

	/** Says a cell's ground changed (the sim's CellMined event does this). */
	void MarkCellDirty(colony::CellPos Cell);
	/** Says the plant in a cell changed (PlantRemoved, PlantHarvested). */
	void MarkPlantDirty(colony::CellPos Cell);

	bool IsBuilt() const { return bBuilt; }
	bool IsTextured() const { return bTextured; }

	/** How wet the ground looks, 0 dry to 1 soaked (darker and shinier; only the terrain material has it, and only once the
	 *  TerrainMaterials commandlet has been run again since U13). The sky subsystem sets it from the rain. */
	void SetGroundWetness(float Wetness);
	float GetGroundWetness() const { return GroundWetness; }
	const FTerrainStats& GetStats() const { return Stats; }

	/** Chunk numbers of a cell. */
	static void ChunkOfCell(colony::CellPos Cell, int32& OutX, int32& OutY);
	bool HasChunk(int32 ChunkX, int32 ChunkY) const;
	/** Vertices in a chunk's ground and roof sections; 0 if there is no such chunk. */
	int32 GetChunkGroundVertices(int32 ChunkX, int32 ChunkY) const;
	int32 GetChunkRoofVertices(int32 ChunkX, int32 ChunkY) const;
	/** Plant instances in a chunk, all kinds. */
	int32 GetChunkPlantInstances(int32 ChunkX, int32 ChunkY) const;
	/** Whether the roof sections are visible right now. */
	bool AreRoofsShown() const { return bRoofsShown; }
	/** The chunk meshes (for tests: bounds, material, visibility). Null if there is no such chunk. */
	UProceduralMeshComponent* GetChunkMesh(int32 ChunkX, int32 ChunkY) const;
	AActor* GetTerrainActor() const { return TerrainActor; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FPlantVisual { int32 DefValue = -1; UStaticMesh* Mesh = nullptr; float CullDistanceCm = 0.0f; float Scale = 1.0f; float Jitter = 0.15f; bool bCastShadow = true; };
	struct FChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		/** Plant instances by plant kind (an index into PlantVisuals); null until the chunk has one. */
		TArray<UInstancedStaticMeshComponent*> Plants;
		int32 GroundVertices = 0;
		int32 RoofVertices = 0;
		int32 PlantCount = 0;
		uint64 PlantSignature = 0;
		bool bGroundDirty = false;
		bool bPlantsDirty = false;
	};

	void OnSimEvent(const colony::Event& Event);
	void Teardown();
	void LoadPlantVisuals(const colony::Sim& Sim);
	void ReadThinRoof(bool& bOutChanged);
	void CreateChunk(int32 ChunkX, int32 ChunkY);
	void RebuildChunkGround(int32 ChunkX, int32 ChunkY);
	void RebuildChunkPlants(int32 ChunkX, int32 ChunkY);
	void ApplyChunkPlants(FChunk& Chunk, TArray<TArray<FTransform>>& Transforms);
	/** The plants of a chunk by kind as instance transforms (null: only work out the signature); the signature changes when a plant appears, goes or steps to another size. */
	uint64 GatherChunkPlants(int32 ChunkX, int32 ChunkY, TArray<TArray<FTransform>>* Out) const;
	void ScanPlants();
	void UpdateRoofVisibility(bool bForce);
	bool WantRoofs() const;
	int32 ChunkIndex(int32 ChunkX, int32 ChunkY) const { return ChunkY * ChunksX + ChunkX; }

	UPROPERTY()
	TObjectPtr<AActor> TerrainActor;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> PlantMeshes;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> GroundMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> PlantMaterial;

	/** The terrain material as an instance of its own (null without the terrain material); GroundMaterial points at it. */
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GroundDynamic;

	float GroundWetness = 0.0f;

	TArray<FChunk> Chunks;
	TArray<FPlantVisual> PlantVisuals;
	/** By DefId value: an index into PlantVisuals, or -1 for a def that isn't drawn. */
	TArray<int32> VisualOfDef;
	/** One byte per map cell: 1 where a thin roof is drawn (the snapshot's roofCells). */
	TArray<uint8> ThinRoof;
	TArray<int32> ThinRoofCells;
	uint64 LastRoofChanges = 0;
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	int32 BuiltGeneration = -1;
	const colony::Sim* BuiltSim = nullptr;
	FVector BuiltOrigin = FVector::ZeroVector;
	int32 ScanCursor = 0;
	bool bBuilt = false;
	/** Whether the ground is drawn with the terrain material (made by the TerrainMaterials commandlet) and not flat colours. */
	bool bTextured = false;
	bool bRoofsShown = true;
	FTerrainStats Stats;

	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> Bridge;
};
