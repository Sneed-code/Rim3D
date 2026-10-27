#pragma once

#include "CoreMinimal.h"

#include "ColonySim/Entity.h"
#include "ColonySim/Grid.h"
#include "ColonySim/Snapshot.h"

class USimSubsystem;

/** What a click or a ray found in the sim world. */
enum class ESimPickKind : uint8
{
	None,
	/** A pawn, item or building. */
	Thing,
	/** A ground cell (nothing standing in the way of the ray). */
	Cell,
};

struct FSimPick
{
	ESimPickKind Kind = ESimPickKind::None;
	colony::Entity Thing = colony::kNoEntity;
	colony::CellPos Cell;
	/** Where the ray hit, in Unreal world units. */
	FVector Location = FVector::ZeroVector;

	bool IsValid() const { return Kind != ESimPickKind::None; }
	bool operator==(const FSimPick& Other) const
	{
		return Kind == Other.Kind && Thing == Other.Thing && Cell == Other.Cell;
	}
};

/**
 * Picking against the sim, not against Unreal collision: the sim decides what is where, and the
 * art (U3, U4, U5) can change without changing what a click means. Things are boxes around their
 * blended position; the ground is the sim's height map, the same surface the terrain mesh shows.
 * Upper floors (docs/design/levels-draw.md) are surfaces too, up to the highest level shown: in the
 * overseer view the levels above the one looked at are cut away, and so are their things.
 */
namespace SimPicking
{
	/** The box a click on this thing hits, in Unreal world units, at its blended position.
	 *  Boxes are slightly generous so a small pawn is easy to hit; building heights are
	 *  placeholders until the buildings' meshes (U4) say how tall they are. */
	bool ThingBox(const USimSubsystem& Bridge, const colony::ThingView& View, FBox& OutBox);

	/** Where a ray first meets a surface within the map. `Level` 0 or more: that level's floor height everywhere (the ground on
	 *  level 0), laid or not. -1: the ground, or the highest upper floor at most `TopLevel` over it; `OutLevel` says which. */
	bool RayHitGround(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction, double& OutDistance,
		int32 Level = 0, int32 TopLevel = 0, int32* OutLevel = nullptr);

	/** The nearest thing along the ray; else the cell of the ground or upper floor it lands on; else nothing. Things and floors
	 *  above the highest level shown (ULevelsSubsystem) are not hit. */
	FSimPick PickFromRay(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction);

	/** The cell the ray lands on at the height of level `Level`, ignoring every thing in the way (a cell for a drag of the architect
	 *  menu on the level looked at, docs/design/orders.md); a Cell pick with its Location, or none. */
	FSimPick PickGroundFromRay(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction, int32 Level = 0);
}
