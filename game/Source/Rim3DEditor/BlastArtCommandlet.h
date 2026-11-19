#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "BlastArtCommandlet.generated.h"

/**
 * Makes the blasts' textures and materials (docs/design/blast-draw.md) into /Game/Rim3D/Generated/Blast (not in Git; run
 * this again to make them):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=BlastArt -unattended -nosplash -nullrhi [-force]
 *
 * - T_BlastFire, T_BlastSmoke: 8 x 8 frame flipbooks of a fireball and a smoke puff, ray-marched through noise
 *   (BlastFlipbooks.h): R heat, G baked light, B depth, A opacity. T_BlastScorch: the scorch mark.
 * - M_BlastFire, M_BlastSmoke: camera-facing sprites (instanced; per-instance data, see BlastFxSubsystem.h): flipbook frame
 *   blended between neighbours, lit translucent smoke in its instance's tint, fire glowing as a black body, soft where it
 *   meets the ground.
 * - M_BlastSpark: a glowing streak (additive) for hot fragments and sparks.
 * - M_BlastHaze: a ring of heat shimmer (refraction only).
 * - M_BlastScorch: the scorch decal; the decal colour's alpha fades it.
 *
 * Without -force, assets that are there already are kept.
 */
UCLASS()
class UBlastArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBlastArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
