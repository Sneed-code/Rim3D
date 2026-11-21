#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "HairGroomsCommandlet.generated.h"

/**
 * Our own strand hair (docs/design/hair-physics.md "Growing our own grooms"): imports the grooms that
 * tools/makehuman/grow_hair.py grew on each creator base (game/Saved/Grooms/<base>/<Style>.abc, listed in
 * assets/hair_grooms.json) as /Game/Rim3D/Generated/Grooms/GA_<base>_<Style> (not in Git), on the MetaHuman hair
 * material (coloured per pawn by melanin and redness), with guides made on import for the physics, the physics on,
 * and a thinner second LOD for people further off. Run the MakeHumanArt commandlet first (the bases), then:
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=HairGrooms -unattended -nosplash -nullrhi
 */
UCLASS()
class UHairGroomsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UHairGroomsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
