#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "ProtagonistSubsystem.generated.h"

class AProtagonistPawn;
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
	/** Tab: the overseer view becomes direct mode and the other way round. */
	void ToggleMode();
	/** In direct mode: control another colonist instead. Returns false if it isn't a colonist on the map. */
	bool TakeOver(colony::Entity Pawn);
	/** T: takes over the colonist under the crosshair. */
	bool TakeOverLookedAt();
	/** Asks to control this colonist at the start of the next frame (safe to call from a UI button). */
	void RequestControl(colony::Entity Pawn) { RequestedControl = Pawn; }

	bool IsDirectMode() const;
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

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void OnBeforeStep();
	void OnSimEvent(const colony::Event& Event);
	void DrawHud(USimSubsystem& Bridge);
	void ApplyBodyVisibility();

	UPROPERTY()
	TObjectPtr<AProtagonistPawn> Body;

	colony::Entity Controlled = colony::kNoEntity;
	colony::Entity RequestedControl = colony::kNoEntity;
	bool bExitRequested = false;
	bool bDirect = false;
	int32 SpeedBeforeDirect = 1;
	const colony::Sim* LastSim = nullptr;
	FDelegateHandle BeforeStepHandle;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 HudFramesDrawn = 0;
	int32 ReconcileCount = 0;
	int64 MovesSent = 0;
};
