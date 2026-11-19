#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"

#include "PowerOverlaySubsystem.generated.h"

class USimSubsystem;
struct ImDrawList;

/**
 * The power and temperature views of the overseer screen (sneed, 2026-11-19: "power generation, electrical grids,
 * refrigeration, coolers"; docs/design/panels.md "Power and temperature views"; stage 2 task U19's overlays).
 *
 * - Two toggle buttons at the bottom right, "Power" and "Temperature" (console Overlay.Power, Overlay.Temperature).
 * - **Power view:** a dot over every building with a power part, the colour of its grid, ringed green (has power),
 *   red (on but no power: not wired, or the grid is short) or grey (switched off); generators show the watts they make,
 *   batteries a charge bar. A list of the grids at the right: watts made and used, battery charge, short in red.
 * - **Temperature view:** each enclosed room's temperature over its middle, coloured from blue (freezing) through
 *   green (comfortable) to red, the outdoor temperature in the corner, and on each cooler a blue dot on the side it
 *   cools and a red one on the side it heats.
 * - **The selected building's power** (with or without the view on): on, off or no power, its watts, its grid's
 *   numbers, a battery's charge, and a Switch on / Switch off button (FlickCommand) for anything that can be flicked.
 *
 * Everything is read from the snapshot (ThingView power, powerNet, powerWatts, batteryFill; Snapshot::powerNets,
 * rooms and outdoorTemperature) and the sim's const API; the only command sent is FlickCommand.
 */
UCLASS()
class RIM3D_API UPowerOverlaySubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	bool IsPowerViewOn() const;
	bool IsTemperatureViewOn() const;
	void SetPowerView(bool bOn);
	void SetTemperatureView(bool bOn);

	/** For the smoke tests: frames each part was drawn, and how many markers the last frame drew. */
	struct FStats
	{
		int32 PowerFrames = 0;
		int32 TemperatureFrames = 0;
		int32 SelectedPanelFrames = 0;
		int32 PowerMarkers = 0;
		int32 RoomLabels = 0;
		int32 FlicksSent = 0;
	};
	const FStats& GetStats() const { return Stats; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	bool IsOverseerActive() const;
	void DrawToggles(float Scale);
	void DrawPowerView(USimSubsystem& Bridge, float Scale);
	void DrawGridList(USimSubsystem& Bridge, float Scale, float& InOutTop);
	void DrawTemperatureView(USimSubsystem& Bridge, float Scale, float& InOutTop);
	void DrawSelectedPower(USimSubsystem& Bridge, float Scale, float Top);

	FStats Stats;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
};
