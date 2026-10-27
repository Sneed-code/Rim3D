#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "PawnFacesCommandlet.generated.h"

/**
 * V2/V3 faces, hair and skin marks (docs/design/looks.md): makes /Game/Rim3D/Generated/PawnFaces (not in Git) from
 * the engine's MetaHuman Creator plugin, nothing downloaded: face shape morphs on a copy of Kellan's face from the
 * Creator's preset faces, the hair styles and beards of assets/pawn_faces.json copied from its grooms and bound to
 * that face, eye colour, and the tattoo, scar and body hair decals. Run PawnLooks first, then:
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=PawnFaces -unattended -nosplash -nullrhi
 *
 * -Probe only logs what it finds (Kellan's face and grooms, how the preset faces line up) and saves nothing.
 */
UCLASS()
class UPawnFacesCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPawnFacesCommandlet();
	virtual int32 Main(const FString& Params) override;
};
