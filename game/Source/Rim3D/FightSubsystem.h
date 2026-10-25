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
namespace colony { class Sim; struct HitboxView; struct ThingView; }

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
	int64 GameSeconds = 0;
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
	/** Crouch or stand up. */
	bool ToggleCrouch();
	/** Lie down or stand up. */
	bool ToggleProne();
	/** Picks up the nearest gun lying within 8 m. False if there is none or one is in hand. */
	bool EquipNearest();
	/** Where the crosshair points, in Unreal world units: the sim's trace from the view along the look
	 *  direction (ground, rock, building or pawn hitbox), else a point 150 m out. False if not in direct mode. */
	bool ComputeAimPoint(FVector& OutWorldPoint, colony::Entity* OutPawnHit = nullptr) const;

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
	int32 GetFlashes() const { return Flashes; }
	int32 GetTracersSpawned() const { return TracersSpawned; }
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
	int32 GetBodyDecals() const { return BodyDecals; }
	int32 GetGroundDecals() const { return GroundDecals; }
	int32 GetRagdollsStarted() const { return RagdollsStarted; }
	int32 GetFireOrders() const { return FireOrders; }
	int32 GetHitMarkers() const { return HitMarkers; }
	int32 GetActiveTracers() const { return Tracers.Num(); }
	int32 GetActiveParticles() const { return Particles.Num(); }
	int32 GetLiveDecals() const;
	int32 GetHeldGuns() const;
	/** Largest distance (cm) between a pawn hit and the pawn's hitboxes over all hits seen. */
	float GetMaxHitboxMissCm() const
	{
		float Max = 0.0f;
		for (const FFightHitRecord& Record : HitLog)
		{
			Max = FMath::Max(Max, Record.MissCm);
		}
		return Max;
	}
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
	/** The held gun component for a pawn, if it has one. */
	UStaticMeshComponent* GetHeldGun(colony::Entity Pawn) const;

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

	struct FHeldGun
	{
		TWeakObjectPtr<UStaticMeshComponent> Component;
		colony::DefId Def;
	};

	void OnSimEvent(const colony::Event& Event);
	void OnShotFired(const colony::Event& Event);
	void OnProjectileHit(const colony::Event& Event);
	void SpawnImpact(const FFightTracer& Tracer);
	void SpawnFlash(const FVector& Muzzle);
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
	void UpdateHeldGun(ASimPawnActor& Actor, const colony::ThingView& View);
	UStaticMesh* GunMesh(colony::DefId Weapon);
	void UpdatePlayerInput();
	void DrawHitboxes();
	void DrawHud();
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
	TMap<uint32, TObjectPtr<UStaticMesh>> GunMeshes;

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
	TArray<float> LightAges;
	TMap<uint32, FHeldGun> HeldGuns;
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
	bool bHaveKeyState = false;
	bool bNoGunMessage = false;
	float HitMarkerSeconds = 0.0f;
	float EquipMessageSeconds = 0.0f;

	int32 ShotsSeen = 0;
	int32 HitsSeen = 0;
	int32 PawnHitsSeen = 0;
	int32 Flashes = 0;
	int32 TracersSpawned = 0;
	int32 TracersArrived = 0;
	int32 TracersDropped = 0;
	int32 Impacts = 0;
	int32 ImpactsByKind[8] = {};
	int32 HitReactionsPlayed = 0;
	int32 BodyDecals = 0;
	int32 GroundDecals = 0;
	int32 RagdollsStarted = 0;
	int32 FireOrders = 0;
	int32 HitMarkers = 0;
	int32 HitboxesDrawn = 0;
	int32 HudFramesDrawn = 0;
};
