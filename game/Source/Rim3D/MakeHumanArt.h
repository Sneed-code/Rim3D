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
