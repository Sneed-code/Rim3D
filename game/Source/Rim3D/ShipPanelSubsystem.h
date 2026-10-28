#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Snapshot.h"

#include "ShipPanelSubsystem.generated.h"

class USimSubsystem;

/** What the ship window and the ending screen drew and did (smoke test `ship`). */
struct FShipPanelStats
{
	int32 WindowFrames = 0;
	int32 EndingFrames = 0;
	/** Casket rows drawn in the last frame the window was open. */
	int32 CasketRows = 0;
	/** The Launch button was drawn enabled in the last frame the window was open. */
	bool bLaunchEnabled = false;
	/** Commands the panel sent since the sim started. */
	int32 CommandsSent = 0;
};

/**
 * The spaceship ending on screen (docs/design/panels.md "Ship", sim side docs/design/ship.md), ImGui, all through
 * USimSubsystem:
 *
 * - A Ship button in the panels' menu row (UPanelsSubsystem draws it) once a ship part stands or the reactor runs; it
 *   shows the days the reactor still needs, and turns green when the ship can launch.
 * - The ship window: what the ship has against what a launch needs, the reactor (Start reactor, how far it has warmed
 *   up, the waves of raiders so far), every cryptosleep casket with its rider and a list to pick one (ShipCommand Board),
 *   and Launch, greyed with the sim's reason while it can't go.
 * - The ending screen when the ship has launched: who left, and Keep playing (the sim runs on, as in RimWorld).
 * - The panels' message log tells of the reactor, the waves and the launch (UPanelsSubsystem::OnSimEvent).
 *
 * The ship parts themselves are built from the build menu's Ship category (UOrdersSubsystem). It only draws while the
 * overseer view is the one in use. Console: Ship.UI 0|1, Ship.Open.
 */
UCLASS()
class RIM3D_API UShipPanelSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bInOpen);
	/** The ending screen is up (the ship launched and Keep playing was not pressed yet). */
	bool IsEndingShown() const { return bEndingShown; }
	void DismissEnding() { bEndingShown = false; }

	// ---- What a click does (the window calls these; the smoke test calls them too) --------------
	/** Sends ShipCommand StartReactor for the first finished reactor. False (nothing sent) if there is none or one runs. */
	bool StartReactor();
	/** Sends ShipCommand Board: `Pawn` rides in `Casket`, or kNoEntity empties it. False if `Casket` is not a casket. */
	bool Board(colony::Entity Casket, colony::Entity Pawn);
	/** Sends ShipCommand Launch. False (nothing sent) while the snapshot says the ship can't launch. */
	bool Launch();

	/** The Ship button is shown: a ship part stands, or the reactor runs or ran. */
	bool HasShip() const;
	/** Finished ship cryptosleep caskets / reactors on the map, from the snapshot, in id order. */
	void GetCaskets(TArray<colony::Entity>& Out) const;
	void GetReactors(TArray<colony::Entity>& Out) const;
	/** Who rides in the casket (kNoEntity for no one). */
	colony::Entity RiderOf(colony::Entity Casket) const;
	/** The names of those who left on the ship, as heard when it launched. */
	const TArray<FString>& GetEscapedNames() const { return EscapedNames; }

	const FShipPanelStats& GetStats() const { return Stats; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	bool IsOverseerActive() const;
	void Submit(colony::Command Command);
	void OnSimEvent(const colony::Event& Event);
	void SyncWithSim();
	void ResetForNewSim();
	void GetParts(const char* Part, TArray<colony::Entity>& Out) const;
	FString NameOf(colony::Entity Pawn) const;

	void DrawWindow(USimSubsystem& Bridge);
	void DrawEnding(USimSubsystem& Bridge);

	bool bOpen = false;
	bool bEndingShown = false;
	TArray<FString> EscapedNames;
	/** The riders' names as the last tick saw them, so they are known when the launch takes them off the map. */
	TArray<FString> LastRiderNames;
	int32 ContentGeneration = -1;

	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> EventSource;

	FShipPanelStats Stats;
};
