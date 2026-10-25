// Smoke test `buildings` (task U4): the sim's buildings, blueprints, frames, doors and items drawn by UBuildingSubsystem, in the game, offscreen.
#include "SmokeTest.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include "BuildingSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <string>
#include <vector>

namespace
{
	const FVector BuildingsSmokeOrigin(-600000.0, 0.0, 0.0);

	// The room: 6 x 5 cells of wall around 4 x 3 of floor, a door in the south wall.
	constexpr int32 RoomW = 6;
	constexpr int32 RoomH = 5;

	struct FBuildingsSmokeState
	{
		bool bListening = false;
		int32 FailedCommands = 0;
		FString FirstFailure;
		colony::CellPos Room;  // the south-west corner wall
		bool bRoomFound = false;
		int32 Expected = 0;  // buildings placed
		colony::CellPos StackCell[2];
		int32 StackInstancesBefore = 0;
		bool bStackPlaced = false;
		float MaxDoorShown = 0.0f;
		float MaxLeafCm = 0.0f;
		colony::Entity Door = colony::kNoEntity;
		colony::Entity Walker = colony::kNoEntity;
		bool bDoorOrdered = false;
		bool bArrived = false;
		bool bDoorOpened = false;
		TArray<FString> DoorLog;
	};

	void BuildingsSmokeSetRoofs(int32 Mode)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(Mode, ECVF_SetByCode);
		}
	}

	bool BuildingsSmokeFree(const colony::Map& Map, int32 X, int32 Y)
	{
		const colony::CellPos Cell{X, Y, 0};
		return Map.inBounds(Cell) && Map.terrain(Cell) == colony::Terrain::Soil && Map.buildingAt(Cell) == colony::kNoEntity && Map.groundWalkable(Cell);
	}

	/** A rectangle of open, level soil near `Near`: its south-west corner. It keeps clear of the `Avoid` cells (and a margin of 3 around them). */
	bool BuildingsSmokeFindRect(const colony::Map& Map, colony::CellPos Near, int32 W, int32 H, colony::CellPos& Out, const colony::CellPos* Avoid = nullptr, int32 AvoidCount = 0)
	{
		for (int32 R = 5; R <= 45; ++R)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				for (int32 DX = -R; DX <= R; ++DX)
				{
					if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != R)
					{
						continue;
					}
					const int32 X0 = Near.x + DX;
					const int32 Y0 = Near.y + DY;
					bool bOk = true;
					for (int32 A = 0; A < AvoidCount && bOk; ++A)
					{
						bOk = Avoid[A].x < X0 - 4 || Avoid[A].x > X0 + W + 3 || Avoid[A].y < Y0 - 4 || Avoid[A].y > Y0 + H + 3;
					}
					float Low = 1.0e9f;
					float High = -1.0e9f;
					for (int32 Y = Y0 - 1; Y <= Y0 + H && bOk; ++Y)
					{
						for (int32 X = X0 - 1; X <= X0 + W && bOk; ++X)
						{
							bOk = BuildingsSmokeFree(Map, X, Y);
							if (bOk)
							{
								Low = FMath::Min(Low, Map.groundHeight(X, Y));
								High = FMath::Max(High, Map.groundHeight(X, Y));
							}
						}
					}
					if (bOk && High - Low < 0.5f)
					{
						Out = colony::CellPos{X0, Y0, 0};
						return true;
					}
				}
			}
		}
		return false;
	}
}

void USmokeTestSubsystem::BuildBuildingsPhases()
{
	const TSharedRef<FBuildingsSmokeState> State = MakeShared<FBuildingsSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Drawn = [this]() { return GetWorld()->GetSubsystem<UBuildingSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto Home = [=, this]() -> colony::CellPos
	{
		const colony::Sim* Sim = SimOf();
		return Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
	};
	const auto LookAt = [=, this](colony::CellPos Cell, double Metres, double HeadingDegrees, double PitchDegrees, double RaiseMetres = 0.0, float OffsetX = 0.5f, float OffsetY = 0.5f)
	{
		USimSubsystem* B = Bridge();
		const FVector Target = B->SimToWorld(Cell.x + OffsetX, Cell.y + OffsetY, 0) + FVector(0.0, 0.0, RaiseMetres * 100.0);
		const FRotator Look(-PitchDegrees, HeadingDegrees, 0.0);
		ViewFrom(Target - Look.Vector() * Metres * 100.0, Look);
	};
	const auto Cell = [State](int32 DX, int32 DY) { return colony::CellPos{State->Room.x + DX, State->Room.y + DY, 0}; };
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
			if (Event.type == colony::EventType::CommandFailed)
			{
				++State->FailedCommands;
				if (State->FirstFailure.IsEmpty())
				{
					State->FirstFailure = UTF8_TO_TCHAR(Event.message.c_str());
				}
			}
		});
	};
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
	// What is drawn must match what the snapshot says, kind by kind.
	const auto CheckMatches = [=, this](const TCHAR* Phase)
	{
		UBuildingSubsystem* Buildings = Drawn();
		USimSubsystem* B = Bridge();
		if (!Buildings || !B || !Buildings->IsBuilt())
		{
			Fail(FString::Printf(TEXT("%s: the building subsystem was not built"), Phase));
			return;
		}
		Buildings->Refresh();
		int32 Built = 0, Frames = 0, Blueprints = 0, Items = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Item)
			{
				++Items;
			}
			else if (View.category == colony::ThingCategory::Building)
			{
				Built += View.stage == colony::ConstructionStage::Built ? 1 : 0;
				Frames += View.stage == colony::ConstructionStage::Frame ? 1 : 0;
				Blueprints += View.stage == colony::ConstructionStage::Blueprint ? 1 : 0;
			}
		}
		const FBuildingStats& Stats = Buildings->GetStats();
		if (Stats.Built != Built || Stats.Frames != Frames || Stats.Blueprints != Blueprints || Stats.Items != Items)
		{
			Fail(FString::Printf(TEXT("%s: drawn built/frames/blueprints/items %d/%d/%d/%d but the snapshot has %d/%d/%d/%d"), Phase, Stats.Built, Stats.Frames, Stats.Blueprints,
				Stats.Items, Built, Frames, Blueprints, Items));
		}
		// Every blueprint is a ghost, every frame has its ghost too; every built thing and every item has at least one instance.
		if (Buildings->GetGhostInstances() != Blueprints + Frames)
		{
			Fail(FString::Printf(TEXT("%s: %d ghost instances for %d blueprints and %d frames"), Phase, Buildings->GetGhostInstances(), Blueprints, Frames));
		}
		if (Stats.Instances < Built + Frames * 2 + Blueprints + Items)
		{
			Fail(FString::Printf(TEXT("%s: only %lld instances for %d buildings, %d frames, %d blueprints and %d items"), Phase, Stats.Instances, Built, Frames, Blueprints, Items));
		}
	};
	const auto CountBuilt = [=, this]() -> int32
	{
		int32 Built = 0;
		for (const colony::ThingView& View : Bridge()->GetCurrentSnapshot().things)
		{
			Built += View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built ? 1 : 0;
		}
		return Built;
	};

	// 1. A new sim: the items the crashed colonists brought are drawn, each stack as a pile.
	FPhase Items;
	Items.Name = TEXT("buildings_items");
	Items.WarmupSeconds = 4.0f;
	Items.MeasureSeconds = 2.0f;
	Items.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Drawn())
		{
			Fail(TEXT("USimSubsystem or UBuildingSubsystem did not start"));
			return;
		}
		BuildingsSmokeSetRoofs(0);
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(BuildingsSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		Listen();
		LookAt(Home(), 16.0, 30.0, 72.0);
	};
	Items.Verify = [=, this]()
	{
		CheckMatches(TEXT("buildings_items"));
		UBuildingSubsystem* Buildings = Drawn();
		if (Buildings && Buildings->IsBuilt())
		{
			const FBuildingStats& Stats = Buildings->GetStats();
			Notes.Add(FString::Printf(TEXT("buildings_items: %d items drawn as %lld instances in %d components, %d meshes, first build %lld chunk rebuilds, slowest %.2f ms"), Stats.Items, Stats.Instances,
				Stats.Components, Stats.Meshes, Stats.ChunkRebuilds, Stats.SlowestChunkMs));
			if (Stats.Items < 10)
			{
				Fail(FString::Printf(TEXT("buildings_items: only %d items drawn, the scenario drops far more"), Stats.Items));
			}
		}
	};
	Phases.Add(MoveTemp(Items));

	// 2. A stack of 75 steel is a pile of six, a single piece one.
	FPhase Stacks;
	Stacks.Name = TEXT("buildings_stacks");
	Stacks.WarmupSeconds = 1.0f;
	Stacks.MeasureSeconds = 2.0f;
	Stacks.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = SimOf();
		UBuildingSubsystem* Buildings = Drawn();
		if (!B || !Sim || !Buildings)
		{
			return;
		}
		// Two clear cells side by side, far from anything, so no other thing changes that chunk.
		colony::CellPos Spot;
		if (!BuildingsSmokeFindRect(Sim->map(), Home(), 4, 2, Spot))
		{
			Fail(TEXT("buildings_stacks: no clear ground near the colonists"));
			return;
		}
		State->StackCell[0] = colony::CellPos{Spot.x + 1, Spot.y, 0};
		State->StackCell[1] = colony::CellPos{Spot.x + 3, Spot.y, 0};
		State->bStackPlaced = true;
		B->SetSpeed(0);
		B->StepOnce();
		Buildings->Refresh();
		State->StackInstancesBefore = static_cast<int32>(Buildings->GetStats().Instances);
		B->Submit(colony::SpawnThingCommand{"Steel", State->StackCell[0], 1});
		B->Submit(colony::SpawnThingCommand{"Steel", State->StackCell[1], 75});
		B->StepOnce();
		Buildings->Refresh();
		LookAt(State->StackCell[0], 9.0, 20.0, 72.0, 0.0, 2.0f, 0.5f);
	};
	Stacks.Verify = [=, this]()
	{
		UBuildingSubsystem* Buildings = Drawn();
		if (!Buildings || !State->bStackPlaced)
		{
			return;
		}
		CheckMatches(TEXT("buildings_stacks"));
		const int32 Added = static_cast<int32>(Buildings->GetStats().Instances) - State->StackInstancesBefore;
		// The colonists are asleep, so nothing was picked up in the meantime: one piece plus a pile of six.
		if (Added != 7)
		{
			Fail(FString::Printf(TEXT("buildings_stacks: a single steel and a full stack of 75 added %d instances, wanted 1 + 6 = 7"), Added));
		}
		Notes.Add(FString::Printf(TEXT("buildings_stacks: a stack of 1 and a stack of 75 added %d instances"), Added));
	};
	Phases.Add(MoveTemp(Stacks));

	// 3. The plans: a room of walls with a door, a bed, a table and a stove, all as blueprints.
	FPhase Plans;
	Plans.Name = TEXT("buildings_blueprints");
	Plans.WarmupSeconds = 1.0f;
	Plans.MeasureSeconds = 2.0f;
	Plans.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = SimOf();
		if (!B || !Sim)
		{
			return;
		}
		if (!BuildingsSmokeFindRect(Sim->map(), Home(), RoomW + 2, RoomH + 2, State->Room, State->StackCell, 2))
		{
			Fail(TEXT("buildings_blueprints: no open level ground near the colonists for a room"));
			return;
		}
		// Keep the stack test's cells out of the way: the room starts a cell further out if it would cover them.
		State->bRoomFound = true;
		State->Room.x += 1;
		State->Room.y += 1;
		const auto Place = [&](const char* Def, const char* Stuff, int32 DX, int32 DY)
		{
			B->Submit(colony::PlaceBlueprintCommand{Def, Stuff, Cell(DX, DY), colony::Rot4::North});
			++State->Expected;
		};
		for (int32 X = 0; X < RoomW; ++X)
		{
			for (int32 Y = 0; Y < RoomH; ++Y)
			{
				const bool bEdge = X == 0 || Y == 0 || X == RoomW - 1 || Y == RoomH - 1;
				if (bEdge && !(X == 3 && Y == 0))
				{
					Place("Wall", "Steel", X, Y);
				}
			}
		}
		Place("Door", "Steel", 3, 0);
		Place("Bed", "WoodLog", 4, 1);
		Place("Table2x2", "WoodLog", 1, 1);
		Place("FueledStove", "", 1, 3);
		B->SetSpeed(0);
		B->StepOnce();
		LookAt(Cell(RoomW / 2, RoomH / 2), 14.0, 60.0, 50.0, 0.0, 0.0f, 0.5f);
	};
	Plans.Verify = [=, this]()
	{
		UBuildingSubsystem* Buildings = Drawn();
		if (!Buildings || !State->bRoomFound)
		{
			return;
		}
		if (State->FailedCommands > 0)
		{
			Fail(FString::Printf(TEXT("buildings_blueprints: %d plans were refused (%s)"), State->FailedCommands, *State->FirstFailure));
		}
		CheckMatches(TEXT("buildings_blueprints"));
		if (Buildings->GetStats().Blueprints != State->Expected)
		{
			Fail(FString::Printf(TEXT("buildings_blueprints: %d blueprints drawn, %d placed"), Buildings->GetStats().Blueprints, State->Expected));
		}
		Notes.Add(FString::Printf(TEXT("buildings_blueprints: %d blueprints drawn as ghosts, %d meshes made"), Buildings->GetStats().Blueprints, Buildings->GetStats().Meshes));
	};
	Phases.Add(MoveTemp(Plans));

	// 4. The colonists carry materials and start building: frames rise inside their ghosts, finished walls stand.
	FPhase Frames;
	Frames.Name = TEXT("buildings_frames");
	Frames.WarmupSeconds = 1.0f;
	Frames.MeasureSeconds = 30.0f;
	Frames.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !State->bRoomFound)
		{
			return;
		}
		WorkAllDay();
		B->SetSpeed(3);
		LookAt(Cell(RoomW / 2, RoomH / 2), 14.0, 60.0, 50.0, 0.0, 0.0f, 0.5f);
	};
	Frames.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UBuildingSubsystem* Buildings = Drawn();
		if (!B || !Buildings || !State->bRoomFound)
		{
			return;
		}
		// Run on until the picture holds all three stages at once, then hold it for the screenshot.
		const FBuildingStats& Stats = Buildings->GetStats();
		const bool bAllStages = Stats.Frames >= 1 && Stats.Built >= 1 && Stats.Blueprints >= 1;
		if (bAllStages)
		{
			B->SetSpeed(0);
			return;
		}
		for (int32 I = 0; I < 40; ++I)
		{
			B->StepOnce();
		}
		Buildings->Refresh();
	};
	Frames.Verify = [=, this]()
	{
		UBuildingSubsystem* Buildings = Drawn();
		if (!Buildings || !State->bRoomFound)
		{
			return;
		}
		CheckMatches(TEXT("buildings_frames"));
		const FBuildingStats& Stats = Buildings->GetStats();
		if (Stats.Frames < 1 || Stats.Built < 1 || Stats.Blueprints < 1)
		{
			Fail(FString::Printf(TEXT("buildings_frames: after 30 s there were %d frames, %d built and %d blueprints, wanted some of each"), Stats.Frames, Stats.Built, Stats.Blueprints));
		}
		Notes.Add(FString::Printf(TEXT("buildings_frames: %d frames, %d built, %d blueprints at once, slowest chunk rebuild %.2f ms"), Stats.Frames, Stats.Built, Stats.Blueprints, Stats.SlowestChunkMs));
		Bridge()->SetSpeed(0);
	};
	Phases.Add(MoveTemp(Frames));

	// 5. Everything gets built.
	FPhase Done;
	Done.Name = TEXT("buildings_built");
	Done.WarmupSeconds = 1.0f;
	Done.MeasureSeconds = 60.0f;
	Done.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (B && State->bRoomFound)
		{
			B->SetSpeed(3);
		}
	};
	Done.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UBuildingSubsystem* Buildings = Drawn();
		if (!B || !Buildings || !State->bRoomFound)
		{
			return;
		}
		if (CountBuilt() >= State->Expected)
		{
			B->SetSpeed(0);
			return;
		}
		for (int32 I = 0; I < 60; ++I)
		{
			B->StepOnce();
		}
		Buildings->Refresh();
	};
	Done.Verify = [=, this]()
	{
		UBuildingSubsystem* Buildings = Drawn();
		if (!Buildings || !State->bRoomFound)
		{
			return;
		}
		const int32 Built = CountBuilt();
		if (Built < State->Expected)
		{
			Fail(FString::Printf(TEXT("buildings_built: only %d of %d buildings were finished in 60 s"), Built, State->Expected));
			return;
		}
		CheckMatches(TEXT("buildings_built"));
		const FBuildingStats& Stats = Buildings->GetStats();
		if (Stats.Frames != 0 || Stats.Blueprints != 0)
		{
			Fail(FString::Printf(TEXT("buildings_built: %d frames and %d blueprints are still drawn"), Stats.Frames, Stats.Blueprints));
		}
		if (Stats.Doors != 1)
		{
			Fail(FString::Printf(TEXT("buildings_built: %d doors drawn, wanted 1"), Stats.Doors));
		}
		for (const colony::ThingView& View : Bridge()->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built && View.def == SimOf()->defs().idOf("Things", "Door"))
			{
				State->Door = View.id;
			}
		}
		Notes.Add(FString::Printf(TEXT("buildings_built: %d buildings standing as %lld instances in %d components, %d meshes, slowest chunk rebuild %.2f ms"), Built, Stats.Instances, Stats.Components,
			Stats.Meshes, Stats.SlowestChunkMs));
		LookAt(Cell(RoomW / 2, RoomH / 2), 12.0, 60.0, 62.0, 0.0, 0.0f, 0.5f);
	};
	Phases.Add(MoveTemp(Done));

	// 6. The room from above with its roof off (the overseer view), then from the side with the roof on.
	FPhase Inside;
	Inside.Name = TEXT("buildings_inside");
	Inside.WarmupSeconds = 2.0f;
	Inside.MeasureSeconds = 2.0f;
	Inside.Setup = [=, this]()
	{
		if (State->bRoomFound)
		{
			BuildingsSmokeSetRoofs(0);
			LookAt(Cell(RoomW / 2, RoomH / 2), 11.0, 60.0, 62.0, 0.0, 0.0f, 0.5f);
		}
	};
	Phases.Add(MoveTemp(Inside));

	FPhase Roofed;
	Roofed.Name = TEXT("buildings_roofed");
	Roofed.WarmupSeconds = 2.0f;
	Roofed.MeasureSeconds = 2.0f;
	Roofed.Setup = [=, this]()
	{
		if (State->bRoomFound)
		{
			BuildingsSmokeSetRoofs(1);
			LookAt(Cell(RoomW / 2, 0), 17.0, 110.0, 38.0, 0.0, 0.0f, 0.5f);
		}
	};
	Roofed.Verify = [=, this]() { BuildingsSmokeSetRoofs(0); };
	Phases.Add(MoveTemp(Roofed));

	// 7. A stockpile is made inside the room and a colonist is ordered to haul the big steel stack to it: the door
	// opens as the colonist comes to it, the leaf slides into the wall, and it shuts when they are through.
	FPhase Door;
	Door.Name = TEXT("buildings_door");
	Door.WarmupSeconds = 0.5f;
	Door.MeasureSeconds = 25.0f;
	Door.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !State->bRoomFound || State->Door == colony::kNoEntity)
		{
			return;
		}
		// The colonist nearest the steel stack that is the player's own.
		colony::Entity Steel = colony::kNoEntity;
		const colony::DefId SteelDef = SimOf()->defs().idOf("Things", "Steel");
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Item && View.def == SteelDef && View.stackCount == 75 && Steel == colony::kNoEntity)
			{
				Steel = View.id;
			}
		}
		float Nearest = 1.0e9f;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && !View.downed)
			{
				const float Distance = FVector2D::Distance(FVector2D(View.x, View.y), FVector2D(State->StackCell[1].x, State->StackCell[1].y));
				if (Distance < Nearest)
				{
					Nearest = Distance;
					State->Walker = View.id;
				}
			}
		}
		if (State->Walker == colony::kNoEntity || Steel == colony::kNoEntity)
		{
			Fail(TEXT("buildings_door: no colonist or no steel stack to haul"));
			return;
		}
		B->SetSpeed(0);
		B->Submit(colony::CreateStockpileCommand{colony::CellRect{State->Room.x + 3, State->Room.y + 1, 1, 2, 0}});
		B->StepOnce();
		B->Submit(colony::OrderHaulCommand{State->Walker, Steel});
		B->StepOnce();
		State->bDoorOrdered = true;
		LookAt(Cell(3, 0), 5.5, 90.0, 22.0, 1.0);
	};
	Door.OnFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UBuildingSubsystem* Buildings = Drawn();
		if (!B || !Buildings || !State->bDoorOrdered || State->bArrived)
		{
			return;
		}
		// One sim step a frame (about 10x real time here): the door's opening is drawn between snapshots.
		B->StepOnce();
		const float Shown = Buildings->GetDoorShown(State->Door);
		if (PhaseTime > 0.0f && FMath::FloorToInt32(PhaseTime * 2.0f) != State->DoorLog.Num())
		{
			const colony::ThingView* Walker = B->GetCurrentSnapshot().find(State->Walker);
			const colony::ThingView* Door = B->GetCurrentSnapshot().find(State->Door);
			State->DoorLog.Add(FString::Printf(TEXT("t=%.1f walker (%.1f, %.1f) job %d door raw %.2f shown %.2f"), PhaseTime, Walker ? Walker->x : -1.0f, Walker ? Walker->y : -1.0f,
				Walker ? static_cast<int32>(Walker->job) : -1, Door ? Door->doorOpen : -1.0f, Shown));
		}
		State->MaxDoorShown = FMath::Max(State->MaxDoorShown, Shown);
		State->MaxLeafCm = FMath::Max(State->MaxLeafCm, Buildings->GetDoorLeafOffsetCm(State->Door));
		// Opened wide and shut again: done.
		State->bDoorOpened = State->bDoorOpened || State->MaxDoorShown >= 0.95f;
		if (State->bDoorOpened && Shown >= 0.0f && Shown < 0.02f)
		{
			State->bArrived = true;
		}
	};
	Door.Verify = [=, this]()
	{
		UBuildingSubsystem* Buildings = Drawn();
		if (!Buildings || !State->bDoorOrdered)
		{
			return;
		}
		const float Now = Buildings->GetDoorShown(State->Door);
		if (!State->bArrived)
		{
			for (const FString& Line : State->DoorLog)
			{
				Notes.Add(Line);
			}
			Fail(FString::Printf(TEXT("buildings_door: in 25 s the door was drawn open at most %.2f (leaf %.0f cm) and %.2f at the end; it should open fully and shut again"), State->MaxDoorShown, State->MaxLeafCm, Now));
		}
		else if (State->MaxLeafCm < 50.0f)
		{
			Fail(FString::Printf(TEXT("buildings_door: the door leaf moved only %.0f cm while it opened"), State->MaxLeafCm));
		}
		Notes.Add(FString::Printf(TEXT("buildings_door: drawn open up to %.2f (leaf moved up to %.0f cm), shut again: %d"), State->MaxDoorShown, State->MaxLeafCm, State->bArrived));
	};
	Phases.Add(MoveTemp(Door));

	// 8. A new sim wipes the picture.
	FPhase Fresh;
	Fresh.Name = TEXT("buildings_new_sim");
	Fresh.WarmupSeconds = 3.0f;
	Fresh.MeasureSeconds = 1.0f;
	Fresh.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B)
		{
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 7;
		Options.MapSize = 256;
		B->SetWorldOrigin(BuildingsSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("buildings_new_sim: the game's data did not load"));
			return;
		}
		B->SetSpeed(0);
	};
	Fresh.Verify = [=, this]()
	{
		UBuildingSubsystem* Buildings = Drawn();
		if (!Buildings)
		{
			return;
		}
		CheckMatches(TEXT("buildings_new_sim"));
		if (Buildings->IsBuilt() && (Buildings->GetStats().Built != 0 || Buildings->GetStats().Blueprints != 0))
		{
			Fail(FString::Printf(TEXT("buildings_new_sim: %d buildings and %d blueprints are still drawn after a new start"), Buildings->GetStats().Built, Buildings->GetStats().Blueprints));
		}
	};
	Phases.Add(MoveTemp(Fresh));
}
