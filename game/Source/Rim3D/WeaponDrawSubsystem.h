#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "MeleePose.h"

#include "HoldDraw.h"

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
	/** Elden Ring style melee (snapshot souls, docs/design/souls-combat.md): one press is one attack, light or heavy, a
	 *  running or jump attack when sprinting or in the air. Not held back by the swing under way: the sim keeps the
	 *  press and plays it when the combo opens. bHold: the heavy button is still held (a charged heavy until released). */
	bool SoulsSwing(bool bHeavy, bool bHold);
	/** Lets go of a held (charging) heavy. */
	bool SoulsRelease();
	/** Holds the melee weapon up to block. False unless the colonist holds a melee weapon and is ready. */
	bool Block();
	/** Right mouse with a shield on the arm: raises the shield (facing where the camera looks) or lowers it. Sends a
	 *  ShieldCommand when the wish changes, and again when the camera turns more than 15 degrees. False if nothing went out. */
	bool HoldShield(bool bUp);
	/** Tests: hold the right mouse button from code (`bScripted` false gives it back to the mouse). */
	void ScriptRightMouse(bool bScripted, bool bHeld) { bScriptedRight = bScripted; bScriptRightHeld = bHeld; }
	/** Whether this pawn's view has a shield on its arm (worn, not slung behind a two-handed weapon). */
	static bool HasShieldOnArm(const colony::ThingView& View);
	/** Each material slot of a model: material, parent, base colour map and factor, metallic and roughness (for the logs and tests). */
	static FString DescribeMaterials(const UStaticMesh* Mesh);
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
	/** Tests: how the weapon and stance were drawn on this pawn last frame (pinned, why not, the stance clip and weights). */
	FString DescribePawn(colony::Entity Pawn) const;
	/** Tests: frames an impale's blade was turned fully through a chest, and how far its line passed from the chest's centre (cm). */
	int32 GetImpaleFrames() const { return ImpaleFrames; }
	float GetImpaleMiss() const { return ImpaleMiss; }
	/** Weapons drawn in the hands of pawns now. */
	int32 CountWeaponsInHands() const;
	/** How fast a shown spinning gun part (the minigun's barrels) turns, 0 to 1 of its top speed (the motor loop's pitch). */
	float GetSpinShare(colony::Entity Pawn) const;
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
	/** Frames a weapon in the hands was drawn away from its hand, and times the drawing put it back in the hand
	 *  (sneed 2026-11-20: "the weapon then floats but the hand animations still work"); finisher frames drawn. */
	int32 GetFloatFrames() const { return FloatFrames; }
	int32 GetFloatFixes() const { return FloatFixes; }
	int32 GetFinisherFrames() const { return FinisherFrames; }
	/** The most a long gun was slid forward so its stock cleared the head (cm, smoke tests and Weapons.Debug). */
	float GetStockSlideCm() const { return StockSlideCm; }
	/** The worst gap seen between a held weapon's grip and its hand (cm) since the last call, and resets it. */
	float TakeWorstGripGap() { const float W = WorstGripGap; WorstGripGap = 0.0f; return W; }
	/** The last finisher frame: the weapon's point and the body point it is driven at (world cm). */
	FVector GetFinisherTip() const { return FinisherTip; }
	FVector GetFinisherGoal() const { return FinisherGoal; }
	/** For the player's colonist: why the weapon in its hands was or wasn't held at its place this frame (smoke tests). */
	const FString& GetPinDebug() const { return PinDebug; }
	float GetFinisherShort() const { return FinisherShort; }
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
		/** Each of Parts' index in the gun's assets/guns.json parts, and a spinning part's speed (turns a second) and angle. */
		TArray<int32> PartDefs;
		float SpinSpeed = 0.0f;
		float SpinAngle = 0.0f;
		float SpinShare = 0.0f;  // SpinSpeed over the part's top speed (0 to 1): the motor sound follows it
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
		/** What the drawing did with this pawn's weapon last frame (smoke tests, DescribePawn). */
		FString Debug;
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
		/** Weapon first, then the hands (docs/design/weapon-hold.md): how far it is aimed (guns) and raised to block
		 *  (melee), how free the main hand is of its place through a swing or a full-body clip (0 to 1), and whether
		 *  the hands have goals from this drawing (so it lets them go once). */
		float AimBlend = 0.0f;
		float BlockBlend = 0.0f;
		float HandFree = 0.0f;
		float SwingBlend = 0.0f;  // a swing with no clip: the weapon itself follows the procedural swing
		/** Through a full-body clip that is not a swing (a roll, a stagger), the weapon rides with the upper body: its
		 *  place in the neck's frame from before the clip (captured while no clip plays), how far it rides (0 to 1). Both
		 *  hands stay on it the whole time, so an arm never crosses the tumbling body and snaps back (sneed 2026-11-24). */
		FTransform WeaponInNeck = FTransform::Identity;
		bool bWeaponInNeck = false;
		float ClipRide = 0.0f;
		bool bPinned = false;
		/** A held weapon drawn away from its hand this long (real seconds), and how long it is kept on the hand's own
		 *  grip after that (no place, no pinned hands) so it never floats. */
		float FloatSeconds = 0.0f;
		float ForceHandSeconds = 0.0f;
		bool bFloatLogged = false;
		/** The finisher's share of the weapon's pose (0 to 1). */
		float FinisherBlend = 0.0f;
		/** The impale (a riposte, sneed 2026-11-28): how far the blade is turned to run through the victim's chest (0 to 1),
		 *  and the grip and point in the weapon's frame (cm) from the last placing. */
		float ImpaleBlend = 0.0f;
		colony::Entity ImpaleTarget = colony::kNoEntity;  // the snapshot names the target only through the wind-up
		FVector PinMain = FVector::ZeroVector;
		FVector PinTip = FVector::ZeroVector;
		/** How far the finisher's weapon had to come back to stay in reach (cm), and the extra forward bend that answers it (degrees). */
		float FinisherShort = 0.0f;
		float FinisherReachBend = 0.0f;
		/** Aimed anchors held steady (sneed 2026-11-23: "whenever i move aiming my character vibrates and wobbles"): the
		 *  head and right shoulder in the actor's frame, smoothed so the step bob of the walk cycle doesn't shake the
		 *  aimed gun (the hands follow the gun, so the arms stay steady; the legs and body animate as before). */
		FVector SteadyHead = FVector::ZeroVector;
		FVector SteadyShoulder = FVector::ZeroVector;
		bool bSteadyValid = false;
	};
	int32 FloatFrames = 0;
	int32 FloatFixes = 0;
	int32 FinisherFrames = 0;
	float StockSlideCm = 0.0f;
	float WorstGripGap = 0.0f;
	FVector FinisherTip = FVector::ZeroVector;
	FVector FinisherGoal = FVector::ZeroVector;
	FString PinDebug;
	/** How far the last finisher pose was pulled back to stay in the hands (cm). */
	float FinisherShort = 0.0f;
	/** How far ahead of the shoulder (cm, along the facing) the finisher's grip wants to be. */
	float FinisherAhead = 0.0f;
	/** Why PinWeapon or FinisherPose gave up, for PinDebug. */
	const TCHAR* PinFailReason = TEXT("");
	/** The finisher's weapon pose now (docs/design/lock-on.md "Finishers"): drawn, and the one the hands close on (they
	 *  differ for a reverse grip: the blade comes out under the fist), and how much of it shows. False if none. */
	/** Through an impale: the weapon in the hand turned about its grip so the blade runs through the victim's chest. */
	FTransform ImpaleAim(const colony::ThingView& View, FPawnWeapons& Pawn, const FTransform& Weapon);
	/** Impales drawn into a chest (frames), and how far the last one's blade line passed from the chest's centre (cm). */
	int32 ImpaleFrames = 0;
	float ImpaleMiss = -1.0f;
	bool FinisherPose(const ASimPawnActor& Actor, const colony::ThingView& View, const HoldDraw::FPoints& Points, float Size, FTransform& OutDrawn,
					  FTransform& OutPinned, float& OutWeight);

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
	/** Holds the weapon at its style's place on the body and puts both hands on its grips (the arm solver's goals);
	 *  OutWeaponInHand is the weapon in the main hand bone's space. False when the hold has no place or the body no
	 *  MakeHuman hands (the weapon then follows the hand as before). */
	bool PinWeapon(ASimPawnActor& Actor, const colony::ThingView& View, FPawnWeapons& Pawn, const colony::WeaponHold& Hold, const HoldDraw::FPoints& Points,
				   float Size, float BodyPlace, bool bControlled, float Alpha, FTransform& OutWeaponInHand, FTransform& OutWeaponWorld);
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
	/** World seconds each pawn last fired (ShotFired), for a minigun's spinning barrels. */
	TMap<uint32, float> LastShotAt;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 LastGeneration = -1;
	int64 LastOrderStep = -1;
	/** A click during a swing is kept this long (real seconds) and swings the moment the last one lets it (sneed
	 *  2026-11-24: "make sure you can still punch immediately after so there's no delay between"). */
	double SwingBufferedUntil = 0.0;
	/** A souls press not sent yet (one order per step): kept until the next step, as heavy or light. */
	bool bSoulsPending = false;
	bool bSoulsPendingHeavy = false;
	double SoulsPendingAt = 0.0;
	/** A heavy press is held: its release goes to the sim (the charge ends). */
	bool bSoulsHolding = false;
	/** The attack button went down at this real time and is still down: let go before WeaponSoulsHeavyHoldSeconds it is a
	 *  light attack, held past it a heavy that charges while held (sneed 2026-11-27: hold left click for the strong attack). */
	bool bSoulsButtonDown = false;
	bool bSoulsHeavyFired = false;
	double SoulsButtonAt = 0.0;
	bool bSoulsReleasePending = false;
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
