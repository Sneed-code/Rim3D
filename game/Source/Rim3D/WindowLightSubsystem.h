#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "WindowLightSubsystem.generated.h"

class AActor;
class URectLightComponent;
class ULocalFogVolumeComponent;
class USimSubsystem;

/**
 * Daylight through windows (docs/design/windows.md "Drawing"): the sim lets the sky's light into a roofed room through each
 * window (Lights::windowShareAt), and this draws it. The nearest `Windows.MaxLights` windows with open sky on one side and a
 * roofed room on the other get a rect light the size of the opening, just inside the wall and facing into the room, as bright
 * as a patch of sky (`Windows.LightCd` candelas times the sim's sky glow now, so it follows the day and the weather). The
 * nearest `Windows.ShadowLights` cast shadows. And each closed, roofed room near the camera with a window gets a
 * little dust in its air while the camera is indoors (a local fog volume filling the room), so the volumetric fog shows the
 * sun through a window as a soft shaft while the outdoors stays clear. Cosmetic only.
 *
 * Console: Windows.Lights (0 off), Windows.MaxLights, Windows.ShadowLights, Windows.LightCd, Windows.Dust (extinction per metre,
 * 0 off), Windows.DustRooms.
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
	/** How many rooms have dust in their air now (smoke tests). */
	int32 GetDustRooms() const { return DustRooms; }
	/** Where the dust volumes are and what they hold (smoke tests). */
	FString DescribeDust() const;

private:
	void Teardown();

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TArray<TObjectPtr<URectLightComponent>> Lights;

	UPROPERTY()
	TArray<TObjectPtr<ULocalFogVolumeComponent>> DustVolumes;
	/** The dust each volume was last given, so it is set only when it changes. */
	TArray<float> DustSet;

	TWeakObjectPtr<USimSubsystem> Bridge;
	int32 Shining = 0;
	int32 DustRooms = 0;
};
