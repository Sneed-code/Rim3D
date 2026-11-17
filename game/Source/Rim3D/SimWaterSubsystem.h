#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SimWaterSubsystem.generated.h"

class AActor;
class UMaterialInterface;
class UProceduralMeshComponent;
class USimSubsystem;
namespace colony { class Map; class Sim; }

/** What the water subsystem has built (smoke test `water`, docs/design/water.md). */
struct FWaterStats
{
	int32 Chunks = 0;
	int64 Triangles = 0;
	int64 Vertices = 0;
	int64 Rebuilds = 0;
	double FullBuildMs = 0.0;
	double LastRebuildMs = 0.0;
	double SlowestRebuildMs = 0.0;
};

/**
 * Water drawn as a real surface (docs/design/water.md, step W1). One mesh per 32 m chunk (the terrain's chunks) over
 * every ground cell of open water (Water and ShallowWater with a surface; marsh is left to the terrain material), plus
 * one cell of margin so the surface runs under the bank and the ground's own height cuts the shoreline. A vertex
 * stands at the sim's water surface (`Map::waterSurface`) and carries the cell's flow (`Map::flowAt`, metres per real
 * second) in UV 1 and the water's depth over the ground there in UV 2, so the material (`M_Water`, Single Layer Water,
 * made by the WaterArt commandlet) tints, ripples and foams without reading the scene's depth.
 *
 * A chunk is remade when its water changes (a moisture pump drying a cell): a few chunks a frame are hashed and
 * compared. The whole thing is remade when the sim is replaced.
 *
 * Without the material (the commandlet not run) or with `Water.Enable 0` nothing is drawn and the terrain paints
 * water cells in its old opaque water colour.
 */
UCLASS()
class RIM3D_API USimWaterSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Whether the water surface is to be drawn: `Water.Enable` is 1 and M_Water exists. The terrain reads it to paint
	 *  the bed under the surface instead of the old water colour. */
	static bool WantsSurface();

	/** Builds every chunk again from the sim now. */
	void RebuildAll();

	bool IsBuilt() const { return Holder != nullptr; }
	const FWaterStats& GetStats() const { return Stats; }
	/** The chunk mesh (tests), null if none. */
	UProceduralMeshComponent* GetChunkMesh(int32 ChunkX, int32 ChunkY) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		uint64 Hash = 0;
		int32 Triangles = 0;
		int32 Vertices = 0;
	};

	void Reset();
	uint64 HashChunk(const colony::Map& Map, int32 ChunkX, int32 ChunkY) const;
	void BuildChunk(const colony::Map& Map, int32 ChunkX, int32 ChunkY);
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
	int32 Cursor = 0;
	FVector BuiltOrigin = FVector::ZeroVector;
	FWaterStats Stats;

	TWeakObjectPtr<USimSubsystem> Bridge;
};
