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
 * The skin's own open edge round a cut, as it is drawn once cut (measured from the skinned mesh): for each of `Bins`
 * directions about the stump's axis (bin 0 along the stump's front, then turning towards its +Y), how far out the edge
 * is and how far along the axis (cm, + the way the lost part went, from the stump's `At`). A stump given one fits it:
 * its face closes the hole flush with the skin instead of a round disc standing over it (sneed 2026-11-27: "the neck
 * gore is like a cut piece of steak that sits over the neck ... it should be a seamless cover over the hole").
 */
struct FGoreOutline
{
	static constexpr int32 Bins = 32;
	float Radius[Bins] = {};
	float Height[Bins] = {};
	bool bValid = false;

	/** The edge at angle Theta (radians from the front), between the bins on either side. */
	void At(float Theta, float& OutRadius, float& OutHeight) const
	{
		// Each bin's value stands at the middle of its arc.
		const float B = FMath::Fmod(Theta / (2.0f * PI) * Bins - 0.5f + 4.0f * Bins, static_cast<float>(Bins));
		const int32 I0 = FMath::FloorToInt32(B) % Bins;
		const int32 I1 = (I0 + 1) % Bins;
		const float T = B - FMath::FloorToFloat(B);
		OutRadius = FMath::Lerp(Radius[I0], Radius[I1], T);
		OutHeight = FMath::Lerp(Height[I0], Height[I1], T);
	}
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
 * moves with that bone. `DomeCm`: how far the skin or cloth still bulges out past the cut (measured from the meshes'
 * skin weights); the face stands that far out, the rim hanging back over the gap. Below 0: not measured, then
 * `Gore.ClothLift` radii on a clothed limb. `Outline` (valid): the skin's measured edge, which the face then follows
 * exactly, flush (DomeCm is not used, and `RadiusCm` only sizes the bones). Cosmetic.
 */
UProceduralMeshComponent* GoreMakeStump(AActor* Owner, USceneComponent* Parent, FName Socket, const FVector& At, const FVector& Outward,
	const FVector& Front, float RadiusCm, EGoreStumpKind Kind, const FLinearColor& Skin, int32 Seed, float ClothOuter = 1.0f, float DomeCm = -1.0f,
	const FGoreOutline* Outline = nullptr);

/** A loose chunk of a body that burst (body-health.md part F, "Trunks burst"). */
enum class EGoreGibKind : uint8
{
	Meat,   // a lump of muscle with a patch of skin and fat on one side
	Bone,   // a broken shard of bone, pale and bloody, with marrow at the snapped ends
	Rib,    // a thin curved rib, meat still on it
	Offal,  // a dark, very wet lump
};

/**
 * One gib: a lumpy closed mesh about `SizeCm` across (a rib or shard that long), centred on its owner's root, in M_Gore
 * vertex colours like the stumps. Attached to `Parent` with no collision (the owner's own sphere does the physics).
 * Faces wound clockwise as seen from outside (Unreal's front). Cosmetic.
 */
UProceduralMeshComponent* GoreMakeGibChunk(AActor* Owner, USceneComponent* Parent, EGoreGibKind Kind, float SizeCm, const FLinearColor& Skin, int32 Seed);
