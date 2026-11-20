#pragma once

#include "CoreMinimal.h"

// Names shared by the MakeHuman art importer (Rim3DEditor's MakeHumanArt commandlet) and the game. See
// docs/design/makehuman.md.

/** Where the imported MakeHuman characters go (not in Git: export with tools/makehuman, then run the commandlet). */
inline const TCHAR* const MakeHumanFolder = TEXT("/Game/Rim3D/Generated/MakeHuman");
/** Local-only comparison models (other people's characters): never in Git, never shipped. */
inline const TCHAR* const MakeHumanLocalFolder = TEXT("/Game/ThirdParty/Local/MakeHuman");

inline FString MakeHumanMeshName(const FString& Name) { return TEXT("SK_MH_") + Name; }
inline FString MakeHumanSkinName(const FString& Name) { return TEXT("MI_MH_") + Name + TEXT("_Skin"); }
inline FString MakeHumanRigName(const FString& Name) { return TEXT("IK_MH_") + Name; }
inline FString MakeHumanRetargeterName(const FString& Name) { return TEXT("RTG_UEFN_to_MH_") + Name; }

/** Garments for the MakeHuman bodies (docs/design/clothes-draw.md "MakeHuman bodies"): one skinned mesh per garment per
 *  creator base, on that base's skeleton and with its slider morphs (tools/makehuman/mh_export.py "garments"). */
inline const TCHAR* const MakeHumanGarmentFolder = TEXT("/Game/Rim3D/Generated/MakeHuman/Garments");
inline FString MakeHumanGarmentName(const FString& Base, const FString& Garment) { return TEXT("SK_MHG_") + Base + TEXT("_") + Garment; }
/** Custom primitive data on a garment (M_MH_Garment): the stuff's colour (RGB at 0), the wear 0 to 1 at 3, and how much
 *  the colour replaces the garment's own (0 keeps its texture's colours, 1 takes the stuff's, keeping its shading) at 4. */
inline constexpr int32 MakeHumanGarmentTintIndex = 0;
inline constexpr int32 MakeHumanGarmentWearIndex = 3;
inline constexpr int32 MakeHumanGarmentTintAmountIndex = 4;
/** Custom primitive data on a MakeHuman body: the cover bits of the garments worn (low 16 at 30, high 16 at 31); the skin
 *  material hides the skin where its GarmentCover texture has one of them (docs/design/clothes-draw.md "Skin under clothes"). */
inline constexpr int32 MakeHumanCoverLowIndex = 30;
inline constexpr int32 MakeHumanCoverHighIndex = 31;
inline FString MakeHumanCoverTextureName(const FString& Base) { return TEXT("T_MHG_") + Base + TEXT("_Cover"); }

/** "/Game/.../Name.Name" in a folder. */
inline FString MakeHumanAssetPath(const TCHAR* Folder, const FString& Name)
{
	return FString(Folder) / Name + TEXT(".") + Name;
}

/** One character to import: its FBX, its skin colour map, and whether it is a local-only comparison model. */
struct FMakeHumanCharacterFiles
{
	FString Name;
	FString Fbx;
	FString SkinTexture;
	bool bLocalOnly = false;
};
