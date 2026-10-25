#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "AnimalArtCommandlet.generated.h"

/**
 * A4 animal art: imports the free animal FBX files `tools/animal_art.py` downloaded (game/Content/ThirdParty/Animals/,
 * not in Git) as skeletal meshes with their skeletons and animation clips, at /Game/Rim3D/Generated/Animals/<model>
 * (not in Git). Which model stands in for which animal def is assets/animal_art.json. Run it again after a fresh clone:
 *
 *   python tools/animal_art.py
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=AnimalArt -unattended -nosplash -nullrhi
 */
UCLASS()
class UAnimalArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAnimalArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
