#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "FightSubsystem.generated.h"

class AActor;
class ASimPawnActor;
class UDecalComponent;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UPointLightComponent;
class USceneComponent;
class USimSubsystem;
class UStaticMesh;
class UStaticMeshComponent;
namespace colony { class Sim; struct DrawnAim; struct HitboxView; struct ThingView; }

/** What the particles are (each has its own mesh and colour). */
enum class EFightParticle : uint8
{
	Dust,
	Chip,
	Spark,
	Blood,
	Smoke,
	Flash,
	Count,
};

/** One bullet's picture: from the muzzle along the shot, at the bullet's speed in real time. */
struct FFightTracer
{
	colony::Entity Id = colony::kNoEntity;
	colony::Entity Shooter = colony::kNoEntity;
	FVector Start = FVector::ZeroVector;
	FVector Dir = FVector::ForwardVector;
	bool bDirKnown = false;
	/** cm per real second at 1x. */
	float Speed = 21000.0f;
	/** How far the head has flown, cm. */
	float Distance = 0.0f;
	float Age = 0.0f;
	int32 Frames = 0;
	/** The hit event came: the bullet stops at End and the impact happens when the head gets there. */
	bool bEnded = false;
	FVector End = FVector::ZeroVector;
	colony::ProjectileHitKind HitKind = colony::ProjectileHitKind::None;
	colony::Entity Target = colony::kNoEntity;
	int32 Part = -1;
	float Amount = 0.0f;
};

/** A hit on a pawn as the sim reported it, and how far the hit point was from the pawn's hitboxes. */
struct FFightHitRecord
{
	colony::Entity Pawn = colony::kNoEntity;
	colony::ProjectileHitKind Kind = colony::ProjectileHitKind::None;
	int32 Part = -1;
	float Amount = 0.0f;
	/** Sim metres. */
	FVector Where = FVector::ZeroVector;
	/** Cm from the nearest of the pawn's hitboxes (0 on or inside one); -1 if the pawn had none. */
	float MissCm = -1.0f;
	/** The two distances MissCm is the nearer of: the sim's hitboxes and the body as drawn (-1: not measured). */
	float StockCm = -1.0f;
	float DrawnCm = -1.0f;
	/** For the report: where the drawn body stood and faced (world cm, degrees) and where the sim had it (sim metres). */
	FVector DrawnAt = FVector::ZeroVector;
	float DrawnYaw = 0.0f;
	FVector SimAt = FVector::ZeroVector;
	float SimYaw = 0.0f;
	int64 GameSeconds = 0;  // the step it happened in (USimSubsystem::GetSteps)
};

/**
 * Fights on screen (docs/design/fights.md): what the sim's ShotFired and ProjectileHit events and the
 * snapshot look like. All of it is cosmetic; the sim decides who is hit.
 *
 * - Muzzle flashes (a bright blob and a short light) and a puff of smoke at every shot.
 * - Tracers: a streak that flies from the muzzle to where the bullet ended, at the bullet's own speed.
 * - Impacts when the streak arrives: dust on ground and rock, sparks and chips on buildings, sparks on
 *   armor, a spray of blood on flesh.
 * - Hit reactions: a pawn that is hit flinches away from the shot.
 * - Ragdolls: downed and dead pawns go limp and fall (physics on a floor made under them).
 * - Wound decals (placeholder): a dark patch on the body where it was hit, and blood on the ground.
 *   Needs the M_WoundDecal material the FightMaterials commandlet makes.
 * - A placeholder gun at each armed pawn's chest (from the weapon def's `visual.parts`).
 * - The protagonist's gun: in direct mode the left mouse button fires at the crosshair through the sim
 *   (ShootCommand), R reloads, C crouches, Z lies down, E picks up the nearest gun.
 * - A hitbox debug view: `Fight.Hitboxes 1` draws every pawn's hitboxes where the sim has them.
 *
 * Console: Fight.Effects (0 turns the effects off), Fight.Hitboxes.
 */
UCLASS()
class RIM3D_API UFightSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// --- The protagonist's gun (direct mode). The keys call these; tests call them directly. ---

	/** Fires at the crosshair: sends ShootCommand{player} if the gun is ready. False (and nothing sent) if
	 *  not in direct mode, no gun, the game is paused, an order went out this step, or the gun isn't ready. */
	bool Fire();
	bool Reload();
	/** Patches your worst bleeding zone (H; body-health.md part B). False if nothing bleeds, you are down or patching already. */
	bool Patch();
	/** Crouch or stand up. */
	bool ToggleCrouch();
	/** Lie down or stand up. */
	bool ToggleProne();
	/** Picks up the nearest gun lying within 8 m. False if there is none or one is in hand. */
	bool EquipNearest();
	/** Dodge roll (P2): sends DodgeRollCommand in the direction of `Input` (x right, y forward, both -1 to 1, relative to
	 *  where the view looks; zero is a backstep). False (and nothing sent) if not in direct mode, the pawn can't roll now or
	 *  an order went out this step. */
	bool Roll(const FVector2D& Input);
	/** Throws the chosen grenade (R picks; the first on the belt if none): at the locked-on enemy in third person, else at
	 *  the crosshair's point (F9's ThrowCommand with `player`). G held shows the arc first (DrawGrenadeArc); letting go throws. */
	bool ThrowGrenade();
	/** R: the next kind of grenade on the belt. False if the belt holds one kind or none. */
	bool NextGrenade();
	/** The grenade G throws now (a Things def), invalid with an empty belt. */
	colony::DefId GetGrenadeChoice() const;
	/** G is held: the throw's arc and landing ring are drawn. */
	bool IsAimingGrenade() const { return bAimingGrenade; }
	/** Frames the aim arc was drawn (smoke tests). */
	int32 GetGrenadeArcFrames() const { return GrenadeArcFrames; }
	/** Where the crosshair points, in Unreal world units: the nearest of the sim's ground, rock and buildings and the
	 *  bodies as they are drawn this frame (DrawnHit; animals by the sim's hitboxes), else a point 150 m out. On a drawn
	 *  body `OutDrawn` gets the pawn, the sim body part and the spot in that pawn's frame (docs/design/aiming.md).
	 *  False if not in direct mode. */
	bool ComputeAimPoint(FVector& OutWorldPoint, colony::Entity* OutPawnHit = nullptr, colony::DrawnAim* OutDrawn = nullptr) const;

	// --- Hitbox debug view ---

	void SetShowHitboxes(bool bShow);
	bool IsShowingHitboxes() const;
	/** Shapes drawn in the last frame. */
	int32 GetHitboxesDrawn() const { return HitboxesDrawn; }
	/** Cm from a sim point (metres) to the nearest hitbox of the pawn as the sim has them now; -1 if none. */
	float DistanceToHitboxesCm(colony::Entity Pawn, float X, float Y, float Z) const;

	// --- What has happened, for the smoke test and the screen ---

	int32 GetShotsSeen() const { return ShotsSeen; }
	int32 GetHitsSeen() const { return HitsSeen; }
	int32 GetPawnHitsSeen() const { return PawnHitsSeen; }
	int32 GetSnappedHits() const { return SnappedHits; }
	/** Where the last bullet met this pawn's drawn body (for the smoke tests' close looks). */
	bool GetLastBodyHit(colony::Entity Pawn, FVector& Out) const
	{
		const FVector* Found = LastBodyHits.Find(entt::to_integral(Pawn));
		if (Found)
		{
			Out = *Found;
		}
		return Found != nullptr;
	}
	int32 GetFlashes() const { return Flashes; }
	int32 GetTracersSpawned() const { return TracersSpawned; }
	int32 GetMuzzleShots() const { return MuzzleShots; }
	int32 GetTracersArrived() const { return TracersArrived; }
	/** Tracers given up on: the bullet never showed in a snapshot, or flew too far or too long without a hit. */
	int32 GetTracersDropped() const { return TracersDropped; }
	const TArray<FFightTracer>& GetTracers() const { return Tracers; }
	int32 GetParticlesOfKind(EFightParticle Kind) const
	{
		int32 Count = 0;
		for (const FParticle& Particle : Particles)
		{
			Count += Particle.Kind == Kind ? 1 : 0;
		}
		return Count;
	}
	int32 GetImpacts() const { return Impacts; }
	int32 GetImpactsOfKind(colony::ProjectileHitKind Kind) const { return ImpactsByKind[static_cast<int32>(Kind)]; }
	int32 GetHitReactions() const { return HitReactionsPlayed; }
	/** Whole-body react clips started on bullet hits (a stumble when the sim staggered the pawn). */
	int32 GetHitClips() const { return HitClipsPlayed; }
	/** Damage numbers put up over your own hits so far. */
	int32 GetDamageNumbersShown() const { return DamageNumbersShown; }
	/** Stealth marks drawn over heads so far: "?" for a suspicious enemy, "!" when one is alerted (docs/design/stealth.md). */
	int32 GetAlertMarksDrawn() const { return AlertMarksDrawn; }
	int32 GetAlertEventsSeen() const { return AlertEventsSeen; }
	/** Of those, the hits on you (drawn red); zones knocked by hits (the hit layer, body-health.md part E); patch orders sent. */
	int32 GetDamageNumbersOnMe() const { return DamageNumbersOnMe; }
	int32 GetZoneKicks() const { return ZoneKicks; }
	int32 GetPatchOrders() const { return PatchOrders; }
	int32 GetBodyDecals() const { return BodyDecals; }
	int32 GetGroundDecals() const { return GroundDecals; }
	int32 GetRagdollsStarted() const { return RagdollsStarted; }
	/** Shots and blows that jolted a lying ragdoll (a corpse) where they struck. */
	int32 GetRagdollHitPushes() const { return RagdollHitPushes; }
	/** Severed limbs and heads on the ground now, and how many were thrown since the start (body-health.md part F). */
	int32 GetSeveredPiecesShown() const;
	int32 GetSeveredPiecesThrown() const { return SeveredPiecesThrown; }
	/** For the tests: corpses whose trunk burst into gibs, and the chunks thrown. */
	int32 GetTrunksBurst() const { return TrunksBurst; }
	int32 GetGibsThrown() const { return GibsThrown; }
	/** How many times a bullet's line knocked a severed piece. */
	int32 GetSeveredPiecesPushed() const { return SeveredPiecesPushed; }
	/** How many spent cases guns have thrown out (smoke tests). */
	int32 GetCasingsEjected() const { return CasingsEjected; }
	/** Knocks every severed piece the line from `From` to `To` passes through (body-health.md part F, "Physics"). */
	void PushSeveredPieces(const FVector& From, const FVector& To, const FVector& Dir, float Damage);
	int32 GetFireOrders() const { return FireOrders; }
	int32 GetHitMarkers() const { return HitMarkers; }
	int32 GetActiveTracers() const { return Tracers.Num(); }
	int32 GetActiveParticles() const { return Particles.Num(); }
	int32 GetLiveDecals() const;
	int32 GetHeldGuns() const;
	/** Largest distance (cm) between a pawn hit and the pawn's hitboxes or drawn body (the nearer; the drawn body less the
	 *  5 cm it may move on while the bullet flies) over all hits seen. */
	float GetMaxHitboxMissCm() const
	{
		float Max = 0.0f;
		for (const FFightHitRecord& Record : HitLog)
		{
			Max = FMath::Max(Max, Record.MissCm);
		}
		return Max;
	}
	/** The worst checked hit, spelled out for a smoke test's failure message (empty if none). */
	FString DescribeWorstHit() const;
	/** Hits that could be checked: a hit that downed the pawn or took off a part is not (its hitboxes changed in the same step). */
	int32 GetHitboxChecks() const
	{
		int32 Count = 0;
		for (const FFightHitRecord& Record : HitLog)
		{
			Count += Record.MissCm >= 0.0f ? 1 : 0;
		}
		return Count;
	}
	const TArray<FFightHitRecord>& GetHitLog() const { return HitLog; }
	bool IsDecalMaterialMissing() const { return bDecalMaterialMissing; }
	/** Frames in which the HUD was drawn. */
	int32 GetHudFramesDrawn() const { return HudFramesDrawn; }
	/** The weapon in a pawn's hands, if it has one (drawn by UWeaponDrawSubsystem since U22). */
	UStaticMeshComponent* GetHeldGun(colony::Entity Pawn) const;

	/** The picture of a melee blow landing at `At` (world cm), going along `Direction`: blood and a flinch on
	 *  flesh, sparks on armor or a blocking weapon, chips on a building (docs/design/melee-draw.md). The same
	 *  effects a bullet's arrival makes; nothing here changes the sim. */
	void SpawnMeleeImpact(colony::ProjectileHitKind Kind, colony::Entity Attacker, colony::Entity Target, const FVector& At,
		const FVector& Direction, int32 Part, float Amount);

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FParticle
	{
		EFightParticle Kind = EFightParticle::Dust;
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float Life = 1.0f;
		float SizeCm = 5.0f;
		float Gravity = 980.0f;
		float Drag = 2.0f;
	};

	/** A spent case thrown out of a gun's ejection port (sneed, 2026-11-22: "just have the casing fire"): brass for rifles
	 *  and pistols, a red shell for shotguns. It tumbles, bounces on the ground, lies a while and goes. Cosmetic. */
	struct FCasing
	{
		int32 Kind = 0;  // 0 rifle, 1 pistol, 2 shotgun shell
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector Spin = FVector::ZeroVector;  // degrees a second about the case's own axes
		float Age = 0.0f;
		bool bRest = false;
	};

	void OnSimEvent(const colony::Event& Event);
	void OnShotFired(const colony::Event& Event);
	/** Throws a spent case out of the gun that fired, drawn in first person or held in third, within 25 m of the camera. */
	void EjectCasing(const colony::Event& Event);
	void UpdateCasings(float GameDelta);
	void OnProjectileHit(const colony::Event& Event);
	void SpawnImpact(const FFightTracer& Tracer);
	void SpawnFlash(const FVector& Muzzle);
	/** Where the drawn gun's muzzle is for a shot by Shooter: the first-person view gun's, else the front end of the gun
	 *  in its hands. False (Out unchanged) when no drawn gun is near the sim's muzzle point. */
	bool FindDrawnMuzzle(colony::Entity Shooter, FVector& InOutMuzzle) const;
	/** The player's thin yellow bar under the gun's name: how far the gun's shots have refilled, and pips for the shots left. */
	void DrawShotStore(const colony::ThingView& View, float Scale) const;
	void SpawnBurst(EFightParticle Kind, const FVector& At, const FVector& Direction, int32 Count, float Speed, float Spread, float SizeCm, float Life, float Gravity);
	void SpawnBodyDecal(ASimPawnActor& Actor, const FVector& At, const FVector& Direction, float Amount);
	void SpawnGroundDecal(const FVector& At, float RadiusCm);
	void TrimDecals();

	void EnsureEffects();
	UInstancedStaticMeshComponent* MakeInstanced(UStaticMesh* Mesh);
	void ResetEffects();
	void UpdateTracers(float DeltaTime, float GameDelta);
	void UpdateParticles(float GameDelta);
	void UpdateLights(float GameDelta);
	void SyncPawns();
	void UpdatePlayerInput();
	void DrawHitboxes();
	void DrawHud();
	/** While G is held: the arc the grenade would fly and a ring where it bursts, on the screen. */
	void DrawGrenadeArc(USimSubsystem& Bridge, colony::Entity Pawn);
	/** The world point a throw goes to now: the crosshair's, pulled in to the arm's range. False with no aim. */
	bool GrenadeAimSim(USimSubsystem& Bridge, colony::Entity Pawn, colony::DefId Item, float& OutX, float& OutY) const;
	bool EffectsOn() const;

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> TracerInstances;

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ParticleInstances;

	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> Lights;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> DecalBase;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> WoundMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> PoolMaterial;

	UPROPERTY()
	TArray<TWeakObjectPtr<UDecalComponent>> Decals;

	TArray<FFightTracer> Tracers;
	TArray<FParticle> Particles;
	TArray<FCasing> Casings;
	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> CasingInstances;  // by kind
	int32 CasingsEjected = 0;
	TArray<float> LightAges;
	TMap<uint32, TPair<FVector, int64>> RecentImpulse;
	TArray<FFightHitRecord> HitLog;

	FRandomStream Random = FRandomStream(0xF1647);
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> BridgeSource;
	int32 LastGeneration = -1;
	bool bDecalMaterialTried = false;
	bool bDecalMaterialMissing = false;
	int32 NextLight = 0;
	int64 LastFireStep = -1;
	colony::DefId GrenadeChoice;
	bool bAimingGrenade = false;
	int32 GrenadeArcFrames = 0;
	bool bHaveKeyState = false;
	bool bNoGunMessage = false;
	float HitMarkerSeconds = 0.0f;
	/** The damage of your own hits, floating up over where they landed for a second (direct mode). */
	struct FFightDamageNumber
	{
		FVector At = FVector::ZeroVector;
		float Amount = 0.0f;
		float Age = 0.0f;
		bool bArmour = false;
		/** A hit on you (red), not one of yours. */
		bool bOnMe = false;
	};
	TArray<FFightDamageNumber> DamageNumbers;
	int32 DamageNumbersShown = 0;
	/** Stealth (docs/design/stealth.md): seconds since each enemy was alerted (by entity id), for the "!" that flashes
	    over its head; a "?" stays over a suspicious one from the snapshot. Direct mode only. */
	TMap<uint32, float> AlertFlash;
	int32 AlertMarksDrawn = 0;
	int32 AlertEventsSeen = 0;
	void DrawAlertMarks(float Scale);
	int32 DamageNumbersOnMe = 0;
	int32 ZoneKicks = 0;
	int32 PatchOrders = 0;
	void AddDamageNumber(const FFightTracer& Tracer, bool bArmour, bool bOnMe);
	void SpawnBodyThroughHit(const FFightTracer& Tracer);
	int32 BodyThroughHits = 0;
	/** Body hits whose point moved onto the drawn body (SpawnImpact). */
	int32 SnappedHits = 0;
	TMap<uint32, FVector> LastBodyHits;
	void DrawDamageNumbers(float Scale);
	float EquipMessageSeconds = 0.0f;

	int32 ShotsSeen = 0;
	int32 HitsSeen = 0;
	int32 PawnHitsSeen = 0;
	int32 Flashes = 0;
	int32 TracersSpawned = 0;
	/** Shots whose flash and tracer started at the drawn gun's muzzle. */
	int32 MuzzleShots = 0;
	int32 TracersArrived = 0;
	int32 TracersDropped = 0;
	int32 Impacts = 0;
	int32 ImpactsByKind[8] = {};
	int32 HitReactionsPlayed = 0;
	int32 HitClipsPlayed = 0;
	int32 BodyDecals = 0;
	int32 GroundDecals = 0;
	int32 RagdollsStarted = 0;
	int32 RagdollHitPushes = 0;
	TMap<uint32, TWeakObjectPtr<class ASeveredPieceActor>> SeveredPieces;  // by the sim's piece id
	int32 SeveredPiecesThrown = 0;
	int32 TrunksBurst = 0;
	int32 GibsThrown = 0;
	int32 SeveredPiecesPushed = 0;
	int32 FireOrders = 0;
	int32 HitMarkers = 0;
	int32 HitboxesDrawn = 0;
	int32 HudFramesDrawn = 0;
};
