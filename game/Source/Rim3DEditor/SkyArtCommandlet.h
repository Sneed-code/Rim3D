#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "SkyArtCommandlet.generated.h"

/**
 * Makes the night sky (docs/design/look.md "Night sky") into /Game/Rim3D/Generated/Sky/ (not in Git; run this again to
 * make it):
 *
 *   python tools/sky_textures.py
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=SkyArt -unattended -nosplash -AllowCommandletRendering
 *
 * T_StarMap is NASA's Deep Star Maps 2020 (Gaia DR2, Hipparcos and Tycho stars and the Milky Way, celestial
 * coordinates, plate carree; NASA/Goddard SVS, free to use with credit) as a compressed HDR texture. M_StarSky is an
 * unlit, additive material for the inside of a huge sphere: it turns each view direction into right ascension and
 * declination for the colony's latitude and the time (StarLatitude, StarTurn) and looks the star map up there, times
 * StarBrightness (the sky subsystem sets it: zero by day and under cloud).
 */
UCLASS()
class USkyArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USkyArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
