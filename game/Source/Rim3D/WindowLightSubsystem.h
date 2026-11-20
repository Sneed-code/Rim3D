#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "WindowLightSubsystem.generated.h"

class AActor;
class URectLightComponent;
class USimSubsystem;

/**
 * Daylight through windows (docs/design/windows.md "Drawing"): the sim lets the sky's light into a roofed room through each
 * window (Lights::windowShareAt), and this draws it. The nearest `Windows.MaxLights` windows with open sky on one side and a
 * roofed room on the other get a rect light the size of the opening, just inside the wall and facing into the room, as bright
 * as a patch of sky (`Windows.LightCd` candelas times the sim's sky glow now, so it follows the day and the weather). The
 * nearest `Windows.ShadowLights` cast shadows. Cosmetic only.
 *
 * Console: Windows.Lights (0 off), Windows.MaxLights, Windows.ShadowLights, Windows.LightCd.
 */
UCLASS()
class RIM3D_API UWindowLightSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** How many window lights are shining now (smoke tests). */
	int32 GetShining() const { return Shining; }

private:
	void Teardown();

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TArray<TObjectPtr<URectLightComponent>> Lights;

	TWeakObjectPtr<USimSubsystem> Bridge;
	int32 Shining = 0;
};
