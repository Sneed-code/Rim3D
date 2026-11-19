#pragma once

#include "CoreMinimal.h"

#include "ColonySim/ArmPose.h"
#include "ColonySim/Hold.h"

class ASimPawnActor;
struct FMakeHumanHandGoal;
class UAnimSequence;
class USkeletalMeshComponent;
namespace colony { class DefDatabase; }

/**
 * Hands on weapons (docs/design/weapon-hold.md): the one place that knows how a hand closes on a weapon. Every weapon
 * and shield, on every body (third person, the first-person arms, the creator), goes through these: the hand's grip
 * frame from its bones, the weapon placed so its grip point (the def's `hold.main`) sits in the palm, and the other
 * hand pulled onto `hold.off` by the body's two-bone IK. Names follow the UE mannequin skeleton that MakeHuman's
 * game_engine skeleton shares (hand_r, index_01_r, pinky_01_r, middle_01_r, lowerarm_l...).
 */
namespace HoldDraw
{
	/** The body a pawn holds things with: its visible MakeHuman mesh, else its mannequin. */
	USkeletalMeshComponent* VisibleMesh(const ASimPawnActor& Pawn);

	/** Whether the mesh has the bones a hand needs (hand, index, middle and little finger knuckles). */
	bool HasHand(const USkeletalMeshComponent* Mesh, bool bRight);

	/**
	 * A fist round a hilt or haft, in world space: x along the grip from the little finger's knuckle to the index
	 * finger's (so a blade leaves the fist on the thumb side), z the way the fingers point (a blade's edge), the origin
	 * in the curled fingers. The blade leans a little the way the fingers point, never runs back along the forearm or
	 * through its holder's chest. False without the hand's bones.
	 */
	bool BladeFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out);

	/** A pistol grip, in world space: x out of the muzzle (along the hand, raked back by RakeDegrees), z up the grip from
	 *  the little finger to the index finger, the origin in the palm. False without the hand's bones. */
	bool GunFrame(const USkeletalMeshComponent* Mesh, bool bRight, float RakeDegrees, FTransform& Out);

	// A pole (spear) in both fists: from the holding hand's grip through the other hand's, so whatever pose the arms take
	// (guard, thrust) the shaft lies along both hands; one hand alone (or the hands together) falls back to BladeFrame.
	bool PoleFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out);

	/** A shield on the left forearm: its face (+x of a shield def's parts, which stand in the y-z plane) out from the
	 *  back of the arm, its z along the forearm towards the hand. */
	bool ShieldFrame(const USkeletalMeshComponent* Mesh, FTransform& Out);

	/** The holding hand's frame for this hold (by its grip and hand). */
	bool HandFrame(const USkeletalMeshComponent* Mesh, const colony::WeaponHold& Hold, FTransform& Out);

	/** The bone the held weapon hangs from between updates: the holding hand's, or the left forearm for a shield. */
	FName HandBone(const colony::WeaponHold& Hold);

	/** The weapon's world transform: its grip point (Hold.main, scaled by Size) on the hand frame, scaled by Size. */
	FTransform WeaponWorld(const FTransform& Frame, const colony::WeaponHold& Hold, float Size);

	/**
	 * Puts the other hand on the weapon (Hold.off) through the body's MakeHuman two-bone IK, Alpha 0 to 1 (0 lets it go).
	 * Only the left hand has the IK, so only weapons held in the right hand get a second hand. Nothing happens on a body
	 * without the MakeHuman animation. `ElbowOut` is the world direction the elbow bends towards (zero: out to the side
	 * and down).
	 */
	void SetOffHand(USkeletalMeshComponent* Mesh, const FTransform& Weapon, const colony::WeaponHold& Hold, float Size, float Alpha,
					const FVector& ElbowOut = FVector::ZeroVector);

	/** A clip of the hold style (`stance`, `aim`, `block`, `reload`, `shieldStance`, `shieldBlock`, `fpStance`, `fpBlock`),
	 *  else the one named by Fallback (a first-person key falls back to its third-person one), loaded and cached; null
	 *  if the style has none or the asset is missing. */
	UAnimSequence* StyleClip(const colony::DefDatabase& Defs, const colony::WeaponHold& Hold, const char* Key, const char* Fallback = nullptr);

	// ---- Weapon first, then the hands (docs/design/weapon-hold.md, sneed 2026-11-19: "make weapon holding realistic in
	// both views"). The weapon goes to its hold place on the body (a gun's sight before the eye, its stock in the shoulder,
	// a blade's guard); both hands are then put on its grips by the arm solver, inside human joint limits. Every weapon,
	// gun or melee, current or future, needs only its grip points (and a style's places) for this.

	/** A weapon's points in its own frame (cm, before its size): the main grip, the other hand's grip, a gun's rear sight
	 *  and butt. */
	struct FPoints
	{
		FVector Main = FVector::ZeroVector;
		FVector Off = FVector::ZeroVector;
		bool bOff = false;      // a second hand goes on Off (two-handed, or a pistol's support hand)
		FVector Sight = FVector(0.0, 0.0, 8.0);
		FVector Stock = FVector(-25.0, 0.0, 2.0);
	};

	/** The points of a hold (its main, off or support point), a gun's sight and a butt guessed behind the grip. With a
	 *  shield on the left arm a one-handed weapon gets no second hand. */
	FPoints PointsOf(const colony::WeaponHold& Hold, bool bShieldOnArm);

	/** A gun's butt from its model's bounds in the gun's frame: the rearmost point, a little under the sight line. */
	FVector StockFromBounds(const FBox& BoxInGunFrame, const FVector& Sight);

	/** Where the weapon is (its transform, unscaled) in a place's anchor frame (x ahead, y right, z up, origin at the
	 *  anchor): the place's point (grip, sight or butt, times Size) at `at` (times BodyPlace), turned by `turn`. */
	FTransform PlaceInAnchor(const colony::HoldPlace& Place, const FPoints& Points, float Size, float BodyPlace);

	/** A fist round a hilt (BladeFrame without its guards against clip wrists): the frame a hand closes on. */
	bool FistFrame(const USkeletalMeshComponent* Mesh, bool bRight, FTransform& Out);

	/** The hand bone's transform relative to the frame its grip closes on (a fist, or a pistol grip for Gun on the main
	 *  hand), from the fingers as they are now: so HandInGrip * GripTarget is where the hand bone goes. */
	bool HandInGrip(const USkeletalMeshComponent* Mesh, bool bRight, bool bPistolGrip, float RakeDegrees, FTransform& Out);

	/** A weapon held at a place: the transforms the hands' goals and the weapon's drawing need. */
	struct FPinned
	{
		FTransform MainHand;      // the main hand bone's goal, in the goal's space
		FTransform OffInMain;     // the other hand bone's goal in the main hand bone's space
		FTransform WeaponInHand;  // the weapon (with its size) in the main hand bone's space
		bool bOff = false;
	};

	/** Hands for a weapon placed at Weapon (world space, unscaled): the main hand's goal on its grip (world), the other
	 *  hand's on its own (turned by the hold's offTurn) in the main hand's space, and the weapon in the main hand's space
	 *  (all in world cm). False without the hands' bones. */
	bool PinHands(const USkeletalMeshComponent* Mesh, const colony::WeaponHold& Hold, const FPoints& Points, float Size, const FTransform& Weapon,
				  FPinned& Out);

	/** Sets the arm solver's goals for a pinned weapon (PinHands' world transforms) on a MakeHuman body: the main hand
	 *  in Space (component space, or offset from Anchor's bone for EMakeHumanGoalSpace::Anchor, so it rides with the
	 *  body's pose), the other hand after it on its grip. Alphas 0 let the hands go. */
	void ApplyPinned(USkeletalMeshComponent* Mesh, const colony::WeaponHold& Hold, const FPinned& Pinned, uint8 Space, FName Anchor, float MainAlpha,
					 float OffAlpha);

	/** Measures a drawn arm as people's joints do (colony::arm::measureArm on the bones as drawn now): elbow bend,
	 *  forearm twist, wrist bend. Up, Forward and Right are the body's axes in world space. False without the bones. */
	bool MeasureDrawnArm(const USkeletalMeshComponent* Mesh, bool bRight, const FVector& Up, const FVector& Forward, const FVector& Right,
						 colony::arm::ArmAngles& Out);

	/** Lets both hands go (no goals on the arm solver). */
	void ReleaseHands(USkeletalMeshComponent* Mesh);

	/** Loads /Game/Rim3D/Generated/Animations/<Rel>.<name> once (null if missing; asked again only after a while). */
	UAnimSequence* LoadClip(const FString& Rel);
}
