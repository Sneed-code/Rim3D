#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ColonySim/Entity.h"

#include "GoreGibActor.generated.h"

class UProceduralMeshComponent;

/**
 * One chunk of a body whose trunk burst (docs/design/body-health.md part F, "Trunks burst"): a lump of meat, a bone
 * shard, a rib or a dark wet lump (GoreMakeGibChunk), thrown out from where the trunk lay. It flies by its own small
 * sums like a severed piece (the drawn ground has no physics shape): gravity, bounces on the sim's ground, tumbling,
 * a splat of blood where it first comes down. It lies as long as blood does (`Gore.GibSeconds`, 5 minutes, sneed's
 * blood fade) and then shrinks away. At most `Gore.GibMax` lie at once (the oldest go first). Cosmetic: the sim
 * keeps no gibs.
 */
UCLASS(NotBlueprintable)
class RIM3D_API AGoreGibActor : public AActor
{
	GENERATED_BODY()

public:
	AGoreGibActor();

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Thrown: a speed in cm/s and a spin in degrees a second. `InRadius`: how far its middle rests over the ground, cm.
	 *  `bInSplat`: it leaves a splat of blood where it first lands. */
	void Throw(const FVector& InVelocity, const FRotator& InSpin, float InRadius, bool bInSplat);
	/** Knocked by a bullet or blast: a change of speed in cm/s; one at rest flies again. */
	void Push(const FVector& DeltaVelocity);
	void SetChunk(UProceduralMeshComponent* InChunk) { Chunk = InChunk; }
	bool IsAtRest() const { return bAtRest; }
	bool HasLanded() const { return bLanded; }

	/** How many gibs lie in this world now (for the tests and the cap). */
	static int32 CountLive(const UWorld* World);

	/**
	 * A trunk bursts: `Count` (Gore.GibCount) chunks thrown out from `Centre` (world cm), harder for a bigger `Strength`
	 * (about 1; the damage that burst it over 25), sized by `BodyScale`, with sprays of the victim's blood all round.
	 * Returns how many chunks were made.
	 */
	static int32 Burst(UWorld* World, const FVector& Centre, float BodyScale, float Strength, const FLinearColor& Skin, int32 Seed, colony::Entity Victim);

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Chunk;
	FVector Velocity = FVector::ZeroVector;
	FRotator Spin = FRotator::ZeroRotator;
	float Radius = 4.0f;
	float Age = 0.0f;
	bool bAtRest = false;
	bool bLanded = false;
	bool bSplat = false;
};
