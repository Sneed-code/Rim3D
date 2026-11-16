#pragma once

#include "CoreMinimal.h"

class ASimPawnActor;
class UWorld;

/**
 * The character creator's WEAPONS window (docs/design/creator-weapons.md): the picked person holds a weapon in the
 * hands of its visible (MakeHuman) body, you pick a fighting style and an attack and watch it, and a fight demo puts two
 * people face to face taking turns, with blades that stop on bodies, shields, blades and the ground, staggers, and blood
 * (spray, drips and spots on the floor, a pool under a badly hurt person, stains on the body that darken).
 * All of it is cosmetic and lives in the creator only: it plays the Mixamo clips on the pawns' mannequins and moves
 * nothing in the sim.
 */
namespace CreatorWeapons
{
	/** Once a frame, before the panels are drawn: weapons follow the hands, the preview and the demo run, blood moves.
	 *  `People` are the creator's people in order (null where a pawn is missing); the demo turns `bInOutWalk` off. */
	void Tick(UWorld* World, const TArray<ASimPawnActor*>& People, int32 Selected, float DeltaSeconds, bool& bInOutWalk);

	/** The WEAPONS window, inside the creator's ImGui frame (its style pushed); `Scale` is the creator panel's scale. */
	void DrawWindow(float Scale);
}
