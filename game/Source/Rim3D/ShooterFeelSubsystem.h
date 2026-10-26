#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "ShooterFeelSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UPointLightComponent;
class UStaticMesh;
class UStaticMeshComponent;
class USimSubsystem;
namespace colony { class Sim; struct ThingView; }

/** What the player's camera is nudged by this frame, on top of where the aim puts it (docs/design/shooter-feel-draw.md). */
struct FCameraFeel
{
	/** Degrees: the gun's kick (the sim's `kickUp`, `kickSide`), the shake and the dip of a roll. Added to the view only; the
	 *  crosshair and the shots keep the aim without it. */
	FRotator Rotation = FRotator::ZeroRotator;
	/** Centimetres, in the camera's own space: shake, and the camera ducking in a roll. */
	FVector Location = FVector::ZeroVector;
	/** Post-process: motion blur amount (0.5 is the engine's default), vignette and chromatic fringe pulses. */
	float MotionBlur = 0.5f;
	float Vignette = 0.0f;
	float Fringe = 0.0f;
};

/** A bit of debris from a blast (cosmetic Chaos rigid body). */
struct FFeelDebris
{
	TWeakObjectPtr<UStaticMeshComponent> Piece;
	FVector Start = FVector::ZeroVector;
	float Age = 0.0f;
};

/**
 * Shooter feel on screen (docs/design/shooter-feel-draw.md, task U21): what the sim's recoil kick, shots, hits, blasts and
 * dodge rolls (P2, F9) look like and feel like. All of it is cosmetic; the sim decides what is hit, and the camera
 * offsets here never touch the aim.
 *
 * - **Camera feel** (`GetCameraFeel`, read by AProtagonistPawn every frame): the gun's kick (the snapshot's `kickUp` and
 *   `kickSide`, lifting the view while the crosshair, and so the bullet, stay where the player aims), camera shake started
 *   by the sim's events (shots, being hit, being knocked down, blasts by distance), the camera ducking in a dodge roll, and
 *   post-process pulses (motion blur, vignette, chromatic fringe) near blasts and in rolls.
 * - **Grenades and blasts**: a grenade in the air or on the ground is drawn from the snapshot's projectiles; an `Explosion`
 *   event makes a fireball, smoke, sparks, dust, a short flash of light and debris (small rigid bodies that Chaos moves over an
 *   invisible floor made under the blast, for a few seconds). Debris is for looks only.
 * - **Settings**: `Feel.Shake`, `Feel.Blur`, `Feel.Kick` (0 turns each off, 1 is the default), `Feel.AimView`, `Feel.AimFov`,
 *   `Feel.Debris`; the overseer view has a "Feel" panel with sliders for them. They are kept in the user settings file.
 */
UCLASS()
class RIM3D_API UShooterFeelSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** The camera's nudges for this frame (worked out once a frame, however many ask). */
	const FCameraFeel& GetCameraFeel();

	/** The settings (also the console variables Feel.*). */
	static float GetShakeScale();
	static float GetBlurScale();
	static float GetKickScale();
	static bool IsAimFirstPerson();
	static float GetAimFov();

	/** Draws the settings panel with ImGui (the overseer calls this inside its own ImGui scope). */
	void DrawSettingsPanel(float ViewportWidth, float Scale);
	/** Saves the settings to the user settings file. */
	static void SaveSettings();

	/** Starts a shake by hand (tests, and the sim's events do it): degrees at the peak and seconds it lasts. */
	void AddShake(float Degrees, float Seconds, float Frequency = 22.0f);
	/** The size of the shake right now (degrees of the largest part of the offset). */
	float GetShakeNow() const { return ShakeNow; }
	float GetKickPitch() const { return KickPitch; }
	float GetKickYaw() const { return KickYaw; }

	// --- Grenades and blasts, for the smoke test and the screen ---

	int32 GetGrenadesDrawn() const { return GrenadesDrawn; }
	int32 GetExplosionsSeen() const { return ExplosionsSeen; }
	int32 GetShotsFelt() const { return ShotsFelt; }
	int32 GetHitsFelt() const { return HitsFelt; }
	int32 GetBlastParticles() const { return Particles.Num(); }
	int32 GetDebrisAlive() const;
	int32 GetDebrisMade() const { return DebrisMade; }
	/** Debris pieces' heights above their blast's ground, cm: the lowest and the highest reached so far. */
	float GetDebrisLowestCm() const { return DebrisLowestCm; }
	float GetDebrisHighestCm() const { return DebrisHighestCm; }
	/** The farthest a piece of debris has been from where its blast went off, cm. */
	float GetDebrisFarthestCm() const { return DebrisFarthestCm; }
	float GetFlashIntensity() const { return FlashIntensity; }
	/** The camera's motion blur now. */
	float GetBlurNow() const { return Feel.MotionBlur; }
	float GetLargestShakeSeen() const { return LargestShake; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	enum class EBlastParticle : uint8
	{
		FireCore,
		FireOuter,
		Smoke,
		Spark,
		Dust,
		Grenade,
		Count,
	};

	struct FShake
	{
		float Age = 0.0f;
		float Duration = 0.2f;
		float Degrees = 1.0f;
		float Frequency = 22.0f;
		float Seed = 0.0f;
	};

	struct FParticle
	{
		EBlastParticle Kind = EBlastParticle::FireCore;
		FVector Position = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		float Age = 0.0f;
		float Life = 1.0f;
		float SizeCm = 30.0f;
		float Gravity = 0.0f;
		float Seed = 0.0f;
	};

	void OnSimEvent(const colony::Event& Event);
	void OnExplosion(const colony::Event& Event);
	void EnsureScene();
	void Teardown();
	UStaticMesh* MakeSolid(int32 Shape, float X, float Y, float Z, const FLinearColor& Color);
	UInstancedStaticMeshComponent* MakeInstanced(UStaticMesh* Mesh);
	void Advance(float DeltaTime);
	void UpdateKick(float DeltaTime, const colony::Sim& Sim, const USimSubsystem& Bridge);
	void UpdateParticles(float DeltaTime, const USimSubsystem& Bridge);
	void UpdateDebris(float DeltaTime);
	void UpdateFlash(float DeltaTime);
	void SpawnDebris(const FVector& Centre, float RadiusM, const FVector& GroundNormal);
	void Burst(EBlastParticle Kind, const FVector& At, int32 Count, float Speed, float UpBias, float SizeCm, float Life, float Gravity);
	colony::Entity ControlledPawn() const;

	UPROPERTY()
	TObjectPtr<AActor> Holder;

	UPROPERTY()
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ParticleInstances;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMesh>> KeepMeshes;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SolidMaterial;

	UPROPERTY()
	TObjectPtr<UStaticMesh> DebrisMesh;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;

	UPROPERTY()
	TArray<TObjectPtr<UPointLightComponent>> FlashLights;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Floors;

	TArray<FFeelDebris> Debris;
	TArray<float> FloorAges;
	TArray<FShake> Shakes;
	TArray<FParticle> Particles;
	FCameraFeel Feel;
	FRandomStream Random = FRandomStream(0x5400);
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> Bridge;
	uint64 FeelFrame = ~0ULL;
	int32 LastGeneration = -1;
	float KickPitch = 0.0f;
	float KickYaw = 0.0f;
	float ShakeNow = 0.0f;
	float LargestShake = 0.0f;
	float RollDip = 0.0f;
	float BlurPulse = 0.0f;
	float VignettePulse = 0.0f;
	float FlashIntensity = 0.0f;
	float FlashAge = 99.0f;
	float FlashPeak = 0.0f;
	FVector FlashAt = FVector::ZeroVector;
	int32 GrenadesDrawn = 0;
	int32 ExplosionsSeen = 0;
	int32 ShotsFelt = 0;
	int32 HitsFelt = 0;
	int32 DebrisMade = 0;
	float DebrisLowestCm = 0.0f;
	float DebrisHighestCm = 0.0f;
	float DebrisFarthestCm = 0.0f;
};
