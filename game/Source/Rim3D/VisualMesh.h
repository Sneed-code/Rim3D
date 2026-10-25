#pragma once

#include "CoreMinimal.h"

#include "ColonySim/Defs.h"

#include <string>

class UMaterialInterface;
class UObject;
class UStaticMesh;

/**
 * Small solid shapes (boxes, cylinders, cones, spheres) put together into one static mesh, with the colours
 * baked into the vertices, for everything the game draws from a def's `visual` before A1's real meshes
 * replace them: plants (terrain.md) and buildings and items (building-draw.md).
 */
namespace VisualMesh
{
	enum class EShape : uint8
	{
		Cylinder,
		Cone,
		Sphere,
		Box,
	};

	/** One solid part of a thing's picture. Lengths are centimetres. */
	struct FPart
	{
		EShape Shape = EShape::Box;
		/** Box: width (x), depth (y), height (z). Cylinder, cone, sphere: radius along x, radius along y, height. */
		FVector3f Size = FVector3f(100.0f, 100.0f, 100.0f);
		/** The bottom centre of the unrotated part, from the middle of the thing's footprint at ground level; x east, y north when the thing faces north. */
		FVector3f At = FVector3f::ZeroVector;
		/** Turns the part about its own middle. */
		FRotator3f Rotation = FRotator3f::ZeroRotator;
		/** Linear colour. A part made of `stuff` multiplies the stuff's colour by it. */
		FLinearColor Color = FLinearColor(0.5f, 0.5f, 0.5f);
		bool bStuff = false;
		/** Centimetres this part moves along x when a door is fully open; 0 for a part that stays put. */
		float Slide = 0.0f;
	};

	/** Which parts a mesh is made of. */
	enum class EPartSet : uint8
	{
		All,
		/** The parts that stay put. */
		Fixed,
		/** Only the parts that slide (a door's leaf). */
		Sliding,
	};

	/** A colour written in a def as red, green, blue from 0 to 1 the way you'd pick it in a paint program, as a linear colour. */
	FLinearColor ReadColor(const colony::Def& Def, const std::string& Path, const FLinearColor& Fallback);

	/** A plant's stem or crown: `Path` holds shape, radius, height, lift and color (terrain.md). False if the def has no such part. */
	bool ReadPlantPart(const colony::Def& Def, const std::string& Path, FPart& Out);

	/**
	 * Reads `<Path>.parts`, a list of parts, as docs/design/building-draw.md describes. A box edge written as 0
	 * or less is the footprint's length on that axis plus the number, so 0 fills it. `FootprintCm` is the thing's
	 * footprint as the def has it, before rotation. A part with `corners` is repeated at the footprint's four
	 * corners. `<Path>.tint`, if there, multiplies every part's colour.
	 */
	void ReadParts(const colony::Def& Def, const std::string& Path, const FVector2f& FootprintCm, TArray<FPart>& Out);

	/** How high the parts reach, cm. */
	float HeightOf(const TArray<FPart>& Parts);

	/**
	 * A mesh of the parts, flat shaded, standing on z = 0 with the footprint's middle at x = y = 0; null if
	 * the set is empty. `StuffColor` colours the parts made of stuff (a grey if it is null). `bBakeLight` darkens
	 * downward faces and the lower parts a little, for the plants, which the shared material would otherwise draw flat.
	 */
	UStaticMesh* MakeMesh(UObject* Outer, UMaterialInterface* Material, const TArray<FPart>& Parts, EPartSet Set, const FLinearColor* StuffColor, bool bBakeLight);
}
