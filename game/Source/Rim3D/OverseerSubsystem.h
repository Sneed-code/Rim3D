#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SimPicking.h"

#include "OverseerSubsystem.generated.h"

class AOverseerPawn;
class USimSubsystem;

/**
 * The overseer's view of the colony: the camera pawn, what is selected, and the ImGui panels
 * (speed controls at the top, the inspect panel at the bottom left, a key hint at the bottom
 * right). It reads the sim only through USimSubsystem. Start it with -Overseer on the command line
 * or Overseer.Enter in the console; the protagonist task (U9) switches between this and direct play.
 * Console: Overseer.Enter, Overseer.UI (0 hides the panels).
 */
UCLASS()
class RIM3D_API UOverseerSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Spawns the camera pawn over the middle of the map and makes the first player control it. */
	AOverseerPawn* EnterOverseerMode();
	AOverseerPawn* GetOverseerPawn() const;
	bool IsOverseerActive() const;

	const FSimPick& GetSelection() const { return Selection; }
	/** Selects what was picked. An empty pick clears the selection. */
	void Select(const FSimPick& Pick);
	void ClearSelection();
	/** Centres the camera on the selection (a thing or a cell). */
	void FocusOnSelection();

	/** Frames in which the ImGui panels were drawn: 0 means ImGui never became usable. */
	int32 GetPanelFramesDrawn() const { return PanelFramesDrawn; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void DrawSelectionHighlight(const USimSubsystem& Bridge) const;
	void DrawPanels(USimSubsystem& Bridge);

	UPROPERTY()
	TObjectPtr<AOverseerPawn> Pawn;

	FSimPick Selection;
	int32 PanelFramesDrawn = 0;
};
