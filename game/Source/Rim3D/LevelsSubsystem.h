#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "LevelsSubsystem.generated.h"

class AActor;
class UMaterialInterface;
class UProceduralMeshComponent;
class USimSubsystem;
namespace colony { class Sim; }

/** What the levels subsystem draws (smoke test `levels`, docs/design/levels-draw.md). */
struct FLevelsStats
{
	/** Upper floor cells and thin roof cells over upper-level rooms drawn, all levels. */
	int32 FloorCells = 0;
	int32 RoofCells = 0;
	/** Level chunks rebuilt since the sim last started, and full rebuilds. */
	int64 ChunkRebuilds = 0;
	int32 FullBuilds = 0;
	/** Times the shown levels changed (Page Up, Page Down, a switch of view). */
	int32 CutChanges = 0;
};

/**
 * The upper levels on screen (docs/design/levels-draw.md): which level the overseer looks at, and the upper floors
 * and the roofs of upper-level rooms. Buildings, items and pawns up there are drawn by their own subsystems, which
 * ask GetTopShownLevel what to cut away.
 *
 * - **The level looked at.** In the overseer view Page Up and Page Down choose a level (0, the ground, to 3); every
 *   level above it is cut away, so the rooms of the chosen one are seen from above. Clicks and the architect's tools
 *   work on that level. Direct mode and every other view show all levels.
 * - **Upper floors** are slabs 20 cm thick whose top is the floor a pawn stands on (the ground's height plus 3 m a
 *   level, as the sim has it), with a ceiling on their underside and edges where the floor ends. One procedural mesh
 *   per level and 32 m chunk, rebuilt when the sim's floors in it change (Map::upperFloorChanges) or a roof changes.
 * - **The stairwell**: no floor is drawn over stairs or a ladder, though the sim may have one there.
 * - **Roofs** of enclosed rooms on upper levels are drawn like the ground's thin roofs, 3 m over the room's floor,
 *   and hidden with them in the overseer view (the terrain's Terrain.Roofs).
 *
 * Console: Levels.Enable (0 draws no floors), Levels.View N (the overseer's level).
 */
UCLASS()
class RIM3D_API ULevelsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The level the overseer looks at, 0 (the ground) to the top level. */
	int32 GetViewLevel() const { return ViewLevel; }
	void SetViewLevel(int32 Level);
	/** Page Up (+1) and Page Down (-1). */
	void StepViewLevel(int32 Delta) { SetViewLevel(ViewLevel + Delta); }
	/** The highest level drawn right now: the one looked at while the overseer view is in use, every level otherwise. */
	int32 GetTopShownLevel() const;
	/** The same for any world: every level when there is no levels subsystem. */
	static int32 TopShownLevel(const UWorld* World);
	/** "Ground", "Level 1"... for the overseer's panel. */
	static FString DescribeLevel(int32 Level);

	/** Reads the sim's floors and roofs now and rebuilds what changed (the tick does this itself). */
	void Refresh();

	bool IsBuilt() const { return bBuilt; }
	const FLevelsStats& GetStats() const { return Stats; }
	/** Floor and roof cells drawn on a level. */
	int32 GetFloorCells(int32 Level) const;
	int32 GetRoofCells(int32 Level) const;
	/** The mesh of a level's chunk (section 0 floors, 1 roofs); null if nothing was ever drawn there. */
	UProceduralMeshComponent* GetChunkMesh(int32 Level, int32 ChunkX, int32 ChunkY) const;
	/** Whether a level's floors are visible right now. */
	bool IsLevelShown(int32 Level) const { return Level <= ShownTop; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FChunk
	{
		UProceduralMeshComponent* Mesh = nullptr;
		uint64 Signature = 0;
		int32 FloorCells = 0;
		int32 RoofCells = 0;
	};

	void Teardown();
	void RebuildAll();
	/** `Flags` holds, per level and cell: 1 a roof of an upstairs room, 2 the stairwell over stairs or a ladder. */
	void RebuildChunk(int32 Level, int32 ChunkX, int32 ChunkY, const TArray<uint8>& Flags);
	void ApplyVisibility(bool bForce);
	bool WantRoofs() const;
	int32 ChunkIndex(int32 Level, int32 ChunkX, int32 ChunkY) const { return (Level * ChunksY + ChunkY) * ChunksX + ChunkX; }

	UPROPERTY()
	TObjectPtr<AActor> DrawActor;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;

	TArray<FChunk> Chunks;
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	int32 ViewLevel = 0;
	int32 ShownTop = -1;
	bool bRoofsShown = false;
	uint64 LastFloorChanges = ~0ULL;
	uint64 LastRoofChanges = ~0ULL;
	uint64 LastCollapses = ~0ULL;
	uint64 LastLinks = ~0ULL;
	int32 BuiltGeneration = -1;
	const colony::Sim* BuiltSim = nullptr;
	FVector BuiltOrigin = FVector::ZeroVector;
	bool bBuilt = false;
	FLevelsStats Stats;

	TWeakObjectPtr<USimSubsystem> Bridge;
};
