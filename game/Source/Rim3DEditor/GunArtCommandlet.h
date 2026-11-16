#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "GunArtCommandlet.generated.h"

/**
 * Imports the gun range's gun models (docs/design/gun-range.md) listed in assets/guns.json ("art" of each gun) into
 * /Game/Rim3D/Generated/Guns/<Name>/ (not in Git): each FBX object becomes its own static mesh (so a bolt, trigger or
 * magazine can move), and the listed textures go onto M_GunPBR (made here once), a plain metal/rough material with a
 * base colour, normal, and either roughness and metallic maps or an ORM map.
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=GunArt -unattended -nosplash -nullrhi [-Guns=Name,Name]
 *
 * Source files are absolute paths or relative to the repo; models (Fab, Codex) are not in Git.
 */
UCLASS()
class UGunArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGunArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
