// Smoke test `world` (task U3): the sim's ground and plants drawn by UTerrainSubsystem, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "ProceduralMeshComponent.h"

#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "TerrainSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <string>
#include <vector>

namespace
{
	const FVector WorldSmokeOrigin(-600000.0, 0.0, 0.0);

	struct FWorldSmokeState
	{
		int32 MinedEvents = 0;
		int32 RemovedEvents = 0;
		bool bListening = false;
		colony::CellPos Placed;
		colony::CellPos Tree;
		int32 ChunkX = 0;
		int32 ChunkY = 0;
		int32 VerticesBefore = 0;
		int64 RebuildsBefore = 0;
		int64 PlantsBefore = 0;
		colony::CellPos Tunnel[4];
		int32 TunnelLength = 0;
		int32 RoofVerticesBefore = 0;
		int32 TunnelChunkX = 0;
		int32 TunnelChunkY = 0;
		FVector TunnelLook = FVector::ZeroVector;
		FVector TunnelDirection = FVector(1.0, 0.0, 0.0);
	};

	void WorldSmokeSetRoofs(int32 Mode)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(Mode, ECVF_SetByCode);
		}
	}

	bool WorldSmokeIsSoilAndClear(const colony::Sim& Sim, int32 X, int32 Y)
	{
		const colony::CellPos Cell{X, Y, 0};
		return Sim.map().inBounds(Cell) && Sim.map().terrain(Cell) == colony::Terrain::Soil && !Sim.plants().has(Cell) && Sim.map().buildingAt(Cell) == colony::kNoEntity;
	}

	/** A soil cell with no plant, no building, within a ring around the stockpile, and flat enough for a pawn to walk up to. */
	bool WorldSmokeFindSoil(const colony::Sim& Sim, colony::CellPos Near, int32 MinRadius, int32 MaxRadius, colony::CellPos& Out)
	{
		for (int32 R = MinRadius; R <= MaxRadius; ++R)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				for (int32 DX = -R; DX <= R; ++DX)
				{
					if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) == R && WorldSmokeIsSoilAndClear(Sim, Near.x + DX, Near.y + DY)
						&& WorldSmokeIsSoilAndClear(Sim, Near.x + DX + 1, Near.y + DY) && WorldSmokeIsSoilAndClear(Sim, Near.x + DX - 1, Near.y + DY))
					{
						Out = colony::CellPos{Near.x + DX, Near.y + DY, 0};
						return true;
					}
				}
			}
		}
		return false;
	}

	/** A rock cell with soil or sand on one side and at least `Length` rock cells in a line the other way. */
	bool WorldSmokeFindTunnel(const colony::Map& Map, int32 Length, colony::CellPos Out[4], FVector& OutDirection)
	{
		static const int32 Dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
		for (int32 Y = 2; Y < Map.height() - 2 - Length; ++Y)
		{
			for (int32 X = 2; X < Map.width() - 2 - Length; ++X)
			{
				for (const int32(&D)[2] : Dirs)
				{
					const colony::CellPos Before{X - D[0], Y - D[1], 0};
					if (!Map.inBounds(Before) || !Map.groundWalkable(Before) || Map.terrain(Before) == colony::Terrain::Water)
					{
						continue;
					}
					bool bAllRock = true;
					for (int32 I = 0; I < Length && bAllRock; ++I)
					{
						const colony::CellPos C{X + D[0] * I, Y + D[1] * I, 0};
						bAllRock = Map.inBounds(C) && Map.terrain(C) == colony::Terrain::Rock && Map.roof(C) == colony::Roof::Thick;
					}
					if (bAllRock)
					{
						for (int32 I = 0; I < Length; ++I)
						{
							Out[I] = colony::CellPos{X + D[0] * I, Y + D[1] * I, 0};
						}
						OutDirection = FVector(D[0], D[1], 0.0);
						return true;
					}
				}
			}
		}
		return false;
	}
}

void USmokeTestSubsystem::BuildWorldPhases()
{
	const TSharedRef<FWorldSmokeState> State = MakeShared<FWorldSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto TerrainOf = [this]() { return GetWorld()->GetSubsystem<UTerrainSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	// The colonists start here; the sim's own stockpile cell is the middle of them.
	const auto Home = [=, this]() -> colony::CellPos
	{
		const colony::Sim* Sim = SimOf();
		return Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
	};
	// A camera that looks at a sim cell from a distance (metres) at a heading and a pitch.
	const auto LookAt = [=, this](colony::CellPos Cell, double Metres, double HeadingDegrees, double PitchDegrees, double RaiseMetres = 0.0)
	{
		USimSubsystem* B = Bridge();
		const FVector Target = B->SimToWorld(Cell.x + 0.5f, Cell.y + 0.5f, 0) + FVector(0.0, 0.0, RaiseMetres * 100.0);
		const FRotator Look(-PitchDegrees, HeadingDegrees, 0.0);
		ViewFrom(Target - Look.Vector() * Metres * 100.0, Look);
	};
	const auto CheckPlantsMatch = [=, this](const TCHAR* Phase)
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		const colony::Sim* Sim = SimOf();
		if (!Terrain || !Sim || !Terrain->IsBuilt())
		{
			Fail(FString::Printf(TEXT("%s: the terrain was not built"), Phase));
			return;
		}
		const int64 Drawn = Terrain->GetStats().PlantInstances;
		const int64 Wanted = static_cast<int64>(Sim->plants().count());
		if (Drawn != Wanted)
		{
			Fail(FString::Printf(TEXT("%s: %lld plant instances are drawn but the sim has %lld plants"), Phase, Drawn, Wanted));
		}
	};
	// The sim starts at midnight, when colonists sleep: every colonist works at every hour.
	const auto WorkAllDay = [=, this]()
	{
		USimSubsystem* B = Bridge();
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn)
			{
				for (int32 Hour = 0; Hour < 24; ++Hour)
				{
					B->Submit(colony::SetScheduleCommand{View.id, Hour, colony::ScheduleKind::Work});
				}
			}
		}
	};
	// What the colonists are doing, for a failure message.
	const auto Describe = [=, this]() -> FString
	{
		USimSubsystem* B = Bridge();
		const colony::Sim* Sim = SimOf();
		FString Out = FString::Printf(TEXT("hour %d, %d marks"), Sim ? Sim->time().hourOfDay() : -1, Sim ? static_cast<int32>(Sim->designations().count()) : -1);
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn)
			{
				Out += FString::Printf(TEXT(", job %d%s"), static_cast<int32>(View.job), View.asleep ? TEXT(" asleep") : TEXT(""));
			}
		}
		return Out;
	};
	const auto Listen = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (State->bListening || !B)
		{
			return;
		}
		State->bListening = true;
		B->OnSimEvent.AddLambda([State](const colony::Event& Event)
		{
			State->MinedEvents += Event.type == colony::EventType::CellMined ? 1 : 0;
			State->RemovedEvents += Event.type == colony::EventType::PlantRemoved ? 1 : 0;
		});
	};

	// 1. A new sim: every chunk is made, every plant has an instance, and it looks like land.
	FPhase Overview;
	Overview.Name = TEXT("world_overview");
	Overview.WarmupSeconds = 4.0f;
	Overview.MeasureSeconds = 3.0f;
	Overview.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !TerrainOf())
		{
			Fail(TEXT("USimSubsystem or UTerrainSubsystem did not start"));
			return;
		}
		WorldSmokeSetRoofs(1);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(WorldSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		Listen();
		LookAt(Home(), 90.0, 35.0, 38.0);
	};
	Overview.Verify = [=, this]()
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		const colony::Sim* Sim = SimOf();
		if (!Terrain || !Sim)
		{
			return;
		}
		CheckPlantsMatch(TEXT("world_overview"));
		const FTerrainStats& Stats = Terrain->GetStats();
		const int32 Expected = (Sim->map().width() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
		if (Stats.ChunksX != Expected || Stats.ChunksY != Expected || Stats.GroundVertices <= 0)
		{
			Fail(FString::Printf(TEXT("world_overview: %d x %d chunks and %lld vertices, wanted %d x %d chunks"), Stats.ChunksX, Stats.ChunksY, Stats.GroundVertices, Expected, Expected));
		}
		int32 RockCells = 0;
		for (int32 Y = 0; Y < Sim->map().height(); ++Y)
		{
			for (int32 X = 0; X < Sim->map().width(); ++X)
			{
				RockCells += Sim->map().terrain(X, Y) == colony::Terrain::Rock ? 1 : 0;
			}
		}
		Notes.Add(FString::Printf(TEXT("world_overview: %d x %d chunks, %lld ground vertices, %lld plants in %d kinds (%d components), %d rock cells, built in %.0f ms"),
			Stats.ChunksX, Stats.ChunksY, Stats.GroundVertices, Stats.PlantInstances, Stats.PlantKinds, Stats.PlantComponents, RockCells, Stats.FullBuildMs));
		if (Stats.PlantKinds < 5)
		{
			Fail(FString::Printf(TEXT("world_overview: only %d plant kinds have a mesh"), Stats.PlantKinds));
		}
	};
	Phases.Add(MoveTemp(Overview));

	// 2. Low to the ground: plants stand on the surface, trees have trunks and crowns.
	FPhase Low;
	Low.Name = TEXT("world_low_view");
	Low.WarmupSeconds = 2.0f;
	Low.MeasureSeconds = 2.0f;
	Low.Setup = [=, this]() { LookAt(Home(), 16.0, 200.0, 22.0, 0.5); };
	Phases.Add(MoveTemp(Low));

	// 3. A rock face: blocks standing on the ground.
	FPhase Rock;
	Rock.Name = TEXT("world_rock_face");
	Rock.WarmupSeconds = 2.0f;
	Rock.MeasureSeconds = 2.0f;
	Rock.Setup = [=, this]()
	{
		const colony::Sim* Sim = SimOf();
		if (!Sim)
		{
			return;
		}
		if (!WorldSmokeFindTunnel(Sim->map(), 4, State->Tunnel, State->TunnelDirection))
		{
			Fail(TEXT("world_rock_face: the map has no rock cell with open ground beside it and rock behind"));
			return;
		}
		State->TunnelLength = 4;
		const double Heading = FMath::RadiansToDegrees(FMath::Atan2(State->TunnelDirection.Y, State->TunnelDirection.X));
		// From the open side, looking into the rock.
		LookAt(State->Tunnel[0], 24.0, Heading, 22.0, 1.0);
	};
	Phases.Add(MoveTemp(Rock));

	// 4. Colonists mine a rock cell: CellMined rebuilds that chunk, the block is gone and rough stone shows.
	FPhase Mining;
	Mining.Name = TEXT("world_mining");
	Mining.WarmupSeconds = 1.0f;
	Mining.MeasureSeconds = 30.0f;
	Mining.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = SimOf();
		UTerrainSubsystem* Terrain = TerrainOf();
		if (!B || !Sim || !Terrain || Sim->mineables().rockTypes().empty())
		{
			Fail(TEXT("world_mining: the sim has no rock types to place"));
			return;
		}
		if (!WorldSmokeFindSoil(*Sim, Home(), 4, 14, State->Placed))
		{
			Fail(TEXT("world_mining: no clear soil near the colonists to put a rock on"));
			return;
		}
		// The test puts a rock cell on open ground (a dev placement, not a command), then marks it like the player would.
		Sim->mineables().place(State->Placed, Sim->mineables().rockTypes()[0]);
		Terrain->FlushDirty();
		Terrain->MarkCellDirty(State->Placed);
		Terrain->FlushDirty();
		UTerrainSubsystem::ChunkOfCell(State->Placed, State->ChunkX, State->ChunkY);
		State->VerticesBefore = Terrain->GetChunkGroundVertices(State->ChunkX, State->ChunkY);
		State->RebuildsBefore = Terrain->GetStats().ChunkRebuilds;
		State->MinedEvents = 0;
		WorkAllDay();
		B->Submit(colony::DesignateCommand{colony::CellRect{State->Placed.x, State->Placed.y, 1, 1, 0}, colony::DesignationKind::Mine});
		B->SetSpeed(3);
		LookAt(State->Placed, 14.0, 30.0, 35.0);
	};
	// Colonists take a game hour or two to dig; run extra steps each frame until the rock is mined.
	Mining.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		for (int32 I = 0; B && State->MinedEvents == 0 && I < 20; ++I)
		{
			B->StepOnce();
		}
	};
	Mining.Verify = [=, this]()
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		const colony::Sim* Sim = SimOf();
		if (!Terrain || !Sim)
		{
			return;
		}
		if (State->MinedEvents == 0 || Sim->map().terrain(State->Placed) == colony::Terrain::Rock)
		{
			Fail(FString::Printf(TEXT("world_mining: the colonists did not mine the rock in 30 s (%s)"), *Describe()));
			return;
		}
		Terrain->FlushDirty();
		const int32 After = Terrain->GetChunkGroundVertices(State->ChunkX, State->ChunkY);
		if (Terrain->GetStats().ChunkRebuilds <= State->RebuildsBefore || After == State->VerticesBefore)
		{
			Fail(FString::Printf(TEXT("world_mining: the chunk was not rebuilt after the rock was mined (%d vertices before and after, %lld rebuilds)"), After,
				Terrain->GetStats().ChunkRebuilds - State->RebuildsBefore));
		}
		Notes.Add(FString::Printf(TEXT("world_mining: rock mined, chunk (%d, %d) went from %d to %d vertices in %lld rebuilds, slowest chunk rebuild %.2f ms"), State->ChunkX, State->ChunkY,
			State->VerticesBefore, After, Terrain->GetStats().ChunkRebuilds - State->RebuildsBefore, Terrain->GetStats().SlowestChunkMs));
		Bridge()->SetSpeed(0);
	};
	Phases.Add(MoveTemp(Mining));

	// 5. A tree appears without an event (the scan finds it), then is felled (the event redraws the chunk).
	FPhase Tree;
	Tree.Name = TEXT("world_tree");
	Tree.WarmupSeconds = 1.0f;
	Tree.MeasureSeconds = 30.0f;
	Tree.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = SimOf();
		UTerrainSubsystem* Terrain = TerrainOf();
		if (!B || !Sim || !Terrain)
		{
			return;
		}
		const colony::DefId Poplar = Sim->defs().idsOfKind("Things").empty() ? colony::DefId{} : Sim->defs().idOf("Things", "Plant_TreePoplar");
		if (!Poplar.valid() || !WorldSmokeFindSoil(*Sim, Home(), 3, 12, State->Tree))
		{
			Fail(TEXT("world_tree: no poplar def or no clear soil near the colonists"));
			return;
		}
		State->PlantsBefore = Terrain->GetStats().PlantInstances;
		State->RemovedEvents = 0;
		Sim->plants().place(State->Tree, Poplar, 1.0f);
		WorkAllDay();
		B->Submit(colony::DesignateCommand{colony::CellRect{State->Tree.x, State->Tree.y, 1, 1, 0}, colony::DesignationKind::CutPlant});
		B->SetSpeed(3);
		LookAt(State->Tree, 12.0, 60.0, 20.0, 2.0);
	};
	Tree.OnFrame = [=, this]()
	{
		if (PhaseTime > 2.0f)
		{
			for (int32 I = 0; Bridge() && State->RemovedEvents == 0 && I < 20; ++I)
			{
				Bridge()->StepOnce();
			}
		}
		// Within about a second of the tree being placed the scan must have drawn it.
		UTerrainSubsystem* Terrain = TerrainOf();
		const colony::Sim* Sim = SimOf();
		if (PhaseTime < 1.0f || PhaseTime > 1.2f || !Terrain || !Sim || State->RemovedEvents > 0)
		{
			return;
		}
		if (Terrain->GetStats().PlantInstances != State->PlantsBefore + 1)
		{
			Fail(FString::Printf(TEXT("world_tree: the new tree was not drawn (%lld instances, wanted %lld)"), Terrain->GetStats().PlantInstances, State->PlantsBefore + 1));
		}
	};
	Tree.Verify = [=, this]()
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		const colony::Sim* Sim = SimOf();
		if (!Terrain || !Sim)
		{
			return;
		}
		if (State->RemovedEvents == 0 || Sim->plants().has(State->Tree))
		{
			Fail(FString::Printf(TEXT("world_tree: the colonists did not fell the tree in 30 s (%s)"), *Describe()));
			return;
		}
		Bridge()->SetSpeed(0);
		Terrain->FlushDirty();
		CheckPlantsMatch(TEXT("world_tree"));
	};
	Phases.Add(MoveTemp(Tree));

	// 6. A tunnel in the mountain: mined-out rock keeps its roof. With roofs shown the tunnel is closed; hidden, its floor shows.
	FPhase TunnelOn;
	TunnelOn.Name = TEXT("world_tunnel_roofs_on");
	TunnelOn.WarmupSeconds = 2.0f;
	TunnelOn.MeasureSeconds = 2.0f;
	TunnelOn.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = SimOf();
		UTerrainSubsystem* Terrain = TerrainOf();
		if (!B || !Sim || !Terrain || State->TunnelLength == 0)
		{
			return;
		}
		UTerrainSubsystem::ChunkOfCell(State->Tunnel[0], State->TunnelChunkX, State->TunnelChunkY);
		State->RoofVerticesBefore = Terrain->GetChunkRoofVertices(State->TunnelChunkX, State->TunnelChunkY);
		// Dig the tunnel the way the sim does, and tell the terrain the way the sim's event does.
		for (int32 I = 0; I < State->TunnelLength; ++I)
		{
			Sim->mineables().hit(State->Tunnel[I], 1000000);
			colony::Event Event;
			Event.type = colony::EventType::CellMined;
			Event.cell = State->Tunnel[I];
			B->OnSimEvent.Broadcast(Event);
		}
		WorldSmokeSetRoofs(1);
		const double Heading = FMath::RadiansToDegrees(FMath::Atan2(State->TunnelDirection.Y, State->TunnelDirection.X));
		LookAt(State->Tunnel[1], 16.0, Heading, 55.0);
	};
	TunnelOn.Verify = [=, this]()
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		if (!Terrain || State->TunnelLength == 0)
		{
			return;
		}
		const int32 Roof = Terrain->GetChunkRoofVertices(State->TunnelChunkX, State->TunnelChunkY);
		if (Roof <= State->RoofVerticesBefore || !Terrain->AreRoofsShown())
		{
			Fail(FString::Printf(TEXT("world_tunnel_roofs_on: mined rock has no roof drawn (%d roof vertices, were %d), roofs shown: %d"), Roof, State->RoofVerticesBefore, Terrain->AreRoofsShown()));
		}
		Notes.Add(FString::Printf(TEXT("world_tunnel: %d mined cells put %d vertices of mountain roof in chunk (%d, %d)"), State->TunnelLength, Roof - State->RoofVerticesBefore, State->TunnelChunkX, State->TunnelChunkY));
	};
	Phases.Add(MoveTemp(TunnelOn));

	FPhase TunnelOff;
	TunnelOff.Name = TEXT("world_tunnel_roofs_off");
	TunnelOff.WarmupSeconds = 2.0f;
	TunnelOff.MeasureSeconds = 2.0f;
	TunnelOff.Setup = [=, this]() { WorldSmokeSetRoofs(0); };
	TunnelOff.Verify = [=, this]()
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		if (Terrain && State->TunnelLength > 0 && Terrain->AreRoofsShown())
		{
			Fail(TEXT("world_tunnel_roofs_off: roofs are still shown with Terrain.Roofs 0"));
		}
		WorldSmokeSetRoofs(-1);
	};
	Phases.Add(MoveTemp(TunnelOff));

	// 7. A 1 km map: the cost of the first build, and of a frame with the whole map in view.
	FPhase Big;
	Big.Name = TEXT("world_1km");
	Big.WarmupSeconds = 10.0f;
	Big.MeasureSeconds = 5.0f;
	Big.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B)
		{
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 1024;
		B->SetWorldOrigin(WorldSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("world_1km: the game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		ViewFrom(WorldSmokeOrigin + FVector(51200.0 - 14000.0, 51200.0, 16000.0), FRotator(-38.0, 0.0, 0.0));
	};
	Big.Verify = [=, this]()
	{
		UTerrainSubsystem* Terrain = TerrainOf();
		if (!Terrain)
		{
			return;
		}
		CheckPlantsMatch(TEXT("world_1km"));
		const FTerrainStats& Stats = Terrain->GetStats();
		Notes.Add(FString::Printf(TEXT("world_1km: %d x %d chunks, %lld ground vertices, %lld plants (%d components), full build %.0f ms (sim map generation not included)"),
			Stats.ChunksX, Stats.ChunksY, Stats.GroundVertices, Stats.PlantInstances, Stats.PlantComponents, Stats.FullBuildMs));
		if (Stats.ChunksX != 32 || Stats.ChunksY != 32)
		{
			Fail(FString::Printf(TEXT("world_1km: %d x %d chunks, wanted 32 x 32"), Stats.ChunksX, Stats.ChunksY));
		}
	};
	Phases.Add(MoveTemp(Big));

	FPhase Close;
	Close.Name = TEXT("world_1km_close");
	Close.WarmupSeconds = 2.0f;
	Close.MeasureSeconds = 3.0f;
	Close.Setup = [=, this]()
	{
		ViewFrom(WorldSmokeOrigin + FVector(51200.0, 51200.0, 9000.0), FRotator(-35.0, 20.0, 0.0));
	};
	Phases.Add(MoveTemp(Close));
}
