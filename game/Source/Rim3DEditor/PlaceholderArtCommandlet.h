#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "PlaceholderArtCommandlet.generated.h"

/**
 * A1 placeholder art (docs/design/building-draw.md): imports the free meshes `tools/placeholder_art.py` downloaded
 * (game/Content/ThirdParty/PlaceholderArt/, not in Git) and makes one static mesh per def listed in
 * assets/placeholder_art.json, at /Game/Rim3D/Generated/Art/SM_<def> (not in Git). Each one is turned to face the
 * way the list says, scaled to the size it stands in for, stood on z = 0 with its middle at x = y = 0, and cut down
 * to the triangle count in the list, so the game can use it as it is. Run it again after a fresh clone:
 *
 *   python tools/placeholder_art.py
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=PlaceholderArt -unattended -nosplash -nullrhi
 */
UCLASS()
class UPlaceholderArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPlaceholderArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
