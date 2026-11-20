#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Events.h"

#include "BlastFxSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UPointLightComponent;
class UStaticMesh;
class USimSubsystem;

/**
 * What a blast looks like (docs/design/blast-draw.md): the sim's `Explosion` event drawn as a fireball, a white flash,
 * smoke that hangs for 10 to 20 seconds and drifts with the wind, dirt thrown up, a ring of dust racing out along the ground
 * (the shock wave), heat shimmer, glowing fragments, a scorch mark that fades over 5 minutes and a flash of light that
 * lights the scene. A fire blast (a molotov) is a splash of burning fuel and black smoke. All of it is cosmetic: the sim has
 * already decided who is hurt.
 *
 * Sprites are camera-facing squares in instanced meshes (one per kind), with per-instance data for the flipbook frame,
 * opacity, tint and glow (BlastFxData.h), sorted far to near each frame so the see-through smoke layers right. The art
 * comes from `-run=BlastArt`; without it `IsReady` is false and UShooterFeelSubsystem draws its old simple blast.
 *
 * Console: `Blast.Enable`, `Blast.FireBrightness`, `Blast.SmokeSeconds`, `Blast.ScorchSeconds`, `Blast.MaxSprites`.
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

	/** A puff of dust behind a piece of debris in flight (UShooterFeelSubsystem calls it). */
	void AddDebrisTrail(const FVector& At, float SizeCm);

	// For the smoke test and the screen.
	int32 GetBlastsDrawn() const { return BlastsDrawn; }
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
		FLinearColor Tint = FLinearColor(0.1f, 0.1f, 0.1f);
	};

	struct FScorch
	{
		FTransform Where;  // a 1 m plane, laid on the ground and scaled to the mark's size
		float Age = 0.0f;
		float Strength = 1.0f;
	};

	void OnSimEvent(const colony::Event& Event);
	void SpawnBlast(const FVector& Centre, const FVector& Ground, float RadiusM, bool bFire);
	void SpawnScorch(const FVector& Ground, const FVector& GroundNormal, float RadiusM);
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

	TArray<FSprite> Sprites;
	TArray<FScorch> Scorches;
	TWeakObjectPtr<USimSubsystem> Bridge;
	FDelegateHandle EventHandle;
	FRandomStream Random = FRandomStream(0xB1A57);
	int32 LastGeneration = -1;
	bool bTriedLoad = false;
	bool bReady = false;
	int32 BlastsDrawn = 0;
	// The light: a white-hot peak, then an orange glow dying away.
	FVector FlashAt = FVector::ZeroVector;
	float FlashAge = 99.0f;
	float FlashScale = 1.0f;
	bool bFlashFire = false;
	bool bLoggedScorchDraw = false;
	float FlashCandelas = 0.0f;
};
