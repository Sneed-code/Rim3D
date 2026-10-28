#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "SurfaceMaterialsCommandlet.generated.h"

/**
 * Makes the materials buildings take from the stuff they are made of (task L4, docs/design/building-draw.md):
 * imports the textures tools/stuff_textures.py downloaded (assets/stuff_textures.json) and makes the master
 * M_Surface and one MI_Surface_<surface> per surface into /Game/Rim3D/Generated/Surfaces/ (not in Git):
 *
 *   python tools/stuff_textures.py
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=SurfaceMaterials -unattended -nosplash -AllowCommandletRendering
 *
 * The material projects its textures along the world's three axes (no UVs needed, so walls side by side run on
 * without a seam), multiplies the colour by the vertex colour (a part's own shade, so a wall's base course reads
 * darker), and is marked for instanced static meshes.
 */
UCLASS()
class USurfaceMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	USurfaceMaterialsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
