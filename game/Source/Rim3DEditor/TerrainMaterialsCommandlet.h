#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "TerrainMaterialsCommandlet.generated.h"

/**
 * Makes the terrain material (docs/design/terrain.md) from the textures tools/terrain_textures.py downloaded, into
 * /Game/Rim3D/Generated/Terrain/ (not in Git; run this again to make it):
 *
 *   python tools/terrain_textures.py
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=TerrainMaterials -unattended -nosplash -AllowCommandletRendering
 *
 * The master material M_Terrain blends four texture layers (grass, sand, gravel, rock) and water by weights the
 * terrain mesh carries in its second and third UV sets, draws steep ground as rock, and shows the rock layer on
 * every side of a block (world aligned, so no stretching). The instance MI_Terrain holds the textures and numbers
 * from assets/terrain_textures.json; changing the list (to Megascans, say) and running this again is the whole swap.
 */
UCLASS()
class UTerrainMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UTerrainMaterialsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
