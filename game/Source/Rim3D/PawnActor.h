#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "PawnActor.generated.h"

class USimPawnSubsystem;
class USimSubsystem;
class USkeletalMeshComponent;
class UStaticMeshComponent;
namespace colony { struct ThingView; }

/**
 * One sim pawn on screen. It has no movement component and no collision: USimPawnSubsystem puts
 * it where the sim says every frame and it only animates (docs/design/pawns.md). The body is the
 * sample's mannequin; with the MetaHuman look the mannequin is hidden and drives a MetaHuman's
 * body through the leader pose.
 */
UCLASS(NotBlueprintable)
class RIM3D_API ASimPawnActor : public AActor
{
	GENERATED_BODY()

public:
	ASimPawnActor();

	/** Called once after spawning. */
	void Setup(USimPawnSubsystem& Pawns, colony::Entity InThing, float InCruiseSpeed, bool bMetaHuman);

	/** Moves and turns the pawn to the sim's blended state and hands the animation its inputs.
	 *  `Speed` is the sim speed (0 paused, 1 to 3); `DeltaTime` is real seconds. */
	void UpdateFromSim(const USimSubsystem& Bridge, const colony::ThingView& Now, const colony::ThingView* Before,
		float DeltaTime, int32 Speed);

	/** Full or simple animation, and how often the mesh updates (0: every frame). Strand hair
	 *  shows only at full detail. */
	void SetDetail(ESimPawnDetail InDetail, float TickInterval);

	/** Plays the short animation for what the pawn did (picked up, put down). */
	void PlayAction(colony::PawnAction Action);

	/** Gives the pawn its seeded looks (docs/design/looks.md): height, and with the MetaHuman its body
	 *  shape, skin and hair colour; horns for lineages that have them. Does nothing if they are unchanged. */
	void ApplyLooks(USimPawnSubsystem& Pawns, const FSimPawnLooks& InLooks);
	bool HasLooks() const { return bHasLooks; }
	const FSimPawnLooks& GetLooks() const { return Looks; }
	/** The MetaHuman's meshes that carry the body shape morphs, and its skin meshes (body, face). */
	const TArray<TObjectPtr<USkeletalMeshComponent>>& GetShapedMeshes() const { return ShapedMeshes; }
	const TArray<TObjectPtr<USkeletalMeshComponent>>& GetSkinMeshes() const { return SkinMeshes; }
	const TArray<TObjectPtr<UStaticMeshComponent>>& GetHorns() const { return Horns; }

	/** Hides or shows the whole pawn, MetaHuman included (first person view hides the player's own body). */
	void SetBodyHidden(bool bHide);
	bool IsBodyHidden() const { return IsHidden(); }

	/** A hit (docs/design/fights.md): the pawn flinches, leaning away from the shot for about half a
	 *  second. `ShotDirection` is the way the bullet was travelling; `Strength` is 0 to 1. Cosmetic. */
	void PlayHitReaction(const FVector& ShotDirection, float Strength);
	int32 GetHitReactions() const { return HitReactions; }
	bool IsReacting() const { return ReactionAge < kHitReactionSeconds; }
	/** The lean now, in degrees (0 when still). */
	float GetLeanDegrees() const { return LeanDegrees; }

	/** Downed and dead pawns go limp: the mannequin simulates physics on a floor made under it, and the
	 *  MetaHuman follows it. `Impulse` is a velocity change in cm/s (the shot that did it). Cosmetic: the
	 *  sim already has the pawn lying prone where it fell. */
	void StartRagdoll(const FVector& Impulse);
	/** Gets up again: the animation takes over. */
	void StopRagdoll();
	bool IsRagdoll() const { return bRagdoll; }
	/** Where the ragdoll's pelvis is now (world cm); false if the pawn is not a ragdoll. */
	bool GetRagdollPelvis(FVector& OutLocation) const;
	int32 GetRagdollsStarted() const { return RagdollsStarted; }

	/** The mesh that wears wound decals: the MetaHuman's body if there is one, else the mannequin. */
	USkeletalMeshComponent* GetDecalMesh() const;

	static constexpr float kHitReactionSeconds = 0.55f;

	colony::Entity GetThing() const { return Thing; }
	ESimPawnDetail GetDetail() const { return Detail; }
	USkeletalMeshComponent* GetBody() const { return Body; }
	USimPawnAnimInstance* GetAnim() const;
	AActor* GetMetaHuman() const { return MetaHuman; }
	/** How many strand hair (groom) components are visible now. */
	int32 CountVisibleGrooms() const;
	/** Actions started, and whether one is playing now. */
	int32 GetActionsPlayed() const { return ActionsPlayed; }
	bool IsActionPlaying() const;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USkeletalMeshComponent> Body;

	UPROPERTY()
	TObjectPtr<AActor> MetaHuman;

	UPROPERTY()
	TArray<TObjectPtr<USceneComponent>> Grooms;

	UPROPERTY()
	TArray<TObjectPtr<USkeletalMeshComponent>> ShapedMeshes;

	UPROPERTY()
	TArray<TObjectPtr<USkeletalMeshComponent>> SkinMeshes;

	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Horns;

	/** The MetaHuman's own body (leader of its clothes), where horns go. */
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> MetaHumanBody;

	void SpawnRagdollFloor();
	void ApplyLean(float DeltaTime);

	/** An invisible slab at the ground under a ragdoll (the ground is drawn from the sim and has no collision). */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> RagdollFloor;

	FSimPawnLooks Looks;
	bool bHasLooks = false;
	bool bRagdoll = false;
	int32 RagdollsStarted = 0;
	float ReactionAge = kHitReactionSeconds;
	float ReactionStrength = 0.0f;
	FVector ReactionDirection = FVector::ForwardVector;
	float LeanDegrees = 0.0f;
	int32 HitReactions = 0;

	colony::Entity Thing = colony::kNoEntity;
	ESimPawnDetail Detail = ESimPawnDetail::Full;
	float CruiseSpeed = 400.0f;
	float Yaw = 0.0f;
	bool bPlaced = false;
	double ActionEndSeconds = 0.0;
	int32 ActionsPlayed = 0;
};
