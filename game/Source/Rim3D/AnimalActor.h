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
	Dead,
	Attack,  // a melee strike (models with an Attack clip), played once
	Shoot    // a shot (models with a Shoot clip), played once
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
	/** About how long the drawn body is, nose to tail, in cm (for the tests). */
	float GetDrawLengthCm() const;
	const UAnimSequence* GetClip() const;
	/** The scale the body is drawn at (model scale from the art table, life stage, body size). */
	float GetDrawScale() const { return DrawScale; }

	/** The Quaternius meshes' forward axis is this many degrees off the actor's (+X); other models set their own. */
	static constexpr float kMeshYawOffset = -90.0f;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USkeletalMeshComponent> Body;

	void SetPose(ESimAnimalPose NewPose, float PlayRate);
	/** Turns the mesh to face forward, or onto its side for a dead model with no death clip. */
	void PlaceBody(bool bOnSide);
	/** For a `"fit": "pose"` model: sets ArtScale from the skeleton's reach in the idle pose. */
	void FitToPose(float InArtScale);

	FSimAnimalModel Model;
	colony::Entity Thing = colony::kNoEntity;
	ESimAnimalDetail Detail = ESimAnimalDetail::Near;
	ESimAnimalPose Pose = ESimAnimalPose::Idle;
	bool bPlaced = false;
	bool bPoseSet = false;
	float ArtScale = 1.0f;
	float DrawScale = 1.0f;
	/** For a `"recentre"` model: the middle of its skeleton across the ground in the idle pose (mesh space, cm), which
	 *  the body is moved back by so it stands over the actor; zero for the rest. */
	FVector PoseCentre = FVector::ZeroVector;
	/** True while the body lies on its side (PlaceBody). */
	bool bBodyOnSide = false;
	/** The body's place under the actor: zero, or back by PoseCentre at the drawn scale. */
	FVector BodyOffset(const FQuat& Facing) const;
	float Yaw = 0.0f;
	float SwimLiftCm = 0.0f;
	/** Game time a one-shot fight clip (Attack, Shoot) began, and how long it lasts; -1 for none under way. */
	float OneShotStart = -1.0f;
	float OneShotLength = 0.0f;
	/** Half the posed skeleton's size at scale 1 (a pose-fitted model), else zero: the mesh's bounds are used. */
	FVector PoseHalfSize = FVector::ZeroVector;
};
