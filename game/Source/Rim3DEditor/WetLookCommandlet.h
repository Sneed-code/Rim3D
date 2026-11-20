#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "WetLookCommandlet.generated.h"

/**
 * Rain makes things wet (docs/design/wetness.md). Makes, into /Game/Rim3D/Generated/Weather (not in Git):
 * - MPC_Weather: `Wetness` (0 dry .. 1 soaked, written by UWetnessSubsystem) and `CoverMap` (the world origin in cm in
 *   xyz, one over the cover texture's size in w);
 * - T_WeatherCover: 1024 x 1024 single floats, one per map cell: the height in metres (over the world origin) of the
 *   highest roof or upper floor over the cell, or -1000 where the sky is open. The game overwrites it when roofs change.
 *
 * Then it adds the wet look to every opaque or masked lit material under /Game/Rim3D/Generated (walls, floors, furniture,
 * Codex and Fab buildings, trees, plants, rocks, guns, animals, vehicles): darker where porous, glossier, faces pointing
 * up wettest, undersides dry, and nothing under a roof (the cover texture, looked up one half cell out along the normal so
 * the outside face of a wall is wet and the inside face dry). Clothes take the wearer's wetness instead (custom primitive
 * data 12, as the MakeHuman skin and hair already do). A material with its own `Wetness` parameter (the ground) keeps
 * it, masked by the roofs. Each material is changed once; run it again after any commandlet that remakes materials:
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=WetLook -unattended -nosplash
 *
 * -List prints what it would change without saving.
 */
UCLASS()
class UWetLookCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UWetLookCommandlet();
	virtual int32 Main(const FString& Params) override;
};
