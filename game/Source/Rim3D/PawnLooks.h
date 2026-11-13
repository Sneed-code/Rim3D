#pragma once

#include "CoreMinimal.h"

// Names shared by the pawn looks generator (Rim3DEditor's PawnLooks commandlet) and the pawns
// that use what it makes (ASimPawnActor). See docs/design/looks.md.

/** Where the generated looks assets go (not in Git: run the commandlet to make them). */
inline const TCHAR* const PawnLooksFolder = TEXT("/Game/Rim3D/Generated/PawnLooks");

/**
 * Body shape morphs and the MetaHuman body type each comes from: all 18 of the engine's fixed body types (gender, short /
 * medium / tall, under / normal / over weight) but Kellan's own (male medium normal weight), each as a shape without its
 * proportions (height comes from scaling the whole pawn). The game blends the four nearest on its 3 x 3 grid per gender.
 */
inline TArray<TPair<FName, FString>> PawnLooksBodyTypes()
{
	return {
		{TEXT("Thin"), TEXT("m_med_unw")},
		{TEXT("Heavy"), TEXT("m_med_ovw")},
		{TEXT("Female"), TEXT("f_med_nrw")},
		{TEXT("FemaleThin"), TEXT("f_med_unw")},
		{TEXT("FemaleHeavy"), TEXT("f_med_ovw")},
		{TEXT("Short"), TEXT("m_srt_nrw")},
		{TEXT("ShortThin"), TEXT("m_srt_unw")},
		{TEXT("ShortHeavy"), TEXT("m_srt_ovw")},
		{TEXT("Tall"), TEXT("m_tal_nrw")},
		{TEXT("TallThin"), TEXT("m_tal_unw")},
		{TEXT("TallHeavy"), TEXT("m_tal_ovw")},
		{TEXT("FemaleShort"), TEXT("f_srt_nrw")},
		{TEXT("FemaleShortThin"), TEXT("f_srt_unw")},
		{TEXT("FemaleShortHeavy"), TEXT("f_srt_ovw")},
		{TEXT("FemaleTall"), TEXT("f_tal_nrw")},
		{TEXT("FemaleTallThin"), TEXT("f_tal_unw")},
		{TEXT("FemaleTallHeavy"), TEXT("f_tal_ovw")},
	};
}

/** The morph name for a gender, stature (-1 short, 0 medium, 1 tall) and build (-1 thin, 0 normal, 1 heavy); none for Kellan's own. */
inline FName PawnLooksBodyMorph(bool bFemale, int32 Stature, int32 Build)
{
	FString Name = bFemale ? TEXT("Female") : TEXT("");
	Name += Stature < 0 ? TEXT("Short") : Stature > 0 ? TEXT("Tall") : TEXT("");
	Name += Build < 0 ? TEXT("Thin") : Build > 0 ? TEXT("Heavy") : TEXT("");
	return Name.IsEmpty() ? NAME_None : FName(*Name);
}

/**
 * Since the real body (step 1 of the character creator): the shaped copy of Kellan's body is the engine's whole MetaHuman
 * body, all skin, with this morph on it; a body without it is the older hands-and-feet copy (keep the skin layer then).
 */
inline const FName PawnLooksFullBodyMarker(TEXT("FemaleTallHeavy"));

/** Kellan's mesh `Name` with the body shape morphs on it. */
inline FString PawnLooksMorphMeshName(const FString& Name)
{
	return TEXT("SK_Looks_") + Name;
}

/** Kellan's skin material instance `Name` remade on our skin material. */
inline FString PawnLooksSkinInstanceName(const FString& Name)
{
	return TEXT("MI_PawnSkin_") + Name.Replace(TEXT("MI_"), TEXT(""));
}

/** Kellan's clothes material instance `Name` on a copy of its base material that may be used with morph targets. */
inline FString PawnLooksClothInstanceName(const FString& Name)
{
	return TEXT("MI_PawnCloth_") + Name.Replace(TEXT("MI_"), TEXT(""));
}

inline const TCHAR* const PawnLooksSkinMaterial = TEXT("M_PawnSkin");
inline const TCHAR* const PawnLooksPartMaterial = TEXT("M_PawnPart");
inline const TCHAR* const PawnLooksHornMesh = TEXT("SM_PawnHorn");

/** Material parameters. The skin and part colours come from custom primitive data, from this index (RGB). */
inline const FName PawnLooksParamBaseColor(TEXT("BaseColor"));
inline const FName PawnLooksParamNormal(TEXT("Normal"));
inline const FName PawnLooksParamSkinColor(TEXT("SkinColor"));
inline const FName PawnLooksParamSourceAverage(TEXT("SourceAverage"));
inline const FName PawnLooksParamPartColor(TEXT("PartColor"));
constexpr int32 PawnLooksSkinDataIndex = 0;

/** "/Game/Rim3D/Generated/PawnLooks/Name.Name" */
inline FString PawnLooksAssetPath(const FString& Name)
{
	return FString(PawnLooksFolder) / Name + TEXT(".") + Name;
}
