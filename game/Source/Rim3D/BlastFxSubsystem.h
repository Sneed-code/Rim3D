#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Events.h"

#include "BlastFxSubsystem.generated.h"

class AActor;
class ASimPawnActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UPointLightComponent;
class UStaticMesh;
class USimSubsystem;

/**
 * What a blast looks like (docs/design/blast-draw.md): the sim's `Explosion` event drawn as a fireball, a white flash,
 * smoke that hangs for 10 to 20 seconds and drifts with the wind, dirt thrown up, a ring of dust racing out along the ground
 * (the shock wave), heat shimmer, glowing fragments, a scorch mark that fades over 5 minutes and a flash of light that
 * lights the scene. A fire blast (a molotov) is liquid fire: the fuel splashes out in burning drops that trail flame and land
 * as small pools, the splash itself burns as a spreading carpet of flames for ten seconds or so under a column of black
 * smoke, lighting the ground with a flicker, and leaves a dark stain. Every fire in the game (burning ground, burning
 * bodies, campfires, torches) draws its flames here too (AddFlames). All of it is cosmetic: the sim has already decided who
 * is hurt and what burns.
 *
 * Sprites are camera-facing squares in instanced meshes (one per kind), with per-instance data for the flipbook frame,
 * opacity, tint and glow (BlastFxData.h), sorted far to near each frame so the see-through smoke layers right. The art
 * comes from `-run=BlastArt`; without it `IsReady` is false and UShooterFeelSubsystem draws its old simple blast.
 *
 * Console: `Blast.Enable`, `Blast.FireBrightness`, `Blast.SmokeSeconds`, `Blast.ScorchSeconds`, `Blast.MaxSprites`,
 * `Blast.FuelSeconds`, `Blast.PoolLight`, `Fire.Flipbook` (0: fires keep their simple flames).
 */
UCLASS()
class RIM3D_API UBlastFxSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** True once the art has loaded: blasts are drawn here, not by UShooterFeelSubsystem's simple look. */
	bool IsReady();

	/** Takes away every sprite, scorch and light (the smoke test, so an earlier blast's smoke doesn't hide the next). */
	void ClearForTest();

	/** A scorch mark alone at a map point, with no blast over it (the smoke test pictures it clear of smoke). */
	void AddScorchForTest(float MapX, float MapY, float RadiusM);

	/**
	 * What a projectile in flight leaves behind this frame (UShooterFeelSubsystem calls it): `Kind` is the Projectiles def's
	 * visual.trail ("rocket": exhaust smoke and a flame at the nozzle; "puff": a launcher shell's thin smoke; "flame": a
	 * molotov's burning rag). `Back` points the way it came from.
	 */
	void AddTrail(const FVector& At, const FVector& Back, const FString& Kind, float DeltaTime);

	/** A rocket's trail along From -> To as if it had just flown it in `Seconds` (the smoke test pictures the trail). */
	void AddTrailForTest(const FVector& From, const FVector& To, float Seconds);

	/** A puff of dust behind a piece of debris in flight (UShooterFeelSubsystem calls it). */
	void AddDebrisTrail(const FVector& At, float SizeCm);

	/** A bullet striking soil (sneed 2026-11-23: "realistic dirt and particle effects, not poofs"): a fast jet of soil
	 *  thrown up and falling back, with a low brown haze left hanging. `Strength` 1 for a rifle round. */
	void AddDirtImpact(const FVector& At, const FVector& Up, float Strength);
	int32 GetDirtImpacts() const { return DirtImpacts; }

	/**
	 * Flames for a fire this frame (UFireSubsystem calls it for every burning cell, burning pawn, campfire and torch, so all
	 * fire in the game looks the same): tall tongues of the photographed fire flipbook licking up from a disc `SpreadCm`
	 * across (and up to `HeightSpreadCm` above it, a burning body), and soot rising off it. `Size` the sim's fire size (0 to
	 * 1.75), `Scale` how big a flame is beside a cell fire's. False without the art (the caller draws its own simple flames).
	 */
	bool AddFlames(const FVector& At, float SpreadCm, float HeightSpreadCm, float Size, float Scale, bool bSmoke, float DeltaTime);
	/** Flame tongues and fire smoke alive now from AddFlames and burning fuel (the fire smoke test counts them). */
	int32 GetFlameTongues() const { return FlameTongues; }
	int32 GetFireSmoke() const { return FireSmoke; }
	/** Pools of burning fuel (a molotov's splash and the drops it threw), and burning drops in the air. */
	int32 GetFuelPools() const { return Pools.Num(); }
	int32 GetFuelDrops() const { return Drops.Num(); }

	// For the smoke test and the screen.
	int32 GetBlastsDrawn() const { return BlastsDrawn; }
	int32 GetRagdollsThrown() const { return RagdollsThrown; }
	/** A body that goes limp just after a blast near it (the blast killed or knocked it out) is thrown by it too:
	 *  throws it if a blast in the last `Seconds` reached where it lies. True if it did. */
	bool ThrowNewRagdoll(ASimPawnActor& Actor, double Seconds = 0.6);
	int32 GetPiecesThrown() const { return PiecesThrown; }
	int32 GetSpritesAlive() const { return Sprites.Num(); }
	int32 GetSmokeAlive() const;
	int32 GetScorchMarks() const { return Scorches.Num(); }
	float GetFlashCandelas() const { return FlashCandelas; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	enum class EKind : uint8
	{
		Fire,
		Smoke,
		Spark,
		Haze,
		Count,
	};

	struct FSprite
	{
		EKind Kind = EKind::Smoke;
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;       // negative: not born yet (staggered starts)
		float Life = 1.0f;
		float StartCm = 100.0f;  // size at birth and at the end
		float EndCm = 100.0f;
		float Gravity = 0.0f;    // cm/s^2, negative rises
		float Drag = 1.0f;       // per second
		float Wind = 0.0f;       // how much the wind carries it (0..1)
		float Roll = 0.0f;       // degrees, and its spin
		float Spin = 0.0f;
		float Opacity = 1.0f;
		float FadeIn = 0.1f;     // seconds
		float FireGain = 0.0f;
		float Glow = 0.0f;       // orange light from below, fading with age
		float Flip = 0.0f;
		float FrameFrom = 0.0f;  // flipbook frames over its life
		float FrameTo = 63.0f;
		/** Height over width: above 1 a tall tongue of flame, its long side along `Axis` (world up if zero) as seen. */
		float Stretch = 1.0f;
		FVector Axis = FVector::ZeroVector;
		/** 1: a flame tongue of a fire, 2: a fire's smoke (counted for the smoke test). */
		uint8 Source = 0;
		FLinearColor Tint = FLinearColor(0.1f, 0.1f, 0.1f);
	};

	/** Burning fuel lying on the ground: a carpet of low flames over a disc that spreads out, then dies down. */
	struct FFuelPool
	{
		FVector Centre = FVector::ZeroVector;
		float RadiusCm = 50.0f;
		float Age = 0.0f;
		float Life = 10.0f;
		float Strength = 1.0f;
		float Seed = 0.0f;
		/** Flames and smoke owed from earlier frames (fractions carried over). */
		float FlameDebt = 0.0f;
		float SmokeDebt = 0.0f;
		/** Smouldering debris (a blast's burning bits): smaller, smokier flames. */
		bool bSmoulder = false;
	};

	/** A burning blob in flight (fuel flung from a molotov, a burning bit of a blast) trailing flame; a small pool where it lands. */
	struct FFuelDrop
	{
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float PoolCm = 25.0f;
		float PoolLife = 4.0f;
		float Debt = 0.0f;
		bool bDebris = false;
	};

	struct FScorch
	{
		FTransform Where;  // a 1 m plane, laid on the ground and scaled to the mark's size
		float Age = 0.0f;
		float Strength = 1.0f;
	};

	void OnSimEvent(const colony::Event& Event);
	void SpawnBlast(const FVector& Centre, const FVector& Ground, float RadiusM, bool bFire);
	void SpawnScorch(const FVector& Ground, const FVector& GroundNormal, float RadiusM, float Strength = 1.0f);
	void AddPool(const FVector& Centre, float RadiusCm, float Life, float Strength, bool bSmoulder);
	void AddDrop(const FVector& At, const FVector& Velocity, float PoolCm, float PoolLife, bool bDebris);
	void UpdateFuel(float DeltaTime);
	/** One tongue of flame standing on `Base`, `WidthCm` wide, licking up and burning out in about half a second. */
	FSprite& AddTongue(const FVector& Base, float WidthCm, float FireGain);
	/** A puff of a fire's soot rising from `At`. */
	FSprite& AddFireSmoke(const FVector& At, float SizeCm, float Opacity);
	/** The ground's height under a world point (cm), from the sim's map. */
	double GroundZAt(const FVector& At) const;
	bool EnsureScene();
	void Teardown();
	FSprite& Add(EKind Kind);
	void UpdateSprites(float DeltaTime, float Wind);
	void UpdateLight(float DeltaTime);
	void UpdateScorches(float DeltaTime);

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Instances;

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> ScorchInstances;

	UPROPERTY()
	TObjectPtr<UPointLightComponent> FlashLight;

	/** The glow of the biggest burning pool, flickering. */
	UPROPERTY()
	TObjectPtr<UPointLightComponent> PoolLight;

	/** A rocket motor's light, carried along with the nearest rocket in flight. */
	UPROPERTY()
	TObjectPtr<UPointLightComponent> MotorLight;

	TArray<FSprite> Sprites;
	TArray<FScorch> Scorches;
	TArray<FFuelPool> Pools;
	TArray<FFuelDrop> Drops;
	int32 FlameTongues = 0;
	int32 FireSmoke = 0;
	float FuelClock = 0.0f;
	/** Seconds since a rocket last fed the motor light (it goes out a moment after). */
	float MotorAge = 99.0f;
	double MotorNearest = 0.0;
	TWeakObjectPtr<USimSubsystem> Bridge;
	FDelegateHandle EventHandle;
	FRandomStream Random = FRandomStream(0xB1A57);
	int32 LastGeneration = -1;
	bool bTriedLoad = false;
	int32 DirtImpacts = 0;
	bool bReady = false;
	int32 BlastsDrawn = 0;
	int32 RagdollsThrown = 0;
	struct FRecentBlast
	{
		FVector Centre = FVector::ZeroVector;
		double Reach = 0.0;
		float Speed = 0.0f;
		double At = 0.0;
	};
	TArray<FRecentBlast> RecentBlasts;
	int32 PiecesThrown = 0;
	// The light: a white-hot peak, then an orange glow dying away.
	FVector FlashAt = FVector::ZeroVector;
	float FlashAge = 99.0f;
	float FlashScale = 1.0f;
	bool bFlashFire = false;
	bool bLoggedScorchDraw = false;
	float FlashCandelas = 0.0f;
};
