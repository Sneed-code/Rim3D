#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "WaterArtCommandlet.generated.h"

/**
 * Makes the water surface material M_Water (docs/design/water.md, W1) into /Game/Rim3D/Generated/Water/ (not in Git;
 * run this again to make it):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=WaterArt -unattended -nosplash -AllowCommandletRendering
 *
 * Single Layer Water shading: murky green-brown tropical water, clear in the first 20 to 30 cm; two detail normal
 * maps advected along each vertex's flow (UV 1, metres per second) with a two-phase flow-map blend so nothing
 * stretches, slow wind ripples on still water; foam at the shore from the water's depth (UV 2) and where the flow is
 * fast; refraction on, reflections from Lumen. The normal map is the terrain's copy of the Megascans swamp water
 * normal (run the TerrainMaterials commandlet first), or the engine's flat normal.
 */
UCLASS()
class UWaterArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UWaterArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
