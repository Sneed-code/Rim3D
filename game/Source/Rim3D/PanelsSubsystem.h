#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/BillMode.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Snapshot.h"

#include "PanelsSubsystem.generated.h"

class USimSubsystem;

/** Which big table is open in the middle of the screen. */
enum class EColonyTab : uint8
{
	None,
	/** Work tab: a row for each colonist, a column for each kind of work, a priority in each cell. */
	Work,
	/** Schedule tab: a row for each colonist, a column for each hour of the day. */
	Schedule,
};

/** One line of the message log. */
struct FColonyLogLine
{
	int64 GameSeconds = 0;
	FString Text;
	/** Danger (red), trouble (orange), good news (green) or plain. */
	uint8 Severity = 0;
	/** The pawn it is about, to jump to it with a click. */
	colony::Entity Subject = colony::kNoEntity;
};

/** What the panels drew and did (smoke test `panels`). */
struct FPanelsStats
{
	int32 FramesDrawn = 0;
	int32 WorkTabFrames = 0;
	int32 ScheduleTabFrames = 0;
	int32 ColonistPanelFrames = 0;
	int32 BillPanelFrames = 0;
	int32 LogPanelFrames = 0;
	int32 ThermostatPanelFrames = 0;
	/** Commands the panels sent since the sim started. */
	int32 CommandsSent = 0;
};

/**
 * The colony panels (task U8, docs/design/panels.md), all ImGui, all through USimSubsystem:
 *
 * - Top left: buttons for the Work and Schedule tabs and the message log, and a bar of colonists
 *   (name, coloured by mood; click selects, double click centres the camera).
 * - Work tab: every colonist against every kind of work. A click on a cell goes 0 (off), 1 (first),
 *   2, 3, 4 (last) and round again; a right click goes back. Work a colonist can never do is X.
 *   Sends SetWorkPriorityCommand.
 * - Schedule tab: every colonist against the 24 hours of the day, each Anything, Work, Joy or Sleep.
 *   Sends SetScheduleCommand. The current hour is marked.
 * - Colonist panel, with a colonist selected: who they are (name, age, backstory, traits), what they
 *   are doing, food and rest, mood with the thoughts behind it and the break lines, skills and
 *   passions, health (pain, bleeding, injuries, abilities) and their bed (which sends AssignBedCommand).
 * - Bills panel, with a work building (the stove) selected: its bills in order with mode, count,
 *   pause and remove, and a form to add one. Sends AddBill, SetBill and RemoveBillCommand.
 * - Thermostat panel, with a heater or cooler selected (task U19): a slider for its target temperature (sent when
 *   let go), -1 and +1 buttons, the room's temperature now and a reset to the building's own limit. Sends
 *   SetThermostatCommand.
 * - Message log, right side under the alarm: what happened (deaths, wounds, mood trouble, finished
 *   bills, refused orders), newest last, click to centre on the colonist; the Log button opens more lines.
 *
 * It only draws while the overseer view is the one in use. Commands given while the game is paused
 * make the sim step once, so the change shows at once. Console: Panels.Tab 0|1|2 (none, work, schedule).
 */
UCLASS()
class RIM3D_API UPanelsSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	EColonyTab GetTab() const { return Tab; }
	void SetTab(EColonyTab NewTab) { Tab = NewTab; }
	bool IsLogOpen() const { return bLogOpen; }
	void SetLogOpen(bool bOpen) { bLogOpen = bOpen; }

	// ---- What a click does (the panels call these; the smoke test calls them too) ---------------
	/** Moves a colonist's priority for the work type at `WorkIndex` (JobSystem::workTypes order) one step: forward 0, 1, 2, 3, 4, 0; back the other way. False if the work can't be done or the colonist has no work. */
	bool CyclePriority(colony::Entity Pawn, int32 WorkIndex, bool bForward);
	/** Moves one hour of a colonist's schedule one step through Anything, Work, Joy, Sleep. */
	bool CycleSchedule(colony::Entity Pawn, int32 Hour, bool bForward);
	/** Puts a bill on a work building. `Recipe` is a Recipes defName. */
	bool AddBill(colony::Entity Building, const FString& Recipe, colony::BillMode Mode, int32 Count);
	bool RemoveBill(colony::Entity Building, uint32 BillId);
	bool ChangeBill(colony::Entity Building, uint32 BillId, colony::BillMode Mode, int32 Count, bool bSuspended);
	bool AssignBed(colony::Entity Bed, colony::Entity Pawn);
	/** Sets a heater's or cooler's target temperature (SetThermostatCommand), or with `bClear` goes back to the def's own limit. False if the building has no thermostat (nothing is sent). */
	bool SetThermostat(colony::Entity Building, float Celsius, bool bClear);

	// ---- State for tests --------------------------------------------------------------------
	const FPanelsStats& GetStats() const { return Stats; }
	const TArray<FColonyLogLine>& GetLog() const { return Log; }
	/** The colonists the tabs list, in the order of the snapshot. */
	void GetColonists(TArray<colony::Entity>& Out) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	bool IsOverseerActive() const;
	void Submit(colony::Command Command);
	void OnSimEvent(const colony::Event& Event);
	void AddLog(const FString& Text, uint8 Severity, colony::Entity Subject = colony::kNoEntity);
	FString NameOf(colony::Entity Pawn) const;
	void ResetForNewSim();

	void DrawPanels(USimSubsystem& Bridge);
	void DrawMenuAndColonistBar(USimSubsystem& Bridge, float Scale, float Margin, float& OutBottom);
	void DrawWorkTab(USimSubsystem& Bridge, float Scale);
	void DrawScheduleTab(USimSubsystem& Bridge, float Scale);
	void DrawColonistPanel(USimSubsystem& Bridge, const colony::ThingView& View, float Left, float Top, float Scale);
	void DrawBillsPanel(USimSubsystem& Bridge, const colony::ThingView& View, float Left, float Top, float Scale);
	void DrawThermostatPanel(USimSubsystem& Bridge, const colony::ThingView& View, float Left, float Top, float Scale);
	void DrawLog(USimSubsystem& Bridge, float Scale, float Margin);

	EColonyTab Tab = EColonyTab::None;
	bool bLogOpen = false;
	TArray<FColonyLogLine> Log;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> EventSource;
	int32 ContentGeneration = -1;
	int32 LastLogCountShown = 0;

	// The "add a bill" form of the bills panel.
	int32 NewRecipe = 0;
	int32 NewMode = 0;
	int32 NewCount = 1;
	colony::Entity NewBillBuilding = colony::kNoEntity;

	// The thermostat slider: the value being dragged is held here and sent when the mouse is let go.
	colony::Entity ThermoBuilding = colony::kNoEntity;
	float ThermoDragValue = 0.0f;
	bool bThermoDragging = false;

	FPanelsStats Stats;
};
