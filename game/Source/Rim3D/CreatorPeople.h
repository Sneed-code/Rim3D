#pragma once

#include "CoreMinimal.h"
#include "PawnSubsystem.h"

class ASimPawnActor;

/**
 * The character creator's seeded randomizer for use outside the creator (the gun range, docs/design/gun-range.md):
 * the same people the creator's Random person button makes (baseline humans, spread builds and ages, natural hair,
 * skin heavily weighted to fair with tans, never dark), on the CreatorFemale / CreatorMale bases.
 * Implemented in SmokeTestCreator.cpp, next to CreatorRandomize and CreatorApply.
 */
namespace CreatorPeople
{
	/** The looks to give a pawn (SetLooksOverride) for the random person Seed: From with the creator base and skin. */
	FSimPawnLooks RandomLooks(int32 Seed, bool bFemale, const FSimPawnLooks& From);
	/** Puts the random person's sliders (build, face, hair style and colour, skin) on the pawn's MakeHuman body.
	 *  False if its creator base is not on yet (call again next frame). */
	bool ApplyRandom(int32 Seed, bool bFemale, ASimPawnActor& Pawn);
}
