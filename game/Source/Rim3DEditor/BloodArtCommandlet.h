#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "BloodArtCommandlet.generated.h"

/**
 * Makes the blood's textures and materials (docs/design/blood.md) into /Game/Rim3D/Generated/Blood (not in Git; run
 * this again to make them):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=BloodArt -unattended -nosplash -nullrhi [-force]
 *
 * - T_BloodAtlas, T_BloodAtlasN: 4 x 4 splat pictures drawn from shapes (BloodSplatAtlas.h) and their normal map.
 * - M_BloodSplatter: the stain decal. Each UBloodSubsystem decal picks its picture with CellX / CellY and passes its
 *   colour in the decal colour; the colour's alpha packs wetness (floor / 62: glossy and bumpy when wet, matte dry)
 *   and opacity (frac).
 * - M_BloodDrop: flying drops (instanced; colour in per-instance data 0-2), wet and glossy.
 * - M_BloodMist: the fine mist (instanced camera-facing squares; colour 0-2, opacity 3), soft where it meets surfaces.
 *
 * Without -force, assets that are there already are kept.
 */
UCLASS()
class UBloodArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBloodArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
