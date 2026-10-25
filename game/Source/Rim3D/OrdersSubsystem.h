#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Snapshot.h"

#include "SimPicking.h"

#include "OrdersSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class USimSubsystem;

/** What the next click or drag of the left mouse button does (docs/design/orders.md). None: it selects, as before. */
enum class EOrderTool : uint8
{
	None,
	/** Plans the chosen building: a click, or a drag along a line for a 1 x 1 building. */
	Build,
	/** Drag a rectangle: a stockpile zone of its free cells. */
	Stockpile,
	/** Drag a rectangle: takes its cells out of their zones. */
	RemoveZone,
	/** Drag a rectangle: marks rock for mining, plants for cutting, plants for harvesting. */
	Mine,
	Chop,
	Harvest,
	/** Drag a rectangle: takes off marks, plans and frames in it, and the deconstruct mark of buildings. */
	Cancel,
	/** Drag a rectangle: marks the finished buildings in it to be taken down. */
	Deconstruct,
};

/** What the orders overlay draws (smoke test `orders`). */
struct FOrdersStats
{
	/** Instances of the mark overlay, by kind: mine, cut and harvest marks, deconstruct marks, and stockpile cells. */
	int32 MineMarks = 0;
	int32 CutMarks = 0;
	int32 HarvestMarks = 0;
	int32 DeconstructMarks = 0;
	int32 ZoneCells = 0;
	/** Instances of the placement ghost (valid, invalid). */
	int32 GhostValid = 0;
	int32 GhostInvalid = 0;
	/** How many times the overlay was rebuilt since the sim last started. */
	int64 OverlayRebuilds = 0;
};

/**
 * The player's orders (task U7, docs/design/orders.md): the architect menu, the marks overlay and the
 * draft, move and attack orders. Everything it does is a command to the sim (USimSubsystem::Submit);
 * everything it draws comes from the snapshot or from a tool in the middle of a drag.
 *
 * - Tools: Build (one of the buildings the defs let the player plan, made of a stuff, with a grid-snapped
 *   see-through ghost that is green where it can be placed and red where it can't, R turns it), stockpile
 *   zones, the Mine, Chop and Harvest marks, Cancel and Deconstruct. All but a click on a building are
 *   dragged out as a rectangle on the ground (a 1 x 1 building as a line).
 * - Marks overlay: the sim's designations, stockpile cells and deconstruct marks drawn as instanced
 *   meshes (an orange ring and post for rock to mine, a green one for a tree to cut, yellow for
 *   something to harvest, a red plate on a building to take down, a blue tile for a stockpile cell).
 *   Rebuilt only when the marks change.
 * - Fighting: R drafts the selected colonist (or turns the ghost, with a tool in hand); with a drafted colonist
 *   selected, a right click on the ground is a move order and on an enemy an attack order; the
 *   Alarm button drafts every colonist. Drafted colonists get a red ring, a line to the cell they hold
 *   and one to the target they shoot at.
 *
 * It only works while the overseer view is the one in use. Orders given while the game is paused make the sim
 * step once so the marks and plans show at once. Console: Orders.Marks 0 hides the overlay.
 */
UCLASS()
class RIM3D_API UOrdersSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// ---- Tools ------------------------------------------------------------------------------
	EOrderTool GetTool() const { return Tool; }
	/** Picks a tool; stops a drag in progress. Build keeps the building last chosen. */
	void SetTool(EOrderTool NewTool);
	/** Picks the Build tool for this building def (a Things defName), made of the stuff with the most in stock unless `Stuff` names one. False if the def can't be built. */
	bool SelectBuilding(const FString& DefName, const FString& Stuff = FString());
	colony::DefId GetBuildDef() const { return BuildDef; }
	colony::DefId GetBuildStuff() const { return BuildStuff; }
	void SetBuildStuff(colony::DefId Stuff) { BuildStuff = Stuff; }
	colony::Rot4 GetBuildRotation() const { return BuildRot; }
	void RotateBuilding();
	/** Stops the tool and any drag. True if there was one. */
	bool CancelTool();

	// ---- Mouse (called by the overseer pawn; the test calls the cell versions) ----------------
	/** Left button down: starts a drag if a tool is in hand. True if it took the click. */
	bool HandleLeftPressed(const FVector2D& Screen);
	/** Left button up: finishes the drag. True if it took the click. */
	bool HandleLeftReleased(const FVector2D& Screen);
	/** Right click: stops a tool, else with a drafted colonist selected a move or attack order. True if it did something. */
	bool HandleRightClick(const FVector2D& Screen);
	/** R: turns the building ghost, else drafts or undrafts the selected colonist. True if it did something. */
	bool HandleRotateOrDraftKey();

	// ---- The same by cell -------------------------------------------------------------------
	/** Where the cursor is, for the ghost and the dragged rectangle. */
	void SetHoverCell(colony::CellPos Cell);
	/** With false the cursor's position is not read from the mouse, so a test can point at cells itself. */
	void SetMouseDriven(bool bDriven) { bMouseDriven = bDriven; }
	void BeginDrag(colony::CellPos Cell);
	/** Ends the drag at `Cell` and gives the order. Returns how many commands it sent. */
	int32 EndDrag(colony::CellPos Cell);
	bool IsDragging() const { return bDragging; }
	/** The cells a Build drag would plan now, in order, each the footprint's origin. */
	void GetBuildOrigins(TArray<colony::CellPos>& Out) const;
	/** Whether a building of this def could be planned here (the checks of Construction::placeBlueprint that don't need a command). */
	bool CanPlace(colony::DefId Def, colony::CellPos Origin, colony::Rot4 Rot) const;

	// ---- Fighting ---------------------------------------------------------------------------
	/** Drafts (or sends back to work) the selected colonist. False if none is selected. */
	bool DraftSelected(bool bDraft);
	bool ToggleDraftSelected();
	/** The Alarm button: drafts every colonist that isn't, or, when all are, sends them all back to work. Returns how many were changed. */
	int32 Alarm();
	/** Move order for the selected, drafted colonist. */
	bool OrderMoveSelected(colony::CellPos Cell);
	bool OrderAttackSelected(colony::Entity Target);
	/** For the inspect panel: draft button and hint for a selected colonist (inside an ImGui window). */
	void DrawDraftControls(const colony::ThingView& View);

	// ---- State for tests --------------------------------------------------------------------
	const FOrdersStats& GetStats() const { return Stats; }
	/** Draws the marks and the ghost now from the newest snapshot (the tick does this itself). */
	void RefreshOverlay();
	const FString& GetLastMessage() const { return Message; }
	/** Commands sent by the tools since the sim started. */
	int32 GetCommandsSent() const { return CommandsSent; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FBuildOption
	{
		colony::DefId Def;
		FString Label;
		FString Category;
	};
	struct FStuffOption
	{
		colony::DefId Def;
		FString Label;
		int32 InStock = 0;
	};

	USimSubsystem* GetBridge() const;
	FSimPick PickGroundAt(const FVector2D& Screen) const;
	bool IsOverseerActive() const;
	void ResetForNewSim();
	void ReadBuildOptions();
	void RefreshStuffOptions();
	colony::DefId BestStuffFor(colony::DefId Def) const;
	colony::CellRect DragRect() const;
	void Submit(colony::Command Command);
	void FlushIfPaused();
	bool SetDraft(colony::Entity Pawn, bool bDraft);
	/** The selected thing if it is one of the player's colonists on the map. */
	const colony::ThingView* SelectedColonist(const USimSubsystem& Bridge) const;
	void Say(const FString& Text);

	void EnsureOverlayActor();
	UStaticMesh* GetMarkMesh(int32 Kind, int32 TopCm);
	UInstancedStaticMeshComponent* GetOverlayComponent(int32 Key, UStaticMesh* Mesh, UMaterialInterface* Material);
	void RebuildMarks(const USimSubsystem& Bridge);
	void UpdatePreview(USimSubsystem& Bridge);
	void DrawDragOutline(const USimSubsystem& Bridge) const;
	void DrawDraftMarkers(const USimSubsystem& Bridge) const;
	void DrawPanels(USimSubsystem& Bridge);

	// What is in hand.
	EOrderTool Tool = EOrderTool::None;
	colony::DefId BuildDef;
	colony::DefId BuildStuff;
	colony::Rot4 BuildRot = colony::Rot4::North;
	FString OpenCategory;

	// The cursor and the drag.
	bool bMouseDriven = true;
	bool bMouseOverUi = false;
	bool bHasHover = false;
	colony::CellPos Hover;
	bool bDragging = false;
	colony::CellPos DragStart;

	// Menu contents, read from the defs once per sim.
	TArray<FBuildOption> BuildOptions;
	TArray<FString> BuildCategories;
	TArray<FStuffOption> StuffOptions;
	int64 StuffOptionsSeconds = -1;

	FString Message;
	double MessageUntil = 0.0;
	int32 CommandsSent = 0;

	// The overlay: marks, zone tiles, ghost.
	UPROPERTY()
	TObjectPtr<AActor> OverlayActor;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> ZoneMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GhostGoodMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GhostBadMaterial;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;
	UPROPERTY()
	TMap<int32, TObjectPtr<UInstancedStaticMeshComponent>> OverlayComponents;
	UPROPERTY()
	TMap<int32, TObjectPtr<UStaticMesh>> MarkMeshes;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> GhostGood;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> GhostBad;

	int32 OverlayGeneration = -1;
	FVector OverlayOrigin = FVector::ZeroVector;
	int64 OverlaySeconds = -1;
	uint64 MarksSignature = 0;
	uint64 PreviewSignature = 0;
	int32 ContentGeneration = -1;
	FOrdersStats Stats;
};
