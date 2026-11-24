#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ColonySim/Entity.h"

#include "GoreOrgans.generated.h"

class AGoreGibActor;
class UProceduralMeshComponent;
class USkinnedMeshComponent;
class UMaterialInterface;

/** The organs and bones a burst trunk throws out (GoreOrganShapes.h draws them). */
enum class EGoreOrgan : uint8
{
	Heart,
	LungLeft,
	LungRight,
	Liver,
	Stomach,
	Kidney,
	Spleen,
	Windpipe,
	Spine,
	Ribs,
	Fat,
};

/**
 * One organ as a procedural mesh about its own middle, `Scale` times a grown man's (so every body gets the same organs
 * at its own size), in M_Organ (wet, with a clear coat; M_Gore until BloodArt has made it). `OutRest`: how high its
 * middle lies over the ground when it rests on its flattest side (cm); `OutRestAxis`: that side's axis in the mesh's
 * own frame (it comes to rest with this axis up or down). Cosmetic.
 */
UProceduralMeshComponent* GoreMakeOrganMesh(AActor* Owner, USceneComponent* Parent, EGoreOrgan Kind, float Scale, int32 Seed, float& OutRest, FVector& OutRestAxis);

/** M_Organ, else M_Gore, else M_BloodDrop (whichever is there). */
UMaterialInterface* GoreOrganMaterial();
/** M_Gore, else M_BloodDrop. */
UMaterialInterface* GoreFleshMaterial();

/**
 * A trunk bursts open (sneed, 2026-11-23: "torso gibbing needs to show organs, liver, lots of intestines, heart, lungs,
 * viscera, more blood, bone ... the balls of flesh should be chunks of the torso including breasts"): the torso's own
 * skin and clothes cut into slabs of flesh (GoreSliceTorso: breasts, belly, back, the genitals as a piece of their own),
 * the organs, ropes of intestine, a length of spine, ribs, fat and a few small bits, thrown out from `Centre` with a lot
 * of blood. `Meshes`: the body's drawn skinned meshes (skin and clothes); `SkinMesh` the bare body among them. `Up` the
 * body's up (the spine's way, organs above and guts below). Without readable meshes (a cooked build) the slabs are
 * left out and more small chunks stand in. `bOrgans` false (machine fluid, insect blood): only chunks. Returns how many
 * pieces flew.
 */
int32 GoreBurstTrunk(UWorld* World, const TArray<USkinnedMeshComponent*>& Meshes, const USkinnedMeshComponent* SkinMesh, const FVector& Centre,
	const FVector& Up, float BodyScale, float Strength, const FLinearColor& Skin, int32 Seed, colony::Entity Victim, bool bOrgans);

/**
 * The torso's skin and clothes cut into `Count` slabs (Voronoi cells round the breasts, belly, buttocks and points
 * spread over the rest) plus the genitals as one more: each slab copies its triangles of every mesh with their own
 * materials (skin, nipples, cloth), and the bare skin gets thickness: a raw inner face of muscle or fat and torn sides
 * with the fat layer under the skin. Each one an AGoreGibActor in `Out`, not yet thrown. False if the meshes' vertices
 * can't be read.
 */
bool GoreSliceTorso(UWorld* World, const TArray<USkinnedMeshComponent*>& Meshes, const USkinnedMeshComponent* SkinMesh, float BodyScale, int32 Count,
	int32 Seed, TArray<AGoreGibActor*>& Out, TArray<bool>& OutGenital);

/** Knock loose gore (gibs, organs, guts) along a bullet's line: a change of speed growing with `Damage`. */
void GorePushLine(UWorld* World, const FVector& From, const FVector& To, const FVector& Dir, float Damage);
/** Knock loose gore away from a blast or a blow at `At`, falling off to nothing at `RadiusCm`. */
void GorePushRadial(UWorld* World, const FVector& At, float RadiusCm, float Speed, const FVector& Bias = FVector::ZeroVector);

/**
 * A rope of intestine spilled from a burst trunk: a chain of points that falls, uncoils and drapes over the ground
 * (verlet: gravity, fixed lengths between the points, the sim's ground with friction), drawn as a tube of gut rebuilt
 * while it moves. Bullets, blasts and blows knock it about. Lies as long as gibs do, then shrinks away. Cosmetic.
 */
UCLASS(NotBlueprintable)
class RIM3D_API AGoreRopeActor : public AActor
{
	GENERATED_BODY()

public:
	AGoreRopeActor();

	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** `Nodes` world (the actor should sit where they are), `Velocity` per node (cm/s), `bLarge` the large intestine. */
	void Setup(const TArray<FVector>& Nodes, const TArray<FVector>& Velocity, bool bLarge, float Scale, int32 Seed);
	/** A change of speed for the points within `Radius` of `At` (all of them more weakly beyond). */
	void Push(const FVector& DeltaVelocity, const FVector& At, float Radius);
	bool IsAtRest() const { return bAtRest; }
	int32 GetNodeCount() const { return Pos.Num(); }
	/** The middle point (world). */
	FVector GetMiddle() const { return Pos.Num() > 0 ? Pos[Pos.Num() / 2] : GetActorLocation(); }
	/** Each rope in this world. */
	static void GetLive(const UWorld* World, TArray<AGoreRopeActor*>& Out);

private:
	void Rebuild(bool bFirst);

	UPROPERTY()
	TObjectPtr<USceneComponent> Root;
	UPROPERTY()
	TObjectPtr<UProceduralMeshComponent> Tube;
	TArray<FVector> Pos;
	TArray<FVector> Prev;
	float Segment = 6.0f;
	float Radius = 1.25f;
	float Scale = 1.0f;
	float Age = 0.0f;
	float Accum = 0.0f;
	float StillFor = 0.0f;
	int32 Seed = 0;
	bool bLarge = false;
	bool bAtRest = false;
	bool bLanded = false;
};
