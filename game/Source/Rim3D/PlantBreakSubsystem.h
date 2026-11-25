#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "PlantBreakSubsystem.generated.h"

class AActor;
class USimSubsystem;
class UStaticMeshComponent;
namespace colony { struct Event; }

/**
 * Cut grass and bushes break up (sneed 2026-11-25: "when I cut grass it gets destroyed and broken up and fades over 10 sec,
 * same with bush"), all cosmetic. When the sim takes a plant that is not a tree away (a PlantRemoved "cut" by a pawn's
 * cut-plants job or the work tool, "harvested" away, or "broken" by a blow, a shot or a blast), the chunk stops drawing it
 * and a handful of clippings take its place: smaller copies of the same mesh, tilted every way, thrown up and out a
 * little. They fall, bounce once, lie on the ground, then shrink and sink away until the 10 seconds are up (the foliage
 * materials can't fade, so they shrink). Trees fall instead (UTreeFallSubsystem).
 *
 * Cheap: a few components a plant, no shadows, at most PlantBreak.Max clippings at once (the oldest go first). Paused
 * game: they wait. A new game or a load clears them. Console: PlantBreak.Enabled, PlantBreak.Seconds, PlantBreak.Max.
 */
UCLASS()
class RIM3D_API UPlantBreakSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& InCollection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** For a smoke test: clippings alive now, and plants broken up so far. */
	int32 GetPieces() const { return Pieces.Num(); }
	int32 GetBreaksStarted() const { return BreaksStarted; }
	/** For a smoke test: plays a PlantRemoved event as if the sim had sent it. */
	void PlayEventForTest(const colony::Event& Event);

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FPiece
	{
		TWeakObjectPtr<UStaticMeshComponent> Mesh;
		FVector Location = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector SpinAxis = FVector::UpVector;
		float SpinRate = 0.0f;  // radians a second
		FVector Scale = FVector::OneVector;
		float Age = 0.0f;
		int32 Bounces = 0;
		bool bLanded = false;
	};

	void OnSimEvent(const colony::Event& Event);
	void EnsureScene();
	void Teardown();
	void RemovePiece(int32 Index);
	/** The ground's height (world, cm) under a world point, from the sim's map. */
	float GroundZ(const FVector& At) const;

	TWeakObjectPtr<USimSubsystem> Bridge;
	FDelegateHandle EventHandle;
	UPROPERTY()
	TObjectPtr<AActor> Holder;
	TArray<FPiece> Pieces;
	FRandomStream Random{0x47524153u};
	int32 BuiltGeneration = -1;
	int32 BreaksStarted = 0;
};
