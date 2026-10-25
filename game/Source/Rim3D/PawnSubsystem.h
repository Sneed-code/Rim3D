#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "PawnAnimInstance.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "PawnSubsystem.generated.h"

class ASimPawnActor;
class UAnimSequence;
class UPoseSearchDatabase;
class USimSubsystem;
class USkeletalMesh;
namespace colony { class Sim; }

/** Pawns at each detail level this frame (USimPawnSubsystem). */
struct FSimPawnDetailCounts
{
	int32 Full = 0;
	int32 Near = 0;
	int32 Far = 0;
};

/**
 * Draws the sim's pawns: one ASimPawnActor per pawn in the snapshot, spawned when a pawn
 * appears and removed when it leaves, moved every frame to the blended sim position. Picks each
 * pawn's animation detail by distance to the camera: full motion matching for the nearest
 * (Pawns.FullDetail, 20), loops updated 20 times a second out to Pawns.NearDistance (40 m),
 * 8 times a second beyond; strand hair only at full detail. Sim PawnAction events play short
 * action animations. See docs/design/pawns.md.
 * Console: Pawns.FullDetail, Pawns.NearDistance, Pawns.MetaHumans (0 mannequins, 1 MetaHumans),
 * Pawns.ShowDetail (coloured marks over heads: green full, yellow near, red far).
 */
UCLASS()
class RIM3D_API USimPawnSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Removes every pawn actor; they come back next frame (with the current look). */
	void RemoveAll();

	ASimPawnActor* FindPawn(colony::Entity Thing) const;
	/** Every pawn actor, by the sim entity's number (entt::to_integral). */
	const TMap<uint32, TObjectPtr<ASimPawnActor>>& GetPawns() const { return Pawns; }
	const FSimPawnDetailCounts& GetDetailCounts() const { return DetailCounts; }
	/** PawnAction events heard, and how many of them started an animation. */
	int32 GetActionEvents() const { return ActionEvents; }
	int32 GetActionsStarted() const { return ActionsStarted; }
	/** False if a needed asset (mesh, databases, loops) did not load; the log says which. */
	bool AreAssetsLoaded() const { return bAssetsLoaded; }

	USkeletalMesh* GetBodyMesh() const { return BodyMesh; }
	const TArray<TObjectPtr<const UPoseSearchDatabase>>& GetDatabases() const { return Databases; }
	UAnimSequence* GetIdleLoop() const { return IdleLoop; }
	UAnimSequence* GetWalkLoop() const { return WalkLoop; }
	UAnimSequence* GetRunLoop() const { return RunLoop; }
	float GetWalkLoopSpeed() const { return WalkLoopSpeed; }
	float GetRunLoopSpeed() const { return RunLoopSpeed; }
	UAnimSequence* GetActionAnimation(colony::PawnAction Action) const;
	UClass* GetMetaHumanClass() const { return MetaHumanClass; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void LoadAssets();
	void OnSimEvent(const colony::Event& Event);
	void AssignDetail(const FVector& CameraLocation);

	UPROPERTY()
	TMap<uint32, TObjectPtr<ASimPawnActor>> Pawns;

	UPROPERTY()
	TObjectPtr<USkeletalMesh> BodyMesh;

	UPROPERTY()
	TArray<TObjectPtr<const UPoseSearchDatabase>> Databases;

	UPROPERTY()
	TObjectPtr<UAnimSequence> IdleLoop;

	UPROPERTY()
	TObjectPtr<UAnimSequence> WalkLoop;

	UPROPERTY()
	TObjectPtr<UAnimSequence> RunLoop;

	UPROPERTY()
	TObjectPtr<UAnimSequence> CrouchPose;

	UPROPERTY()
	TObjectPtr<UClass> MetaHumanClass;

	float WalkLoopSpeed = 150.0f;
	float RunLoopSpeed = 400.0f;
	bool bAssetsTried = false;
	bool bAssetsLoaded = false;
	bool bMetaHumansShown = false;
	const colony::Sim* LastSim = nullptr;
	FDelegateHandle EventHandle;
	TWeakObjectPtr<USimSubsystem> EventSource;
	FSimPawnDetailCounts DetailCounts;
	int32 ActionEvents = 0;
	int32 ActionsStarted = 0;
};
