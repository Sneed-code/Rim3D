#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Snapshot.h"

#include "VisualMesh.h"

#include "BuildingSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class USimSubsystem;
class UStaticMesh;
namespace colony { class Sim; }

/** What the building subsystem draws and what it cost (smoke test `buildings`, docs/design/building-draw.md). */
struct FBuildingStats
{
	/** Things drawn, by kind: finished buildings, frames, blueprints, items (a stack is one item). */
	int32 Built = 0;
	int32 Frames = 0;
	int32 Blueprints = 0;
	int32 Items = 0;
	int32 Doors = 0;
	/** Instances in all chunks (a stack of items is several, a frame two) and the instance components that hold them. */
	int64 Instances = 0;
	int32 Components = 0;
	/** Meshes made so far (one per building or item kind and stuff). */
	int32 Meshes = 0;
	/** Chunks rebuilt since the sim last started, and the slowest one, in milliseconds. */
	int64 ChunkRebuilds = 0;
	double SlowestChunkMs = 0.0;
	int32 FullBuilds = 0;
};

/**
 * Draws the sim's buildings and items (docs/design/building-draw.md). Everything comes from the snapshot's
 * things, never from Unreal objects of their own, so a load or a new start redraws it all:
 *
 * - finished buildings, from each def's `visual` parts, coloured by the stuff they are made of;
 * - blueprints as see-through blue ghosts, and frames as a solid part-built stage (taller with the
 *   materials delivered and the work done) inside the ghost of the whole;
 * - doors, which slide open and shut as the sim's door opens and shuts;
 * - items lying on the ground and in stockpiles, a stack drawn as a little pile of pieces that grows with the count.
 *
 * Like the terrain, the map is cut into 32 m chunks; each chunk has one instanced mesh component for each kind of thing
 * in it. When a new snapshot arrives the subsystem sums a signature for each chunk and rebuilds the chunks whose
 * signature changed, so a chunk is rebuilt only when something in it appears, goes, or steps to another stage or size.
 * Only the doors move every frame. Each level (docs/design/levels.md) has chunks of its own, so the levels above the one
 * the overseer looks at can be hidden whole (docs/design/levels-draw.md).
 *
 * Console: Buildings.Enable (0 draws nothing).
 */
UCLASS()
class RIM3D_API UBuildingSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Reads the newest snapshot now and rebuilds the chunks that changed (the tick does this itself when a step has run). */
	void Refresh();

	bool IsBuilt() const { return bBuilt; }
	const FBuildingStats& GetStats() const { return Stats; }

	/** Instances drawn in a chunk, all kinds and levels; 0 if there is no such chunk. */
	int32 GetChunkInstances(int32 ChunkX, int32 ChunkY) const;
	/** Instances drawn on a level, all chunks; only the ones visible right now if `bVisibleOnly` (the cut-away levels are hidden). */
	int32 GetLevelInstances(int32 Level, bool bVisibleOnly) const;
	/** How open the door `Door` is drawn right now, 0 to 1; -1 if it is not a drawn door. */
	float GetDoorShown(colony::Entity Door) const;
	/** The slide of the door's moving leaf from its closed place along its length, cm; 0 for no such door. */
	float GetDoorLeafOffsetCm(colony::Entity Door) const;
	/** How high a building or item of this def is drawn, in cm (0 if it has no picture), for picking. */
	float GetDrawnHeightCm(colony::DefId Def) const;
	/** Instance components that currently hold at least one instance and use the ghost material. */
	int32 GetGhostInstances() const;

	/** The mesh of a whole building of this def made of this stuff (invalid for none), made if it is new, for the placement ghost of
	 *  the architect menu (docs/design/orders.md); null if the def has no picture or the subsystem is not built yet. */
	UStaticMesh* GetWholeMesh(colony::DefId Def, colony::DefId Stuff);
	/** Where a building of this def is drawn when its footprint starts at `Cell` and faces `Rot`: where a built one stands (on the lowest
	 *  ground under its footprint). False if the def is not a building with a picture. */
	bool GetPlacedTransform(colony::DefId Def, colony::CellPos Cell, colony::Rot4 Rot, FTransform& OutWorld) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	/** What a def looks like, read once. */
	struct FDefLook
	{
		bool bRead = false;
		bool bItem = false;
		bool bDoor = false;
		/** Joins the walls, doors and rock beside it (visual.link, L4): drawn as a post with an arm to each, `LinkThicknessCm` thick. */
		bool bLink = false;
		float LinkThicknessCm = 50.0f;
		FVector2f FootprintCm = FVector2f(100.0f, 100.0f);
		TArray<VisualMesh::FPart> Parts;
		bool bHasSliding = false;
		float SlideCm = 0.0f;
		int32 MaxPieces = 1;
		float LayerCm = 10.0f;
		int32 StackLimit = 1;
		float HeightCm = 0.0f;
		/** The def's own mesh (visual.mesh, A1), already sized and stood on the ground; null draws the parts. A door's is its leaf. */
		UStaticMesh* ArtMesh = nullptr;
	};

	/** One def made of one stuff: its meshes. */
	struct FLook
	{
		int32 DefValue = -1;
		UStaticMesh* Fixed = nullptr;
		UStaticMesh* Sliding = nullptr;
		UStaticMesh* All = nullptr;
	};

	/** One instance component and the instances put in it by the last rebuild of its chunk. */
	struct FChunk
	{
		TMap<int32, UInstancedStaticMeshComponent*> Components;
		uint64 Signature = 0;
		int32 Instances = 0;
		TArray<colony::Entity> Doors;
	};

	/** A door's moving leaf, for the animation. */
	struct FDoorSlot
	{
		UInstancedStaticMeshComponent* Component = nullptr;
		int32 Index = 0;
		FTransform Base;
		FVector Direction = FVector::ForwardVector;
		float SlideCm = 0.0f;
		float Shown = 0.0f;
	};

	void Teardown();
	void RebuildAll();
	void ReadDefs(const colony::Sim& Sim);
	const FDefLook* LookOfDef(colony::DefId Def) const;
	/**
	 * The index in Looks of this def made of this stuff (invalid for none), made if it is new; -1 if the def has no picture.
	 * `Link` is a linked def's neighbours (1 north, 2 east, 4 south, 8 west); -1 draws its plain parts (the placement ghost).
	 */
	int32 GetLook(colony::DefId Def, colony::DefId Stuff, int32 Link = -1);
	/** The surface material of a stuff (its visual.surface, made by the SurfaceMaterials commandlet); null for none. */
	UMaterialInterface* SurfaceOf(colony::DefId Stuff);
	void RebuildChunk(int32 Level, int32 ChunkX, int32 ChunkY, const TArray<int32>& ThingIndexes, const colony::Snapshot& Snapshot);
	/** Hides the chunks of the levels above the highest one shown (ULevelsSubsystem), and shows the rest. */
	void ApplyCut(bool bForce);
	UInstancedStaticMeshComponent* GetComponent(FChunk& Chunk, int32 Key, UStaticMesh* Mesh, bool bGhost, bool bItem, bool bCastShadow);
	void AnimateDoors();
	int32 ChunkIndex(int32 Level, int32 ChunkX, int32 ChunkY) const { return (Level * ChunksY + ChunkY) * ChunksX + ChunkX; }
	int32 LevelOfChunk(int32 Index) const { return Index / FMath::Max(1, ChunksX * ChunksY); }

	UPROPERTY()
	TObjectPtr<AActor> DrawActor;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> GhostMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;

	/** The stuffs' surface materials by stuff def (null where a stuff has none or it is not made yet). */
	UPROPERTY()
	TMap<int32, TObjectPtr<UMaterialInterface>> Surfaces;

	TArray<FChunk> Chunks;
	TArray<FDefLook> DefLooks;
	TMap<uint64, int32> LookIndex;
	TArray<FLook> Looks;
	TMap<uint32, FDoorSlot> DoorSlots;
	/** Scratch space for one scan: the things of each chunk and the chunk's new signature. */
	TArray<TArray<int32>> ScanLists;
	TArray<uint64> ScanSignatures;
	/** Each snapshot thing's link neighbours (GetLook), -1 for a thing that does not link. */
	TArray<int8> ScanLinks;
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	int32 ShownTop = -1;
	int32 BuiltGeneration = -1;
	int64 LastSeconds = -1;
	const colony::Sim* BuiltSim = nullptr;
	FVector BuiltOrigin = FVector::ZeroVector;
	bool bBuilt = false;
	FBuildingStats Stats;

	TWeakObjectPtr<USimSubsystem> Bridge;
};
