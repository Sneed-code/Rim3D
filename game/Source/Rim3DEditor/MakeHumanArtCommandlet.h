#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "MakeHumanArtCommandlet.generated.h"

/**
 * Brings MakeHuman (MPFB) characters exported from Blender (tools/makehuman/mh_export.py) into the game
 * (docs/design/makehuman.md): imports each FBX as a skeletal mesh, makes its skin material on our skin
 * material, an IK rig for its skeleton and a retargeter from the game's mannequin, into
 * /Game/Rim3D/Generated/MakeHuman (not in Git; run this again to make them).
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=MakeHumanArt -unattended -nosplash -nullrhi
 *
 * -Probe logs what the Game Animation Sample's generic retarget setup looks like and saves nothing.
 * -Local=<fbx> also imports a local-only comparison model into /Game/ThirdParty/Local (never in Git).
 */
UCLASS()
class UMakeHumanArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMakeHumanArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
