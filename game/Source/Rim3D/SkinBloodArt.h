#pragma once

#include "CoreMinimal.h"

// Names shared by the BloodArt commandlet (Rim3DEditor) and USkinBlood (the game): blood painted into a MakeHuman
// body's skin texture (docs/design/blood.md, "Blood in the skin").

/** Where the bind-pose position maps and the paint material go (with the rest of the blood art). */
inline const TCHAR* const SkinBloodFolder = TEXT("/Game/Rim3D/Generated/Blood");
/** A body base's position map: its skin's bind-pose position (cm, plus SkinBloodPositionOffset) at each texel of its UV
 *  layout, alpha 1 where there is skin. */
inline FString SkinBloodPositionName(const FString& Base) { return TEXT("T_SkinPos_") + Base; }
inline constexpr float SkinBloodPositionOffset = 200.0f;
/** The material that paints a wound into a mask, and fades one (Mode 1). */
inline const TCHAR* const SkinBloodPaintName = TEXT("M_SkinBloodPaint");
/** The MakeHuman skin material that reads the mask (MakeHumanArt makes it, BloodArt adds the blood). */
inline const TCHAR* const SkinBloodSkinMaterialName = TEXT("M_MH_SkinDetail");
/** Mask and position map size (texels a side). */
inline constexpr int32 SkinBloodResolution = 1024;
