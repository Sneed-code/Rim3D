#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "SeveredPieceActor.generated.h"

class UPoseableMeshComponent;

/**
 * A limb or a head that came off (docs/design/body-health.md part F, "Gore"): copies of the pawn's meshes with
 * every bone outside the piece flattened or folded into the cut, its open end capped with flesh and bone, thrown
 * clear and left to tumble to the ground. Cosmetic: the sim only knows the piece exists and where its body was
 * (Snapshot::severedPieces); the fight subsystem makes and removes these. It flies by its own small sums (gravity, a bounce on the sim's ground), because the drawn ground has no
 * physics shape to land on.
 */
UCLASS(NotBlueprintable)
class RIM3D_API ASeveredPieceActor : public AActor
{
	GENERATED_BODY()

public:
	ASeveredPieceActor();

	virtual void Tick(float DeltaSeconds) override;

	/** Thrown: a speed in cm/s and a spin in degrees a second. `Radius`: how far its middle rests over the ground, cm. */
	void Throw(const FVector& InVelocity, const FRotator& InSpin, float InRadius);
	/** Knocked by a bullet, a blow or a blast (sneed, 2026-11-22: severed parts must obey physics): a change of speed in
	 *  cm/s, applied at `At` (world) so an off-centre hit sets it spinning; a piece at rest wakes and flies again. */
	void Push(const FVector& DeltaVelocity, const FVector& At);
	/** Roughly how far the piece reaches from its middle, cm (a bullet's line passing this close knocks it). */
	float GetReachCm() const { return FMath::Max(Radius * 2.5f, 12.0f); }
	int32 GetPushes() const { return Pushes; }
	void AddMesh(UPoseableMeshComponent* Mesh) { Meshes.Add(Mesh); }
	int32 GetMeshCount() const { return Meshes.Num(); }
	bool IsAtRest() const { return bAtRest; }
	/** The way a limb runs, in the piece's own space: it comes to rest lying along the ground, not standing on its end. */
	void SetLimbAxis(const FVector& InAxis) { LimbAxis = InAxis.GetSafeNormal(); }
	/** The open end (its stump cap): blood drips from there for a while once the piece has come down. */
	void SetCutEnd(USceneComponent* InEnd) { CutEnd = InEnd; }
	const USceneComponent* GetCutEnd() const { return CutEnd; }

private:
	UPROPERTY()
	TObjectPtr<USceneComponent> Root;
	UPROPERTY()
	TArray<TObjectPtr<UPoseableMeshComponent>> Meshes;
	UPROPERTY()
	TObjectPtr<USceneComponent> CutEnd;
	FVector Velocity = FVector::ZeroVector;
	FRotator Spin = FRotator::ZeroRotator;
	FVector LimbAxis = FVector::ZeroVector;
	float Radius = 8.0f;
	bool bAtRest = false;
	bool bLanded = false;
	int32 Pushes = 0;
};
