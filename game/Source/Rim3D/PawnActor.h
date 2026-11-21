#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "DrawnHit.h"
#include "MeleePose.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include "PawnActor.generated.h"

class UAnimSequence;
class USimPawnSubsystem;
class USimSubsystem;
class USkeletalMeshComponent;
class UMeshComponent;
class UStaticMeshComponent;
class UDecalComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UTexture;
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

	/** A work clip on a loop (felling a tree: UWorkToolSubsystem) in the action slot until StopWorkLoop; it takes over
	 *  from a short action and no action plays over it. Playing the same clip again does nothing. */
	void SetWorkLoop(UAnimSequence* Clip, float Rate);
	void StopWorkLoop();
	bool HasWorkLoop() const { return WorkLoopClip.IsValid(); }

	/** A jump (the player's body, Space): the body goes up and down in an arc over kJumpSeconds of game time while the Game
	 *  Animation Sample's jump plays (take-off by speed, the fall loop, the landing). Only the picture: the sim keeps the
	 *  pawn on the ground. False if it is already in the air, lying or ragdolled. */
	bool StartJump();
	bool IsJumping() const { return JumpAge >= 0.0f; }
	/** How high the body is off the ground now, cm. A cat-strain jumps twice as high (and longer, as a thrown body would). */
	float GetJumpHeight() const;
	int32 GetJumpsPlayed() const { return JumpsPlayed; }
	/** How fast the pawn moves over the ground now (the sim's, blended), cm per second at 1x. */
	float GetGroundSpeed() const { return GroundSpeed; }
	static constexpr float kJumpHeight = 45.0f;
	static constexpr float kJumpSeconds = 0.62f;

	/** Gives the pawn its seeded looks (docs/design/looks.md): height, and with the MetaHuman its body
	 *  shape, skin and hair colour; horns for lineages that have them. Does nothing if they are unchanged. */
	void ApplyLooks(USimPawnSubsystem& Pawns, const FSimPawnLooks& InLooks);
	bool HasLooks() const { return bHasLooks; }
	const FSimPawnLooks& GetLooks() const { return Looks; }
	/** The MetaHuman's meshes that carry the body shape morphs, and its skin meshes (body, face). */
	const TArray<TObjectPtr<USkeletalMeshComponent>>& GetShapedMeshes() const { return ShapedMeshes; }
	const TArray<TObjectPtr<USkeletalMeshComponent>>& GetSkinMeshes() const { return SkinMeshes; }
	const TArray<TObjectPtr<UStaticMeshComponent>>& GetHorns() const { return Horns; }
	/** V2 (docs/design/looks.md): the MetaHuman's face with the face shape morphs, null without the PawnFaces assets. */
	USkeletalMeshComponent* GetFaceMesh() const { return FaceMesh; }
	/** The MakeHuman character's mesh while the looks ask for one (docs/design/makehuman.md); null otherwise. */
	USkeletalMeshComponent* GetMakeHumanMesh() const { return MakeHumanMesh; }
	/** The MetaHuman's body (with the whole body, all of its skin); null for a mannequin. */
	USkeletalMeshComponent* GetSkinBody() const { return MetaHumanBody; }
	/** The eye slots of the face (left, right). */
	const TArray<int32>& GetEyeSlots() const { return EyeSlots; }
	/** The groom a groom component (Hair, Eyebrows, Mustache, Beard, Eyelashes) draws now: the asset's name, empty for none. */
	FString GetGroomName(FName Component) const;
	/** V3: the tattoo, scar and body hair decals on the skin (made only near the camera, at full detail). */
	const TArray<TObjectPtr<UDecalComponent>>& GetMarkDecals() const { return MarkDecals; }

	/** Dresses the MetaHuman in what the sim says it wears (docs/design/clothes-draw.md): Kellan's own top and trousers are put
	 *  away, a bare skin layer (the pawn's skin colour) shows what nothing covers, and each worn piece's meshes go on over it, in
	 *  the order they were put on, tinted by the stuff and torn by the piece's hit points. Pieces that left the list are
	 *  removed. `Key` is USimPawnSubsystem::ApparelKey of the snapshot. Mannequins just remember the key. */
	void ApplyApparel(USimPawnSubsystem& Pawns, const FSimPawnApparel& Wanted, uint64 Key);
	bool HasApparel() const { return bApparelApplied; }
	/** MakeHuman bodies: puts the body's slider and face morphs on its garments when they changed (cheap otherwise; the
	 *  pawn subsystem calls it every frame). */
	void SyncGarmentMorphs();
	/** The garment names each worn piece is drawn as on a MakeHuman body (one cut of its `visual.cuts`), by item; empty on a MetaHuman. */
	const TMap<uint32, FString>& GetGarmentCuts() const { return GarmentCuts; }
	uint64 GetApparelKey() const { return ApparelKey; }
	const FSimPawnApparel& GetApparel() const { return Apparel; }
	/** The meshes of the worn pieces now (skinned garments and head props), and the item each belongs to. */
	const TArray<TObjectPtr<UMeshComponent>>& GetWornMeshes() const { return WornMeshes; }
	const TArray<uint32>& GetWornMeshItems() const { return WornMeshItems; }
	/** Whether the MetaHuman wears the engine's whole body (all skin; no skin layer needed under the clothes). */
	bool HasFullBody() const { return bFullBody; }
	/** The bare skin layer (torso and arms, legs) of the older hands-and-feet body; empty with the whole body or without the clothes assets. */
	const TArray<TObjectPtr<USkeletalMeshComponent>>& GetBareLayer() const { return BareLayer; }
	const TArray<TObjectPtr<UMaterialInstanceDynamic>>& GetBareSkins() const { return BareSkins; }
	const UStaticMeshComponent* GetHairCards() const { return HairCards; }
	/** V2b: the material on the face's first skin slot (a MI_FaceSkin_* or Kellan's), null without the shaped face. */
	UMaterialInterface* GetFaceSkin() const;
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
	/** Hides the pawn while its level is cut away in the overseer view (docs/design/levels-draw.md), on top of SetBodyHidden. */
	void SetCutAway(bool bCut);
	bool IsCutAway() const { return bCutAway; }

	/** A hit (docs/design/fights.md): the pawn flinches, leaning away from the shot for about half a
	 *  second. `ShotDirection` is the way the bullet was travelling; `Strength` is 0 to 1. Cosmetic. */
	void PlayHitReaction(const FVector& ShotDirection, float Strength);
	/** A bullet hit with footwork (docs/design/fights.md): a whole-body react clip by the side the shot came from, the
	 *  large one with steps back when the sim staggered the pawn (`bStaggered`). False if no clip played (lying,
	 *  jumping, ragdoll or the clip is missing); the lean of PlayHitReaction still goes on top. Cosmetic. */
	bool PlayHitClip(const FVector& ShotDirection, bool bStaggered);
	int32 GetHitClipsPlayed() const { return HitClipsPlayed; }
	int32 GetHitReactions() const { return HitReactions; }
	/** Fight faces (docs/design/focus.md "Faces"): how far the pain and angry faces show now (0 to 1), and how many pain
	 *  faces a hit started. Cosmetic. */
	float GetPainFace() const { return PainFace; }
	float GetAngryFace() const { return AngryFace; }
	int32 GetPainFacesShown() const { return PainFacesShown; }
	bool IsReacting() const { return ReactionAge < kHitReactionSeconds; }
	/** The lean now, in degrees (0 when still). */
	float GetLeanDegrees() const { return LeanDegrees; }

	// --- The hit layer (docs/design/body-health.md part E). All cosmetic. ---

	/** A blow or a bullet landed: the zone it hit is knocked along `DirectionWorld` and springs back with a wobble, on
	 *  top of whatever the body is doing (USimPawnAnimInstance's hit layer). `Part` is the sim's body part (the zone is
	 *  the one the sim has it in); without one the zone is taken from how high `AtWorld` is on the body and which side.
	 *  `Strength` is 0 to 1. */
	void KickZone(const USimSubsystem& Bridge, int32 Part, const FVector& AtWorld, const FVector& DirectionWorld, float Strength);
	int32 GetZoneKicks() const { return ZoneKicks; }
	ESimPawnZone GetLastKickZone() const { return LastKickZone; }
	/** The crawl of a pawn downed by its torso and awake: whether the crawl clip plays now, and how often it started. */
	bool IsCrawling() const { return bCrawling; }
	int32 GetCrawlsPlayed() const { return CrawlsPlayed; }
	/** Up or down the face of a wall (docs/design/climbing.md): whether the climb clip plays, and how often it started. */
	bool IsWallClimbing() const { return WallClimbMontage.IsValid(); }
	int32 GetWallClimbsPlayed() const { return WallClimbsPlayed; }
	/** Whether the body is bent over a wound it is patching. */
	bool IsPatchPosed() const { return bPatchPosed; }

	/** Downed and dead pawns go limp: the mannequin simulates physics on a floor made under it, and the
	 *  MetaHuman follows it. `Impulse` is a velocity change in cm/s (the shot that did it). Cosmetic: the
	 *  sim already has the pawn lying prone where it fell. */
	void StartRagdoll(const FVector& Impulse);
	/** Gets up again: the animation takes over. */
	void StopRagdoll();
	bool IsRagdoll() const { return bRagdoll; }
	/** The ragdoll's floor taken away (carried on a back, BodyCarrySubsystem) or made again under where the pawn is now. */
	void ResetRagdollFloor(bool bWanted);
	/** Where the ragdoll's pelvis is now (world cm); false if the pawn is not a ragdoll. */
	bool GetRagdollPelvis(FVector& OutLocation) const;
	int32 GetRagdollsStarted() const { return RagdollsStarted; }

	/** Dismemberment (body-health.md part F): the limbs and head the sim says are gone (SeveredBits) are folded away on
	 *  every mesh, and what hung on them is hidden. Only ever more: a body does not grow them back. */
	void ApplySevered(uint16 Bits);
	uint16 GetSeveredShown() const { return SeveredShown; }
	/** How many stumps got their flesh cap. */
	int32 GetSeveredCaps() const { return SeveredCaps; }
	/** The piece itself, as the body has it now, thrown clear (SeveredPieceActor.cpp). Null if the body has no such bone. */
	class ASeveredPieceActor* SpawnSeveredPiece(uint16 Bit);

	/** The mesh that wears wound decals: the MetaHuman's body if there is one, else the mannequin. */
	USkeletalMeshComponent* GetDecalMesh() const;
	/** The player's crosshair against the body as it is drawn this frame (DrawnHit, docs/design/aiming.md). False if
	 *  hidden or missed; `bOutHasSkeleton` false if the body has no humanlike bones (animals: the sim's hitboxes decide). */
	bool TraceDrawnBody(const FVector& Start, const FVector& Direction, double MaxCm, DrawnHit::FResult& Out, bool& bOutHasSkeleton) const;
	/** How far a world point is from the body as drawn (0 on it), cm; -1 if the body has no humanlike bones. */
	double DistanceToDrawnBodyCm(const FVector& Point) const;
	/** The drawn head's middle this frame (world); false without the bones. For tests that aim at heads. */
	bool GetDrawnHeadCentre(FVector& Out) const;
	/** Where the line From-To first meets the drawn body (the animated mannequin's physics shapes, which every body
	 *  follows; no collision needed). False if it misses. */
	bool TraceDrawnBody(const FVector& From, const FVector& To, FVector& OutHit);

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
	/** Clips the sim moved this body along so far, and whether one is playing now (docs/design/body-motion.md). */
	int32 GetMotionClipsPlayed() const { return MotionClipsPlayed; }
	bool IsInMotionClip() const { return MotionClipDef != MAX_uint32; }
	bool IsMotionClipUpper() const { return IsInMotionClip() && bMotionUpper; }
	FVector GetMotionOffset() const { return MotionOffset; }
	/** While an attack clip is on screen: the seconds of the clip still to play before its blow (negative once past it) and
	 *  the rate it plays at. False when no clip with a blow is playing. */
	bool GetMotionClipToHit(float& OutSeconds, float& OutRate) const;
	float GetRollAngle() const { return RollAngle; }
	int32 GetRollsSeen() const { return RollsSeen; }
	/** 1 standing, 0.66 crouching, 0.24 lying: what weapon heights are scaled by. */
	float GetStanceHeight() const { return StanceHeight; }
	/** How tall the body is drawn, times the 1.80 m man (the looks' height; 1 before the looks arrive). Weapons and
	 *  shields place themselves by it (docs/design/gear-size.md). */
	float GetBodyScale() const { return bHasLooks ? Looks.Height : 1.0f; }
	/** Centimetres the body is drawn above the ground at its feet while it swims (WaterDraw.h). */
	float GetSwimLiftCm() const { return SwimLiftCm; }
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
	/** ApplyApparel for a MakeHuman body: each piece's cut as garments that follow the body's pose and morphs. */
	void ApplyGarments(USimPawnSubsystem& Pawns, const FSimPawnApparel& Wanted);
	void StyleGarment(UMeshComponent* Mesh, const FSimWornPiece& Piece) const;
	/** The body's skin-under-clothes bits (custom primitive data 30/31): skin a worn garment covers is cut away. */
	void SetMakeHumanCoverBits(uint32 Bits);
	/** Hides (or shows again) the MakeHuman body's scalp hair under a covering hat or helmet. */
	void SetMakeHumanHairCovered(bool bCovered);
	/** Hides (or shows again) the MakeHuman body's pubic hair under trousers (it would poke through them). */
	void SetMakeHumanPubicCovered(bool bCovered);
	void UpdateHairVisibility();
	/** Grooms show at full detail, minus hair under a covering hat and grooms with nothing to draw (bald, no beard). */
	void RefreshGroomVisibility();
	/** V2: face morphs, eyes and grooms from the looks. */
	void ApplyFace(USimPawnSubsystem& Pawns);
	/** V2b: the hair style's cards for far pawns, on the face's head bone, in the hair colour. */
	void SetHairCards(USimPawnSubsystem& Pawns);
	/** Puts a PawnFaces groom (by asset name; empty for none) on a groom component, bound to the shaped face. */
	void SetGroom(USimPawnSubsystem& Pawns, FName Component, const FString& GroomName);
	/** V3: remakes the mark decals (only at full detail; otherwise there are none). */
	void ApplyMarks();

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> FaceMesh;

	TArray<int32> EyeSlots;

	/** V2b: the face's skin slots and the skin instance PawnLooks gave each (Kellan's texture). */
	UPROPERTY()
	TMap<int32, TObjectPtr<UMaterialInterface>> FaceSkinSlots;

	UPROPERTY()
	TArray<TObjectPtr<UDecalComponent>> MarkDecals;

	/** The mark decal material and textures, kept from Setup (null without the PawnFaces assets). */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> MarkMaterial;

	UPROPERTY()
	TArray<TObjectPtr<UTexture>> TattooTextures;

	UPROPERTY()
	TArray<TObjectPtr<UTexture>> ScarTextures;

	UPROPERTY()
	TObjectPtr<UTexture> BodyHairTexture;

	/** V2b: hair cards (static mesh on the head bone), shown instead of the hair strands away from full detail. */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> HairCards;

	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> HairCardsMaterial;

	/** Groom components (by name) with nothing to draw. */
	TSet<FName> EmptyGrooms;

	UPROPERTY()
	TArray<TObjectPtr<UMeshComponent>> WornMeshes;

	TArray<uint32> WornMeshItems;

	UPROPERTY()
	TArray<TObjectPtr<USkeletalMeshComponent>> BareLayer;

	/** V2b: the bare layer's skin material instances (empty without the PawnFaces assets: then it keeps the garment colour). */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> BareSkins;

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> DefaultTop;

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> DefaultTrousers;
	TObjectPtr<USkeletalMeshComponent> DefaultShoes;

	FSimPawnApparel Apparel;
	TMap<uint32, FString> GarmentCuts;
	uint32 GarmentMorphHash = 0;
	/** The body's hair sections hidden under a hat (shown again when it comes off). */
	TArray<int32> CoveredHairSections;
	/** The body's pubic hair sections hidden under trousers. */
	TArray<int32> CoveredPubicSections;
	uint64 ApparelKey = 0;
	bool bApparelApplied = false;
	bool bBareLayerTried = false;
	bool bFullBody = false;
	FName AppliedFacePreset;
	// A MakeHuman character drawn instead of the MetaHuman (docs/design/makehuman.md): its mesh, retargeted from the hidden mannequin.
	TObjectPtr<USkeletalMeshComponent> MakeHumanMesh;
	FString AppliedMakeHuman;
	void ApplyMakeHuman(USimPawnSubsystem& Pawns);
	bool bHairHidden = false;
	int32 ApparelRebuilds = 0;

	void SpawnRagdollFloor();
	/** The hit reaction's lean and the melee pose, applied to the body together. */
	void ApplyBodyPose(float GameDelta, bool bBlocking);
	void AdvanceJump(float GameDelta);
	/** Animate after this frame's move: the meshes tick in TG_PostUpdateWork (TG_PrePhysics while a ragdoll). */
	void UpdateMeshTickGroups();
	/** The swing or block, plus the dodge: what the body and the weapon do now. */
	FMeleePose ComputeMeleePose() const;

	/** An invisible slab at the ground under a ragdoll (the ground is drawn from the sim and has no collision). */
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> RagdollFloor;

	FSimPawnLooks Looks;
	bool bHasLooks = false;
	bool bRagdoll = false;
	/** The tick group the meshes were last put in, and how many meshes that was (more come with looks). */
	int32 MeshTickGroup = -1;
	int32 MeshTickCount = 0;
	uint16 SeveredShown = 0;
	int32 SeveredCaps = 0;
	/** Meshes hidden whole because the head is off (a MetaHuman face); they still go into the head's piece. */
	TSet<TWeakObjectPtr<class USkinnedMeshComponent>> SeveredHidden;
	int32 RagdollsStarted = 0;
	float ReactionAge = kHitReactionSeconds;
	int32 HitClipsPlayed = 0;
	// The react to a hit playing on the upper body, how long it has played (game seconds) and when it is let go.
	TWeakObjectPtr<class UAnimMontage> HitMontage;
	float HitClipAge = 0.0f;
	float HitClipEndSeconds = 0.0f;
	float ReactionStrength = 0.0f;
	FVector ReactionDirection = FVector::ForwardVector;
	float LeanDegrees = 0.0f;
	/** The hit lean drawn last, and the one a new hit took over from (eased out under the new one). */
	FQuat LeanShown = FQuat::Identity;
	FQuat LeanCarry = FQuat::Identity;
	int32 HitReactions = 0;

	FMeleeSwingTrack Swing;
	float BlockBlend = 0.0f;
	/** Seconds the drawn guard stays up after the sim's block ends. */
	float BlockHoldLeft = 0.0f;
	float StanceHeight = 1.0f;
	float DodgeAge = kDodgeSeconds;
	float DodgeSide = 1.0f;
	float HitStopLeft = 0.0f;
	/** Real seconds the sim has disagreed with the swing on screen (a swing the sim called off). */
	float SwingMismatch = 0.0f;
	bool bBodyPosed = false;
	/** Game seconds into the jump, or below nought when not jumping; how fast it was going when it jumped (cm/s). */
	float JumpAge = -1.0f;
	float JumpSpeed = 0.0f;
	float JumpStartLength = 0.0f;
	bool bJumpFalling = false;
	int32 JumpsPlayed = 0;
	float GroundSpeed = 0.0f;
	// A clip the sim is moving the body along (docs/design/body-motion.md): the def playing, the montage, and how far
	// the visible body is set back so that its hips stay exactly on the sim's position while the clip carries them.
	uint32 MotionClipDef = MAX_uint32;
	float MotionClipTime = 0.0f;
	float MotionClipFrom = 0.0f;  // the clip def's trim start and blow, in the animation's and the clip's seconds
	float MotionClipHit = 0.0f;
	TWeakObjectPtr<class UAnimMontage> MotionMontage;
	TWeakObjectPtr<class UAnimSequence> MotionAsset;
	int32 MotionPelvisBone = INDEX_NONE;
	FVector MotionPelvisStart = FVector::ZeroVector;
	FVector MotionOffset = FVector::ZeroVector;
	int32 MotionClipsPlayed = 0;
	/** The clip playing is drawn on the upper body only (the hips are the walk's: no set-back). */
	bool bMotionUpper = false;
	// The whole-body clip the one now playing took over from, while it fades out: its share of the set-back.
	TWeakObjectPtr<class UAnimMontage> MotionOutMontage;
	TWeakObjectPtr<class UAnimSequence> MotionOutAsset;
	int32 MotionOutPelvisBone = INDEX_NONE;
	FVector MotionOutPelvisStart = FVector::ZeroVector;
	bool UpdateMotionClip(const USimSubsystem& Bridge, const colony::ThingView& Now, float GameDelta);
	// The hit layer (body-health.md part E): the zone a sim body part or a sim zone is on screen, hits so far, the hurt
	// legs (looked up a few times a second), and the crawl clip with its own set-back (its travel is in its hips too).
	ESimPawnZone ZoneOfSimZone(const USimSubsystem& Bridge, int32 SimZone) const;
	void UpdateHurt(const USimSubsystem& Bridge, const colony::ThingView& Now, float DeltaTime);
	void UpdateCrawl(const colony::ThingView& Now, float GameDelta);
	// Fight faces (docs/design/focus.md "Faces"): the pain face after a hit and the angry face in melee, eased 0 to 1 and
	// laid on the MakeHuman body's Face_ morphs over the face it had before (kept in FaceBase while either shows).
	void UpdateFightFace(const USimSubsystem& Bridge, const colony::ThingView& Now, float GameDelta);
	void ApplyFightFace();
	float PainFaceLeft = 0.0f;
	float AngryHoldLeft = 0.0f;
	float PainFace = 0.0f;
	float AngryFace = 0.0f;
	TMap<FName, float> FaceBase;
	FVector4f FaceBaseWrinkles = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	TWeakObjectPtr<USkeletalMeshComponent> FaceBaseMesh;
	int32 PainFacesShown = 0;
	int32 ZoneKicks = 0;
	ESimPawnZone LastKickZone = ESimPawnZone::Torso;
	float HurtAge = 1.0f;
	float LimpLeft = 0.0f;
	float LimpRight = 0.0f;
	bool bPatchPosed = false;
	ESimPawnZone PatchZone = ESimPawnZone::Torso;
	UPROPERTY()
	TObjectPtr<UAnimSequence> CrawlClip;
	bool bCrawlClipTried = false;
	bool bCrawling = false;
	int32 CrawlsPlayed = 0;
	TWeakObjectPtr<class UAnimMontage> CrawlMontage;
	int32 CrawlPelvisBone = INDEX_NONE;
	FVector CrawlPelvisStart = FVector::ZeroVector;
	float CrawlClipSpeed = 40.0f;
	FVector CrawlOffset = FVector::ZeroVector;
	// A wall climb (docs/design/climbing.md): the Game Animation Sample's climb onto a ledge, its time following the sim's
	// climb (on the face, then over the top), with its own set-back (its travel is in its root), facing the wall.
	void UpdateWallClimb(const USimSubsystem& Bridge, const colony::ThingView& Now);
	UPROPERTY()
	TObjectPtr<UAnimSequence> WallClimbClip;
	bool bWallClimbClipTried = false;
	TWeakObjectPtr<class UAnimMontage> WallClimbMontage;
	FVector WallClimbOffset = FVector::ZeroVector;
	int32 WallClimbsPlayed = 0;
	/** The lineage's jump height (cat-strain 2), from the snapshot, and the one fixed for the jump under way. */
	float JumpHeightFactor = 1.0f;
	float JumpFactor = 1.0f;
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
	/** The facing drawn (degrees): Yaw, through a damped spring, so a body the sim turns 30 degrees in a few frames
	 *  starts and stops turning smoothly; and how fast it turns (degrees a second). */
	float DrawnYaw = 0.0f;
	float DrawnYawRate = 0.0f;
	/** How much of a whole-body clip shows now (the clip playing and the one fading out). */
	float LegsClipWeight = 0.0f;
	float SwimLiftCm = 0.0f;
	bool bPlaced = false;
	bool bBodyHidden = false;
	bool bCutAway = false;
	double ActionEndSeconds = 0.0;
	TWeakObjectPtr<UAnimSequence> WorkLoopClip;
	int32 ActionsPlayed = 0;
};
