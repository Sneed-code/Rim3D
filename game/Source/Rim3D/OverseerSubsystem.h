#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "SimPicking.h"

#include "OverseerSubsystem.generated.h"

class AOverseerPawn;
class UMaterialInterface;
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
	/** True while the overseer view is the one in use (not while the player walks a colonist). */
	bool IsOverseerActive() const;
	/** Hides the overseer's panels while another view has the player controller (direct mode,
	 *  U9). The camera pawn stays, with its focus, distance and the selection. EnterOverseerMode
	 *  brings it back. */
	void LeaveOverseerMode() { bActive = false; }

	/** The first (main) thing or cell selected; the inspect panel shows it. More things may be selected with it. */
	const FSimPick& GetSelection() const { return Selection; }
	/** Selects what was picked, alone. An empty pick clears the selection. */
	void Select(const FSimPick& Pick);
	/** Selects several things at once (a drag box); the first is the main one. An empty list clears the selection. */
	void SelectThings(const TArray<colony::Entity>& Things);
	/** Adds a thing to the selection, or takes it out if it is in already (shift click). */
	void ToggleThingSelected(colony::Entity Thing);
	/** Every thing selected, the main one first (none for a selected cell). */
	void GetSelectedThings(TArray<colony::Entity>& Out) const;
	bool IsThingSelected(colony::Entity Thing) const;
	int32 GetSelectedCount() const { return Selection.Kind == ESimPickKind::Thing ? 1 + Extra.Num() : (Selection.IsValid() ? 1 : 0); }
	void ClearSelection();
	/** Centres the camera on the selection (a thing or a cell). */
	void FocusOnSelection();
	/** Centres the camera on you (the protagonist), else the first colonist; selects them if asked. False with no colonists. */
	bool FocusOnColonists(bool bSelect);

	/**
	 * What a drag box from A to B (viewport pixels) selects, in sneed's order (docs/design/ui-layout.md "Selecting"):
	 * people (your colonists first, then anyone else), else animals, else things of the same kind as the one nearest the
	 * box's start. Fills `Out`; empty if the box holds nothing.
	 */
	void FindThingsInBox(const FVector2D& A, const FVector2D& B, TArray<colony::Entity>& Out) const;

	/** The left button went down on the world with no tool in hand: a click or the start of a drag box (viewport pixels). */
	void BeginSelectPress(const FVector2D& Screen);
	/** The cursor moved while it is held: past a few pixels it becomes a drag box, drawn until the release. */
	void UpdateSelectPress(const FVector2D& Screen);
	/** The left button came up: a drag box selects what is in it, a click what is under the cursor; shift adds to (or takes
	 *  from) the selection. False if no press was started here. */
	bool EndSelectPress(const FVector2D& Screen, bool bAdd);
	bool IsDragBoxActive() const { return bBoxActive; }

	/** Frames in which the ImGui panels were drawn: 0 means ImGui never became usable. */
	int32 GetPanelFramesDrawn() const { return PanelFramesDrawn; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void DrawSelectionHighlight(const USimSubsystem& Bridge) const;
	void DrawPanels(USimSubsystem& Bridge);
	/** Outlines of people and buildings hidden behind trees, roofs and walls (sneed 2026-11-25; SeeThroughArt). */
	void UpdateSeeThrough(const USimSubsystem& Bridge);

	UPROPERTY()
	TObjectPtr<AOverseerPawn> Pawn;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SeeThroughMaterial;
	bool bSeeThroughLoaded = false;
	bool bSeeThroughOnCamera = false;
	double NextSeeThroughMark = 0.0;

	FSimPick Selection;
	/** Things selected with the main one (Selection), in the order picked. */
	TArray<colony::Entity> Extra;
	bool bBoxPressed = false;
	bool bBoxActive = false;
	FVector2D BoxStart = FVector2D::ZeroVector;
	FVector2D BoxEnd = FVector2D::ZeroVector;
	int32 PanelFramesDrawn = 0;
	bool bActive = false;
};
