#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/BuildPlanner.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Snapshot.h"

#include "SimPicking.h"

#include "OrdersSubsystem.generated.h"

class AActor;
class IInputProcessor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class USimSubsystem;
class UTexture;
class UTextureRenderTarget2D;
class USceneCaptureComponent2D;
class UStaticMeshComponent;

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
	/** Drag a rectangle: a growing zone of its free soil cells, sown with the chosen crop (GetGrowCrop). */
	Grow,
	/** Drag a rectangle: marks the laid floors in it to be taken up (DesignationKind::RemoveFloor). */
	RemoveFloor,
	/** Drag a rectangle: adds its cells to the landing zone, where shuttles set down (docs/design/landing.md). */
	LandingZone,
	/** Drag a rectangle: takes its cells out of the landing zone. */
	RemoveLandingZone,
	/** Drag a rectangle: plans its ground dug or filled flat to the height of the cell the drag starts on, nudged by
	 *  the Raise / Lower buttons (docs/design/earthworks.md). */
	LevelEarth,
	/** Drag a rectangle: drops the earthworks planned in it. */
	CancelEarth,
	/** Drag a rectangle: adds its cells to the allowed area picked with SetAreaTool (docs/design/areas.md). */
	PaintArea,
	/** Drag a rectangle: takes its cells out of that area. */
	EraseArea,
	/** Drag a rectangle: an animal zone of its free cells (docs/design/husbandry.md); fenced in all round, it is a pen. */
	AnimalZone,
	/** Drag a rectangle: marks the wild animals in it to be tamed or hunted, the tame ones to be slaughtered. */
	Tame,
	Hunt,
	Slaughter,
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
	/** Cells of growing zones drawn. */
	int32 GrowCells = 0;
	/** Cells of the landing zone drawn. */
	int32 LandingCells = 0;
	/** Cells of animal zones and pens drawn. */
	int32 AnimalZoneCells = 0;
	/** Instances of the placement ghost (valid, invalid). */
	int32 GhostValid = 0;
	int32 GhostInvalid = 0;
	/** How many times the overlay was rebuilt since the sim last started. */
	int64 OverlayRebuilds = 0;
	/** Build tile pictures made from the buildings' own meshes since the sim last started. */
	int32 TileIconsMade = 0;
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

	// ---- The Build tab (docs/design/ui-layout.md) --------------------------------------------
	/** The Build menu above the bottom bar: categories on the left, the category's tiles beside them. The bar's Build tab opens it. */
	bool IsBuildMenuOpen() const { return bBuildMenuOpen; }
	/** Opens or closes the Build menu; closing it puts down the tool in hand. */
	void SetBuildMenuOpen(bool bOpen);
	/** The open category's label ("Orders", "Zone", "Structure", ...), empty for none. */
	const FString& GetOpenCategory() const { return OpenCategory; }
	void SetOpenCategory(const FString& Category) { OpenCategory = Category; }
	/** The Build menu's rectangle last frame (ImGui pixels), so the selected panel can sit above it. */
	bool GetBuildMenuRect(FBox2D& Out) const
	{
		Out = BuildMenuRect;
		return bBuildMenuShown;
	}
	/** What a drag of the building in hand plans: one, a line, a room's outline or a filled rectangle. */
	colony::DragShape GetBuildShape() const { return BuildShape; }
	void SetBuildShape(colony::DragShape Shape) { BuildShape = Shape; }

	// ---- Tools ------------------------------------------------------------------------------
	EOrderTool GetTool() const { return Tool; }
	/** Picks a tool; stops a drag in progress. Build keeps the building last chosen. */
	void SetTool(EOrderTool NewTool);
	/** Picks the Build tool for this building def (a Things defName), made of the stuff with the most in stock unless `Stuff` names one. False if the def can't be built. */
	bool SelectBuilding(const FString& DefName, const FString& Stuff = FString());
	colony::DefId GetBuildDef() const { return BuildDef; }
	/** The crop the Grow tool sows (a Things def such as Plant_Rice); rice until the player picks another. */
	colony::DefId GetGrowCrop() const { return GrowCrop; }
	/** Picks the Grow tool's crop by def name; false if it is not a crop the colony may grow. */
	bool SetGrowCrop(const FString& DefName);
	colony::DefId GetBuildStuff() const { return BuildStuff; }
	void SetBuildStuff(colony::DefId Stuff) { BuildStuff = Stuff; }
	colony::Rot4 GetBuildRotation() const { return BuildRot; }
	void RotateBuilding();
	/** Stops the tool and any drag. True if there was one. */
	bool CancelTool();
	/** Picks the PaintArea (bAdd) or EraseArea tool for allowed area `Area` (AreaView::id): the Areas window's Paint / Erase. */
	void SetAreaTool(uint32 Area, bool bAdd);
	/** The allowed area the area tools work on, and whose cells the overlay shows while one is in hand (0 for none). */
	uint32 GetAreaToolArea() const { return AreaToolArea; }

	// ---- Walking the colony (task U10, docs/design/handson.md) ---------------------------------
	/** In direct mode the Build tool still works: the crosshair aims the ghost, one click plans it. */
	bool IsDirectBuilding() const;
	/** The next (+1) or previous (-1) building in the menu; picks the Build tool if it wasn't. */
	bool CycleBuilding(int32 Direction);
	/** The next stuff the chosen building can be made of. */
	bool CycleStuff();
	/** "Wall (steel)" for the screen, empty with no building in hand. */
	FString GetBuildSummary() const;
	/** No cell under the crosshair: the ghost goes. */
	void ClearHover() { bHasHover = false; }
	/** Plans the building at the hovered cell. False if there is none or it can't go there. */
	bool PlaceAtHover();

	// ---- Mouse (called by the overseer pawn; the test calls the cell versions) ----------------
	/** Left button down: starts a drag if a tool is in hand. True if it took the click. */
	bool HandleLeftPressed(const FVector2D& Screen);
	/** Left button up: finishes the drag. True if it took the click. */
	bool HandleLeftReleased(const FVector2D& Screen);
	/**
	 * A left button event straight from Slate (FOrdersClickProcessor, ahead of the ImGui overlay's input handler). With a tool in hand
	 * and no ImGui window or popup under the cursor, a press on the world is the tool's, whatever ImGui's want-the-mouse flag or the
	 * player controller's key state says; the release ends the drag. `ScreenSpace` is a desktop position. True if it took the event.
	 */
	bool HandleSlateLeftButton(bool bPressed, const FVector2D& ScreenSpace);
	/** A desktop position as game viewport pixels; false outside the viewport. */
	bool ScreenToViewport(const FVector2D& ScreenSpace, FVector2D& OutPixel) const;
	/** Where Slate's cursor is, in game viewport pixels. */
	bool CursorViewportPosition(FVector2D& OutPixel) const;
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
	/** Where the architect bar drew the button with this label last frame, in ImGui pixels (smoke test `orders_world` clicks it). */
	bool GetButtonRect(const FString& Label, FBox2D& Out) const
	{
		const FBox2D* Found = ButtonRects.Find(Label);
		if (Found)
		{
			Out = *Found;
		}
		return Found != nullptr;
	}
	bool HasHoverCell() const { return bHasHover; }
	colony::CellPos GetHoverCell() const { return Hover; }
	bool IsMouseOverUi() const { return bMouseOverUi; }
	/** The architect bar's rectangle last frame (ImGui pixels), so the inspect panel can stay clear of it. */
	bool GetBarRect(FBox2D& Out) const
	{
		Out = BarRect;
		return bBarShown;
	}

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FBuildOption
	{
		colony::DefId Def;
		FString DefName;
		FString Label;
		/** The category's label, as its button says. */
		FString Category;
		/** A floor (a Terrain def): PlaceBlueprintCommand lays a floor plan. */
		bool bFloor = false;
		/** The research it still needs, in plain words; empty if it can be planned. */
		FString Locked;
		colony::DragShape Drag = colony::DragShape::Single;
		int32 SizeX = 1;
		int32 SizeY = 1;
	};
	struct FStuffOption
	{
		colony::DefId Def;
		FString Label;
		int32 InStock = 0;
	};

	USimSubsystem* GetBridge() const;
	/** The picture of a building's tile (its own mesh, seen from above at an angle), or null until it is made; asking queues it.
	 *  Made one a frame by UpdateTileIcons. Floors and orders keep their drawn icons. */
	UTexture* GetTileIcon(colony::DefId Def);
	void UpdateTileIcons();
	FSimPick PickGroundAt(const FVector2D& Screen) const;
	bool IsOverseerActive() const;
	void ResetForNewSim();
	void ReadBuildOptions();
	void RefreshStuffOptions();
	colony::DefId BestStuffFor(colony::DefId Def) const;
	colony::CellRect DragRect() const;
	/** The ground height at a cell's middle, cm on the 25 cm step (Level earth). */
	int32 LevelHeightAtCm(colony::CellPos Cell) const;
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
	/** The Build menu: categories, tiles, and the strip about the thing in hand. */
	void DrawBuildMenu(USimSubsystem& Bridge, float Scale);
	const FBuildOption* FindOption(colony::DefId Def) const;

	// What is in hand.
	EOrderTool Tool = EOrderTool::None;
	colony::DefId BuildDef;
	colony::DefId GrowCrop;
	colony::DefId BuildStuff;
	colony::Rot4 BuildRot = colony::Rot4::North;
	FString OpenCategory;
	colony::DragShape BuildShape = colony::DragShape::Single;
	bool bBuildMenuOpen = false;
	bool bBuildMenuShown = false;
	FBox2D BuildMenuRect = FBox2D(ForceInit);
	char BuildSearch[64] = {};
	int32 ResearchDoneSeen = -1;
	// What the drag in hand would plan and cost, worked out again only when the drag changes.
	uint64 PlanSignature = 0;
	FString PlanText;
	bool bPlanAffordable = true;

	// The cursor and the drag.
	bool bMouseDriven = true;
	bool bMouseOverUi = false;
	bool bHasHover = false;
	colony::CellPos Hover;
	bool bDragging = false;
	colony::CellPos DragStart;
	/** Level earth: the height the drag started on (cm, on a 25 cm step), and the player's nudge on top of it. */
	int32 LevelStartCm = 0;
	int32 LevelNudgeCm = 0;
	uint32 AreaToolArea = 0;

	// Menu contents, read from the defs once per sim.
	TArray<FBuildOption> BuildOptions;
	TArray<FString> BuildCategories;
	TArray<FStuffOption> StuffOptions;
	int64 StuffOptionsSeconds = -1;

	FString Message;
	double MessageUntil = 0.0;
	TMap<FString, FBox2D> ButtonRects;
	FBox2D BarRect = FBox2D(ForceInit);
	bool bBarShown = false;
	/** Takes world clicks ahead of the ImGui overlay (see HandleSlateLeftButton); moved to the front once ImGui is up. */
	TSharedPtr<IInputProcessor> ClickProcessor;
	bool bClickProcessorAtFront = false;
	bool bWarnedImGuiHeld = false;
	int32 CommandsSent = 0;

	// Build tile pictures (GetTileIcon): a render target a def, made by a capture on a stage of its own.
	UPROPERTY()
	TMap<int32, TObjectPtr<UTextureRenderTarget2D>> TileIcons;
	TArray<int32> WantedTileIcons;
	/** Defs whose picture was made for the running sim. */
	TSet<int32> TileIconsReady;
	/** Orders.TileIcons and Orders.TileIconsLit when the pictures were made: a change makes them again. */
	int32 TileIconMode = -1;
	/** Defs whose mesh could not be had (no picture); asked no more until the sim starts again. */
	TSet<int32> NoTileIcon;
	UPROPERTY()
	TObjectPtr<AActor> IconStage;
	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> IconCapture;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> IconMesh;

	// The overlay: marks, zone tiles, ghost.
	UPROPERTY()
	TObjectPtr<AActor> OverlayActor;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> ZoneMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> GrowZoneMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> LandZoneMaterial;
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
