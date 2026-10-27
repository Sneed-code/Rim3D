#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/World.h"

#include <optional>

#include "WorldScreenSubsystem.generated.h"

class USimSubsystem;

/** What the world screen drew and did (smoke test `worldmap`). */
struct FWorldScreenStats
{
	int32 FramesDrawn = 0;
	/** Tiles drawn in the last frame. */
	int32 TilesDrawn = 0;
	int32 TilesPicked = 0;
	int32 ColoniesStarted = 0;
};

/**
 * The world screen (task W3, docs/design/world-map.md): the planet's tiles drawn as a map with ImGui,
 * a tile picked with the mouse, the tile's facts beside it, and a button that starts a colony there.
 *
 * - Open with M or the "World map" button (top right), close with M, Esc or Close. Console: World.Screen 0|1.
 * - Colours show the biome (darker for hills), open water by depth; rivers are blue lines, roads brown
 *   to black (dirt, stone, asphalt), settlements red dots, the colony's tile a yellow ring.
 * - A click picks a tile; the panel gives its biome, hills, height, climate, river, road and settlement.
 * - "Start colony here" restarts the sim on this world with that start tile (the same scenario as the
 *   running game). A tile a colony can't start on (sea, impassable, a settlement) says why and has no button.
 * - With no world in the running sim, the map shown is a preview made from a seed ("Make world" changes it);
 *   with one, it is the sim's own world, and "Make world" swaps in a preview of another seed.
 */
UCLASS()
class RIM3D_API UWorldScreenSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	bool IsOpen() const { return bOpen; }
	void SetOpen(bool bNewOpen);

	/** The map on screen: the preview's, or the running sim's. Null when there are no world defs. */
	const colony::World* GetShownWorld() const;
	/** True when the map is a preview rather than the running sim's world. */
	bool IsPreview() const { return bPreview; }
	/** Makes a preview world from a seed and shows it. Clears the picked tile. */
	void MakeWorld(int32 NewSeed);
	int32 GetSeed() const { return Seed; }

	/** -1 when no tile is picked. */
	int32 GetSelectedTile() const { return Selected; }
	/** Picks a tile as a click does. False for a tile off the map. */
	bool SelectTile(int32 Tile);
	/** The tile under a screen position given the map's rectangle (the same maths the click uses). */
	static int32 TileAt(const colony::World& World, float MapLeft, float MapTop, float TileSize, float X, float Y);
	/** Why a colony can't start on a tile, or an empty string when it can. */
	FString WhyNotStart(int32 Tile) const;
	/** The facts the panel lists for a tile, one per line. */
	TArray<FString> DescribeTile(int32 Tile) const;
	/** Restarts the sim on the shown world with the picked tile as the start. False (and nothing changes) if
	 *  no tile is picked or a colony can't start there. */
	bool StartHere();

	const FWorldScreenStats& GetStats() const { return Stats; }

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	void DrawScreen(USimSubsystem& Bridge);
	void DrawOpenButton(float Scale, float Margin);

	bool bOpen = false;
	bool bPreview = true;
	std::optional<colony::World> Preview;
	int32 Seed = 42;
	int32 SeedInput = 42;
	int32 Selected = -1;
	int32 SimGeneration = -1;
	FWorldScreenStats Stats;
};
