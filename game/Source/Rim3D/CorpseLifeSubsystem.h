#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"

#include "CorpseLifeSubsystem.generated.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;
namespace colony { class Sim; }

/**
 * Flies and maggots on dead bodies (docs/design/spoilage.md "Corpses"). Cosmetic: the sim says how much of each a
 * body has (ThingView `flies`, `fliesFlying`, `maggots`, 0 to 1); this draws them on the body where its ragdoll lies.
 *
 * - Flies: up to Corpse.MaxFlies a body (scaled by the body's size). The share on the wing loops round the body in
 *   quick, jerky arcs and lands now and then; the rest sit on the body and take off again. In the cold they all sit.
 * - Maggots: up to Corpse.MaxMaggots a body, pale grubs crawling slowly over the top of it, swelling and shrinking.
 * - Only bodies within Corpse.Distance metres of the camera are drawn.
 *
 * The fly is the imported house fly (/Game/Rim3D/Generated/Insects/SM_HouseFly, made by the AnimalArt commandlet
 * from assets/raw/sketchfab); without it a small dark blob stands in. Maggots are stretched spheres.
 *
 * Console: Corpse.Life (0 off), Corpse.MaxFlies, Corpse.MaxMaggots, Corpse.Distance, Corpse.Debug.
 */
UCLASS()
class RIM3D_API UCorpseLifeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// For smoke tests.
	int32 GetBodiesDrawn() const { return BodiesDrawn; }
	int32 GetFliesDrawn() const { return FliesDrawn; }
	int32 GetMaggotsDrawn() const { return MaggotsDrawn; }
	bool HasRealFly() const { return bRealFly; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FFly
	{
		FVector Seat = FVector::ZeroVector;  // where it sits, from the body's centre (cm)
		FVector Pos = FVector::ZeroVector;   // where it is now, from the body's centre
		FVector Vel = FVector::ZeroVector;
		FVector Goal = FVector::ZeroVector;  // the point it is darting to
		float Retarget = 0.0f;               // seconds until it picks a new point
		float Switch = 0.0f;                 // seconds until it lands or takes off
		bool bFlying = false;
	};
	struct FMaggot
	{
		FVector Pos = FVector::ZeroVector;  // on the body's top, from its centre
		float Heading = 0.0f;
		float Phase = 0.0f;
		float Size = 1.0f;
	};
	struct FBody
	{
		TArray<FFly> Flies;
		TArray<FMaggot> Maggots;
		FVector Centre = FVector::ZeroVector;
		FVector Extent = FVector(40.0f, 40.0f, 10.0f);
		uint32 Seed = 0;
		uint64 SeenFrame = 0;
	};

	bool EnsureComponents();
	void Clear();
	bool BodyBounds(colony::Entity Thing, const FVector& Fallback, FVector& Centre, FVector& Extent) const;
	void StepFlies(FBody& Body, int32 Want, float FlyingShare, float Dt);
	void StepMaggots(FBody& Body, int32 Want, float Dt);

	UPROPERTY()
	TObjectPtr<AActor> Holder;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> FlyMeshes;
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> MaggotMeshes;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> FlyMaterial;
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> MaggotMaterial;

	TMap<uint32, FBody> Bodies;
	TArray<FTransform> FlyXforms;
	TArray<FTransform> MaggotXforms;
	FRandomStream Rand;
	uint64 Frame = 0;
	const colony::Sim* LastSim = nullptr;
	bool bTriedComponents = false;
	bool bRealFly = false;
	float FlyScale = 1.0f;  // the imported fly's mesh to a 7 mm fly
	int32 BodiesDrawn = 0;
	int32 FliesDrawn = 0;
	int32 MaggotsDrawn = 0;
};
