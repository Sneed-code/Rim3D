#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "MeleePose.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "PawnActor.generated.h"

class USimPawnSubsystem;
class USimSubsystem;
class USkeletalMeshComponent;
class UMeshComponent;
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

	/** Dresses the MetaHuman in what the sim says it wears (docs/design/clothes-draw.md): Kellan's own top and trousers are put
	 *  away, a bare skin layer (the pawn's skin colour) shows what nothing covers, and each worn piece's meshes go on over it, in
	 *  the order they were put on, tinted by the stuff and torn by the piece's hit points. Pieces that left the list are
	 *  removed. `Key` is USimPawnSubsystem::ApparelKey of the snapshot. Mannequins just remember the key. */
	void ApplyApparel(USimPawnSubsystem& Pawns, const FSimPawnApparel& Wanted, uint64 Key);
	bool HasApparel() const { return bApparelApplied; }
	uint64 GetApparelKey() const { return ApparelKey; }
	const FSimPawnApparel& GetApparel() const { return Apparel; }
	/** The meshes of the worn pieces now (skinned garments and head props), and the item each belongs to. */
	const TArray<TObjectPtr<UMeshComponent>>& GetWornMeshes() const { return WornMeshes; }
	const TArray<uint32>& GetWornMeshItems() const { return WornMeshItems; }
	/** The bare skin layer (torso and arms, legs); empty while the clothes assets are missing. */
	const TArray<TObjectPtr<USkeletalMeshComponent>>& GetBareLayer() const { return BareLayer; }
	/** Kellan's own top and trousers (hidden while the clothes are drawn); null for a mannequin. */
	USkeletalMeshComponent* GetDefaultTop() const { return DefaultTop; }
	USkeletalMeshComponent* GetDefaultTrousers() const { return DefaultTrousers; }
	/** Whether the hair is hidden under a hat that covers it. */
	bool IsHairHidden() const { return bHairHidden; }
	/** How many times the worn meshes were remade (not just retinted). */
	int32 GetApparelRebuilds() const { return ApparelRebuilds; }

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

	// --- Melee on screen (docs/design/melee-draw.md). All cosmetic: the sim decides who is hit. ---

	/** The sim began a swing; the hit moment comes `StepsToHit` sim steps later. The body draws back and
	 *  strikes so that the blow lands on that step. */
	void BeginMeleeSwing(EMeleeStyle Style, float StepsToHit);
	/** The sim's MeleeHit: the blow lands now (if it has not on screen yet) and the follow-through starts. */
	void MeleeHitMoment();
	/** A sidestep toward the world point: the body dips and tips that way for a third of a second. */
	void PlayDodge(const FVector& TowardWorld);
	/** Freezes the swing, the animation and the flinch for `Seconds` of real time (the hit-stop). */
	void HitStop(float Seconds);
	bool IsHitStopped() const { return HitStopLeft > 0.0f; }
	const FMeleeSwingTrack& GetSwing() const { return Swing; }
	/** 0 to 1: how far the weapon has come up to block. */
	float GetBlockBlend() const { return BlockBlend; }
	/** The dodge roll's somersault (U21): 0 when not rolling, then how far through the sim's 27 steps the roll is, and the angle the
	 *  body has turned over its way, 0 to 360 (docs/design/shooter-feel-draw.md). The sim moves the body; this only turns it. */
	float GetRollProgress() const { return RollProgress; }
	float GetRollAngle() const { return RollAngle; }
	int32 GetRollsSeen() const { return RollsSeen; }
	/** 1 standing, 0.66 crouching, 0.24 lying: what weapon heights are scaled by. */
	float GetStanceHeight() const { return StanceHeight; }
	/** Where the held melee weapon is now, in this actor's space (cm, degrees): the swing or the block,
	 *  the dodge and the body's twist and lunge together. */
	void GetMeleeWeaponPose(FVector& OutLocation, FRotator& OutRotation) const;
	/** How far the body is bent by melee now (lean and tilt together, degrees), for tests. */
	float GetMeleeBodyDegrees() const;
	int32 GetDodgesPlayed() const { return DodgesPlayed; }
	int32 GetHitStops() const { return HitStops; }
	bool IsDodging() const { return DodgeAge < kDodgeSeconds; }
	static constexpr float kDodgeSeconds = 0.35f;

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

	void EnsureBareLayer(USimPawnSubsystem& Pawns);
	void DestroyWorn();
	/** Colour, wear and morphs of a garment mesh. */
	void StyleWornMesh(UMeshComponent* Mesh, const FSimWornPiece& Piece);
	void UpdateHairVisibility();

	UPROPERTY()
	TArray<TObjectPtr<UMeshComponent>> WornMeshes;

	TArray<uint32> WornMeshItems;

	UPROPERTY()
	TArray<TObjectPtr<USkeletalMeshComponent>> BareLayer;

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> DefaultTop;

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> DefaultTrousers;

	FSimPawnApparel Apparel;
	uint64 ApparelKey = 0;
	bool bApparelApplied = false;
	bool bBareLayerTried = false;
	bool bHairHidden = false;
	int32 ApparelRebuilds = 0;

	void SpawnRagdollFloor();
	/** The hit reaction's lean and the melee pose, applied to the body together. */
	void ApplyBodyPose(float GameDelta, bool bBlocking);
	/** The swing or block, plus the dodge: what the body and the weapon do now. */
	FMeleePose ComputeMeleePose() const;

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

	FMeleeSwingTrack Swing;
	float BlockBlend = 0.0f;
	float StanceHeight = 1.0f;
	float DodgeAge = kDodgeSeconds;
	float DodgeSide = 1.0f;
	float HitStopLeft = 0.0f;
	/** Real seconds the sim has disagreed with the swing on screen (a swing the sim called off). */
	float SwingMismatch = 0.0f;
	bool bBodyPosed = false;
	float RollProgress = 0.0f;
	float RollAngle = 0.0f;
	FVector RollDirection = FVector::ForwardVector;
	int32 RollsSeen = 0;
	int32 DodgesPlayed = 0;
	int32 HitStops = 0;

	colony::Entity Thing = colony::kNoEntity;
	ESimPawnDetail Detail = ESimPawnDetail::Full;
	float CruiseSpeed = 400.0f;
	float Yaw = 0.0f;
	bool bPlaced = false;
	double ActionEndSeconds = 0.0;
	int32 ActionsPlayed = 0;
};
