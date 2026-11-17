#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "GrimeSubsystem.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMeshComponent;
class USimSubsystem;

/**
 * Dirt and blood on bodies, clothes and melee weapons (docs/design/grime.md), all cosmetic: the sim's grime levels
 * (ThingView dirtLegs, dirtBody, blood, weaponBlood) drawn as one overlay material over every mesh of a pawn, so skin
 * and clothes get it together without touching their own materials. M_GrimeOverlay (made by -run=BloodArt) lays
 * patchy dirt from the feet up (legs) and all over (body), and patchy blood in the body's fluid colour, wet and glossy
 * when fresh and dark and matte once dry (Blood.DryMinutes). The held weapon gets blood only.
 *
 * Cheap: one dynamic material per pawn shown, updated four times a second; pawns further than Grime.MaxDistance from
 * the camera, or clean, have no overlay at all. Without the material nothing is drawn.
 *
 * Console: Grime.Enabled, Grime.MaxDistance (metres), Grime.Strength.
 */
UCLASS()
class RIM3D_API UGrimeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Pawns and weapons drawing grime now, for smoke tests. */
	int32 GetGrimyPawns() const { return GrimyPawns; }
	int32 GetBloodyWeapons() const { return BloodyWeapons; }
	bool IsMaterialMissing() const { return bMaterialTried && !Overlay; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FPawnGrime
	{
		TWeakObjectPtr<UMaterialInstanceDynamic> Material;
		TWeakObjectPtr<UMaterialInstanceDynamic> WeaponMaterial;
		TArray<TWeakObjectPtr<UMeshComponent>> Meshes;
		TWeakObjectPtr<UMeshComponent> Weapon;
		float LocalMinZ = 0.0f;
		float LocalHeight = 180.0f;
		bool bSeen = false;
	};

	bool EnsureMaterial();
	void ClearPawn(FPawnGrime& Pawn);

	TWeakObjectPtr<USimSubsystem> BridgeSource;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Overlay;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;  // keeps the dynamic materials alive
	bool bMaterialTried = false;
	TMap<uint32, FPawnGrime> Pawns;
	float Accumulated = 0.0f;
	int32 GrimyPawns = 0;
	int32 BloodyWeapons = 0;
};
