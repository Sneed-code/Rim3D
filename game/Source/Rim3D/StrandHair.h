#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "StrandHair.generated.h"

class UGroomAsset;
class UGroomComponent;
class USkeletalMeshComponent;

/**
 * Strand hair on MakeHuman bodies (docs/design/hair-physics.md "Strand hair" and "Growing our own grooms").
 *
 * Two kinds of groom:
 * - A groom made for the MetaHuman head (the PawnFaces copies, GA_Hair_*), moved onto this body's head (its head bone
 *   where the MetaHuman's is, scaled by the two heads' neck-to-head lengths): the creator's -CreatorGroom= test.
 * - Our own grooms, grown on the creator bases by tools/makehuman/grow_hair.py and imported by the HairGrooms
 *   commandlet (GA_<base>_<Style> in /Game/Rim3D/Generated/Grooms): their curves are already relative to that base's
 *   head bone, so they only need scaling by the neck-to-head lengths.
 * Either rides on the head bone, so it turns and moves with it; Unreal's groom physics swings the strands and keeps them
 * off the body through the mannequin physics asset (same bone names). Cosmetic only.
 *
 * Tuning (console): Pawns.StrandHairScale (times the fitted size), Pawns.StrandHairOffsetX/Y/Z (cm, the body's
 * component axes), Pawns.StrandHairYaw (degrees, if a body faces another way than the MetaHuman meshes).
 */
UGroomComponent* StrandHairAttach(USkeletalMeshComponent* Body, UGroomAsset* Groom, UGroomComponent* Existing);

/** The groom asset of that name in the PawnFaces folder (e.g. "GA_Hair_Long"), or null. */
UGroomAsset* StrandHairFind(const FString& Name);

/** Where the HairGrooms commandlet puts our own grooms, and their names. */
inline const TCHAR* const StrandHairGrownFolder = TEXT("/Game/Rim3D/Generated/Grooms");
inline FString StrandHairGrownName(const FString& Base, const FString& Style) { return TEXT("GA_") + Base + TEXT("_") + Style; }

/** Our own groom grown on the creator base Base (e.g. "CreatorFemale") for the creator hair style CreatorStyle (e.g.
 *  "Long", mapped through assets/hair_grooms.json), or null (no such style, or not imported). */
UGroomAsset* StrandHairFindGrown(const FString& Base, const FString& CreatorStyle);

/** Puts our own groom (grown on the mesh Base's head) on Body. Existing is reused if given. */
UGroomComponent* StrandHairAttachGrown(USkeletalMeshComponent* Body, UGroomAsset* Groom, const USkeletalMesh* Base, UGroomComponent* Existing);

/** The pawn's hair colour (linear) on a groom's MetaHuman hair material (melanin and redness). */
void StrandHairSetColour(UGroomComponent* Groom, const FLinearColor& Hair);

/**
 * Who wears strands (docs/design/hair-physics.md "Near and far"). Strands cost GPU time per person, so only the people
 * nearest the camera wear them: up to Pawns.StrandHairMax people within Pawns.StrandHairNear metres (a metre's grace
 * before they switch back). Everyone else wears their wig mesh, which is shown again as their strands go. Bodies ask
 * with Want (the creator's apply does, for every MakeHuman person); a style with no grown groom keeps its wig.
 * Pawns.StrandHair 0 puts everyone in wigs.
 */
UCLASS()
class RIM3D_API UStrandHairSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Body wants strands for the creator hair style CreatorStyle (e.g. "Long") in this colour (linear); WigSections are
	 *  the body's material sections of that style's wig (hidden while the strands show). Base is the creator base the
	 *  body is built on. An empty style (bald) or a style with no grown groom leaves the wig as it is. */
	void Want(USkeletalMeshComponent* Body, const FString& Base, const FString& CreatorStyle, const FLinearColor& Colour, const TArray<int32>& WigSections);

	/** How many people wear strands now (for tests and notes). */
	int32 NumWearing() const;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	struct FWearer
	{
		TWeakObjectPtr<USkeletalMeshComponent> Body;
		TWeakObjectPtr<UGroomAsset> Groom;
		TWeakObjectPtr<const USkeletalMesh> Base;
		TWeakObjectPtr<UGroomComponent> Strands;
		FLinearColor Colour = FLinearColor::Black;
		TArray<int32> WigSections;
		bool bStrands = false;
	};
	TArray<FWearer> Wearers;
	float SinceSort = 1.0f;

	void Show(FWearer& Wearer, bool bStrands);
};
