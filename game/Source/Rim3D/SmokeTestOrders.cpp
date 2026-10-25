// Smoke test `orders` (task U7): the architect menu, the marks overlay and the draft, move and attack orders, driven the way the mouse does, in the game, offscreen.
#include "SmokeTest.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "UnrealClient.h"

#include "BuildingSubsystem.h"
#include "OrdersSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "TerrainSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
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
	const FVector OrdersSmokeOrigin(-700000.0, 0.0, 0.0);

	// The site: SiteW x SiteH cells of open, level soil. Offsets of everything the test does, from its south-west corner.
	constexpr int32 SiteW = 16;
	constexpr int32 SiteH = 14;

	struct FOrdersSmokeState
	{
		bool bListening = false;
		int32 FailedCommands = 0;
		FString FirstFailure;
		colony::CellPos Site;
		bool bSite = false;
		bool bGhostMaterial = false;
		// architect
		int32 WallsPlanned = 0;
		int32 GhostValidInLine = -1;
		int32 BlockedPlaced = -1;
		// marks
		bool bMarksDone = false;
		int32 MineSent = 0;
		// cancel
		bool bCancelDone = false;
		// fight
		bool bFightDone = false;
		colony::Entity Colonist = colony::kNoEntity;
		colony::Entity Enemy = colony::kNoEntity;
		colony::CellPos MoveTarget;
		int32 ColonistCount = 0;
		int32 AlarmDrafted = -1;
		int32 AlarmStandDown = -1;
	};

	bool OrdersSmokeProject(UWorld* World, const FVector& Point, FVector2D& OutScreen)
	{
		APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
		if (!PC || !GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
		{
			return false;
		}
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		if (!PC->ProjectWorldLocationToScreen(Point, OutScreen, false))
		{
			return false;
		}
		return OutScreen.X >= 0.05f * Size.X && OutScreen.Y >= 0.05f * Size.Y && OutScreen.X < 0.95f * Size.X && OutScreen.Y < 0.80f * Size.Y;
	}

	/** Open, level soil: the south-west corner of a rectangle of that size, with the ground around it. */
	bool OrdersSmokeFindSite(const colony::Map& Map, colony::CellPos Near, int32 W, int32 H, colony::CellPos& Out)
	{
		for (int32 R = 6; R <= 110; ++R)
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
					float Low = 1.0e9f;
					float High = -1.0e9f;
					for (int32 Y = Y0 - 1; Y <= Y0 + H && bOk; ++Y)
					{
						for (int32 X = X0 - 1; X <= X0 + W && bOk; ++X)
						{
							const colony::CellPos Cell{X, Y, 0};
							bOk = Map.inBounds(Cell) && Map.terrain(Cell) == colony::Terrain::Soil && Map.buildingAt(Cell) == colony::kNoEntity && Map.groundWalkable(Cell);
							if (bOk)
							{
								Low = FMath::Min(Low, Map.groundHeight(X, Y));
								High = FMath::Max(High, Map.groundHeight(X, Y));
							}
						}
					}
					if (bOk && High - Low < 2.5f)
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

void USmokeTestSubsystem::BuildOrdersPhases()
{
	const TSharedRef<FOrdersSmokeState> State = MakeShared<FOrdersSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Orders = [this]() { return GetWorld()->GetSubsystem<UOrdersSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto Home = [=, this]() -> colony::CellPos
	{
		const colony::Sim* Sim = SimOf();
		return Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
	};
	const auto At = [State](int32 DX, int32 DY) { return colony::CellPos{State->Site.x + DX, State->Site.y + DY, 0}; };
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
	// The overseer camera over a cell, `Notches` of zoom in from the default distance.
	const auto Aim = [=, this](colony::CellPos Cell, float Notches)
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (Pawn)
		{
			Pawn->ResetView();
			Pawn->Zoom(Notches);
			Pawn->FocusOn(Bridge()->SimToWorld(Cell.x + 0.5f, Cell.y + 0.5f, 0));
			Pawn->SnapCamera();
		}
	};
	const auto CountThings = [=, this](const char* DefName, colony::ConstructionStage Stage) -> int32
	{
		int32 Count = 0;
		const colony::Sim* Sim = SimOf();
		for (const colony::ThingView& View : Bridge()->GetCurrentSnapshot().things)
		{
			Count += View.category == colony::ThingCategory::Building && View.stage == Stage && Sim->defs().get(View.def).defName() == DefName ? 1 : 0;
		}
		return Count;
	};
	const auto CountMarks = [=, this](colony::DesignationKind Kind) -> int32
	{
		int32 Count = 0;
		for (const colony::DesignationView& Mark : Bridge()->GetCurrentSnapshot().designations)
		{
			Count += Mark.kind == Kind ? 1 : 0;
		}
		return Count;
	};
	const auto Drag = [=, this](EOrderTool Tool, colony::CellPos From, colony::CellPos To) -> int32
	{
		UOrdersSubsystem* O = Orders();
		O->SetTool(Tool);
		O->BeginDrag(From);
		O->SetHoverCell(To);
		return O->EndDrag(To);
	};

	// 1. A new sim, the overseer view, and the menu knows the buildings.
	FPhase Setup;
	Setup.Name = TEXT("orders_setup");
	Setup.WarmupSeconds = 3.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Orders() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UOrdersSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(0, ECVF_SetByCode);
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(OrdersSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		Listen();
		Orders()->SetMouseDriven(false);
		if (!Overseer()->EnterOverseerMode())
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		State->bGhostMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Rim3D/Generated/Buildings/M_BuildingGhost.M_BuildingGhost")) != nullptr;
		if (!OrdersSmokeFindSite(SimOf()->map(), Home(), SiteW, SiteH, State->Site))
		{
			Fail(TEXT("orders_setup: no open level ground near the colonists for the test"));
			return;
		}
		State->bSite = true;
		// Clear the wild plants off the site so the pictures show the plans and marks (a dev placement, found by the terrain's plant scan).
		for (int32 Y = -1; Y <= SiteH; ++Y)
		{
			for (int32 X = -1; X <= SiteW; ++X)
			{
				SimOf()->plants().remove(At(X, Y));
			}
		}
		Aim(At(SiteW / 2, SiteH / 2), 2.0f);
	};
	Setup.Verify = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		if (!O || !State->bSite)
		{
			return;
		}
		// Every building the player can plan is in the menu; something that isn't a building is not.
		for (const TCHAR* Name : {TEXT("Wall"), TEXT("Door"), TEXT("Bed"), TEXT("Stool"), TEXT("Table1x2"), TEXT("Table2x2"), TEXT("Table2x4"), TEXT("FueledStove")})
		{
			if (!O->SelectBuilding(Name))
			{
				Fail(FString::Printf(TEXT("orders_setup: the architect menu does not offer %s"), Name));
			}
		}
		if (O->SelectBuilding(TEXT("Steel")) || O->SelectBuilding(TEXT("Nonsense")))
		{
			Fail(TEXT("orders_setup: the architect menu offered something that is not a building"));
		}
		O->SelectBuilding(TEXT("Wall"));
		if (!O->GetBuildStuff().valid())
		{
			Fail(TEXT("orders_setup: a wall was given no stuff to be made of"));
		}
		O->CancelTool();
		if (O->GetTool() != EOrderTool::None)
		{
			Fail(TEXT("orders_setup: cancelling did not put the tool down"));
		}
		Notes.Add(FString::Printf(TEXT("orders_setup: site at cell %d, %d; ghost material %s"), State->Site.x, State->Site.y, State->bGhostMaterial ? TEXT("present") : TEXT("MISSING (ghosts are not drawn)")));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. The architect: a line of walls, a turned bed, a refused spot, a door, a table, a stove, and a ghost left hovering for the picture.
	FPhase Architect;
	Architect.Name = TEXT("orders_architect");
	Architect.WarmupSeconds = 1.5f;
	Architect.MeasureSeconds = 1.0f;
	Architect.Setup = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		UBuildingSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingSubsystem>();
		if (!O || !Buildings || !State->bSite)
		{
			return;
		}
		Aim(At(SiteW / 2, SiteH / 2), 2.0f);

		// A line of six walls: dragged out along x, six ghosts, six commands.
		O->SelectBuilding(TEXT("Wall"), TEXT("Steel"));
		O->BeginDrag(At(0, 0));
		O->SetHoverCell(At(5, 1));  // the longer way wins: along x
		TArray<colony::CellPos> Origins;
		O->GetBuildOrigins(Origins);
		if (Origins.Num() != 6 || Origins[0].x != At(0, 0).x || Origins[5].x != At(5, 0).x || Origins[5].y != At(0, 0).y)
		{
			Fail(FString::Printf(TEXT("orders_architect: dragging 6 cells along x gave %d wall cells"), Origins.Num()));
		}
		O->RefreshOverlay();
		State->GhostValidInLine = O->GetStats().GhostValid;
		if (State->bGhostMaterial && Buildings->IsBuilt() && (O->GetStats().GhostValid != 6 || O->GetStats().GhostInvalid != 0))
		{
			Fail(FString::Printf(TEXT("orders_architect: the line of walls showed %d green and %d red ghosts, wanted 6 and 0"), O->GetStats().GhostValid, O->GetStats().GhostInvalid));
		}
		const int32 Sent = O->EndDrag(At(5, 1));
		if (Sent != 6)
		{
			Fail(FString::Printf(TEXT("orders_architect: the wall line sent %d commands, wanted 6"), Sent));
		}
		State->WallsPlanned = CountThings("Wall", colony::ConstructionStage::Blueprint);
		if (State->WallsPlanned != 6)
		{
			Fail(FString::Printf(TEXT("orders_architect: %d wall blueprints after a line of 6"), State->WallsPlanned));
		}

		// A bed, turned to lie east and west: its blueprint is two wide and one tall.
		O->SelectBuilding(TEXT("Bed"));
		O->RotateBuilding();
		O->SetHoverCell(At(2, 3));
		O->BeginDrag(At(2, 3));
		if (O->EndDrag(At(2, 3)) != 1)
		{
			Fail(TEXT("orders_architect: the bed was not planned"));
		}
		bool bBedTurned = false;
		for (const colony::ThingView& View : Bridge()->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Blueprint && SimOf()->defs().get(View.def).defName() == "Bed")
			{
				bBedTurned = View.sizeX == 2 && View.sizeY == 1;
			}
		}
		if (!bBedTurned)
		{
			Fail(TEXT("orders_architect: the turned bed's blueprint is not two wide and one tall"));
		}

		// Where a plan already stands nothing more is planned, and the ghost there is red.
		O->SelectBuilding(TEXT("Wall"), TEXT("Steel"));
		O->SetHoverCell(At(0, 0));
		if (O->CanPlace(O->GetBuildDef(), At(0, 0), O->GetBuildRotation()))
		{
			Fail(TEXT("orders_architect: a wall can be planned on a wall plan"));
		}
		O->RefreshOverlay();
		if (State->bGhostMaterial && Buildings->IsBuilt() && O->GetStats().GhostInvalid != 1)
		{
			Fail(FString::Printf(TEXT("orders_architect: the ghost on top of a plan was not red (%d red ghosts)"), O->GetStats().GhostInvalid));
		}
		O->BeginDrag(At(0, 0));
		State->BlockedPlaced = O->EndDrag(At(0, 0));
		if (State->BlockedPlaced != 0)
		{
			Fail(FString::Printf(TEXT("orders_architect: %d commands sent for a spot that is taken"), State->BlockedPlaced));
		}

		// A door beside the walls, a table of two cells, a stove that needs no stuff.
		O->SelectBuilding(TEXT("Door"), TEXT("Steel"));
		O->SetHoverCell(At(6, 0));
		O->BeginDrag(At(6, 0));
		int32 Doors = O->EndDrag(At(6, 0));
		O->SelectBuilding(TEXT("Table1x2"), TEXT("WoodLog"));
		O->SetHoverCell(At(2, 8));
		O->BeginDrag(At(2, 8));
		const int32 Tables = O->EndDrag(At(2, 8));
		O->SelectBuilding(TEXT("FueledStove"));
		if (O->GetBuildStuff().valid())
		{
			Fail(TEXT("orders_architect: the stove was given a stuff"));
		}
		O->SetHoverCell(At(11, 3));
		O->BeginDrag(At(11, 3));
		const int32 Stoves = O->EndDrag(At(11, 3));
		if (Doors != 1 || Tables != 1 || Stoves != 1)
		{
			Fail(FString::Printf(TEXT("orders_architect: door, table and stove sent %d, %d and %d commands, wanted 1 each"), Doors, Tables, Stoves));
		}
		const int32 Plans = CountThings("Wall", colony::ConstructionStage::Blueprint) + CountThings("Door", colony::ConstructionStage::Blueprint) + CountThings("Bed", colony::ConstructionStage::Blueprint) +
			CountThings("Table1x2", colony::ConstructionStage::Blueprint) + CountThings("FueledStove", colony::ConstructionStage::Blueprint);
		if (Plans != 10)
		{
			Fail(FString::Printf(TEXT("orders_architect: %d blueprints stand, wanted 10 (6 walls, door, bed, table, stove)"), Plans));
		}
		if (State->FailedCommands > 0)
		{
			Fail(FString::Printf(TEXT("orders_architect: the sim refused %d commands, first: %s"), State->FailedCommands, *State->FirstFailure));
		}

		// For the picture: another bed hovering, green, with the menu open.
		O->SelectBuilding(TEXT("Bed"));
		O->RotateBuilding();
		O->SetHoverCell(At(8, 8));
	};
	Architect.Verify = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		if (!O || !State->bSite)
		{
			return;
		}
		O->RefreshOverlay();
		Notes.Add(FString::Printf(TEXT("orders_architect: %d walls dragged, %d green ghosts in the line, %d commands sent so far"), State->WallsPlanned, State->GhostValidInLine, O->GetCommandsSent()));
		if (State->bGhostMaterial && O->GetStats().GhostValid != 1)
		{
			Fail(FString::Printf(TEXT("orders_architect: the hovering bed showed %d green ghosts, wanted 1"), O->GetStats().GhostValid));
		}
		O->CancelTool();
		O->RefreshOverlay();
		if (O->GetStats().GhostValid + O->GetStats().GhostInvalid != 0)
		{
			Fail(TEXT("orders_architect: ghosts are still drawn with no tool in hand"));
		}
	};
	Phases.Add(MoveTemp(Architect));

	// 3. The marks: rock to mine (a real mouse drag on screen), a tree to cut, a bush to harvest, a stockpile; the overlay shows every one.
	FPhase Marks;
	Marks.Name = TEXT("orders_marks");
	Marks.WarmupSeconds = 2.5f;
	Marks.MeasureSeconds = 1.0f;
	Marks.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		colony::Sim* Sim = SimOf();
		UTerrainSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainSubsystem>();
		if (!B || !Sim || !Terrain || !State->bSite || Sim->mineables().rockTypes().empty())
		{
			Fail(TEXT("orders_marks: no sim, terrain or rock types"));
			return;
		}
		// The test puts four rock cells, a tree and a bush on the open ground (dev placements), then orders them like the player.
		for (int32 X = 1; X <= 4; ++X)
		{
			Sim->mineables().place(At(X, 12), Sim->mineables().rockTypes()[0]);
			Terrain->MarkCellDirty(At(X, 12));
		}
		Terrain->FlushDirty();
		Sim->plants().remove(At(7, 12));
		Sim->plants().remove(At(9, 12));
		const colony::DefId Poplar = Sim->defs().idOf("Things", "Plant_TreePoplar");
		const colony::DefId Berry = Sim->defs().idOf("Things", "Plant_Berry");
		if (!Poplar.valid() || !Berry.valid() || !Sim->plants().place(At(7, 12), Poplar, 1.0f) || !Sim->plants().place(At(9, 12), Berry, 1.0f))
		{
			Fail(TEXT("orders_marks: the tree or the bush could not be placed"));
			return;
		}
		Aim(At(SiteW / 2, SiteH / 2), 2.0f);
		Orders()->CancelTool();
	};
	Marks.OnFrame = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		USimSubsystem* B = Bridge();
		if (State->bMarksDone || PhaseTime < 0.8f || !O || !B || !State->bSite)
		{
			return;
		}
		State->bMarksDone = true;
		// Mine: press on the first rock cell and let go on the last, by screen position, as the mouse does.
		FVector2D Press, Release;
		const double TopCm = TerrainDraw::RockHeightM * 100.0;
		if (!OrdersSmokeProject(GetWorld(), B->SimToWorld(At(1, 12).x + 0.5f, At(1, 12).y + 0.5f, 0) + FVector(0.0, 0.0, TopCm), Press) ||
			!OrdersSmokeProject(GetWorld(), B->SimToWorld(At(4, 12).x + 0.5f, At(4, 12).y + 0.5f, 0) + FVector(0.0, 0.0, TopCm), Release))
		{
			Fail(TEXT("orders_marks: the rock cells are not on screen"));
			return;
		}
		O->SetTool(EOrderTool::Mine);
		if (!O->HandleLeftPressed(Press) || !O->IsDragging())
		{
			Fail(TEXT("orders_marks: pressing the mouse with the Mine tool did not start a drag"));
			return;
		}
		if (!O->HandleLeftReleased(Release) || O->IsDragging())
		{
			Fail(TEXT("orders_marks: letting go of the mouse did not end the drag"));
			return;
		}
		State->MineSent = CountMarks(colony::DesignationKind::Mine);
		// Chop the tree, harvest the bush: one cell each.
		const int32 Chop = Drag(EOrderTool::Chop, At(7, 12), At(7, 12));
		const int32 Harvest = Drag(EOrderTool::Harvest, At(9, 12), At(9, 12));
		// A stockpile zone, dragged out as a rectangle.
		const int32 ZonesBefore = static_cast<int32>(B->GetCurrentSnapshot().zones.size());
		const int32 Zone = Drag(EOrderTool::Stockpile, At(10, 10), At(13, 12));
		if (Chop != 1 || Harvest != 1 || Zone != 1)
		{
			Fail(FString::Printf(TEXT("orders_marks: chop, harvest and stockpile sent %d, %d and %d commands, wanted 1 each"), Chop, Harvest, Zone));
		}
		if (static_cast<int32>(B->GetCurrentSnapshot().zones.size()) != ZonesBefore + 1)
		{
			Fail(TEXT("orders_marks: the stockpile drag made no zone"));
		}
		O->RefreshOverlay();
	};
	Marks.Verify = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		USimSubsystem* B = Bridge();
		if (!O || !B || !State->bSite)
		{
			return;
		}
		if (!State->bMarksDone)
		{
			Fail(TEXT("orders_marks: the mouse drag never ran"));
			return;
		}
		O->RefreshOverlay();
		const FOrdersStats& Stats = O->GetStats();
		const int32 Mine = CountMarks(colony::DesignationKind::Mine);
		const int32 Cut = CountMarks(colony::DesignationKind::CutPlant);
		const int32 Harvest = CountMarks(colony::DesignationKind::HarvestPlant);
		if (Mine != 4 || Cut != 1 || Harvest != 1)
		{
			Fail(FString::Printf(TEXT("orders_marks: the sim has %d mine, %d cut and %d harvest marks, wanted 4, 1 and 1"), Mine, Cut, Harvest));
		}
		int32 ZoneCells = static_cast<int32>(B->GetCurrentSnapshot().zoneCells.size());
		if (ZoneCells < 10)
		{
			Fail(FString::Printf(TEXT("orders_marks: the stockpile has %d cells, wanted about 12"), ZoneCells));
		}
		// The overlay draws one instance for every mark and every stockpile cell.
		if (Stats.MineMarks != Mine || Stats.CutMarks != Cut || Stats.HarvestMarks != Harvest || Stats.ZoneCells != ZoneCells)
		{
			Fail(FString::Printf(TEXT("orders_marks: the overlay draws %d/%d/%d marks and %d zone cells; the sim has %d/%d/%d and %d"), Stats.MineMarks, Stats.CutMarks, Stats.HarvestMarks,
				Stats.ZoneCells, Mine, Cut, Harvest, ZoneCells));
		}
		if (State->FailedCommands > 0)
		{
			Fail(FString::Printf(TEXT("orders_marks: the sim refused %d commands, first: %s"), State->FailedCommands, *State->FirstFailure));
		}
		Notes.Add(FString::Printf(TEXT("orders_marks: overlay mine %d, cut %d, harvest %d, zone cells %d; %lld overlay rebuilds"), Stats.MineMarks, Stats.CutMarks, Stats.HarvestMarks, Stats.ZoneCells,
			Stats.OverlayRebuilds));
	};
	Phases.Add(MoveTemp(Marks));

	// 4. Deconstruct a finished wall, then cancel plans, the mark on the wall and the mine marks.
	FPhase Cancel;
	Cancel.Name = TEXT("orders_cancel");
	Cancel.WarmupSeconds = 1.0f;
	Cancel.MeasureSeconds = 2.5f;
	Cancel.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UOrdersSubsystem* O = Orders();
		if (!B || !O || !State->bSite)
		{
			return;
		}
		Aim(At(SiteW / 2, SiteH / 2), 2.0f);
		B->Submit(colony::SpawnThingCommand{"Wall", At(14, 2), 1, colony::Rot4::North});
		B->StepOnce();
		if (Drag(EOrderTool::Deconstruct, At(14, 2), At(14, 2)) != 1)
		{
			Fail(TEXT("orders_cancel: the Deconstruct tool sent no command for the wall"));
		}
		bool bMarked = false;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			bMarked = bMarked || (View.category == colony::ThingCategory::Building && View.markedForDeconstruct);
		}
		if (!bMarked)
		{
			Fail(TEXT("orders_cancel: the wall is not marked for deconstruction"));
		}
		O->RefreshOverlay();
		if (O->GetStats().DeconstructMarks != 1)
		{
			Fail(FString::Printf(TEXT("orders_cancel: the overlay shows %d deconstruct marks, wanted 1"), O->GetStats().DeconstructMarks));
		}
		O->CancelTool();
	};
	Cancel.OnFrame = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		USimSubsystem* B = Bridge();
		// After the picture of the mark has been taken.
		if (State->bCancelDone || PhaseTime < 2.4f || !O || !B || !State->bSite)
		{
			return;
		}
		State->bCancelDone = true;
		Drag(EOrderTool::Cancel, At(0, 0), At(6, 0));    // the walls and the door
		Drag(EOrderTool::Cancel, At(14, 2), At(14, 2));  // the deconstruct mark
		Drag(EOrderTool::Cancel, At(1, 12), At(4, 12));  // the mine marks
		O->CancelTool();
		O->RefreshOverlay();
	};
	Cancel.Verify = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		USimSubsystem* B = Bridge();
		if (!O || !B || !State->bSite)
		{
			return;
		}
		if (!State->bCancelDone)
		{
			Fail(TEXT("orders_cancel: the cancels never ran"));
			return;
		}
		const int32 Walls = CountThings("Wall", colony::ConstructionStage::Blueprint);
		const int32 Doors = CountThings("Door", colony::ConstructionStage::Blueprint);
		const int32 Beds = CountThings("Bed", colony::ConstructionStage::Blueprint);
		const int32 Mine = CountMarks(colony::DesignationKind::Mine);
		bool bStillMarked = false;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			bStillMarked = bStillMarked || (View.category == colony::ThingCategory::Building && View.markedForDeconstruct);
		}
		if (Walls != 0 || Doors != 0 || Beds != 1)
		{
			Fail(FString::Printf(TEXT("orders_cancel: %d wall, %d door and %d bed plans stand after cancelling the walls and door, wanted 0, 0 and 1"), Walls, Doors, Beds));
		}
		if (bStillMarked || O->GetStats().DeconstructMarks != 0)
		{
			Fail(TEXT("orders_cancel: the deconstruct mark is still there"));
		}
		if (Mine != 0 || O->GetStats().MineMarks != 0)
		{
			Fail(FString::Printf(TEXT("orders_cancel: %d mine marks (%d drawn) are left"), Mine, O->GetStats().MineMarks));
		}
		if (CountMarks(colony::DesignationKind::CutPlant) != 1 || CountMarks(colony::DesignationKind::HarvestPlant) != 1)
		{
			Fail(TEXT("orders_cancel: the cut and harvest marks outside the cancelled area were lost"));
		}
		if (State->FailedCommands > 0)
		{
			Fail(FString::Printf(TEXT("orders_cancel: the sim refused %d commands, first: %s"), State->FailedCommands, *State->FirstFailure));
		}
	};
	Phases.Add(MoveTemp(Cancel));

	// 5. Draft with R, a right click to move, a right click on an enemy to attack, and the alarm.
	FPhase Fight;
	Fight.Name = TEXT("orders_fight");
	Fight.WarmupSeconds = 2.5f;
	Fight.MeasureSeconds = 1.0f;
	Fight.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !State->bSite)
		{
			return;
		}
		// Two colonists: the one that is ordered, and the one that plays the enemy.
		std::vector<colony::Entity> Colonists;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && !View.downed && SimOf()->colonistOf(View.id))
			{
				Colonists.push_back(View.id);
			}
		}
		State->ColonistCount = static_cast<int32>(Colonists.size());
		if (Colonists.size() < 3)
		{
			Fail(FString::Printf(TEXT("orders_fight: only %d colonists in the Crashlanded start"), static_cast<int32>(Colonists.size())));
			return;
		}
		State->Colonist = Colonists[0];
		State->Enemy = Colonists[1];
		FVector Where;
		B->GetBlendedLocation(State->Colonist, Where);
		const FVector Local = (Where - B->GetWorldOrigin()) / 100.0;
		Aim(colony::CellPos{FMath::FloorToInt32(Local.X) + 4, FMath::FloorToInt32(Local.Y), 0}, 3.0f);
		Orders()->CancelTool();
	};
	Fight.OnFrame = [=, this]()
	{
		UOrdersSubsystem* O = Orders();
		USimSubsystem* B = Bridge();
		UOverseerSubsystem* Ov = Overseer();
		if (State->bFightDone || PhaseTime < 0.8f || !O || !B || !Ov || State->Colonist == colony::kNoEntity)
		{
			return;
		}
		State->bFightDone = true;
		const auto Find = [B](colony::Entity Id) { return B->GetCurrentSnapshot().find(Id); };

		// R with nothing in hand drafts the selected colonist.
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = State->Colonist;
		Ov->Select(Pick);
		if (!O->HandleRotateOrDraftKey() || !Find(State->Colonist) || !Find(State->Colonist)->drafted)
		{
			Fail(TEXT("orders_fight: R did not draft the selected colonist"));
			return;
		}
		// A right click on the ground, by screen position, sends a move order; the colonist holds near that cell.
		FVector Where;
		B->GetBlendedLocation(State->Colonist, Where);
		const FVector Local = (Where - B->GetWorldOrigin()) / 100.0;
		State->MoveTarget = colony::CellPos{FMath::FloorToInt32(Local.X) + 6, FMath::FloorToInt32(Local.Y) + 1, 0};
		FVector2D Screen;
		if (!OrdersSmokeProject(GetWorld(), B->SimToWorld(State->MoveTarget.x + 0.5f, State->MoveTarget.y + 0.5f, 0), Screen))
		{
			Fail(TEXT("orders_fight: the cell to move to is not on screen"));
			return;
		}
		const int32 SentBefore = O->GetCommandsSent();
		if (!O->HandleRightClick(Screen) || O->GetCommandsSent() != SentBefore + 1)
		{
			Fail(TEXT("orders_fight: a right click with a drafted colonist selected did not send a move order"));
			return;
		}
		for (int32 I = 0; I < 3; ++I)
		{
			B->StepOnce();
		}
		const colony::ThingView* Drafted = Find(State->Colonist);
		if (!Drafted || FMath::Abs(Drafted->fightSpot.x - State->MoveTarget.x) > 3 || FMath::Abs(Drafted->fightSpot.y - State->MoveTarget.y) > 3)
		{
			Fail(FString::Printf(TEXT("orders_fight: the colonist holds cell %d, %d after a move order to %d, %d"), Drafted ? Drafted->fightSpot.x : -1, Drafted ? Drafted->fightSpot.y : -1,
				State->MoveTarget.x, State->MoveTarget.y));
		}

		// A colonist of another side: a right click on it is an attack order.
		B->Submit(colony::SetFactionCommand{State->Enemy, 2});
		B->StepOnce();
		B->GetBlendedLocation(State->Enemy, Where);
		const int32 FailedBefore = State->FailedCommands;
		const int32 SentBeforeAttack = O->GetCommandsSent();
		if (OrdersSmokeProject(GetWorld(), Where + FVector(0.0, 0.0, 90.0), Screen))
		{
			O->HandleRightClick(Screen);
		}
		else
		{
			O->OrderAttackSelected(State->Enemy);
		}
		for (int32 I = 0; I < 3; ++I)
		{
			B->StepOnce();
		}
		if (O->GetCommandsSent() != SentBeforeAttack + 1 || State->FailedCommands != FailedBefore)
		{
			Fail(FString::Printf(TEXT("orders_fight: the attack order sent %d commands and %d failed"), O->GetCommandsSent() - SentBeforeAttack, State->FailedCommands - FailedBefore));
		}
		const colony::ThingView* Attacker = Find(State->Colonist);
		Notes.Add(FString::Printf(TEXT("orders_fight: after the attack order the colonist's target is %s"),
			Attacker && Attacker->fightTarget == State->Enemy ? TEXT("the enemy") : (Attacker && Attacker->fightTarget != colony::kNoEntity ? TEXT("someone else") : TEXT("none yet"))));
		B->Submit(colony::SetFactionCommand{State->Enemy, 1});
		B->StepOnce();

		// The alarm drafts everyone not drafted yet; pressed again it sends them all back.
		// (Someone may have been shot down in the fight above; the alarm counts the colonists that can act.)
		const auto CountAble = [=, this](bool bOnlyUndrafted) -> int32
		{
			int32 Count = 0;
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				Count += View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && !View.downed && SimOf()->colonistOf(View.id) && (!bOnlyUndrafted || !View.drafted) ? 1 : 0;
			}
			return Count;
		};
		const int32 Able = CountAble(false);
		const int32 Undrafted = CountAble(true);
		State->AlarmDrafted = O->Alarm();
		if (State->AlarmDrafted != Undrafted || CountAble(true) != 0)
		{
			Fail(FString::Printf(TEXT("orders_fight: the alarm changed %d colonists (wanted %d) and %d are still undrafted"), State->AlarmDrafted, Undrafted, CountAble(true)));
		}
		State->AlarmStandDown = O->Alarm();
		if (State->AlarmStandDown != Able || CountAble(true) != Able)
		{
			Fail(FString::Printf(TEXT("orders_fight: standing down changed %d colonists (wanted %d) and %d are undrafted"), State->AlarmStandDown, Able, CountAble(true)));
		}
		// Draft the selected colonist again for the picture, and R once more undrafts.
		if (!O->HandleRotateOrDraftKey() || !Find(State->Colonist) || !Find(State->Colonist)->drafted)
		{
			Fail(TEXT("orders_fight: R did not draft again"));
		}
		O->OrderMoveSelected(State->MoveTarget);
		B->StepOnce();
	};
	Fight.Verify = [=, this]()
	{
		if (!State->bFightDone)
		{
			Fail(TEXT("orders_fight: the orders never ran"));
			return;
		}
		if (State->FailedCommands > 0)
		{
			Fail(FString::Printf(TEXT("orders_fight: the sim refused %d commands, first: %s"), State->FailedCommands, *State->FirstFailure));
		}
		Notes.Add(FString::Printf(TEXT("orders_fight: %d colonists; the alarm drafted %d, standing down sent %d back"), State->ColonistCount, State->AlarmDrafted, State->AlarmStandDown));
	};
	Phases.Add(MoveTemp(Fight));
}
