#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "TreeArtCommandlet.generated.h"

/**
 * Imports the L6 tree models (assets/tree_art.json, downloaded by tools/tree_art.py) whole, with their own materials,
 * into /Game/Rim3D/Generated/Trees/<id>/ (not in Git; run this again to make them):
 *
 *   python tools/tree_art.py
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=TreeArt -unattended -nosplash -AllowCommandletRendering
 *
 * Unlike the A1 placeholder art they are not cut down: Nanite draws them (foliage keeps its area as it simplifies), and
 * the glTF's colour, normal and roughness maps and its masked leaves stay. A model made of several meshes (trunk and
 * leaves) is joined into one, SM_<id>, so a plant def can name it in visual.meshes.
 */
UCLASS()
class UTreeArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UTreeArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
