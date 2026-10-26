#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "AnimalSubsystem.h"

#include "ColonySim/Entity.h"

#include "AnimalActor.generated.h"

class USimAnimalSubsystem;
class USimSubsystem;
class USkeletalMeshComponent;
class UAnimSequence;
namespace colony { struct ThingView; }

/** What an animal is doing, from the snapshot (the sim has no pose of its own). */
enum class ESimAnimalPose : uint8
{
	Idle,
	Walk,
	Gallop,
	Eat,
	Sleep,
	Dead
};

/**
 * One sim animal on screen. Like ASimPawnActor it has no movement or collision: USimAnimalSubsystem
 * puts it where the sim says every frame. The body is a skeletal mesh playing one of the model's
 * clips (idle, walk, gallop, eat, death) as a single-node animation, so it needs no animation
 * blueprint. See docs/design/animal-life.md and assets/animal_art.json.
 */
UCLASS(NotBlueprintable)
class RIM3D_API ASimAnimalActor : public AActor
{
	GENERATED_BODY()

public:
	ASimAnimalActor();

	void Setup(const FSimAnimalModel& InModel, float InArtScale, colony::Entity InThing);

	/** Moves, turns, picks the pose and scales the animal to the sim's blended state. */
	void UpdateFromSim(const USimSubsystem& Bridge, const colony::ThingView& Now, const colony::ThingView* Before, float DeltaTime, int32 Speed);

	void SetDetail(ESimAnimalDetail InDetail);

	colony::Entity GetThing() const { return Thing; }
	ESimAnimalDetail GetDetail() const { return Detail; }
	ESimAnimalPose GetPose() const { return Pose; }
	USkeletalMeshComponent* GetBody() const { return Body; }
	/** The clip playing now (null before the first update). */
	const UAnimSequence* GetClip() const;
	/** The scale the body is drawn at (model scale from the art table, life stage, body size). */
	float GetDrawScale() const { return DrawScale; }

	/** The mesh's forward axis is this many degrees off the actor's (+X). */
	static constexpr float kMeshYawOffset = -90.0f;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USkeletalMeshComponent> Body;

	void SetPose(ESimAnimalPose NewPose, float PlayRate);

	FSimAnimalModel Model;
	colony::Entity Thing = colony::kNoEntity;
	ESimAnimalDetail Detail = ESimAnimalDetail::Near;
	ESimAnimalPose Pose = ESimAnimalPose::Idle;
	bool bPlaced = false;
	bool bPoseSet = false;
	float ArtScale = 1.0f;
	float DrawScale = 1.0f;
	float Yaw = 0.0f;
};
