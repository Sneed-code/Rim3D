#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "CreatorPeople.h"

#include "ColonyHudSubsystem.generated.h"

class ACameraActor;

class AActor;
class USceneCaptureComponent2D;
class USimSubsystem;
class UTexture;
class UTextureRenderTarget2D;
struct ImGuiViewport;
namespace colony
{
class DefDatabase;
class Sim;
struct Colonist;
}

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

/** The manager windows opened from the Assign tab (ColonyHudScreens.cpp), one at a time. */
enum class EHudScreen : uint8
{
	None,
	Outfits,
	Areas,
	Medical,
	Prisoners,
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
	int32 ScreenFrames = 0;
	int32 MainMenuFrames = 0;
	int32 LessonFrames = 0;
};

/**
 * The overseer's screen layout (docs/design/ui-layout.md), ImGui:
 *
 * - The bottom bar, full width: Build, Work, Schedule, Assign, Animals, Mechs, Wildlife, Research, Quests, World, Inventory;
 *   at the far right the speed buttons, the key help (book), Factions, the learning helper (book) and Menu. A click opens a tab's window above the bar,
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

	// ---- Manager windows (Outfits, Areas, Medical, Prisoners) ----------------------------------
	void OpenScreen(EHudScreen Screen) { OpenedScreen = Screen; }
	EHudScreen GetOpenScreen() const { return OpenedScreen; }

	// ---- The main menu (new colony, load, settings, quit) ------------------------------------
	/** Shown at launch when the game was started without a scenario (not in smoke tests), and from Menu > New colony. */
	void SetMainMenuOpen(bool bOpen) { bMainMenuOpen = bOpen; }
	bool IsMainMenuOpen() const { return bMainMenuOpen; }
	/** The main menu's page: 0 title, 1 scenario, 2 load, 3 people (docs/design/new-game.md). For the smoke test. */
	int32 GetMainMenuPage() const { return MainMenuPage; }
	void SetMainMenuPageForTest(int32 Page) { MainMenuPage = Page; }
	/** The new game's seed and map size, and the people page's picked colonist and tab (smoke test `newgame`). */
	void SetNewGameForTest(int32 Seed, int32 MapSize) { NewSeed = Seed; NewMapSize = MapSize; }
	/** Slot 0 opens "You", another slot "Your companions" on that stranger; Tab 0 traits, 1 looks. */
	void SetPeopleForTest(int32 Selected, int32 Tab) { PeopleSelected = Selected; PeopleTab = Tab; MainMenuPage = Selected == 0 ? 3 : 4; }
	void SetPeopleClothesForTest(bool bOn) { bPeopleClothes = bOn; }
	void SetPeopleSpinForTest(float Rate) { PeopleSpinRate = Rate; }
	/** Start on the people page, as the button does. */
	void LandColonyForTest();

	// ---- The learning helper (lessons at the top right) -----------------------------------------
	bool AreLessonsOn() const { return bLessonsOn; }
	static int32 CountLessons();
	/** Smoke tests: turns the helper on or off and, with bForget, forgets the lessons learned, saving neither. */
	void SetLessonsForTest(bool bOn, bool bForget)
	{
		bLessonsOn = bOn;
		if (bForget)
		{
			LearnedLessons.Reset();
		}
	}
	/** Starts a new colony with the main menu's choices (scenario, narrator, difficulty, seed); the site is the scenario
	 *  biome's unless bPickSite, which opens the world map to pick one. True if the sim started. */
	bool StartNewColony(bool bPickSite);

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

	// The manager windows and the main menu (ColonyHudScreens.cpp).
	void DrawScreen(USimSubsystem& Bridge, float Scale);
	void DrawOutfitsScreen(USimSubsystem& Bridge, float Scale);
	void DrawAreasScreen(USimSubsystem& Bridge, float Scale);
	void DrawMedicalScreen(USimSubsystem& Bridge, float Scale);
	void DrawPrisonersScreen(USimSubsystem& Bridge, float Scale);
	void DrawMainMenu(USimSubsystem& Bridge);
	/** Sends the narrator and difficulty picked in the main menu once the new sim runs. */
	void SendPendingNarrator(USimSubsystem& Bridge);

	// The title screen and the new game (ColonyHudNewGame.cpp, docs/design/new-game.md).
	void DrawTitlePage(USimSubsystem& Bridge, float Scale);
	void DrawScenarioPage(USimSubsystem& Bridge, float Scale);
	void DrawPeoplePage(USimSubsystem& Bridge, float Scale);
	/** After "Choose landing site": the world screen's landing opens the people page, its Back the scenario page. */
	void CheckLanding(USimSubsystem& Bridge);
	/** The people page over the new colony, paused at its first second. */
	void EnterPeoplePage(USimSubsystem& Bridge);
	/** Start on the people page: the game begins. */
	void LandColony(USimSubsystem& Bridge);
	void UpdatePeopleCamera(colony::Entity Pawn);
	void EndPeopleCamera();
	/** Sends a colonist's creator look to the sim (EditColonistCommand) and remembers it. */
	void SubmitPeopleLook(colony::Entity Pawn, int32 Slot, const CreatorPeople::FLook& Look);
	/** The character screens' parts (ColonyHudNewGame.cpp): names, the looks sliders, the trait shop and skills. */
	void DrawPeopleNames(const colony::DefDatabase& Defs, const colony::Colonist& Colonist, colony::Entity Picked, int32 Slot, float Scale);
	void DrawPeopleLooks(const colony::Sim& Sim, colony::Entity Picked, int32 Slot, bool bYou, float Scale);
	void DrawTraitShop(const colony::Sim& Sim, const colony::Colonist& Colonist, int32 Slot, float Scale);

	// The learning helper (ColonyHudLessons.cpp).
	void DrawLessons(USimSubsystem& Bridge, float Scale);

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
	/** The learning helper is on (the book on the bar); the lessons marked "Got it". Saved with the HUD's settings. */
	bool bLessonsOn = true;
	TSet<FString> LearnedLessons;
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

	// Manager windows.
	EHudScreen OpenedScreen = EHudScreen::None;
	uint32 OutfitPicked = 1;
	float HpDrag[2] = {0.0f, 100.0f};
	bool bHpDragging = false;
	char NewName[32] = {};
	uint32 AreaPicked = 0;
	colony::Entity PatientPicked = colony::kNoEntity;

	// The main menu: its page and the new colony's choices.
	bool bMainMenuOpen = false;
	bool bMainMenuChecked = false;
	int32 MainMenuPage = 0;  // 0 the title, 1 new colony (scenario), 2 load, 3 you, 4 your companions (docs/design/new-game.md)
	FString NewScenario = TEXT("CrashLanded");
	FString NewNarrator = TEXT("Orrin");
	// The only difficulty for now, never shown (sneed 2026-11-21: "standard").
	FString NewDifficulty = TEXT("StriveToSurvive");
	// The new game's landing site and people pages.
	bool bAwaitingLanding = false;
	int32 PeopleSelected = 0;
	int32 PeopleGeneration = -1;
	int32 PeopleRerolls = 0;
	int32 PeopleTab = 0;  // on the companions screen: 0 traits and skills, 1 looks
	uint32 PeopleTraitRoll = 0;
	bool bPeopleClothes = false;  // "clothes on": the cargo pack's cotton on everyone, cosmetic
	float PeopleSpin = 0.0f;      // the turntable, degrees past the body's facing
	float PeopleSpinRate = 0.0f;  // degrees a second, easing off after a drag
	TMap<uint32, FString> PeopleLooks;  // by pawn: the look text being edited (shown before the sim has it)
	colony::Entity PeopleNamesFor = colony::kNoEntity;
	char PeopleFirst[32] = {};
	char PeopleNick[32] = {};
	char PeopleLast[32] = {};
	float PeoplePitch = -4.0f;
	float PeopleDistance = 280.0f;
	TWeakObjectPtr<ACameraActor> PeopleCamera;
	int32 NewSeed = 42;
	int32 NewMapSize = 512;
	bool bNarratorPending = false;
	int32 NarratorGeneration = -1;
	TArray<FString> SaveSlots;

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
