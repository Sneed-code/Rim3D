#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "PawnLooksCommandlet.generated.h"

/**
 * Makes the assets seeded pawn looks need (docs/design/looks.md), from the MetaHuman sample and
 * the engine's MetaHuman body type files, into /Game/Rim3D/Generated/PawnLooks (not in Git; run
 * this again to make them):
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=PawnLooks -unattended -nosplash
 *
 * -Probe only logs what it finds (meshes, materials, how the body files line up) and saves nothing.
 */
UCLASS()
class UPawnLooksCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPawnLooksCommandlet();
	virtual int32 Main(const FString& Params) override;
};
