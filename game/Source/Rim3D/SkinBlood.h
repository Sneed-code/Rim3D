#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "BloodSubsystem.h"

#include "SkinBlood.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class USkeletalMeshComponent;
class UTexture2D;
class UTextureRenderTarget2D;

/** One wound or splash painted into a body's skin (world cm, UBloodSubsystem fills it in). */
struct FSkinBloodPaint
{
	/** Where it is on the body. */
	FVector At = FVector::ZeroVector;
	/** The far end of a gash; equal to At for a hole or a splash. */
	FVector To = FVector::ZeroVector;
	/** Into the body (a bullet's path, or towards the bone): only skin near this line takes it, not the far side. */
	FVector Inward = FVector::ForwardVector;
	/** Radii in cm: the wound itself, blood soaked round it, spatter; and how far blood runs down from it. */
	float Core = 1.5f;
	float Soak = 0.0f;
	float Spatter = 0.0f;
	float Run = 0.0f;
	/** How dark its open core is (0 for a splash, 1 for a bullet hole). */
	float Hole = 0.0f;
	/** 0 to 1. */
	float Strength = 1.0f;
	/** How far from the line (cm along it) skin still counts as this side of the body. */
	float Depth = 6.0f;
	/** The noise pattern (streaks, spatter); below 0: a new one. A run painted again as it grows keeps its seed. */
	float Seed = -1.0f;
};

/**
 * Blood in the skin (docs/design/blood.md, "Blood in the skin"): each bloodied MakeHuman body near the camera gets a
 * blood mask in its skin's own UV space, which its skin material reads. Wounds, soak, spatter, runs down the body and
 * drops landing on it are painted into the mask on the GPU (M_SkinBloodPaint) with the body base's bind-pose position
 * map (T_SkinPos_<Base>), so the blood is part of the skin: it bends, stretches and moves with it, never slides or
 * floats, and seams match. Fresh blood is bright and glossy, dries dark and matte over Blood.DryMinutes, fades 3% every
 * 10 s (Blood.FadeStep) and goes when the pawn washes. Head wounds darken and wet the hair (its Tint).
 *
 * Bodies without a position map (MetaHumans, animals) keep UBloodSubsystem's body decals.
 *
 * Console: Blood.SkinPaint (0 falls back to decals), Blood.SkinMasks (how many bodies at once).
 */
UCLASS()
class RIM3D_API USkinBlood : public UObject
{
	GENERATED_BODY()

public:
	/** True if this body can take blood in its skin (on, assets there, a MakeHuman body with a position map). */
	bool CanPaint(const USkeletalMeshComponent* Mesh);

	/** Paints a mark into the body's skin. `Clock`: UBloodSubsystem's game-speed clock (seconds). */
	bool Paint(USkeletalMeshComponent* Mesh, const FSkinBloodPaint& Mark, const FBloodFluid& Fluid, float Clock);

	/** Fades, dries and drops masks. `SimBlood`: the sim's blood level on the body's pawn (below 0 when not known);
	 *  a fall to nothing (washed in water) wipes its mask. */
	void Tick(float Clock, TFunctionRef<float(const USkeletalMeshComponent*)> SimBlood);

	/** Wipes every mask (sim reload, the creator's Clean up). */
	void Reset();

	/** Blood soaked into the clothes over a body's wounds: up to 8 spots in the body's bind pose (xyz cm, w radius;
	 *  newest first) and how much is left of each, and how wet the newest still is. Clothes share the body's bind pose, so
	 *  M_GrimeOverlay on them draws the spots where the wounds are. Returns how many. */
	int32 GetClothSpots(const USkeletalMeshComponent* Body, FLinearColor OutSpots[8], float OutAmounts[8], float& OutWet) const;

	/** For smoke tests. */
	int32 GetPaints() const { return Paints; }
	int32 GetMasks() const { return Masks.Num(); }
	bool IsMissingAssets() const { return bTried && !PaintMaterial; }

private:
	struct FHairTint
	{
		TWeakObjectPtr<UMaterialInstanceDynamic> Material;
		FLinearColor Original = FLinearColor::White;
		FLinearColor Written = FLinearColor::White;
	};
	struct FClothSpot
	{
		FVector3f At = FVector3f::ZeroVector;
		float Radius = 0.0f;
		float Strength = 1.0f;
		float Clock = 0.0f;
	};
	struct FMask
	{
		TWeakObjectPtr<USkeletalMeshComponent> Mesh;
		UTexture2D* Positions = nullptr;
		UTextureRenderTarget2D* Targets[2] = {nullptr, nullptr};
		int32 Current = 0;
		float LastPaint = 0.0f;
		float NextFade = 0.0f;
		float HeadBlood = 0.0f;
		float LastSimBlood = -1.0f;
		FBloodFluid Fluid;
		TArray<FHairTint> Hair;
		TArray<FClothSpot> ClothSpots;  // oldest first, at most 8
	};

	bool EnsureAssets();
	UTexture2D* PositionsFor(const USkeletalMeshComponent* Mesh);
	FMask* FindOrAddMask(USkeletalMeshComponent* Mesh, float Clock);
	void DropMask(int32 Index);
	void ApplyToSkin(FMask& Mask, bool bOn);
	void ApplyToHair(FMask& Mask, bool bRestore);
	void Draw(FMask& Mask);
	UTextureRenderTarget2D* TakeTarget();

	UPROPERTY()
	TObjectPtr<UMaterialInterface> PaintBase;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> PaintMaterial;
	UPROPERTY()
	TMap<FName, TObjectPtr<UTexture2D>> PositionMaps;
	UPROPERTY()
	TArray<TObjectPtr<UTextureRenderTarget2D>> AllTargets;  // keeps every render target alive
	TArray<UTextureRenderTarget2D*> FreeTargets;
	TSet<FName> NoPositions;
	TArray<FMask> Masks;
	bool bTried = false;
	int32 Paints = 0;
	float LastClock = 0.0f;
};
