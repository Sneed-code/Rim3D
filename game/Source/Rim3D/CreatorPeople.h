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
	/** The creator's untouched base person (every slider at its default, the base's own hair and fair skin): the
	 *  generic woman or man. Looks and apply as for the random ones. */
	FSimPawnLooks DefaultLooks(bool bFemale, const FSimPawnLooks& From);
	bool ApplyDefault(bool bFemale, ASimPawnActor& Pawn);

	/**
	 * The character screen's look of a colonist (docs/design/new-game.md): the random person Seed (as RandomLooks makes),
	 * or for a negative Seed the creator's untouched base person (DefaultLooks, the gun range's woman: kDefaultSeed),
	 * with the player's changes on top, by name. It travels through the sim as text (PawnLooks::creator,
	 * "seed=12;female=1;height=1.02;"), so in-game pawns are drawn with exactly the same CreatorApply as the creator:
	 * hair, clothes and body updates show in both. Names: height (mesh scale), sex, older, weight, muscle, breasts (-1..1
	 * where they are signed), hairStyle (index, HairStyleLabel), hairR/G/B, skinR/G/B, irisR/G/B (sRGB 0..1),
	 * pubic (0 none, 1 natural, 2 trimmed, 3 shaved).
	 */
	constexpr int32 kDefaultSeed = -1;
	struct FLook
	{
		int32 Seed = 0;
		bool bFemale = true;
		TMap<FString, float> Values;

		float Get(const TCHAR* Name, float Default) const
		{
			const float* Found = Values.Find(Name);
			return Found ? *Found : Default;
		}
	};
	FString Encode(const FLook& Look);
	/** False for text that is not a look (no seed). */
	bool Decode(const FString& Text, FLook& Out);
	/** The look's values as the random person has them before any change (for the sliders' starting points). */
	FLook Expanded(const FLook& Look);
	FSimPawnLooks LooksFor(const FLook& Look, const FSimPawnLooks& From);
	/** Puts the look on the pawn's MakeHuman body; false if its creator base is not on yet (call again next frame). */
	bool Apply(const FLook& Look, ASimPawnActor& Pawn);
	/** Face shape sliders (face variety, sneed 2026-11-25): the look's value Key (-1 .. 1, 0 the base's face) and the label
	 *  to show. Every random person has a face of their own; RandomFace gives the look a new random one. */
	int32 FaceShapeCount();
	FString FaceShapeLabel(int32 Index);
	FString FaceShapeKey(int32 Index);
	void RandomFace(FLook& Look);
	int32 HairStyleCount();
	FString HairStyleLabel(int32 Index);
	/** Whether the pawn's base has this hair style (bald always). */
	bool HasHairStyle(int32 Index, const ASimPawnActor& Pawn);

	/**
	 * Chosen cosmetics (never generated; docs/design/new-game.md): genital (-1 smaller .. 1 larger), pigment (0 .. 1, how
	 * much darker the nipples and genitals are than the skin), jewel<j> (the piece's metal + 1; 0 or absent: not worn),
	 * and up to four marks mark<m>Design (index into the designs, -1 none), mark<m>Area (index into MarkAreas),
	 * mark<m>Size (cm), mark<m>Angle (degrees), mark<m>Opacity, mark<m>R/G/B (ink or paint, sRGB).
	 */
	int32 JewelCount();
	FString JewelLabel(int32 Index);
	int32 MetalCount();
	FString MetalLabel(int32 Index);
	int32 MarkDesignCount();
	FString MarkDesignLabel(int32 Index);
	/** Ink and paint take a colour; scars and brands are the skin's own. */
	bool MarkDesignTakesColour(int32 Index);
	/** The body areas a mark can go on for this look's base (sorted names, e.g. UpperArmL). */
	TArray<FString> MarkAreas(const FLook& Look);
}
