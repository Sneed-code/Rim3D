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

/** One gun of the range as assets/guns.json describes it (docs/design/gun-range.md). Points in cm, the gun's frame. */
struct FGunRangeGun
{
	struct FPart
	{
		FString Name;
		FString Match;
		FVector Pivot = FVector::ZeroVector;
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
	/** A telescopic sight: fully aimed, the view goes through it (this field of view, a scope ring drawn over it, the arms
	 *  and gun hidden). 0: open sights, the gun stays in view. */
	float ScopeFov = 0.0f;
};

/** A melee weapon on the combat range's rack, after the guns: the main game draws and swings it (third person). */
struct FGunRangeMelee
{
	FString Def;
	FString Label;
};

/**
 * The gun range (docs/design/gun-range.md): the first-person arms and gun the player sees, for the guns in
 * assets/guns.json. Cosmetic only: the sim fires, reloads (the magazine is free: ammunition is not counted) and decides
 * every hit; this draws the player's own arms (their MakeHuman body, head and legs hidden) holding a real gun model,
 * the stance clip per gun, a reload clip timed to the sim's reload, the bolt or slide cycling, the magazine coming out,
 * kick, sway and bob, and the sights lined up on the crosshair while aiming (right mouse button).
 *
 * Keys while it is on: 1-4 or the mouse wheel pick a gun from the rack (EquipCommand), F6 reads assets/guns.json again.
 * Off unless a scene turns it on (the gunrange smoke test).
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
	/** A line for the panel from the scene (targets left and so on). */
	void SetSceneLine(const FString& Line) { SceneLine = Line; }

	// For the smoke test.
	bool IsViewShown() const { return bShown; }
	/** The first-person view gun's muzzle in the world (its guns.json `muzzle`), false while no view gun is drawn. */
	bool GetViewMuzzle(FVector& Out) const;
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

private:
	void OnSimEvent(const colony::Event& Event);
	void HideAll();
	bool EnsureRig(ASimPawnActor& Player);
	void CopyLooks(ASimPawnActor& Player);
	void SetupGun(int32 Index);
	UAnimSequence* LoadClip(const FString& Rel);
	UStaticMesh* FindMesh(const FString& Art, const FString& Match);
	void UpdateInput();
	void UpdateView(float Dt, ASimPawnActor& Player);
	void DrawPanel();
	void DrawScope();

	bool bEnabled = false;
	bool bShown = false;
	TArray<FGunRangeGun> Guns;
	TArray<FGunRangeMelee> Melee;
	TArray<colony::Entity> Rack;
	int32 HeldRack = -1;
	int32 LastHeldRack = -1;
	/** The range turned third person on for a melee weapon (and turns first person back on for a gun). */
	bool bAutoThirdPerson = false;
	FString AnimRoot = TEXT("/Game/Rim3D/Generated/Animations/");
	/** Where the arms' head bone sits in the view (cm, x ahead): a little behind and below the eyes. */
	FVector HeadInView = FVector(-8.0f, 0.0f, -7.0f);
	FString SceneLine;
	TMap<FString, TWeakObjectPtr<UAnimSequence>> Clips;
	TSet<FString> MissingClips;
	TMap<FString, TWeakObjectPtr<UStaticMesh>> MeshCache;

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

	TWeakObjectPtr<USkeletalMeshComponent> CopiedFrom;
	float LooksCopyTimer = 0.0f;
	int32 Current = -1;
	FString CurrentDef;
	bool bWasReloading = false;
	float ReloadSeconds = 0.0f;
	float ReloadClock = 0.0f;
	bool bReloadPlaying = false;
	FQuat AimCorrection = FQuat::Identity;
	bool bHaveCorrection = false;
	int32 ValidFrames = 0;
	float Kick = 0.0f;
	float KickVelocity = 0.0f;
	float SinceShot = 99.0f;
	FVector2D Sway = FVector2D::ZeroVector;
	float BobPhase = 0.0f;
	float Dip = 0.0f;
	float SprintBlend = 0.0f;
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
	FString BoneLine;
};
