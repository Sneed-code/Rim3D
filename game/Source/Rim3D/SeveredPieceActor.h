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
};
