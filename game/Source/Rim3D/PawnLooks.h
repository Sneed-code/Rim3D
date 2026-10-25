#pragma once

#include "CoreMinimal.h"

// Names shared by the pawn looks generator (Rim3DEditor's PawnLooks commandlet) and the pawns
// that use what it makes (ASimPawnActor). See docs/design/looks.md.

/** Where the generated looks assets go (not in Git: run the commandlet to make them). */
inline const TCHAR* const PawnLooksFolder = TEXT("/Game/Rim3D/Generated/PawnLooks");

/** Body shape morphs and the MetaHuman body type each comes from (male medium normal weight is Kellan's own). */
inline TArray<TPair<FName, FString>> PawnLooksBodyTypes()
{
	return {
		{TEXT("Thin"), TEXT("m_med_unw")},
		{TEXT("Heavy"), TEXT("m_med_ovw")},
		{TEXT("Female"), TEXT("f_med_nrw")},
		{TEXT("FemaleThin"), TEXT("f_med_unw")},
		{TEXT("FemaleHeavy"), TEXT("f_med_ovw")},
	};
}

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
