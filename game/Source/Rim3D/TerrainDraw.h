#pragma once

#include "CoreMinimal.h"

#include "ColonySim/Map.h"

/**
 * How the ground is drawn where the sim's height map isn't the whole story (docs/design/terrain.md).
 * The sim keeps one ground height per cell corner; rock stands on it as a solid block, and roofs
 * hang over it. Everything that has to agree with the picture (picking, the camera's floor) reads
 * the numbers here.
 */
namespace TerrainDraw
{
	/** Metres a rock cell stands above the ground height. The mountain's roof is at this height. */
	constexpr float RockHeightM = 4.0f;

	/** Metres above the ground at which a built (thin) roof is drawn. */
	constexpr float RoofHeightM = 3.0f;

	/** Cells along one side of a terrain chunk (one mesh, one set of plant instances). */
	constexpr int32 ChunkCells = 32;

	/** Centimetres the drawn surface at a point (metres) stands above the sim's ground height there: the rock block. */
	inline double SurfaceBonusCm(const colony::Map& Map, double XMetres, double YMetres)
	{
		const colony::CellPos Cell{FMath::FloorToInt32(XMetres), FMath::FloorToInt32(YMetres), 0};
		return Map.inBounds(Cell) && Map.terrain(Cell) == colony::Terrain::Rock ? RockHeightM * 100.0 : 0.0;
	}
}
