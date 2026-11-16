#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "FightMaterialsCommandlet.generated.h"

/**
 * Makes the decal materials the fight effects use for wounds and blood (docs/design/fights.md), into
 * /Game/Rim3D/Generated/Fights (not in Git; run this again to make them; one that is there already is kept):
 * M_WoundDecal, and M_BloodSplat for the creator's fight demo (docs/design/creator-weapons.md: a ragged splash with
 * thrown-off drops, different per `Seed`, with `Color`, `Roughness` and `Opacity`).
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=FightMaterials -unattended -nosplash -nullrhi
 *
 * Why not an engine material: the engine's default decal has a fixed picture and no colour. This one is
 * a deferred decal that blends a round soft-edged blob of the `Color` parameter into the surface it lands on.
 */
UCLASS()
class UFightMaterialsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFightMaterialsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
