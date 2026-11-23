#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "GunRangeSubsystem.generated.h"

class AActor;
class ASimPawnActor;
class UAnimSequence;
class UGunViewAnimInstance;
class UPointLightComponent;
class USceneComponent;
class USimSubsystem;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
namespace colony { struct ThingView; }

/** One gun of the range as assets/guns.json describes it (docs/design/gun-range.md). Points in cm, the gun's frame. */
struct FGunRangeGun
{
	struct FPart
	{
		FString Name;
		FString Match;
		FVector Pivot = FVector::ZeroVector;
		/** Spins about `SpinAxis` through Pivot while the gun fires (a minigun's barrels, "spin" in guns.json): up to
		 *  `SpinRps` turns a second over `SpinUp` seconds from the first shot, and down over `SpinDown` after the last. */
		bool bSpin = false;
		FVector SpinAxis = FVector::ForwardVector;
		float SpinRps = 5.0f;
		float SpinUp = 1.0f;
		float SpinDown = 1.5f;
	};
	FString Def;
	FString Label;
	FString Credit;
	FString ArtName;
	TArray<FPart> Parts;
	FRotator MeshRotation = FRotator::ZeroRotator;
	FVector MeshOffset = FVector::ZeroVector;
	FVector Grip = FVector::ZeroVector;
	FRotator GripTurn = FRotator::ZeroRotator;
	bool bOffHand = false;
	FVector OffHand = FVector::ZeroVector;
	FVector Sight = FVector(0, 0, 8);
	FVector Muzzle = FVector(60, 0, 3);
	float Rake = 18.0f;
	FString Stance;
	float StanceHold = -1.0f;
	FString Reload;
	float ReloadFrom = 0.0f;
	float ReloadTo = 0.0f;
	bool bMagazine = false;
	FString MagazinePart;
	float MagazineOut = 0.25f;
	float MagazineIn = 0.62f;
	FVector Hip = FVector(0, 9, -8);
	float SightDistance = 10.0f;
	/** The right hand reaches to put the sight at its spot (false: the stance clip's own hold). */
	bool bReach = true;
	/** Where the sight goes through a reload (cm from the eye, x ahead): lower and nearer, still in view. */
	FVector ReloadSpot = FVector(22.0f, 6.0f, -14.0f);
	float KickBack = 4.0f;
	float KickUp = 3.0f;
	float KickRecover = 12.0f;
	bool bCycle = false;
	FString CyclePart;
	float CycleStart = 0.25f;
	float CycleSeconds = 0.9f;
	float CycleLift = 60.0f;
	float CycleBack = -9.0f;
	bool bReloadBolt = false;
	bool bPump = false;
	float PumpStart = 0.3f;
	float PumpSeconds = 0.5f;
	/** How much the barrel keeps pointing ahead through a reload (0: it goes wherever the clip swings it, 1: held on the
	 *  view's line; the clip still turns it about the barrel). A shotgun's shell-by-shell reload swings the stock about. */
	float ReloadSteady = 0.0f;
	/** A telescopic sight: fully aimed, the view goes through it (this field of view, a scope ring drawn over it, the arms
	 *  and gun hidden). 0: open sights, the gun stays in view. */
	float ScopeFov = 0.0f;
	/** The over-the-shoulder aim camera with this gun (guns.json "aimCamera": [right cm, up cm]); negative keeps the
	 *  pawn's own (AProtagonistPawn). A big shoulder launcher needs the camera further right and higher to see past it. */
	FVector2D AimCamera = FVector2D(-1.0, -1.0);
};

/** A spinning part's speed after `Dt` seconds (turns a second): up to its full speed while the gun has fired in the last
 *  0.35 s, down to 0 after (docs/design/neon-assets.md "Minigun"). */
float GunRangeSpinSpeed(const FGunRangeGun::FPart& Part, float Speed, float SinceShot, float Dt);
/** The part turned `AngleDegrees` about its spin axis through its pivot, in the gun's frame. */
FTransform GunRangeSpinTransform(const FGunRangeGun::FPart& Part, float AngleDegrees);

/** A melee weapon on the combat range's rack, after the guns: drawn on the first-person arms like any weapon. */
struct FGunRangeMelee
{
	FString Def;
	FString Label;
	/** Its real model's folder under /Game/Rim3D/Generated/Guns (GunArt commandlet, the entry's "art"), empty for the
	 *  def's own shapes. Frame as the def's shapes: +X along the weapon to the tip, origin where the def puts it (the
	 *  palm at the hold's `main`), +Z the edge or the head's top. */
	FString ArtName;
};

/**
 * The gun range (docs/design/gun-range.md): the first-person arms and gun the player sees, for the guns in
 * assets/guns.json. Cosmetic only: the sim fires, reloads (the magazine is free: ammunition is not counted) and decides
 * every hit; this draws the player's own arms (their MakeHuman body, head and legs hidden) holding a real gun model,
 * the stance clip per gun, a reload clip timed to the sim's reload, the bolt or slide cycling, the magazine coming out,
 * kick, sway and bob, and the sights lined up on the crosshair while aiming (right mouse button).
 *
 * Keys while it is on: 1-4 or the mouse wheel pick a gun from the rack (EquipCommand), F6 reads assets/guns.json again.
 * The range itself (its rack, keys and panel) is off unless a scene turns it on (the gunrange smoke test).
 *
 * The first-person arms are drawn in the main game too (View.Arms, docs/design/weapon-hold.md): a gun with a model
 * here as above, and anything else the player holds (a melee weapon, a gun without a model, a shield, bare fists)
 * through its hold style: the style's first-person stance and block clips, the sim's swing clip played on the arms in
 * step with the sim, the weapon on the hand's grip and the other hand on a two-handed one.
 */
UCLASS()
class RIM3D_API UGunRangeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	void SetEnabled(bool bOn);
	bool IsEnabled() const { return bEnabled; }
	/** The items to pick from: the guns in the order of assets/guns.json, then its melee weapons (kNoEntity where one
	 *  is missing). */
	void SetRack(const TArray<colony::Entity>& Items) { Rack = Items; }
	const TArray<FGunRangeGun>& GetGuns() const { return Guns; }
	const TArray<FGunRangeMelee>& GetMelee() const { return Melee; }
	/** Rack index of the weapon in the hands (guns first, then melee), -1 for none the range knows. */
	int32 GetHeldRackIndex() const { return HeldRack; }
	/** Puts rack gun Index in the player's hands. False if there is no such gun or the sim is not ready. */
	bool SelectGun(int32 Index);
	/** Reads assets/guns.json again. */
	bool LoadConfig();
	/** The gun art for a weapon def (assets/guns.json, read on first use), null if it has none or its model is not
	 *  imported: the third-person hands and the first-person arms hold this model instead of the def's own shapes. */
	const FGunRangeGun* FindGunByDef(const FString& Def);
	/** The real model of a melee weapon def (assets/guns.json "melee" entries with "art"), null if it has none or it
	 *  is not imported: drawn instead of the def's own shapes, in the hands and in first person. */
	UStaticMesh* FindMeleeMesh(const FString& Def);
	/** The largest imported mesh of a gun's art whose name has Match in it (cached). */
	UStaticMesh* FindMesh(const FString& Art, const FString& Match);
	/** The held gun's scope field of view while the player holds a scoped gun (bolt-action rifle), else 0. Fully aimed,
	 *  the third-person camera goes to the scope (AProtagonistPawn::IsScopeView) and the scope ring is drawn. */
	float GetScopeFov() const { return ScopeFovNow; }
	/** The held gun's aim camera offsets (FGunRangeGun::AimCamera), negative when none. */
	FVector2D GetAimCamera() const { return AimCameraNow; }
	/** Whether the scope's ring is drawn this frame. */
	bool IsScoped() const { return bScoped; }
	/** A line for the panel from the scene (targets left and so on). */
	void SetSceneLine(const FString& Line) { SceneLine = Line; }

	// For the smoke test.
	bool IsViewShown() const { return bShown; }
	/** The first-person view gun's muzzle in the world (its guns.json `muzzle`), false while no view gun is drawn. */
	bool GetViewMuzzle(FVector& Out) const;
	/** The first-person gun's frame (+X out of the barrel, +Y right, +Z up) and its muzzle in that frame, if one is drawn. */
	bool GetViewGunFrame(FTransform& OutFrame, FVector& OutMuzzle) const;
	int32 GetCurrentGun() const { return Current; }
	bool HasModel(int32 Index) const;
	int32 GetShotsSeen() const { return ShotsSeen; }
	int32 GetReloadsSeen() const { return ReloadsSeen; }
	float GetReloadFraction() const;
	/** How far the gun's barrel points off the view's centre line, degrees (0 when lined up). */
	float GetBarrelOffDegrees() const { return BarrelOffDegrees; }
	/** Where the gun's sight is from the view's centre line while aimed, cm. */
	float GetSightOffCm() const { return SightOffCm; }
	/** Where the arms' main bones are in the view (cm, x ahead, y right, z up), for the smoke test's notes. */
	const FString& GetBoneLine() const { return BoneLine; }
	/** The gun model's parts this frame (mesh, shown or not, size, place in the view), for the smoke test's notes. */
	FString DescribeGunParts() const;
	/** Whether the arms show a weapon by its hold style this frame (a melee weapon, fists, a shield), not a gun model. */
	bool IsHeldViewShown() const { return bShown && bHeldView; }
	/** The first-person arms' mesh (the player's MakeHuman body, only its arms shown), for the smoke test's arm checks. */
	USkeletalMeshComponent* GetArmsMesh() const { return Arms; }
	/** How far the first-person body leans in so its arms reach the weapon (cm). */
	float GetArmsLean() const { return ArmsLean; }
	/** Swing clips the first-person arms have started. */
	int32 GetHeldSwingsSeen() const { return HeldSwingsSeen; }

private:
	void OnSimEvent(const colony::Event& Event);
	void HideAll();
	bool EnsureRig(ASimPawnActor& Player);
	void CopyLooks(ASimPawnActor& Player);
	void SetupGun(int32 Index);
	UAnimSequence* LoadClip(const FString& Rel);
	void UpdateInput();
	void UpdateView(float Dt, ASimPawnActor& Player);
	void UpdateHeldView(float Dt, ASimPawnActor& Player, const colony::ThingView& Me);
	void HideHeld();
	void DrawPanel();
	void DrawScope();

	bool bEnabled = false;
	bool bConfigTried = false;
	bool bShown = false;
	TArray<FGunRangeGun> Guns;
	TArray<FGunRangeMelee> Melee;
	TArray<colony::Entity> Rack;
	int32 HeldRack = -1;
	FString AnimRoot = TEXT("/Game/Rim3D/Generated/Animations/");
	/** Where the arms' head bone sits in the view (cm, x ahead): a little behind and below the eyes. */
	FVector HeadInView = FVector(-8.0f, 0.0f, -7.0f);
	FString SceneLine;
	TMap<FString, TWeakObjectPtr<UAnimSequence>> Clips;
	TSet<FString> MissingClips;
	TMap<FString, TWeakObjectPtr<UStaticMesh>> MeshCache;
	TMap<FString, double> MeshMisses;

	TWeakObjectPtr<USimSubsystem> BridgeSource;
	FDelegateHandle EventHandle;

	UPROPERTY()
	TObjectPtr<AActor> Holder;
	UPROPERTY()
	TObjectPtr<USceneComponent> ViewRoot;
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Source;
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> Arms;
	UPROPERTY()
	TObjectPtr<USceneComponent> GunFrame;
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> PartComponents;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> HeldMag;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> DroppedMag;
	UPROPERTY()
	TObjectPtr<UPointLightComponent> Flash;
	UPROPERTY()
	TObjectPtr<UStaticMesh> FallbackMesh;
	/** The held weapon and shield of the hold-style path (not a gun model). */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> HeldWeapon;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> HeldShield;

	TWeakObjectPtr<USkeletalMeshComponent> CopiedFrom;
	float LooksCopyTimer = 0.0f;
	int32 Current = -1;
	FString CurrentDef;
	bool bWasReloading = false;
	float ReloadSeconds = 0.0f;
	float ReloadClock = 0.0f;
	bool bReloadPlaying = false;
	FQuat AimCorrection = FQuat::Identity;
	/** The aim correction when a reload began (ReloadSteady works from it). */
	FQuat SteadyBase = FQuat::Identity;
	bool bHaveCorrection = false;
	int32 ValidFrames = 0;
	float Kick = 0.0f;
	float KickVelocity = 0.0f;
	float SinceShot = 99.0f;
	/** The first-person view's spinning part: turns a second now and the angle it is at (degrees). */
	float ViewSpinSpeed = 0.0f;
	float ViewSpinAngle = 0.0f;
	FVector2D Sway = FVector2D::ZeroVector;
	float BobPhase = 0.0f;
	float Dip = 0.0f;
	float SprintBlend = 0.0f;
	/** How far (cm) the first-person body leans forward so its arms reach the weapon's grips (never stretched). */
	float ArmsLean = 0.0f;
	/** How far a melee weapon is raised to block in first person (0 to 1). */
	float HeldBlockBlend = 0.0f;
	float FlashAge = 99.0f;
	float DropAge = 99.0f;
	FVector DropVelocity = FVector::ZeroVector;
	bool bMagOut = false;
	int32 ShotsSeen = 0;
	int32 ReloadsSeen = 0;
	float BarrelOffDegrees = 0.0f;
	float SightOffCm = 0.0f;
	/** The stand-in box's own place and size in the gun frame (no model imported). */
	bool bFallback = false;
	FTransform FallbackFix = FTransform::Identity;
	/** Looking through a scope this frame (see FGunRangeGun::ScopeFov). */
	bool bScoped = false;
	/** The held gun's scope (0: none, or not usable now). */
	float ScopeFovNow = 0.0f;
	FVector2D AimCameraNow = FVector2D(-1.0, -1.0);
	FString BoneLine;
	// The hold-style path.
	bool bHeldView = false;
	TWeakObjectPtr<UAnimSequence> HeldStance;
	uint32 HeldMotionDef = MAX_uint32;
	float HeldMotionTime = 0.0f;
	int32 HeldSwingsSeen = 0;
	/** Bare fists: a punch (0..1 through it) when the sim's swing lands without a clip, and which hand throws it. */
	float PunchAge = 99.0f;
	bool bPunchLeft = false;
	uint8 LastMeleePhase = 0;
};
