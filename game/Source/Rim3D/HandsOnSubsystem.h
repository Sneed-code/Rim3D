#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Jobs.h"

#include "HandsOnSubsystem.generated.h"

class USimSubsystem;
namespace colony { struct Event; }

/** What the crosshair is on: a thing (an item, a blueprint or a frame), or a cell with a rock or a plant, else bare ground. */
struct FHandsOnTarget
{
	colony::Entity Thing = colony::kNoEntity;
	colony::CellPos Cell;
	/** The cell is bare ground (or nothing was hit): no job, only somewhere to put something down. */
	bool bGround = true;
	bool bValid = false;
};

/** What the hands-on layer did (smoke test `handson`). */
struct FHandsOnStats
{
	int32 FramesDrawn = 0;
	/** HandsOn commands sent: work starts, stops and put downs. */
	int32 CommandsSent = 0;
	int32 PromptFrames = 0;
	int32 ProgressFrames = 0;
	int32 GhostPlans = 0;
};

/**
 * Hands-on work (task U10, docs/design/handson.md): in direct mode the player looks at a thing and
 * holds F to do the sim's own job on it, in place: mine a rock, chop a tree, pick berries, pick up
 * an item, put materials into a plan, build a frame. Everything is a HandsOnCommand; the sim
 * decides what the job is (JobSystem::planHandsOn, which this subsystem asks for the prompt) and
 * does the work at the colonist's own speed and skill.
 *
 * - F (hold): start the job on what the crosshair is on; let go: stop it. Picking up is instant.
 * - X: put down what the colonist carries, on the cell under the crosshair if it is near.
 * - B: the build ghost. The architect menu's Build tool in hand, aimed by the crosshair (snapped to the
 *   grid, green where it can stand, red where it can't): the wheel picks the building, Q the stuff,
 *   R turns it, the left button plans it, B or the right button puts it away. While it is in hand the gun is quiet.
 *
 * Draws the prompt under the crosshair, a progress bar for the job, what is carried and the keys.
 */
UCLASS()
class RIM3D_API UHandsOnSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** What the view ray from the controlled colonist's camera is on. */
	FHandsOnTarget FindTarget(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction, const FVector& Head) const;
	/** What the colonist would do with its hands on the target now (the sim's answer). */
	colony::HandsOnPlan PlanFor(const USimSubsystem& Bridge, const FHandsOnTarget& Target) const;

	/** The target and plan of the last frame in direct mode. */
	const FHandsOnTarget& GetTarget() const { return Target; }
	const colony::HandsOnPlan& GetPlan() const { return Plan; }
	/** The ground cell under the crosshair (for the ghost and for putting down). */
	bool GetAimCell(colony::CellPos& OutCell) const { OutCell = AimCell; return bHasAim; }

	/** F pressed or released, as the key does (the smoke test calls these; with SetScripted the key is ignored). */
	void SetInteractHeld(bool bHeld);
	void SetScripted(bool bScriptedInput) { bScripted = bScriptedInput; if (!bScripted) { bScriptedHeld = false; } }
	/** X: put down what is carried. */
	void PutDown();
	/** The wheel in direct mode: with a building in hand it picks the next one and returns true. */
	bool HandleWheel(float Notches);
	/** B: take the build ghost in hand, or put it away. */
	void ToggleBuilding();
	/** The left button with a building in hand. True if a plan was made. */
	bool PlaceBuilding();

	bool IsInteracting() const { return bInteracting; }
	const FString& GetMessage() const { return Message; }
	const FHandsOnStats& GetStats() const { return Stats; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void OnSimEvent(const colony::Event& Event);
	void Say(const FString& Text);
	void Interact(USimSubsystem& Bridge, bool bDown);
	void UpdateBuilding(USimSubsystem& Bridge, class APlayerController* PC);
	void DrawHud(USimSubsystem& Bridge);

	FHandsOnTarget Target;
	colony::HandsOnPlan Plan;
	colony::CellPos AimCell;
	bool bHasAim = false;

	bool bScripted = false;
	bool bScriptedHeld = false;
	bool bWasDown = false;
	bool bInteracting = false;
	uint64 LastCommand = 0;

	FString Message;
	double MessageUntil = 0.0;
	FHandsOnStats Stats;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	const void* LastSim = nullptr;
};
