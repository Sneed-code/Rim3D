#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Map.h"

#include <cstdint>

namespace colony {

// Bodies in water (docs/design/water.md): how deep a pawn stands, whether it wades or swims, how fast
// it can go and how the current pushes it. The map holds the water itself (Map::waterSurface,
// Map::waterDepthAt, Map::flowAt).

// How tall a pawn is when nothing better is known: the average man, 1.80 m (as Looks).
inline constexpr float kPersonHeightM = 1.8f;

// How a body meets the water, by depth as a share of its height (ours; RimWorld has no depths, only
// "shallow" and "chest-deep" water):
//   Dry      no water, or under kWaterAnkleShare (a puddle, a marsh film over a tall body)
//   Ankle    up to the knee: in a water cell RimWorld's shallow water 30; marsh and mud keep their 14
//   Waist    from kWaterWaistShare: RimWorld's shallow water 30
//   Chest    from kWaterChestShare: RimWorld's chest-deep moving water, 42
//   Swimming from kWaterSwimShare (the shoulders): it swims, at kSwimSpeedFactor
enum class WaterBand : uint8_t { Dry, Ankle, Waist, Chest, Swimming };
inline constexpr float kWaterAnkleShare = 0.03f;
inline constexpr float kWaterWaistShare = 0.30f;
inline constexpr float kWaterChestShare = 0.60f;
inline constexpr float kWaterSwimShare = 0.80f;
// RimWorld's path costs of shallow and chest-deep moving water (its Terrain defs).
inline constexpr float kWaterShallowPathCost = 30.0f;
inline constexpr float kWaterChestPathCost = 42.0f;
// A swimmer keeps this share of its walking speed (ours: about 0.9 m/s for a man, a steady crawl).
inline constexpr float kSwimSpeedFactor = 0.2f;
// How much of the current carries a body along: all of it for a swimmer, part for a wader whose
// feet hold the bottom (ours).
inline constexpr float kWaterCarrySwim = 1.0f;
inline constexpr float kWaterCarryChest = 0.5f;
inline constexpr float kWaterCarryWaist = 0.25f;
// Water this deep (metres) puts out a burning body.
inline constexpr float kWaterQuenchDepthM = 0.3f;

COLONY_API WaterBand waterBandFor(float depthM, float bodyHeightM);

// What the water does to a body at a point.
struct WaterContact {
    float depth = 0.0f;  // metres of water over the ground at the point
    WaterBand band = WaterBand::Dry;
    float flowX = 0.0f;  // the current there, metres per real second at 1x (divide by 30 for a step)
    float flowY = 0.0f;
    float carry = 0.0f;  // share of the current that carries this body along (kWaterCarry...)
    bool swimming() const { return band == WaterBand::Swimming; }
    bool wet() const { return band != WaterBand::Dry; }
};
COLONY_API WaterContact waterContactAt(const Map& map, float x, float y, int level, float bodyHeightM = kPersonHeightM);

// Share of its walking speed a body keeps in water of this band (1 when dry: the ground decides then).
COLONY_API float waterSpeedFactor(WaterBand band);

// A swimmer may be where walking can't: in deep water, as long as no building blocks it. True when a
// body of `radius` fits at (x, y) counting deep water cells as open.
COLONY_API bool swimmerFitsAt(const Map& map, float x, float y, float radius, int level = 0);
// The cell a swimmer in deep water heads for to get out: the nearest walkable ground cell within
// `radius` metres (nearest first, ties to the lower row then column). False if none.
COLONY_API bool nearestShore(const Map& map, float x, float y, int level, int radius, CellPos& out);

}  // namespace colony
