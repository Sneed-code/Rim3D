#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"

#include "GarmentCoverData.generated.h"

/**
 * On a MakeHuman garment (SK_MHG_*, made by the MakeHumanArt commandlet): which bit of its body's garment cover texture
 * marks the skin it covers (docs/design/clothes-draw.md "Skin under clothes"). While it is worn, the pawn hides the skin
 * there, so the body never pokes through.
 */
UCLASS()
class RIM3D_API UGarmentCoverData : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** 0 to 31; -1 for a garment that hides no skin (hats, gloves). */
	UPROPERTY(EditAnywhere, Category = "Garment")
	int32 CoverBit = -1;
};
