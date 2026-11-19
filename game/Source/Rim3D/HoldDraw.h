#pragma once

#include "CoreMinimal.h"

#include "ColonySim/Hold.h"

class ASimPawnActor;
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

	/** Loads /Game/Rim3D/Generated/Animations/<Rel>.<name> once (null if missing; asked again only after a while). */
	UAnimSequence* LoadClip(const FString& Rel);
}
