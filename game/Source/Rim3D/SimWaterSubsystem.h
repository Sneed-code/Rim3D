#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SimWaterSubsystem.generated.h"

class AActor;
class UMaterialInstanceDynamic;
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
	/** Sets a parameter of the water material on every chunk (kept across rebuilds). Console: `Water.Param Name V` or `Water.Param Name R G B`. */
	void SetWaterParam(FName Name, float Value);
	void SetWaterVector(FName Name, const FLinearColor& Value);

	/** W2: the wave simulation's newest height field (null while off), how many steps it ran, bodies that pushed it last
	 *  frame and bodies that came into the water. */
	class UTextureRenderTarget2D* GetWaveTarget() const;
	int64 GetWaveSteps() const { return WaveSteps; }
	int32 GetWavePushes() const { return WavePushes; }
	int64 GetWaveEntries() const { return WaveEntries; }

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

	/** M_Water with the parameters set here (the chunks draw with it). */
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> WaterMID;

	/** W2 wave simulation (docs/design/water.md): three height fields in turn (before, now, next), a step material per
	 *  turn, and the map's open water as a mask. */
	UPROPERTY()
	TObjectPtr<class UTextureRenderTarget2D> WaveTargets[3];
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> WaveStepMIDs[3];
	UPROPERTY()
	TObjectPtr<class UTexture2D> WaveMask;

	struct FWaveBody
	{
		FVector2D Last = FVector2D::ZeroVector;
		float Entry = 0.0f;
		bool bIn = false;
		uint64 SeenFrame = 0;
	};
	TMap<uint32, FWaveBody> WaveBodies;
	void TickWaves(float DeltaTime);
	void BuildWaveMask(const colony::Map& Map);
	void StopWaves();
	int32 WaveTurn = 0;
	FIntPoint WaveCorner = FIntPoint(MAX_int32, MAX_int32);
	bool bWaveMaskDirty = true;
	uint64 WaveFrame = 0;
	double WaveTime = 0.0;
	int64 WaveSteps = 0;
	int32 WavePushes = 0;
	int64 WaveEntries = 0;

	TMap<FName, float> ScalarOverrides;
	TMap<FName, FLinearColor> VectorOverrides;

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
