#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "ColonySim/World.h"

#include <optional>

#include "WorldScreenSubsystem.generated.h"

class USimSubsystem;
class UTexture2D;

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
 * The world screen (task W3, docs/design/world-map.md): the planet's tiles drawn as a map of hexagons, a tile picked
 * with the mouse, the tile's facts beside it, and a button that starts a colony there.
 *
 * - Open with M or the "World map" button (top right), close with M, Esc or Close. Console: World.Screen 0|1.
 * - The sim's grid is drawn as hexagons, every other row shifted half a tile (the sim keeps its square grid and eight
 *   neighbours; only the picture is hexagonal). The map is painted once per world into a picture (BuildMapPicture):
 *   biome colours with their own texture (trees, dunes, ice), shaded relief from the tiles' heights (hills and
 *   mountains, snowy peaks where it is cold), seas by depth with shallows and foam along the coasts, and a fine hex
 *   grid. Rivers (smooth blue curves), roads (brown to black: dirt, stone, asphalt), settlements, the colony's tile, the
 *   hovered and the picked hexagon are drawn over it every frame. Mouse wheel zooms (at the pointer), dragging moves.
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
	/** The tile under a screen position given the map's top left corner and the hexagons' width in pixels (the same maths
	 *  the click uses); -1 off the map. */
	static int32 TileAt(const colony::World& World, float MapLeft, float MapTop, float TileSize, float X, float Y);
	/** Where a tile's hexagon has its middle on screen, for the same map corner and hexagon width. */
	static FVector2D TileCentre(const colony::World& World, float MapLeft, float MapTop, float TileSize, int32 Tile);
	/** The whole map's size in hexagon widths (columns plus the half a shifted row sticks out; rows overlap). */
	static FVector2D MapSizeInTiles(const colony::World& World);
	/** Why a colony can't start on a tile, or an empty string when it can. */
	FString WhyNotStart(int32 Tile) const;
	/** The facts the panel lists for a tile, one per line. */
	TArray<FString> DescribeTile(int32 Tile) const;
	/** Restarts the sim on the shown world with the picked tile as the start. False (and nothing changes) if
	 *  no tile is picked or a colony can't start there. */
	bool StartHere();

	const FWorldScreenStats& GetStats() const { return Stats; }

	/** New-game mode (docs/design/new-game.md): only tiles of these biomes (Biomes defNames) can be settled, the others
	 *  are dimmed, the button reads "Land here" and Back returns to the menu. An empty list ends the mode. */
	void SetNewGame(const TArray<FString>& Biomes);
	bool IsNewGame() const { return bNewGame; }
	/** True once after a landing in new-game mode (StartHere started the colony). */
	bool ConsumeLanding();
	/** True once after Back (or Esc) in new-game mode. */
	bool ConsumeCancel();
	/** Picks a random tile a colony may start on (new-game biomes only, in that mode). False if there is none. */
	bool PickRandomSite();

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	USimSubsystem* GetBridge() const;
	void DrawScreen(USimSubsystem& Bridge);
	/** Paints the shown world's map picture (and its halves) if the world or the new-game biomes changed. */
	void BuildMapPicture(const colony::World& World);

	/** The map picture at full size, half and a quarter (the smallest still as big as it is drawn is used). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture2D>> MapPictures;
	FString MapPictureKey;
	/** The view: zoom (1 shows the whole map) and the map point at the middle of the view, in hexagon widths. */
	float MapZoom = 1.0f;
	FVector2D MapViewCentre = FVector2D(-1.0, -1.0);
	bool bMapDragged = false;

	bool bOpen = false;
	bool bPreview = true;
	std::optional<colony::World> Preview;
	int32 Seed = 42;
	int32 SeedInput = 42;
	int32 Selected = -1;
	int32 SimGeneration = -1;
	FWorldScreenStats Stats;
	bool bNewGame = false;
	/** Set by SetNewGame: the next draw picks a random valid site once. */
	bool bPickOnOpen = false;
	bool bLanded = false;
	bool bCancelled = false;
	TArray<FString> NewGameBiomes;
	/** In new-game mode, whether a tile's biome may be settled (always true outside it). */
	bool NewGameAllows(const colony::World& World, int32 Tile) const;
};
