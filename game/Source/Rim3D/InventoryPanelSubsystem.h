#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Snapshot.h"

#include "InventoryPanelSubsystem.generated.h"

class USimSubsystem;

/** What the inventory window drew and did (smoke test later). */
struct FInventoryPanelStats
{
	int32 WindowFrames = 0;
	/** Worn pieces, weapons and bag items drawn in the last frame the window was open. */
	int32 WornRows = 0;
	int32 WeaponRows = 0;
	int32 BagCells = 0;
	int32 CommandsSent = 0;
};

/**
 * A pawn's inventory on screen (docs/design/inventory.md), ImGui, opened and closed with I:
 *
 * - Whose: the protagonist while the player steers it, else the colonist selected in the overseer view.
 * - Worn: every piece of apparel by body slot (head, torso, legs, feet, hands, accessories), with its condition and weight.
 * - Weapons: the one in the hands and the ones on the body, by slot.
 * - Bag: a big grid of what it carries (Snapshot::inventory); a click drops a stack, Drop all empties the bag.
 * - Nearby: items lying within reach; a click takes one into the bag (InventoryCommand Take).
 * - The load bar: kg carried of what it can carry, amber past the free fifth, red over the limit.
 *
 * Console: Inventory.UI 0|1, Inventory.Open.
 */
UCLASS()
class RIM3D_API UInventoryPanelSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bInOpen) { bOpen = bInOpen; }

	/** The pawn the window shows, or kNoEntity. */
	colony::Entity GetShownPawn() const;
	/** Sends InventoryCommand Take / Drop / DropAll for the shown pawn. False (nothing sent) without one. */
	bool Take(colony::Entity Item, int32 Count = 0);
	bool Drop(colony::Entity Item, int32 Count = 0);
	bool DropAll();

	const FInventoryPanelStats& GetStats() const { return Stats; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	void Submit(colony::Command Command);
	void DrawWindow(USimSubsystem& Bridge, colony::Entity Pawn);

	bool bOpen = false;
	FInventoryPanelStats Stats;
};
