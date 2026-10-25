#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "BuildingMaterialsCommandlet.generated.h"

/**
 * Makes the see-through material blueprints and frames are drawn with (docs/design/building-draw.md), into
 * /Game/Rim3D/Generated/Buildings/M_BuildingGhost (not in Git; run this again to make it):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=BuildingMaterials -unattended -nosplash
 *
 * Why not an engine material: the engine's translucent materials are not set up for instanced meshes,
 * so the game draws them as the default dark material. This one is unlit, translucent, two sided and
 * marked for instanced static meshes, with a colour (RGB) and an Opacity.
 */
UCLASS()
class UBuildingMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UBuildingMaterialsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
