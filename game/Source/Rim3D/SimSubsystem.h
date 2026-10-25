#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <memory>

#include "SimSubsystem.generated.h"

class FSimInputProcessor;

/** Called once per sim event, right after the step that produced it. */
DECLARE_MULTICAST_DELEGATE_OneParam(FSimEventSignature, const colony::Event&);

/** Called before every sim step, right before the commands queued so far are applied by it. */
DECLARE_MULTICAST_DELEGATE(FSimBeforeStepSignature);

/** How a sim is started: which data folder, which seed, and the Stage 0 hauling test's size. */
struct FSimStartOptions
{
	/** Folder holding defs/ and tuning.json. Empty: <repo>/data, or -SimData=<folder>. */
	FString DataRoot;
	uint64 Seed = 42;
	int32 MapSize = 256;
	int32 Pawns = 0;
	int32 Items = 0;
	bool bFlatMap = false;
	/** A Scenarios def to start, e.g. "Crashlanded": the protagonist and colonists near the middle
	 *  of the map. Empty: none. Command line: -SimScenario=Crashlanded. */
	FString Scenario;
};

/** Sim cost, averaged over about a second of real time. */
struct FSimTimings
{
	double StepsPerFrame = 0.0;
	/** Time the sim took per frame (steps, snapshots, events): the number the 5 ms budget is about. */
	double AvgFrameMs = 0.0;
	double MaxFrameMs = 0.0;
	/** One sim step on its own. */
	double AvgStepMs = 0.0;
	double MaxStepMs = 0.0;
	/** Writing the snapshots, per frame. */
	double AvgSnapshotMs = 0.0;
};

/**
 * The bridge between Unreal and the colony sim (docs/design/bridge.md). Owns the sim for a game
 * world and steps it one game second at a time, 30 steps per real second at 1x. Unreal changes the
 * sim only through Submit (commands), hears what happened through OnSimEvent, and reads state from
 * the two newest snapshots, blending them by how far the frame is into the next step.
 * This is the only place that converts sim units (metres, Z up from the ground) to Unreal's.
 *
 * Keys (also work in the smoke tests): Space pauses and resumes, 1 / 2 / 3 set the speed,
 * . (period) steps once while paused, F7 saves a replay of the session to Saved/Replays/,
 * F5 quicksaves and F9 quickloads (Saved/Saves/, docs/design/saves.md), F8 writes a bug report
 * (a replay, a save and a readable dump in one folder under Saved/BugReports/). The game also
 * autosaves once a game day, rotating through three files.
 * Console: Sim.Speed, Sim.Spawn, Sim.SaveReplay, Sim.Save, Sim.Load, Sim.BugReport,
 * Sim.ShowTimings, Sim.DebugDraw.
 */
UCLASS()
class RIM3D_API USimSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Starts a new sim, replacing the running one. Returns false if the data folder has broken defs
	 *  (the sim then starts with no defs, and the problems are in the log). */
	bool StartSim(const FSimStartOptions& Options);

	/** 0 pauses; 1, 2 and 3 are the normal speeds. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	void SetSpeed(int32 NewSpeed);

	UFUNCTION(BlueprintPure, Category = "Colony")
	int32 GetSpeed() const { return Speed; }

	/** The fastest speed the keys and buttons may set (3 normally; direct control holds it at 1,
	 *  because the player's body moves in real time). Lowering it lowers the current speed too. */
	void SetMaxSpeed(int32 NewMax);
	int32 GetMaxSpeed() const { return MaxSpeed; }

	/** Pauses, or resumes at the speed it had before. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	void TogglePause();

	/** Runs exactly one step now (useful while paused). */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	void StepOnce();

	UFUNCTION(BlueprintPure, Category = "Colony")
	int64 GetGameSeconds() const;

	/** Writes the session so far as a replay file in Saved/Replays/ and returns its path, or an
	 *  empty string if it could not be written. `simrun --replay FILE` plays it in the cloud. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	FString SaveReplay();

	/** Writes the whole game to Saved/Saves/<Slot>.save (a half-written file never replaces a good
	 *  one). Returns the path, or an empty string if it could not be written. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	FString SaveGame(const FString& Slot);

	/** Replaces the running sim with the one in Saved/Saves/<Slot>.save. On any problem the running
	 *  game is left alone, the reason goes to the log and the screen, and this returns false. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	bool LoadGame(const FString& Slot);

	UFUNCTION(BlueprintCallable, Category = "Colony")
	FString QuickSave() { return SaveGame(TEXT("quicksave")); }

	UFUNCTION(BlueprintCallable, Category = "Colony")
	bool QuickLoad() { return LoadGame(TEXT("quicksave")); }

	/** Writes a folder in Saved/BugReports/ with the session as a replay (`simrun --replay`), a save
	 *  (`simrun --load`), the save as readable JSON and a note about the build. Returns the folder,
	 *  or an empty string on failure. */
	UFUNCTION(BlueprintCallable, Category = "Colony")
	FString WriteBugReport();

	/** Folder the saves go in. */
	static FString GetSaveDirectory();

	/** Game seconds between autosaves; a day unless a test shortens it. 0 turns autosave off. */
	void SetAutosaveInterval(int64 GameSeconds);
	int64 GetAutosaveInterval() const { return AutosaveInterval; }
	/** Number of autosaves written since the sim started or loaded. */
	int32 GetAutosaveCount() const { return AutosaveCount; }

	/** Handles one of the keys above. Returns true if it was one. */
	bool HandleKey(const FKey& Key);

	/** Queues a command for the start of the next step. Returns its id (a CommandFailed event names it). */
	uint64 Submit(colony::Command Command);

	/** Sim events, one call each, straight after the step. Failed commands also show on screen. */
	FSimEventSignature OnSimEvent;

	/** Called before each sim step, so a listener can Submit a command that step will apply
	 *  (the protagonist sends its position this way, once per step). */
	FSimBeforeStepSignature OnBeforeStep;

	const colony::Sim* GetSim() const { return Sim.get(); }
	std::shared_ptr<const colony::DefDatabase> GetDefs() const { return Defs; }
	/** Number of defs loaded from the data folder; 0 when it was missing or broken. */
	int32 GetDefCount() const { return Defs ? static_cast<int32>(Defs->size()) : 0; }
	const FString& GetDataRoot() const { return DataRoot; }
	const FSimStartOptions& GetStartOptions() const { return StartOptions; }

	/** The state after the newest step, and the one before it. Blend positions between them. */
	const colony::Snapshot& GetCurrentSnapshot() const { return Current; }
	const colony::Snapshot& GetPreviousSnapshot() const { return Previous; }
	/** How far the frame is into the next step, 0 to 1: 0 shows the previous snapshot, 1 the current. */
	float GetBlendAlpha() const { return BlendAlpha; }

	/** Where a thing is now, blended between the last two snapshots, in Unreal world units (cm).
	 *  False if it is not on the map. */
	bool GetBlendedLocation(colony::Entity Thing, FVector& OutLocation) const;

	/** Sim metres on a level (x, y) to Unreal centimetres, with Z up from the ground. */
	FVector SimToWorld(float X, float Y, int32 Level) const;

	/** Where sim cell (0, 0) sits in the world. Smoke tests move it away from the sample level. */
	void SetWorldOrigin(const FVector& NewOrigin) { WorldOrigin = NewOrigin; }
	FVector GetWorldOrigin() const { return WorldOrigin; }

	const FSimTimings& GetTimings() const { return Timings; }
	int64 GetEventCount() const { return EventCount; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void RunSteps(int64 Steps);
	void MaybeAutosave();
	void ResetAfterSimChange();
	void HandleEvents();
	void AdvanceTimingWindow(double DeltaTime);
	void ShowTimings() const;
	void DrawDebug() const;

	std::unique_ptr<colony::Sim> Sim;
	std::shared_ptr<const colony::DefDatabase> Defs;
	FSimStartOptions StartOptions;
	FString DataRoot;
	TSharedPtr<FSimInputProcessor> InputProcessor;

	colony::Snapshot Previous;
	colony::Snapshot Current;
	float BlendAlpha = 0.0f;

	double StepAccumulator = 0.0;
	int32 Speed = 1;
	int32 SpeedBeforePause = 1;
	int32 MaxSpeed = 3;
	int64 LastLoggedHour = -1;
	int64 AutosaveInterval = colony::kSecondsPerDay;
	int64 LastAutosaveBucket = 0;
	int32 AutosaveCount = 0;
	int64 EventCount = 0;
	FVector WorldOrigin = FVector::ZeroVector;

	// Timing window (about one second of real time).
	FSimTimings Timings;
	double WindowSeconds = 0.0;
	int64 WindowFrames = 0;
	int64 WindowSteps = 0;
	double WindowFrameMs = 0.0;
	double WindowMaxFrameMs = 0.0;
	double WindowSnapshotMs = 0.0;
	double WindowMaxStepMs = 0.0;
	double WindowStepMs = 0.0;
};
