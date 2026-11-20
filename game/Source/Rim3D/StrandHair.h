#pragma once

#include "CoreMinimal.h"

class UGroomAsset;
class UGroomComponent;
class USkeletalMeshComponent;

/**
 * Strand hair on MakeHuman bodies (docs/design/hair-physics.md "Strand hair"): a groom made for the MetaHuman head (the
 * PawnFaces copies, GA_Hair_*) worn on any body with a "head" bone. The groom's curves are in the MetaHuman face mesh's
 * component space; they are moved onto this body's head (its head bone where the MetaHuman's is, scaled by the two
 * heads' neck-to-head lengths) and ride on the head bone, so they turn and move with it; Unreal's groom physics swings
 * the strands and keeps them off the body through the mannequin physics asset (same bone names). Cosmetic only.
 *
 * Tuning (console): Pawns.StrandHairScale (times the fitted size), Pawns.StrandHairOffsetX/Y/Z (cm, the body's
 * component axes), Pawns.StrandHairYaw (degrees, if a body faces another way than the MetaHuman meshes).
 */
UGroomComponent* StrandHairAttach(USkeletalMeshComponent* Body, UGroomAsset* Groom, UGroomComponent* Existing);

/** The groom asset of that name in the PawnFaces folder (e.g. "GA_Hair_Long"), or null. */
UGroomAsset* StrandHairFind(const FString& Name);
