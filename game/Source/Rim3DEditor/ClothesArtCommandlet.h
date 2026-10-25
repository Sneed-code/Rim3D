#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "ClothesArtCommandlet.generated.h"

/**
 * A5 clothes art (docs/design/looks.md): makes the meshes of the apparel defs listed in assets/clothes_art.json at
 * /Game/Rim3D/Generated/Clothes (not in Git). Body garments are copies of the PawnLooks hoodie and trousers (so the
 * body shape morphs come along) with parts cut away, thickened and given a flat colour material; hats go on the head
 * bone, already in its space. Run PawnLooks and tools/clothes_art.py first, then:
 *
 *   UnrealEditor-Cmd.exe Rim3D.uproject -run=ClothesArt -unattended -nosplash -nullrhi
 */
UCLASS()
class UClothesArtCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UClothesArtCommandlet();
	virtual int32 Main(const FString& Params) override;
};
