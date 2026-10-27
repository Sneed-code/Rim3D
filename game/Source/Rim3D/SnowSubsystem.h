#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SnowSubsystem.generated.h"

class AActor;
class UMaterialInterface;
class UProceduralMeshComponent;
class USimSubsystem;
namespace colony { class Sim; }

/**
 * Snow lying on the ground (docs/design/sky.md): the sim's snow layer (E1, `weather().snowDepth(cell)`, 0 to 100 percent per
 * cell) drawn as a white skin over the ground that thickens with the depth and thins out at its edges. One mesh per 32 m
 * chunk (the terrain's chunks), made only for chunks that hold snow and remade only when the depths in a chunk (and a
 * cell's width around it) have changed. The snapshot's `snowChanges` says when the sim's snow changed (once an hour at
 * most); then a few chunks a frame are checked, so a 1 km map does not hitch.
 *
 * Under 8 percent nothing is drawn (the sim's own limit for a dusting is 3 percent and it slows nobody until 25).
 * Snow is drawn on the ground only: not yet on plants, rock tops or buildings.
 *
 * Console: Snow.Enable (0 draws no snow), Snow.ChunksPerFrame.
 */
UCLASS()
class RIM3D_API USnowSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Checks every chunk against the sim's snow now (tests; the game spreads it over frames). */
	void SyncAll();

	/** Chunks that have a snow mesh now, the snow cells drawn in them, and the vertices. */
	int32 GetChunksWithSnow() const { return ChunksWithSnow; }
	int64 GetCellsDrawn() const { return CellsDrawn; }
	int64 GetVertices() const { return Vertices; }
	/** Chunk meshes made or remade since the sim started or loaded. */
	int64 GetRebuilds() const { return Rebuilds; }
	/** True when every chunk has been checked since the sim's snow last changed. */
	bool IsUpToDate() const { return !bChecking; }
	/** The slowest chunk rebuild, ms. */
	double GetSlowestRebuildMs() const { return SlowestRebuildMs; }
	/** Depth of the thickest snow drawn, in percent. */
	float GetDeepestDrawn() const { return DeepestDrawn; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		uint64 Hash = 0;
		int32 Cells = 0;
		int32 Vertices = 0;
	};

	void Reset();
	void StartChecking(const colony::Sim& Sim);
	/** Looks at one chunk and remakes its mesh if its snow changed. Returns true if it did. */
	bool CheckChunk(const colony::Sim& Sim, int32 ChunkX, int32 ChunkY);
	int32 ChunkIndex(int32 ChunkX, int32 ChunkY) const { return ChunkY * ChunksX + ChunkX; }

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material;

	TArray<FChunk> Chunks;
	const colony::Sim* BuiltSim = nullptr;
	int32 BuiltGeneration = -1;
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	uint64 SeenChanges = ~0ULL;
	int32 Cursor = 0;
	bool bChecking = false;
	FVector BuiltOrigin = FVector::ZeroVector;

	int32 ChunksWithSnow = 0;
	int64 CellsDrawn = 0;
	int64 Vertices = 0;
	int64 Rebuilds = 0;
	double SlowestRebuildMs = 0.0;
	float DeepestDrawn = 0.0f;

	TWeakObjectPtr<USimSubsystem> Bridge;
};
