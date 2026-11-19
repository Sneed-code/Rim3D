#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ShieldArtCommandlet.generated.h"

/**
 * Imports the 12 shield models (docs/design/shields.md "Models") with their own materials into
 * /Game/Rim3D/Generated/Shields/<name>/<name> (not in Git; run this again to make them):
 *
 *   blender -b --python tools/shield_models.py -- game/Content/ThirdParty/Shields
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=ShieldArt -unattended -nosplash -AllowCommandletRendering
 *
 * Every SM_Shield_<Tier>_<Size>.glb in game/Content/ThirdParty/Shields/ becomes one static mesh of the same name
 * (its pieces joined), which the shield defs name in visual.models by material.
 */
UCLASS()
class UShieldArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UShieldArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
