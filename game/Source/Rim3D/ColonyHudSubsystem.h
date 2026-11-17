#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"

#include "ColonyHudSubsystem.generated.h"

class AActor;
class USceneCaptureComponent2D;
class USimSubsystem;
class UTexture;
class UTextureRenderTarget2D;
struct ImGuiViewport;

/** The tabs of the bottom bar, left to right (docs/design/ui-layout.md). */
enum class EHudTab : uint8
{
	None,
	Build,
	Work,
	Schedule,
	Assign,
	Animals,
	Mechs,
	Wildlife,
	Research,
	Quests,
	World,
	Inventory,
	Count
};

/** What the overseer's screen drew (smoke test `hud`). */
struct FHudStats
{
	int32 BarFrames = 0;
	int32 TabWindowFrames = 0;
	int32 HoverFrames = 0;
	int32 InfoFrames = 0;
	int32 SettingsFrames = 0;
	int32 MugshotsMade = 0;
	int32 CommandsSent = 0;
};

/**
 * The overseer's screen layout (docs/design/ui-layout.md), ImGui:
 *
 * - The bottom bar, full width: Build, Work, Schedule, Assign, Animals, Mechs, Wildlife, Research, Quests, World, Inventory;
 *   at the far right the speed buttons, the key help (book), Factions and Menu. A click opens a tab's window above the bar,
 *   a second click closes it; the open tab stays lit. Build, Work, Schedule, Quests and World are drawn by the subsystems
 *   that already had them (Orders, Panels, QuestPanel, WorldScreen); this one opens and closes them and draws the rest.
 * - The date, time, weather and the level looked at, just above the bar's right end.
 * - Bottom left, nothing selected and no Build menu: what the cursor is over (ground, walk speed, fertility, light).
 * - The info window ("i" on the selected panel): Basics and the rest on the left, the description on the right.
 * - Pinned resources (a star in the Inventory tab; silver by default), under the colonist bar.
 * - Colonist mugshots for the colonist bar: a face picture per colonist, made once and remade when their clothes change.
 * - Settings, from Menu, with pages down the left (Game, Display, Sound, Feel, Controls); other code adds pages.
 *
 * Console: Hud.UI 0|1, Hud.Tab <name>, UI.TextSize 0|1.
 */
UCLASS()
class RIM3D_API UColonyHudSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// ---- Tabs ---------------------------------------------------------------------------------
	/** The tab whose window is open now, read from the subsystems that draw them. */
	EHudTab GetOpenTab() const;
	/** Opens a tab (closing the others), or closes it if it is open. */
	void ToggleTab(EHudTab Tab);
	void OpenTab(EHudTab Tab);
	void CloseTabs();
	static const TCHAR* TabLabel(EHudTab Tab);

	// ---- Settings -----------------------------------------------------------------------------
	/** Adds a page to the Settings window (or replaces one of that name). `Draw` draws the page's widgets inside the window. */
	void AddSettingsPage(const FString& Name, TFunction<void(float Scale)> Draw);
	void OpenSettingsPage(const FString& Name);
	bool IsSettingsOpen() const { return bSettingsOpen; }
	void SetSettingsOpen(bool bOpen) { bSettingsOpen = bOpen; }

	// ---- The info window ----------------------------------------------------------------------
	void OpenInfo(colony::Entity Thing) { InfoThing = Thing; }
	void CloseInfo() { InfoThing = colony::kNoEntity; }
	colony::Entity GetInfoThing() const { return InfoThing; }

	// ---- Pinned resources ---------------------------------------------------------------------
	bool IsPinned(const FString& DefName) const { return Pinned.Contains(DefName); }
	void TogglePin(const FString& DefName);
	/** Draws the pinned resources and their counts on the map, one line (inside an open window). False if none are pinned. */
	bool DrawPinnedResources(USimSubsystem& Bridge);

	// ---- Mugshots -----------------------------------------------------------------------------
	/** The colonist's face picture, or null until it has been made. */
	UTexture* GetMugshot(colony::Entity Pawn) const;

	// ---- Work groups (the Work tab) -----------------------------------------------------------
	/** Puts a colonist in a work group (SetWorkGroupCommand); an empty name takes it out. */
	bool SetWorkGroup(colony::Entity Pawn, const FString& Group);

	// ---- State for tests ----------------------------------------------------------------------
	const FHudStats& GetStats() const { return Stats; }
	/** Where the bar drew the button with this label last frame, in ImGui pixels. */
	bool GetButtonRect(const FString& Label, FBox2D& Out) const;
	/** The cell under the cursor last frame (the hover readout), if any. */
	bool GetHoverCell(colony::CellPos& Out) const
	{
		Out = HoverCell;
		return bHasHoverCell;
	}

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FSettingsPage
	{
		FString Name;
		TFunction<void(float)> Draw;
	};

	struct FMugshot
	{
		TObjectPtr<UTextureRenderTarget2D> Target;
		uint64 Signature = 0;
		double MadeAt = 0.0;
	};

	USimSubsystem* GetBridge() const;
	bool IsOverseerActive() const;
	void Submit(colony::Command Command);

	void DrawHud(USimSubsystem& Bridge);
	void DrawBar(USimSubsystem& Bridge, float Scale);
	void DrawDateBox(USimSubsystem& Bridge, float Scale);
	void DrawMenuPopup(USimSubsystem& Bridge, float Scale);
	void DrawHoverReadout(USimSubsystem& Bridge, float Scale);
	void DrawInfoWindow(USimSubsystem& Bridge, float Scale);
	void DrawSettings(USimSubsystem& Bridge, float Scale);
	void DrawKeyListPage(float Scale);

	// The tabs this subsystem draws itself (ColonyHudTabs.cpp).
	void DrawAssignTab(USimSubsystem& Bridge, float Scale);
	void DrawAnimalsTab(USimSubsystem& Bridge, float Scale, bool bWild);
	void DrawMechsTab(USimSubsystem& Bridge, float Scale);
	void DrawResearchTab(USimSubsystem& Bridge, float Scale);
	void DrawInventoryTab(USimSubsystem& Bridge, float Scale);
	/** Puts the next window centred above the bar, at most `WidthShare` of the screen wide. */
	void PlaceTabWindow(float Scale, float Width, float MaxHeightShare = 0.7f) const;

	void UpdateHoverCell(USimSubsystem& Bridge);
	void UpdateMugshots(USimSubsystem& Bridge);
	void LoadPrefs();
	void SavePrefs() const;

	EHudTab OwnTab = EHudTab::None;
	bool bSettingsOpen = false;
	FString SettingsPage = TEXT("Game");
	TArray<FSettingsPage> SettingsPages;
	colony::Entity InfoThing = colony::kNoEntity;
	TArray<FString> Pinned;
	bool bHasHoverCell = false;
	colony::CellPos HoverCell;
	bool bQuitAsked = false;
	TMap<FString, FBox2D> ButtonRects;
	int32 ContentGeneration = -1;

	// The Research tab: the project picked in the tree (shown in the info panel), by defName.
	FString ResearchPicked;
	// The Inventory tab's search box.
	char InventorySearch[64] = {};
	// The Animals and Wildlife tabs: name being typed.
	colony::Entity RenamingAnimal = colony::kNoEntity;
	char AnimalName[32] = {};

	// Mugshots: render targets by entity, one capture component that takes each picture.
	TMap<uint32, FMugshot> Mugshots;
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> MugshotTargets;
	UPROPERTY()
	TObjectPtr<AActor> MugshotActor;
	UPROPERTY()
	TObjectPtr<USceneCaptureComponent2D> MugshotCapture;
	double NextMugshotCheck = 0.0;

	FHudStats Stats;
};
