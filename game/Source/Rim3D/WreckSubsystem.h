#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"

#include "WreckSubsystem.generated.h"

class AActor;
class UMaterialInterface;
class USimSubsystem;
class UStaticMesh;
class UStaticMeshComponent;
namespace colony { struct Event; }

/**
 * Buildings that come down (docs/design/destruction.md), all cosmetic: when the sim wrecks a building (BuildingWrecked:
 * shot, blown up or battered to pieces) the building does not just vanish. It falls apart into broken pieces of its own
 * stuff (wood, stone blocks, steel; the stuff's real surface material, so a granite wall breaks into granite) that
 * tumble down from where it stood, under Chaos physics, with a cloud of dust rising as it goes. Once they settle the
 * pieces stay where they lie as the rubble, for as long as the sim keeps its Rubble filth in those cells: a colonist who
 * cleans the rubble away takes the pieces with it.
 *
 * Bigger and taller buildings make more pieces (a wall about 18 a cell). At most Wreck.MaxPieces lie about; the oldest
 * wreck is cleared first. Console: Wreck.Enabled, Wreck.MaxPieces.
 */
UCLASS()
class RIM3D_API UWreckSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Breaks a building of this def and stuff apart at a map point (metres, the ground's height), as a BuildingWrecked does. */
	void Wreck(colony::DefId Def, colony::DefId Stuff, float MapX, float MapY, float GroundM, int32 SizeX, int32 SizeY, int32 Level);

	// For the smoke test.
	int32 GetWrecks() const { return Wrecks.Num(); }
	int32 GetWrecksMade() const { return WrecksMade; }
	int32 GetPiecesAlive() const;
	int32 GetPiecesSettled() const { return PiecesSettled; }
	int32 GetPiecesCleared() const { return PiecesCleared; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FWreck
	{
		TArray<TWeakObjectPtr<UStaticMeshComponent>> Pieces;
		TWeakObjectPtr<UStaticMeshComponent> Floor;
		/** The footprint's cells (index on the ground), to see whether the sim still has rubble there. */
		TArray<int32> Cells;
		int32 Level = 0;
		float Age = 0.0f;
		bool bSettled = false;
	};

	void OnSimEvent(const colony::Event& Event);
	void EnsureScene();
	void Teardown();
	/** A 10 cm cube of the stuff's surface (its plain colour without one), with a box to collide with; made once a stuff. */
	UStaticMesh* PieceMesh(colony::DefId Stuff);
	void ClearWreck(FWreck& Wreck);
	void ClearCleanedRubble();

	TWeakObjectPtr<USimSubsystem> Bridge;
	FDelegateHandle EventHandle;
	UPROPERTY()
	TObjectPtr<AActor> Holder;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;
	TMap<int32, TObjectPtr<UStaticMesh>> MeshByStuff;
	TArray<FWreck> Wrecks;
	FRandomStream Random{0x5752454Bu};
	int32 BuiltGeneration = -1;
	uint64 SeenFilthChanges = ~0ULL;
	int32 WrecksMade = 0;
	int32 PiecesSettled = 0;
	int32 PiecesCleared = 0;
};
