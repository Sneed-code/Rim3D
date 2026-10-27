#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/Entity.h"

#include "AnimalSubsystem.generated.h"

class ASimAnimalActor;
class UAnimSequence;
class USkeletalMesh;
namespace colony { class Sim; }

/** How far an animal is from the camera decides how often it animates (as pawns do, docs/design/pawns.md). */
enum class ESimAnimalDetail : uint8
{
	Near,    // animation every frame
	Mid,     // updated 8 times a second
	Far,     // pose frozen, no skeleton update; the mesh is still skinned on the GPU
	Hidden   // too far to see anything but a speck
};

/** One animal model from assets/animal_art.json: the skeletal mesh and the clips it needs. */
struct FSimAnimalModel
{
	TObjectPtr<USkeletalMesh> Mesh;
	TObjectPtr<UAnimSequence> Idle;
	TObjectPtr<UAnimSequence> Walk;
	TObjectPtr<UAnimSequence> Gallop;
	TObjectPtr<UAnimSequence> Eat;
	TObjectPtr<UAnimSequence> Death;
	/** Half the model's size in cm, from its bounds (for the log and the tests). */
	FVector Extent = FVector::ZeroVector;
	/** Scale that makes the imported mesh as long as the animal really is (`lengthM` in the art table). */
	float FitScale = 1.0f;
	bool bLoaded = false;
};

/** Which model and size a def uses (assets/animal_art.json). */
struct FSimAnimalArt
{
	FName Model;
	float Scale = 1.0f;
	bool bStandIn = false;
};

/** Animals at each detail level this frame (USimAnimalSubsystem). */
struct FSimAnimalDetailCounts
{
	int32 Near = 0;
	int32 Mid = 0;
	int32 Far = 0;
	int32 Hidden = 0;
};

/**
 * Draws the sim's animals (task U15, docs/design/animal-life.md): one ASimAnimalActor per animal
 * in the snapshot (a pawn whose life stage is 0 to 2), on the skeletal mesh and clips A4 imported
 * for its def. Animation cost goes by distance to the camera: every frame within Animals.NearDistance
 * (40 m), 8 times a second within Animals.MidDistance (120 m), a frozen pose within
 * Animals.HideDistance (400 m), nothing beyond.
 * Console: Animals.NearDistance, Animals.MidDistance, Animals.HideDistance.
 */
UCLASS()
class RIM3D_API USimAnimalSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	void RemoveAll();

	ASimAnimalActor* FindAnimal(colony::Entity Thing) const;
	const TMap<uint32, TObjectPtr<ASimAnimalActor>>& GetAnimals() const { return Animals; }
	const FSimAnimalDetailCounts& GetDetailCounts() const { return DetailCounts; }

	/** The model a def is drawn with, or null (the log says why once). Loads it on first use. */
	const FSimAnimalModel* GetModelFor(FName DefName, FSimAnimalArt& OutArt);
	/** Models loaded so far. */
	int32 GetModelsLoaded() const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void LoadArtTable();
	void AssignDetail(const FVector& CameraLocation);

	UPROPERTY()
	TMap<uint32, TObjectPtr<ASimAnimalActor>> Animals;

	TMap<FName, FSimAnimalArt> Art;
	TMap<FName, float> LengthsCm;
	TMap<FName, FSimAnimalModel> Models;

	/** Keeps every loaded mesh and clip alive (the models above are not a UPROPERTY). */
	UPROPERTY()
	TArray<TObjectPtr<UObject>> KeepAlive;

	TSet<FName> Warned;
	bool bArtTried = false;
	const colony::Sim* LastSim = nullptr;
	FSimAnimalDetailCounts DetailCounts;
};
