#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "WorkToolSubsystem.generated.h"

class ASimPawnActor;
class UAnimSequence;
class UMaterialInterface;
class USimSubsystem;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * Tools in the hands for work (sneed 2026-11-20, docs/design/structure-damage.md "Felling trees"). Cosmetic only: a pawn
 * the snapshot shows `felling` (cutting a tree, at the trunk) holds a hand axe in its right fist and plays a chopping
 * clip on a loop, its own weapon holstered (UWeaponDrawSubsystem); when it stops, the axe goes and the clip blends out.
 * The axe is not an item: it is drawn from `visual.parts` of the Rules def WorkTools ("fellingAxe").
 *
 * Console: Work.Tools (0 off), Work.ChopClip (0 the two-handed mocap chop A_ChopAxe, 1 Mixamo's horizontal axe swing),
 * Work.ChopRate (clip speed), Work.AxeRoll / Work.AxePitch / Work.AxeYaw (degrees, the axe turned in the fist).
 */
UCLASS()
class RIM3D_API UWorkToolSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Pawns drawn with an axe now, and chop loops started (smoke tests). */
	int32 GetAxesShown() const { return AxesShown; }
	int32 GetChopsStarted() const { return ChopsStarted; }
	/** The axe component on a pawn's actor, if it is felling. */
	UStaticMeshComponent* GetAxe(const ASimPawnActor* Actor) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	UStaticMesh* AxeMesh();
	UAnimSequence* ChopClip();

	TWeakObjectPtr<USimSubsystem> BridgeSource;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Axe;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Solid;
	UPROPERTY()
	TObjectPtr<UAnimSequence> Clips[2];
	bool bClipTried[2] = {false, false};
	/** Per felling pawn actor: its axe. Weak, so a pawn that goes away takes its axe with it. */
	TMap<TWeakObjectPtr<ASimPawnActor>, TWeakObjectPtr<UStaticMeshComponent>> Axes;
	/** Pawns playing the chop loop last tick. */
	TSet<TWeakObjectPtr<ASimPawnActor>> Looped;
	int32 AxesShown = 0;
	int32 ChopsStarted = 0;
};
