#pragma once

#include "CoreMinimal.h"

// Names shared by the faces generator (Rim3DEditor's PawnFaces commandlet) and the pawns that use what it makes
// (ASimPawnActor). See docs/design/looks.md, "Faces, hair and marks on screen".

/** Where the generated faces, grooms and marks go (not in Git: run the commandlet to make them). */
inline const TCHAR* const PawnFacesFolder = TEXT("/Game/Rim3D/Generated/PawnFaces");

/** Kellan's face mesh `Name` with the face shape morphs on it. */
inline FString PawnFacesMeshName(const FString& Name)
{
	return TEXT("SK_Face_") + Name;
}

/** Face shape morphs: the female face, then the sim's four sliders (weight 2 x slider - 1, so 0.5 is the average face). */
inline const FName PawnFacesMorphFemale(TEXT("FaceFemale"));
inline const FName PawnFacesMorphShape(TEXT("FaceShape"));
inline const FName PawnFacesMorphBrow(TEXT("FaceBrow"));
inline const FName PawnFacesMorphNose(TEXT("FaceNose"));
inline const FName PawnFacesMorphJaw(TEXT("FaceJaw"));

/** A groom copy for one of the sim's styles: Kind is Hair, Beard, Moustache or Brows (Brows_F0.., Brows_M0..). */
inline FString PawnFacesGroomName(const FString& Kind, const FString& Style)
{
	return FString::Printf(TEXT("GA_%s_%s"), *Kind, *Style);
}

/** The binding of one of our grooms (by its asset name) to the shaped face. */
inline FString PawnFacesBindingName(const FString& GroomName)
{
	return GroomName.Replace(TEXT("GA_"), TEXT("GB_"));
}

/** The binding of one of Kellan's own grooms (by its asset name: his hair, brows, eyelashes, moustache) to the shaped face. */
inline FString PawnFacesKellanBindingName(const FString& GroomName)
{
	return TEXT("GB_Kellan_") + GroomName;
}

/** Eye materials: the MetaHuman eye on a copy marked for morph targets, one instance per eye colour (sRGB hex). */
inline const TCHAR* const PawnFacesEyeMaterial = TEXT("M_PawnEye");
/** Kellan's own eye colour on it, for pawns from older saves. */
inline const TCHAR* const PawnFacesEyeDefault = TEXT("MI_PawnEye_Default");
inline FString PawnFacesEyeInstanceName(const FColor& Srgb)
{
	return FString::Printf(TEXT("MI_PawnEye_%02X%02X%02X"), Srgb.R, Srgb.G, Srgb.B);
}

/** Skin marks (V3): one decal material, a mask texture per tattoo and scar style (by index) and body hair. */
inline const TCHAR* const PawnFacesMarkMaterial = TEXT("M_PawnMark");
inline FString PawnFacesTattooTexture(int32 Style) { return FString::Printf(TEXT("T_Tattoo_%d"), Style); }
inline FString PawnFacesScarTexture(int32 Style) { return FString::Printf(TEXT("T_Scar_%d"), Style); }
inline const TCHAR* const PawnFacesBodyHairTexture = TEXT("T_BodyHair");
inline const FName PawnFacesParamMask(TEXT("Mask"));
inline const FName PawnFacesParamColor(TEXT("Color"));
inline const FName PawnFacesParamOpacity(TEXT("Opacity"));
/** Skin meshes write this custom stencil value; marks only draw where the nearest surface is skin. */
constexpr int32 PawnFacesSkinStencil = 77;

/** "/Game/Rim3D/Generated/PawnFaces/Name.Name" */
inline FString PawnFacesAssetPath(const FString& Name)
{
	return FString(PawnFacesFolder) / Name + TEXT(".") + Name;
}
