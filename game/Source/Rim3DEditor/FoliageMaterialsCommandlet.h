#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FoliageMaterialsCommandlet.generated.h"

/**
 * Makes plain foliage materials for the Electric Dreams Megascans plants (look pass L2, docs/design/terrain.md) into
 * /Game/Rim3D/Generated/Foliage/ (not in Git; run this again after tools/electric_dreams.ps1):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=FoliageMaterials -unattended -nosplash -AllowCommandletRendering
 *
 * The sample's own foliage masters were saved with Substrate on (Rim3D has it off), so in Rim3D they draw black. This
 * makes one master, M_Foliage (masked, two-sided foliage shading, a light wind sway), and for every material instance
 * under /Game/Megascans/3D_Plants an instance of it, MI_<name>_R3D, with the same textures (Albedo with opacity in its
 * alpha, Normal, ART: ambient occlusion, roughness, translucency). The terrain subsystem swaps them onto the plants.
 */
UCLASS()
class UFoliageMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFoliageMaterialsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
