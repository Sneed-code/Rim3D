// Smoke test `worldmap` (task W3): the world screen drawn, a tile picked, its facts, and a colony started on it, in the game, offscreen.
#include "SmokeTest.h"

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"
#include "WorldScreenSubsystem.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/World.h"

#include <string>

namespace
{
	const FVector WorldMapSmokeOrigin(-780000.0, 0.0, 0.0);

	struct FWorldMapSmokeState
	{
		int32 StartTile = -1;
		int32 WaterTile = -1;
		int32 SettlementTile = -1;
		int32 GenerationBefore = 0;
		FString BiomeOfStart;
	};
}

void USmokeTestSubsystem::BuildWorldMapPhases()
{
	const TSharedRef<FWorldMapSmokeState> State = MakeShared<FWorldMapSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Screen = [this]() { return GetWorld()->GetSubsystem<UWorldScreenSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };

	// 1. A sim with no world: the screen has a preview world, and is closed.
	FPhase Setup;
	Setup.Name = TEXT("worldmap_setup");
	Setup.WarmupSeconds = 2.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Screen() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UWorldScreenSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		B->SetWorldOrigin(WorldMapSmokeOrigin);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		if (!Overseer()->EnterOverseerMode())
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		State->GenerationBefore = B->GetSimGeneration();
		Screen()->MakeWorld(7);
	};
	Setup.Verify = [=, this]()
	{
		UWorldScreenSubsystem* S = Screen();
		const colony::World* World = S->GetShownWorld();
		if (!World || World->landTileCount() <= 0)
		{
			Fail(TEXT("worldmap_setup: the screen has no world to show"));
			return;
		}
		if (!S->IsPreview() || S->IsOpen())
		{
			Fail(TEXT("worldmap_setup: wanted a closed screen with a preview world"));
		}
		if (Bridge()->GetSim()->world())
		{
			Fail(TEXT("worldmap_setup: the sim has a world already"));
		}
		// Tile maths: the tile under a pixel.
		if (UWorldScreenSubsystem::TileAt(*World, 100.0f, 50.0f, 8.0f, 100.0f + 8.0f * 3 + 1.0f, 50.0f + 8.0f * 2 + 1.0f) != World->index(3, 2) ||
			UWorldScreenSubsystem::TileAt(*World, 100.0f, 50.0f, 8.0f, 99.0f, 60.0f) != -1 ||
			UWorldScreenSubsystem::TileAt(*World, 100.0f, 50.0f, 8.0f, 100.0f + 8.0f * World->width() + 1.0f, 60.0f) != -1)
		{
			Fail(TEXT("worldmap_setup: TileAt picks the wrong tile"));
		}
		// A start tile, a water tile and a settlement tile to try.
		for (int32 Tile = 0; Tile < World->tileCount(); ++Tile)
		{
			const colony::WorldTile& T = World->tile(Tile);
			if (State->StartTile < 0 && World->canStartAt(Tile) && T.river != colony::RiverSize::None && T.road != colony::RoadKind::None)
			{
				State->StartTile = Tile;
			}
			if (State->WaterTile < 0 && T.water)
			{
				State->WaterTile = Tile;
			}
			if (State->SettlementTile < 0 && T.settlement != colony::kNoSettlement)
			{
				State->SettlementTile = Tile;
			}
		}
		if (State->StartTile < 0)
		{
			for (int32 Tile = 0; Tile < World->tileCount() && State->StartTile < 0; ++Tile)
			{
				if (World->canStartAt(Tile))
				{
					State->StartTile = Tile;
				}
			}
		}
		if (State->StartTile < 0 || State->WaterTile < 0 || State->SettlementTile < 0)
		{
			Fail(FString::Printf(TEXT("worldmap_setup: the world lacks a start tile (%d), water (%d) or a settlement (%d)"), State->StartTile, State->WaterTile, State->SettlementTile));
			return;
		}
		Notes.Add(FString::Printf(TEXT("worldmap_setup: %dx%d tiles, %d land, %d settlements, start tile %d"), World->width(), World->height(), World->landTileCount(),
			static_cast<int32>(World->settlements().size()), State->StartTile));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. The screen open with a tile picked: the map is drawn, the facts are there, water and settlements refuse.
	FPhase Open;
	Open.Name = TEXT("worldmap_open");
	Open.WarmupSeconds = 1.5f;
	Open.MeasureSeconds = 1.0f;
	Open.Setup = [=, this]()
	{
		UWorldScreenSubsystem* S = Screen();
		if (!S || State->StartTile < 0)
		{
			return;
		}
		S->SetOpen(true);
		if (!S->SelectTile(State->StartTile))
		{
			Fail(TEXT("worldmap_open: SelectTile refused a tile on the map"));
		}
		if (S->SelectTile(-1) || S->SelectTile(S->GetShownWorld()->tileCount()))
		{
			Fail(TEXT("worldmap_open: SelectTile took a tile off the map"));
		}
		S->SelectTile(State->StartTile);
	};
	Open.Verify = [=, this]()
	{
		UWorldScreenSubsystem* S = Screen();
		const colony::World* World = S->GetShownWorld();
		if (S->GetStats().FramesDrawn == 0 || S->GetStats().TilesDrawn != World->tileCount())
		{
			Fail(FString::Printf(TEXT("worldmap_open: drew %d frames, %d of %d tiles (ImGui not usable?)"), S->GetStats().FramesDrawn, S->GetStats().TilesDrawn, World->tileCount()));
		}
		const TArray<FString> Facts = S->DescribeTile(State->StartTile);
		bool bBiome = false;
		bool bClimate = false;
		for (const FString& Line : Facts)
		{
			bBiome = bBiome || Line.StartsWith(TEXT("Biome:"));
			bClimate = bClimate || Line.StartsWith(TEXT("Temperature:"));
		}
		if (!bBiome || !bClimate)
		{
			Fail(TEXT("worldmap_open: the tile's facts lack the biome or the temperature"));
		}
		if (!S->WhyNotStart(State->StartTile).IsEmpty())
		{
			Fail(TEXT("worldmap_open: a good tile says a colony can't start there"));
		}
		if (S->WhyNotStart(State->WaterTile).IsEmpty() || S->WhyNotStart(State->SettlementTile).IsEmpty())
		{
			Fail(TEXT("worldmap_open: water or a settlement did not refuse a colony"));
		}
		S->SelectTile(State->WaterTile);
		if (S->StartHere())
		{
			Fail(TEXT("worldmap_open: a colony started on water"));
		}
		S->SelectTile(State->SettlementTile);
		if (S->StartHere())
		{
			Fail(TEXT("worldmap_open: a colony started on a settlement"));
		}
		if (Bridge()->GetSimGeneration() != State->GenerationBefore)
		{
			Fail(TEXT("worldmap_open: a refused start still replaced the sim"));
		}
		S->SelectTile(State->StartTile);
		Notes.Add(FString::Printf(TEXT("worldmap_open: %d tiles drawn, %d facts"), S->GetStats().TilesDrawn, Facts.Num()));
	};
	Phases.Add(MoveTemp(Open));

	// 3. Start the colony: the sim has this world, this start tile, and its map is the tile's.
	FPhase Start;
	Start.Name = TEXT("worldmap_start");
	Start.WarmupSeconds = 2.0f;
	Start.MeasureSeconds = 0.5f;
	Start.Setup = [=, this]()
	{
		UWorldScreenSubsystem* S = Screen();
		if (!S || State->StartTile < 0)
		{
			return;
		}
		const colony::World* World = S->GetShownWorld();
		State->BiomeOfStart = UTF8_TO_TCHAR(World->biomeName(World->tile(State->StartTile)).c_str());
		if (!S->StartHere())
		{
			Fail(TEXT("worldmap_start: StartHere failed on a good tile"));
		}
	};
	Start.Verify = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UWorldScreenSubsystem* S = Screen();
		const colony::Sim* Sim = B->GetSim();
		if (!Sim || !Sim->world() || Sim->world()->startTile() != State->StartTile)
		{
			Fail(TEXT("worldmap_start: the sim does not have the picked start tile"));
			return;
		}
		if (B->GetSimGeneration() == State->GenerationBefore)
		{
			Fail(TEXT("worldmap_start: the sim was not replaced"));
		}
		if (!Sim->site() || UTF8_TO_TCHAR(Sim->site()->biome.c_str()) != State->BiomeOfStart || Sim->site()->tile != State->StartTile)
		{
			Fail(TEXT("worldmap_start: the sim's site is not the picked tile"));
		}
		if (S->IsPreview() || S->IsOpen())
		{
			Fail(TEXT("worldmap_start: the screen should be closed and showing the sim's world"));
		}
		if (S->GetShownWorld() != Sim->world())
		{
			Fail(TEXT("worldmap_start: the screen does not show the sim's world"));
		}
		Notes.Add(FString::Printf(TEXT("worldmap_start: colony on tile %d, %s"), State->StartTile, *State->BiomeOfStart));
	};
	Phases.Add(MoveTemp(Start));

	// 4. The screen again, now marking the colony's tile.
	FPhase Again;
	Again.Name = TEXT("worldmap_colony");
	Again.WarmupSeconds = 1.5f;
	Again.MeasureSeconds = 1.0f;
	Again.Setup = [=, this]()
	{
		if (UWorldScreenSubsystem* S = Screen())
		{
			S->SetOpen(true);
		}
	};
	Again.Verify = [=, this]()
	{
		UWorldScreenSubsystem* S = Screen();
		if (S->GetSelectedTile() != State->StartTile)
		{
			Fail(TEXT("worldmap_colony: the colony's tile is not the picked one"));
		}
		S->SetOpen(false);
	};
	Phases.Add(MoveTemp(Again));
}
