#pragma once

#include "CoreMinimal.h"

class AActor;
class USceneComponent;
class UProceduralMeshComponent;

/** What the cut goes through: one big bone (upper arm, thigh), two (forearm, lower leg), or the neck. */
enum class EGoreStumpKind : uint8
{
	OneBone,
	TwoBones,
	Neck,
};

/**
 * The open end of a cut (docs/design/body-health.md "Gore"): a torn rim of skin that hangs a little back over the limb
 * (it covers the flattened skin's edge), a thin layer of yellow fat, wet dark muscle bulging out unevenly, and the bone
 * standing out of it, broken and jagged, with its marrow; at the neck the spine with its cord and the windpipe. One
 * procedural mesh in M_Gore (vertex colours; M_BloodDrop's red where BloodArt has not made it yet).
 *
 * `At`: the middle of the cut (world). `Outward`: away from what is left, the way the missing part went. `Front`: for
 * the neck, the way the face looked (the windpipe goes there, the spine behind). `Skin`: the body's skin colour.
 * `ClothOuter` over 1: the limb is clothed, and a ring of torn, blood-soaked cloth reaches out to that many radii over
 * the sleeve or trouser leg (else its tube showed hollow round the meat). Attached to `Parent` at `Socket`, so it
 * moves with that bone. Cosmetic.
 */
UProceduralMeshComponent* GoreMakeStump(AActor* Owner, USceneComponent* Parent, FName Socket, const FVector& At, const FVector& Outward,
	const FVector& Front, float RadiusCm, EGoreStumpKind Kind, const FLinearColor& Skin, int32 Seed, float ClothOuter = 1.0f);
