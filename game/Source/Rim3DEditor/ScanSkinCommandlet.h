#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ScanSkinCommandlet.generated.h"

/**
 * Writes what the scanned skin bake needs (docs/design/scanned-skin.md, assets/scan_skin.json) to game/Saved/ScanSkin:
 * the MetaHuman body and face meshes (points, texture layout, material slots, skin weights and skeleton, as JSON),
 * the MetaHuman Creator's scanned skin textures as PNG, a list of every texture under the listed folders, and face
 * colour and normal maps from the Creator's texture synthesis model for the listed presets.
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=ScanSkin -unattended -nosplash -nullrhi [-force]
 *
 * Nothing is saved into the project; tools/makehuman/scan_skin.py does the bake in Blender.
 */
UCLASS()
class UScanSkinCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UScanSkinCommandlet();
	virtual int32 Main(const FString& Params) override;
};
