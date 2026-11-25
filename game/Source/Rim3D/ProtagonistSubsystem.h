#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/FightResponse.h"

#include "ProtagonistSubsystem.generated.h"

class AProtagonistPawn;
class APlayerController;
class FNavigationConfig;
class USimSubsystem;
namespace colony { class Sim; }

/**
 * Direct control: the player walks one colonist (the protagonist to start with) in first or third
 * person, and one key switches between that and the overseer view (docs/design/protagonist.md).
 *
 * The sim owns the colonist: this subsystem tells it who is controlled (ControlPawnCommand) and,
 * once per sim step, where the player's body is (DirectMoveCommand). While a colonist is controlled
 * the sim's own routines leave it alone; letting go (back to the overseer view) hands it back.
 * Game speed is held at 1x or paused while in direct mode, because the body moves in real time.
 *
 * Start in direct mode with -Direct (with -SimScenario=Crashlanded there is a protagonist to
 * control); the console has Protagonist.Enter and Protagonist.Exit. Keys: Tab switches between
 * the overseer view and direct mode (in the overseer view it takes over the selected colonist, or
 * the protagonist); in direct mode V toggles first and third person and T takes over the colonist
 * under the crosshair. Loading a saved game keeps the mode: a save made while controlling puts you
 * back in direct mode.
 */
UCLASS()
class RIM3D_API UProtagonistSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Takes control of a colonist and switches to the first or third person view. With no colonist
	 *  given: the one selected in the overseer view, else the protagonist, else any colonist. Returns
	 *  false (and says why in the log) if there is none or the player controller is missing. */
	bool EnterDirectMode(colony::Entity Pawn = colony::kNoEntity);
	/** Lets the colonist go (the sim's own routines take over again) and returns to the overseer
	 *  view, centred on it, with it selected. */
	void ExitDirectMode();
	/** O (SwitchMode): the overseer view becomes direct mode and the other way round. */
	void ToggleMode();
	/** In direct mode: control another colonist instead. Returns false if it isn't a colonist on the map. */
	bool TakeOver(colony::Entity Pawn);
	/** T: takes over the colonist under the crosshair. */
	bool TakeOverLookedAt();
	/** Asks to control this colonist at the start of the next frame (safe to call from a UI button). */
	void RequestControl(colony::Entity Pawn) { RequestedControl = Pawn; }

	bool IsDirectMode() const;
	/** The death screen is up: the protagonist died, the camera stays on the body in slow motion and grey, Space loads the
	 *  newest save (docs/design/protagonist.md "Death"). */
	bool IsDeathScreenUp() const { return bDead; }
	/** Real seconds since the protagonist died (0 when not dead). */
	float GetDeathSeconds() const;
	/** What Space does on the death screen: loads the newest save, or with none opens the title (main menu). Returns false
	 *  if there was a save but it would not load. */
	bool LeaveDeathScreen();
	/** The controlled colonist is sleeping the night away (SleepCommand, docs/design/needs.md). */
	bool IsAsleep() const;
	/** N: sleep where you stand (a bed is F while looking at it). Any of W, A, S, D, N or Esc wakes you. */
	void SleepHere();
	void WakeUp();
	colony::Entity GetControlled() const { return Controlled; }
	AProtagonistPawn* GetBody() const;

	/** The colonist marked as the protagonist, else the first colonist; kNoEntity if there are none. */
	colony::Entity FindProtagonist() const;
	/** The nearest colonist other than the controlled one that the view ray hits within the distance (cm). */
	colony::Entity GetLookedAtColonist(float MaxDistanceCm = 2500.0f) const;
	/** A colonist's name for the screen: nickname if it has one, else first name. */
	static FString ColonistLabel(const colony::Sim& Sim, colony::Entity Pawn);

	/** Frames in which the direct mode HUD was drawn: 0 means ImGui never became usable. */
	int32 GetHudFramesDrawn() const { return HudFramesDrawn; }
	/** How many times the local position had to be pulled back to the sim's. */
	int32 GetReconcileCount() const { return ReconcileCount; }
	/** DirectMove commands sent so far. */
	int64 GetMovesSent() const { return MovesSent; }
	/** Climbs of stairs and ladders asked of the sim (U17). */
	int64 GetClimbsSent() const { return ClimbsSent; }

	/** Lock-on focus (docs/design/focus.md): the pawn the controlled colonist is locked on to, kNoEntity for none. */
	colony::Entity GetFocusTarget() const;
	/** Tab: lock on to the next nearest enemy (FocusCommand Next); middle mouse: lock on or let go. Both do nothing with the setting off. */
	void FocusNext();
	void EndFocus();
	/** Tab and flicks may lock on to the player's own side too (the practice range's targets). */
	void SetFocusAnySide(bool bAny) { bFocusAnySide = bAny; }
	/** A flick: switch the lock to the nearest enemy to the right (or left) of the one held (the mouse flick, smoke tests). */
	void FlickFocus(bool bRight);
	/** Mouse flicks that switched the lock-on target (smoke tests). */
	int32 GetFlicksSent() const { return FlicksSent; }
	/** FocusCommands sent so far (smoke tests). */
	int64 GetFocusCommandsSent() const { return FocusCommandsSent; }
	/** Frames the focus HP bar was drawn. */
	int32 GetFocusBarFramesDrawn() const { return FocusBarFramesDrawn; }
	/** The world point the focus marker sits over (cm), false with no focus. */
	bool GetFocusPoint(FVector& OutPoint) const;
	/** F12 Unstuck (docs/design/earthworks.md "Stuck"): 5 seconds of standing still with no actions, then the sim puts the
	 *  controlled colonist on the nearest clear spot (UnstuckCommand). Seconds left, 0 when not waiting. */
	void StartUnstuck();
	float GetUnstuckSecondsLeft() const { return UnstuckSecondsLeft; }
	bool IsUnstucking() const { return UnstuckSecondsLeft > 0.0f; }

	/**
	 * The build screen (sneed 2026-11-25, docs/design/handson.md "The build screen"): B in direct mode brings up the
	 * overseer's panels (the bar, with the Build menu open) over the walking view, which keeps its camera. The mouse is
	 * freed and places plans under the cursor (click, or drag for walls); the body's keys are off, so nobody walks, turns
	 * or punches. B again, or Esc, goes back to walking. False if it can't open now (not walking, asleep, dead).
	 */
	bool OpenBuildUi();
	void CloseBuildUi();
	void ToggleBuildUi();
	bool IsBuildUiOpen() const { return bBuildUi && bDirect; }
	/** Frames the build screen was up (smoke tests). */
	int32 GetBuildUiFrames() const { return BuildUiFrames; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	float UnstuckSecondsLeft = 0.0f;
	int32 UnstuckSkipMoves = 0;
	void OnBeforeStep();
	void OnSimEvent(const colony::Event& Event);
	void DrawHud(USimSubsystem& Bridge);
	void UpdateFocus(APlayerController& PC, USimSubsystem& Bridge);
	void DrawFocus(USimSubsystem& Bridge, float Top, float Scale);
	void SubmitFocus(USimSubsystem& Bridge, colony::FocusMode Mode);
	void ApplyBodyVisibility();
	void BeginDeathScreen(USimSubsystem& Bridge);
	/** Puts time, colour and the camera back to normal and lets the body move again. */
	void EndDeathScreen();
	void TickDeathScreen();
	void DrawDeathScreen(float Seconds, bool bHasSave);
	/** The build screen's mouse and keys, each frame it is up: clicks to the order tools (or select), Esc and B close. */
	void UpdateBuildUi(APlayerController& PC);
	bool bBuildUi = false;
	int32 BuildUiFrames = 0;

	UPROPERTY()
	TObjectPtr<AProtagonistPawn> Body;

	colony::Entity Controlled = colony::kNoEntity;
	colony::Entity RequestedControl = colony::kNoEntity;
	bool bExitRequested = false;
	/** The controlled protagonist died in the sim (set from the event, handled at the start of the next frame). */
	bool bDeathRequested = false;
	bool bDead = false;
	/** FPlatformTime::Seconds() when the death screen began (real time: the world runs at a fifth of it). */
	double DeathStartedAt = 0.0;
	/** The newest save when the death screen began ("" for none): decides the prompt's words. */
	FString DeathSaveSlot;
	bool bDirect = false;
	int32 SpeedBeforeDirect = 1;
	const colony::Sim* LastSim = nullptr;
	FDelegateHandle BeforeStepHandle;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 HudFramesDrawn = 0;
	int32 ReconcileCount = 0;
	int64 MovesSent = 0;
	int64 ClimbsSent = 0;
	int64 FocusCommandsSent = 0;
	int32 FocusBarFramesDrawn = 0;
	/** The focus a Clear was last sent for while the setting is off (so it is sent once). */
	colony::Entity FocusClearedFor = colony::kNoEntity;
	/** Seconds the player has been turned away from the focus and moving away from it (lock-on.md). */
	float TurnAwaySeconds = 0.0f;
	void UpdateTurnAway(USimSubsystem& Bridge);
	/** A quick sideways mouse flick while locked on with melee switches target (lock-on.md). */
	void UpdateFlick(APlayerController& PC, USimSubsystem& Bridge, colony::Entity Focus);
	float FlickX = 0.0f;
	float FlickAge = 0.0f;
	float FlickCooldown = 0.0f;
	int32 FlicksSent = 0;
	bool bFocusAnySide = false;
	/** Real seconds the screen has been dark for the night skip (fades in and out). */
	float SleepFade = 0.0f;
	/** Unreal's menu navigation as it was before the game turned Tab and the arrows off (put back when it ends). */
	TSharedPtr<FNavigationConfig> PreviousNavigation;
};
