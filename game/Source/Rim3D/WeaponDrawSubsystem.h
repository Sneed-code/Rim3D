#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "MeleePose.h"

#include "WeaponDrawSubsystem.generated.h"

class ASimPawnActor;
struct FGunRangeGun;
class UMaterialInterface;
class USimSubsystem;
class UStaticMesh;
class UStaticMeshComponent;
namespace colony { struct ThingView; struct GearSize; }

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
 *   front or straight ahead, the right mouse button holds the weapon up to block (or, with a shield on the arm,
 *   holds the shield up facing the camera), Q brings up the next carried weapon.
 * - A worn shield (docs/design/shields.md) hangs on the left arm, comes up in front while `shieldUp`, rides on
 *   the back while `shieldSlung`; a hit it takes (ShieldBlocked) throws sparks off it.
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
	/** Right mouse with a shield on the arm: raises the shield (facing where the camera looks) or lowers it. Sends a
	 *  ShieldCommand when the wish changes, and again when the camera turns more than 15 degrees. False if nothing went out. */
	bool HoldShield(bool bUp);
	/** Tests: hold the right mouse button from code (`bScripted` false gives it back to the mouse). */
	void ScriptRightMouse(bool bScripted, bool bHeld) { bScriptedRight = bScripted; bScriptRightHeld = bHeld; }
	/** Whether this pawn's view has a shield on its arm (worn, not slung behind a two-handed weapon). */
	static bool HasShieldOnArm(const colony::ThingView& View);
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
	/** The mesh drawn for the pawn's shield (on the arm, raised or slung), if it wears one. */
	UStaticMeshComponent* GetShieldComponent(colony::Entity Pawn) const;
	/** 0 shield lowered to 1 raised, as drawn now. */
	float GetShieldRaise(colony::Entity Pawn) const;
	/** Weapons drawn in the hands of pawns now. */
	int32 CountWeaponsInHands() const;
	/** Weapons drawn on one pawn, in the hands and on the body. */
	int32 CountVisibleWeapons(colony::Entity Pawn) const;
	/** "Q switch:  [rifle]  revolver  club": what the pawn carries, the one in the hands in brackets. */
	FString DescribeCarried(const colony::ThingView& View) const;
	/** Whether the weapon def shoots (has a gun verb); melee weapons and nothing do not. */
	bool IsGun(colony::DefId Weapon) const;
	/** The mesh of a weapon def's own shapes (`visual.parts`, grip at the origin), and of a shield in a stuff; made once. */
	UStaticMesh* GetWeaponMesh(colony::DefId Weapon);
	UStaticMesh* GetShieldMesh(colony::DefId Shield, colony::DefId Stuff);
	/** 0 holstered to 1 drawn: how ready the pawn holds its weapon now (docs/design/weapon-hold.md). */
	float GetReady(colony::Entity Pawn) const;

	// --- Counted for the smoke test ---

	int32 GetSwingOrders() const { return SwingOrders; }
	int32 GetBlockOrders() const { return BlockOrders; }
	int32 GetSwitchOrders() const { return SwitchOrders; }
	int32 GetShieldOrders() const { return ShieldOrders; }
	int32 GetShieldBlocksSeen() const { return ShieldBlocksSeen; }
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
	/** Blows that landed while the attacker's body showed no swing to time them by (pushed into a stagger mid-swing). */
	int32 GetHitsUntimed() const { return HitsUntimed; }
	float GetMaxStepsBehind() const { return MaxStepsBehind; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FDisplayed
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		colony::DefId Def;
		/** A real gun model (assets/guns.json): its body is Component, its other parts ride on it. */
		bool bArt = false;
		TArray<TWeakObjectPtr<UStaticMeshComponent>> Parts;
	};

	struct FPawnWeapons
	{
		FDisplayed Slots[GWeaponDisplaySlots];
		/** Steps the switch in progress takes (from the Switch action), to turn steps left into progress. */
		int32 SwitchTotal = 0;
		/** The shield: its component, the def and stuff it was made for, and how far it is raised and slung (0 to 1). */
		TWeakObjectPtr<UStaticMeshComponent> Shield;
		uint64 ShieldKey = 0;
		float ShieldRaise = 0.0f;
		float ShieldSlung = 0.0f;
		/** 0 holstered to 1 drawn (ready), the other hand's grip on a two-handed weapon (0 to 1), and whether this
		 *  drawing set that grip (so it lets it go once, and leaves other users of the hand IK alone). */
		float Ready = 0.0f;
		float OffHand = 0.0f;
		bool bOffHandSet = false;
		/** The pelvis in the actor's frame when no clip plays, how far the holsters follow it through a clip (0 to 1),
		 *  and the frame they ride in this frame (the slung shield uses it too). */
		FTransform PelvisRest = FTransform::Identity;
		bool bPelvisRest = false;
		float PelvisFollow = 0.0f;
		FTransform BodyFrame = FTransform::Identity;
	};

	void OnSimEvent(const colony::Event& Event);
	void OnMeleeSwing(const colony::Event& Event);
	void OnMeleeHit(const colony::Event& Event);
	void OnPawnAction(const colony::Event& Event);
	void OnShieldBlocked(const colony::Event& Event);
	void UpdateShield(ASimPawnActor& Actor, const colony::ThingView& View, FPawnWeapons& Pawn, bool bShow, float Height,
					  const colony::GearSize& Gear);
	UStaticMesh* ShieldMesh(colony::DefId Shield, colony::DefId Stuff);
	bool LoadSolidMaterial();
	void UpdatePawn(ASimPawnActor& Actor, const colony::ThingView& View);
	void UpdatePlayerInput();
	UStaticMesh* WeaponMesh(colony::DefId Weapon);
	void DestroyDisplayed(FDisplayed& Shown);
	bool MakeDisplayed(ASimPawnActor& Actor, FDisplayed& Shown, colony::DefId Def, const FGunRangeGun* Art);
	void UpdateStance(ASimPawnActor& Actor, const colony::ThingView& View, FPawnWeapons& Pawn, bool bShow, bool bControlled);
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

	UPROPERTY()
	TMap<uint64, TObjectPtr<UStaticMesh>> ShieldMeshes;

	TMap<uint32, FPawnWeapons> Weapons;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 LastGeneration = -1;
	int64 LastOrderStep = -1;
	float FrameDelta = 0.0f;
	/** The right-mouse shield: whether the player wants it up, for which pawn, the way it was last sent and when (real seconds). */
	bool bShieldWanted = false;
	bool bScriptedRight = false;
	bool bScriptRightHeld = false;
	colony::Entity ShieldPawn = colony::kNoEntity;
	FVector2D ShieldDirSent = FVector2D::ZeroVector;
	double ShieldSentAt = -1.0;

	int32 SwingOrders = 0;
	int32 BlockOrders = 0;
	int32 SwitchOrders = 0;
	int32 ShieldOrders = 0;
	int32 ShieldBlocksSeen = 0;
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
	int32 HitsUntimed = 0;
	float MaxStepsBehind = 0.0f;
};
