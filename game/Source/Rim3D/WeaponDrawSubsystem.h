#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "MeleePose.h"

#include "WeaponDrawSubsystem.generated.h"

class ASimPawnActor;
class UMaterialInterface;
class USimSubsystem;
class UStaticMesh;
class UStaticMeshComponent;
namespace colony { struct ThingView; }

/** Where a weapon is drawn: the three slots of P1 and one more for a weapon in the hands that no slot holds. */
inline constexpr int32 GWeaponDisplaySlots = 4;

/**
 * Weapons and melee on screen (task U22, docs/design/melee-draw.md). Everything here is cosmetic: it draws
 * what the sim's MeleeSwing, MeleeHit and PawnAction events and the snapshot say, and the player's keys turn
 * into the same commands any pawn could be given.
 *
 * - Every weapon a pawn carries (`ThingView::carried`) is drawn: the one in the hands, the others holstered
 *   on the body (rifle on the back, sidearm on the right hip, melee weapon on the left hip).
 * - A weapon switch (`switchSlot`, `switchStepsLeft`) is shown as it happens: the weapon in the hands goes to
 *   its holster, then the new one comes up, over the steps the sim takes.
 * - A melee swing is a procedural pose track (MeleePose.h) timed to the sim: the wind-up draws the weapon
 *   back, the blow lands on the step of the MeleeHit event, then follow-through and recovery. Blocks hold the
 *   weapon across the chest while the snapshot says `blocking`; a sidestep dips and tips the body.
 * - A hit freezes both pawns for a moment (hit-stop), makes blood or sparks and a flinch (UFightSubsystem).
 * - The player's keys in direct mode: the left mouse button swings a melee weapon (or fists) at the pawn in
 *   front or straight ahead, the right mouse button holds the weapon up to block, Q brings up the next
 *   carried weapon.
 *
 * Console: Weapons.Draw (0 draws no weapons).
 */
UCLASS()
class RIM3D_API UWeaponDrawSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// --- The player's keys (direct mode). The keys call these; tests call them directly. ---

	/** Swings the weapon in the hands (or fists) at the pawn in front of the colonist, else straight ahead.
	 *  False (and nothing sent) if not in direct mode, the game is paused, an order went out this step, or the
	 *  pawn is not ready to swing. */
	bool Swing();
	/** Holds the melee weapon up to block. False unless the colonist holds a melee weapon and is ready. */
	bool Block();
	/** Q: starts bringing up the next carried weapon. False if there is nothing else to take. */
	bool SwitchNext();
	/** Starts switching to the weapon in this slot (0 rifle, 1 sidearm, 2 melee). */
	bool SwitchTo(int32 Slot);
	/** The pawn the swing would go at now: the nearest enemy in reach in front of the colonist, else none. */
	colony::Entity FindSwingTarget() const;

	// --- What is on screen ---

	/** The mesh drawn for the weapon in this display slot (0 rifle, 1 sidearm, 2 melee, 3 other), if any. */
	UStaticMeshComponent* GetSlotComponent(colony::Entity Pawn, int32 Slot) const;
	/** The component of the weapon in a pawn's hands (what the sim calls `weapon`), if it has one. */
	UStaticMeshComponent* GetHandsComponent(colony::Entity Pawn) const;
	/** Weapons drawn in the hands of pawns now. */
	int32 CountWeaponsInHands() const;
	/** Weapons drawn on one pawn, in the hands and on the body. */
	int32 CountVisibleWeapons(colony::Entity Pawn) const;
	/** "Q switch:  [rifle]  revolver  club": what the pawn carries, the one in the hands in brackets. */
	FString DescribeCarried(const colony::ThingView& View) const;
	/** Whether the weapon def shoots (has a gun verb); melee weapons and nothing do not. */
	bool IsGun(colony::DefId Weapon) const;

	// --- Counted for the smoke test ---

	int32 GetSwingOrders() const { return SwingOrders; }
	int32 GetBlockOrders() const { return BlockOrders; }
	int32 GetSwitchOrders() const { return SwitchOrders; }
	int32 GetSwingsSeen() const { return SwingsSeen; }
	int32 GetHitsSeen() const { return MeleeHits; }
	int32 GetDeflectedSeen() const { return MeleeDeflected; }
	int32 GetBlockedSeen() const { return MeleeBlocked; }
	int32 GetDodgedSeen() const { return MeleeDodged; }
	int32 GetMissedSeen() const { return MeleeMissed; }
	int32 GetDodgeActions() const { return DodgeActions; }
	int32 GetBlockActions() const { return BlockActions; }
	int32 GetSwitchesStarted() const { return SwitchesStarted; }
	int32 GetSwitchesDone() const { return SwitchesDone; }
	int32 GetHitStopsPlayed() const { return HitStopsPlayed; }
	/** The sim step the last MeleeHit arrived in. */
	int64 GetLastHitStep() const { return LastHitStep; }
	/** Hit moments that arrived with the blow on screen within 2.5 steps of landing, and those with the picture further
	 *  behind (it is then snapped to the blow), and the most steps it was behind. */
	int32 GetHitsOnTime() const { return HitsOnTime; }
	int32 GetHitsLate() const { return HitsLate; }
	float GetMaxStepsBehind() const { return MaxStepsBehind; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FDisplayed
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		colony::DefId Def;
	};

	struct FPawnWeapons
	{
		FDisplayed Slots[GWeaponDisplaySlots];
		/** Steps the switch in progress takes (from the Switch action), to turn steps left into progress. */
		int32 SwitchTotal = 0;
	};

	void OnSimEvent(const colony::Event& Event);
	void OnMeleeSwing(const colony::Event& Event);
	void OnMeleeHit(const colony::Event& Event);
	void OnPawnAction(const colony::Event& Event);
	void UpdatePawn(ASimPawnActor& Actor, const colony::ThingView& View);
	void UpdatePlayerInput();
	UStaticMesh* WeaponMesh(colony::DefId Weapon);
	UStaticMeshComponent* MakeComponent(ASimPawnActor& Actor, UStaticMesh* Mesh);
	void HoldPose(int32 Slot, float Height, FVector& OutLocation, FRotator& OutRotation) const;
	void HolsterPose(int32 Slot, float Height, FVector& OutLocation, FRotator& OutRotation) const;
	bool DrawOn() const;
	/** The controlled colonist's view in direct mode, with the bridge; null if the player is not in direct mode. */
	const colony::ThingView* ControlledView(USimSubsystem*& OutBridge) const;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;

	UPROPERTY()
	TMap<uint32, TObjectPtr<UStaticMesh>> Meshes;

	TMap<uint32, FPawnWeapons> Weapons;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 LastGeneration = -1;
	int64 LastOrderStep = -1;

	int32 SwingOrders = 0;
	int32 BlockOrders = 0;
	int32 SwitchOrders = 0;
	int32 SwingsSeen = 0;
	int32 MeleeHits = 0;
	int32 MeleeDeflected = 0;
	int32 MeleeBlocked = 0;
	int32 MeleeDodged = 0;
	int32 MeleeMissed = 0;
	int32 DodgeActions = 0;
	int32 BlockActions = 0;
	int32 SwitchesStarted = 0;
	int32 SwitchesDone = 0;
	int32 HitStopsPlayed = 0;
	int64 LastHitStep = -1;
	int32 HitsOnTime = 0;
	int32 HitsLate = 0;
	float MaxStepsBehind = 0.0f;
};
