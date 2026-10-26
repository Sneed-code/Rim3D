// Smoke test `levels` (task U17): upper floors, stairs and ladders on screen, the overseer's level cutaway (Page Up and
// Page Down), picking on the level looked at, and the player's colonist climbing stairs in direct mode, offscreen.
#include "SmokeTest.h"

#include "HAL/PlatformTime.h"
#include "ProceduralMeshComponent.h"

#include "BuildingSubsystem.h"
#include "LevelsSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	const FVector LevelsSmokeOrigin(0.0, -600000.0, 0.0);

	// The house, from the south-west corner of its site A: walls round x 1..7, y 2..8 with a door at (4, 2); stairs at x 9
	// climbing north from (9, 1) on the ground to (9, 5) upstairs; a floor over x 0..8, y 1..9 and a landing over x 9,
	// y 2..9 (over the stairs too, drawn as an open stairwell); upstairs a wall along y 8, a pile of wood and a second colonist; a ladder
// at (10, 8) climbing west onto the landing.
	constexpr int32 LevelsSiteW = 12;
	constexpr int32 LevelsSiteH = 11;

	struct FLevelsSmokeState
	{
		colony::CellPos Site;
		colony::CellPos Low;
		colony::CellPos High;
		colony::Entity Me = colony::kNoEntity;
		colony::Entity Upstairs = colony::kNoEntity;
		int32 FailedCommands = 0;
		FString FirstFailure;
		bool bListening = false;
		// The walk: waypoints (sim metres), then a push the way of the climb.
		TArray<FVector2D> Route;
		int32 RouteIndex = 0;
		float PushYaw = 90.0f;
		bool bPushing = false;
		// The climb up: seen halfway (and paused there for the picture), how high it was drawn.
		bool bMidClimb = false;
		double MidRiseCm = 0.0;
		double MaxRiseCm = 0.0;
		int64 ClimbsBefore = 0;
		bool bPausedMid = false;
	};

	bool LevelsSmokeSiteFree(const colony::Map& Map, int32 X0, int32 Y0, int32 W, int32 H)
	{
		for (int32 Y = Y0 - 1; Y <= Y0 + H; ++Y)
		{
			for (int32 X = X0 - 1; X <= X0 + W; ++X)
			{
				const colony::CellPos Cell{X, Y, 0};
				if (!Map.inBounds(Cell) || Map.terrain(Cell) != colony::Terrain::Soil || Map.buildingAt(Cell) != colony::kNoEntity || !Map.groundWalkable(Cell))
				{
					return false;
				}
			}
		}
		return true;
	}

	/** A site north of `From` whose stairs column runs straight up from it, with nothing built in the way. */
	bool LevelsSmokeFindSite(const colony::Map& Map, colony::CellPos From, colony::CellPos& Out)
	{
		for (int32 Shift = 0; Shift <= 30; ++Shift)
		{
			for (int32 Sign = -1; Sign <= 1; Sign += 2)
			{
				const int32 X0 = From.x - 9 + Sign * Shift;
				for (int32 Y0 = From.y + 3; Y0 <= From.y + 30; ++Y0)
				{
					if (LevelsSmokeSiteFree(Map, X0, Y0, LevelsSiteW, LevelsSiteH) && LevelsSmokeSiteFree(Map, X0 + 9, From.y, 1, Y0 - From.y))
					{
						Out = colony::CellPos{X0, Y0, 0};
						return true;
					}
				}
			}
		}
		return false;
	}

	const colony::ThingView* LevelsSmokeFind(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		return Thing == colony::kNoEntity ? nullptr : Bridge.GetCurrentSnapshot().find(Thing);
	}
}

void USmokeTestSubsystem::BuildLevelsPhases()
{
	const TSharedRef<FLevelsSmokeState> State = MakeShared<FLevelsSmokeState>();
	const auto AddPhase = [this](const TCHAR* Name, TFunction<void()> Setup, float Warm, float Measure, TFunction<void()> EveryFrame, TFunction<void()> Check)
	{
		FPhase Phase;
		Phase.Name = Name;
		Phase.Setup = MoveTemp(Setup);
		Phase.WarmupSeconds = Warm;
		Phase.MeasureSeconds = Measure;
		Phase.OnFrame = MoveTemp(EveryFrame);
		Phase.Verify = MoveTemp(Check);
		Phases.Add(MoveTemp(Phase));
	};
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Levels = [this]() { return GetWorld()->GetSubsystem<ULevelsSubsystem>(); };
	const auto Buildings = [this]() { return GetWorld()->GetSubsystem<UBuildingSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto At = [State](int32 DX, int32 DY, int32 Level = 0) { return colony::CellPos{State->Site.x + DX, State->Site.y + DY, Level}; };
	const auto Centre = [](const colony::CellPos& Cell) { return FVector2D(Cell.x + 0.5, Cell.y + 0.5); };
	// The camera over the house, looking down at it from the south-east.
	const auto HouseView = [=, this](int32 Level, double Metres, double Pitch)
	{
		USimSubsystem* B = Bridge();
		const FVector Target = B->SimToWorld(State->Site.x + 5.0f, State->Site.y + 5.0f, Level);
		const FRotator Look(-Pitch, 120.0, 0.0);
		ViewFrom(Target - Look.Vector() * Metres * 100.0, Look);
	};
	// A ray straight down onto the middle of a cell, from 20 m up.
	const auto PickDown = [=](int32 DX, int32 DY) -> FSimPick
	{
		USimSubsystem* B = Bridge();
		const FVector Top = B->SimToWorld(State->Site.x + DX + 0.5f, State->Site.y + DY + 0.5f, 0) + FVector(0.0, 0.0, 2000.0);
		return SimPicking::PickFromRay(*B, Top, FVector(0.0, 0.0, -1.0));
	};
	const auto Listen = [=]()
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
	// Drives the body along the route, then pushes the way of the climb (scripted input, as the keys would).
	const auto Drive = [=]()
	{
		AProtagonistPawn* Body = Prot() ? Prot()->GetBody() : nullptr;
		if (!Body)
		{
			return;
		}
		if (!State->bPushing && State->RouteIndex >= State->Route.Num())
		{
			State->bPushing = true;
		}
		float Wanted = State->PushYaw;
		if (!State->bPushing)
		{
			const FVector2D To = State->Route[State->RouteIndex] - Body->GetSimPosition();
			if (To.Size() < 0.12)
			{
				++State->RouteIndex;
				Body->SetScriptedInput(0.0, 0.0, true);
				return;
			}
			Wanted = FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X));
			Body->Look(FRotator::NormalizeAxis(Wanted - Body->GetYaw()), 0.0f);
			Body->SetScriptedInput(0.0, 1.0, To.Size() < 1.0);
			return;
		}
		Body->Look(FRotator::NormalizeAxis(Wanted - Body->GetYaw()), 0.0f);
		Body->SetScriptedInput(0.0, 1.0, true);
	};
	// How high the protagonist is drawn over the ground while it climbs.
	const auto MeasureClimb = [=]()
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Me = LevelsSmokeFind(*B, State->Me);
		FVector Drawn;
		if (!Me || Me->climb <= 0.0f || !B->GetBlendedLocation(State->Me, Drawn))
		{
			return -1.0;
		}
		const double Rise = Drawn.Z - B->SimToWorld(Me->x, Me->y, 0).Z;
		State->MaxRiseCm = FMath::Max(State->MaxRiseCm, Rise);
		return Rise;
	};
	const auto StairsView = [=, this]()
	{
		USimSubsystem* B = Bridge();
		const FVector Middle = B->SimToWorld(State->Low.x + 0.5f, State->Low.y + 2.5f, 0) + FVector(0.0, 0.0, 150.0);
		const FVector Eye = Middle + FVector(650.0, -250.0, 260.0);
		ViewFrom(Eye, (Middle - Eye).Rotation());
	};

	// 1. A flat map with the Crashlanded start, run on to the morning so the pictures are lit.
	AddPhase(TEXT("morning"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Levels() || !Buildings() || !Overseer() || !Prot() || !Pawns())
		{
			Fail(TEXT("morning: a subsystem did not start (sim, levels, buildings, overseer, protagonist or pawns)"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(LevelsSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("morning: the game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		Listen();
	}, 8.0f, 0.5f, [=]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		const double Start = FPlatformTime::Seconds();
		for (int32 Count = 0; B->GetSim()->time().hourOfDay() < 10 && Count < 400 && FPlatformTime::Seconds() - Start < 0.03; ++Count)
		{
			B->StepOnce();
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (B && B->GetSim() && B->GetSim()->time().hourOfDay() < 10)
		{
			Fail(FString::Printf(TEXT("morning: the sim only got to %02d:00"), B->GetSim()->time().hourOfDay()));
		}
	});

	// 2. The house: walls, a door and stairs, built at once by dev commands, north of the protagonist; the overseer view.
	AddPhase(TEXT("house"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		State->Me = Prot()->FindProtagonist();
		const colony::ThingView* Me = LevelsSmokeFind(*B, State->Me);
		if (!Me)
		{
			Fail(TEXT("house: no protagonist"));
			return;
		}
		const colony::CellPos From{FMath::FloorToInt32(Me->x), FMath::FloorToInt32(Me->y), 0};
		if (!LevelsSmokeFindSite(B->GetSim()->map(), From, State->Site))
		{
			Fail(TEXT("house: no open ground for the house north of the protagonist"));
			return;
		}
		State->Low = At(9, 1);
		State->High = At(9, 5, 1);
		for (int32 Y = 2; Y <= 8; ++Y)
		{
			for (int32 X = 1; X <= 7; ++X)
			{
				if ((X == 1 || X == 7 || Y == 2 || Y == 8) && !(X == 4 && Y == 2))
				{
					B->Submit(colony::SpawnThingCommand{"Wall", At(X, Y), 1});
				}
			}
		}
		B->Submit(colony::SpawnThingCommand{"Door", At(4, 2), 1});
		B->Submit(colony::SpawnThingCommand{"Stairs", At(9, 2), 1, colony::Rot4::North});
		B->StepOnce();
		Overseer()->EnterOverseerMode();
		Levels()->SetViewLevel(0);
	}, 1.5f, 0.5f, [=]()
	{
		HouseView(0, 22.0, 50.0);
	}, [=, this]()
	{
		const colony::Map& Map = Bridge()->GetSim()->map();
		if (Map.levelLinks().size() != 1 || Map.levelLinks()[0].low != State->Low || Map.levelLinks()[0].high != State->High)
		{
			Fail(FString::Printf(TEXT("house: %d level links, not the one stairs from the cell south of them to the upper floor"), static_cast<int32>(Map.levelLinks().size())));
		}
	});

	// 3. The upper floor, a wall, wood and a colonist up there; the overseer looks at level 1.
	AddPhase(TEXT("upper-floor"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		B->Submit(colony::PlaceUpperFloorCommand{colony::CellRect{State->Site.x, State->Site.y + 1, 9, 9, 1}});
		// The landing covers the stairs too, as the house scenario's floor does: the stairwell is drawn open anyway.
		B->Submit(colony::PlaceUpperFloorCommand{colony::CellRect{State->Site.x + 9, State->Site.y + 2, 1, 8, 1}});
		B->StepOnce();
		for (int32 X = 1; X <= 5; ++X)
		{
			B->Submit(colony::SpawnThingCommand{"Wall", At(X, 8, 1), 1});
		}
		B->Submit(colony::SpawnThingCommand{"WoodLog", At(6, 6, 1), 40});
		// A ladder up to the landing from the east: from (11, 8) on the ground to (9, 8) upstairs.
		B->Submit(colony::SpawnThingCommand{"Ladder", At(10, 8), 1, colony::Rot4::West});
		B->Submit(colony::SpawnThingCommand{"Human", At(3, 5, 1), 1});
		B->StepOnce();
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.level == 1)
			{
				State->Upstairs = View.id;
			}
		}
		Overseer()->GetOverseerPawn()->StepLevel(1);
	}, 2.0f, 1.5f, [=]()
	{
		HouseView(1, 22.0, 50.0);
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		ULevelsSubsystem* L = Levels();
		L->Refresh();
		Buildings()->Refresh();
		int32 Floors = 0;
		for (const colony::CellPos& Cell : B->GetSim()->map().upperFloorCells())
		{
			Floors += Cell.level == 1 ? 1 : 0;
		}
		if (Floors != 89)
		{
			Fail(FString::Printf(TEXT("upper-floor: the sim has %d floor cells on level 1, not 89 (%s)"), Floors, *State->FirstFailure));
		}
		// The three over the stairs are the stairwell, not drawn.
		if (L->GetFloorCells(1) != Floors - 3 || !L->IsLevelShown(1) || L->GetViewLevel() != 1)
		{
			Fail(FString::Printf(TEXT("upper-floor: %d floor cells drawn for %d less the stairwell, level 1 shown %d, looked at %d"), L->GetFloorCells(1), Floors, L->IsLevelShown(1) ? 1 : 0,
				L->GetViewLevel()));
		}
		UProceduralMeshComponent* Mesh = L->GetChunkMesh(1, State->Site.x / 32, State->Site.y / 32);
		if (!Mesh || Mesh->GetNumSections() < 1 || Mesh->GetProcMeshSection(0) == nullptr || Mesh->GetProcMeshSection(0)->ProcVertexBuffer.Num() == 0)
		{
			Fail(TEXT("upper-floor: no floor mesh in the house's chunk on level 1"));
		}
		else
		{
			// The floor's top is the sim's floor height: 3 m over the ground.
			const FBox Box = Mesh->GetProcMeshSection(0)->SectionLocalBox;
			const double Ground = B->GetSim()->map().groundHeight(State->Site.x + 4, State->Site.y + 4) * 100.0;
			if (FMath::Abs(Box.Max.Z - (Ground + 300.0)) > 30.0)
			{
				Fail(FString::Printf(TEXT("upper-floor: the floor's top is drawn at %.0f cm, the sim has it at %.0f"), Box.Max.Z, Ground + 300.0));
			}
		}
		const colony::Map& Map = B->GetSim()->map();
		if (Map.levelLinks().size() != 2 || !Map.levelLinkBetween(At(11, 8), At(9, 8, 1)))
		{
			Fail(TEXT("upper-floor: the ladder does not link the ground east of it to the landing"));
		}
		if (Buildings()->GetLevelInstances(1, true) < 6)
		{
			Fail(FString::Printf(TEXT("upper-floor: %d instances drawn on level 1 for five walls and a pile of wood"), Buildings()->GetLevelInstances(1, true)));
		}
		FTransform Placed;
		const colony::DefId Wall = B->GetSim()->defs().idOf("Things", "Wall");
		if (!Buildings()->GetPlacedTransform(Wall, At(2, 8, 1), colony::Rot4::North, Placed) ||
			FMath::Abs(Placed.GetLocation().Z - B->SimToWorld(State->Site.x + 2.5f, State->Site.y + 8.5f, 1).Z) > 30.0)
		{
			Fail(TEXT("upper-floor: a wall on level 1 does not stand on the floor"));
		}
		const ASimPawnActor* Up = Pawns()->FindPawn(State->Upstairs);
		if (State->Upstairs == colony::kNoEntity || !Up || Up->IsCutAway() || Up->IsHidden())
		{
			Fail(TEXT("upper-floor: the colonist upstairs is missing or hidden while level 1 is looked at"));
		}
		// A click on the floor picks the floor's cell, on level 1.
		const FSimPick Pick = PickDown(2, 3);
		if (Pick.Kind != ESimPickKind::Cell || Pick.Cell != At(2, 3, 1))
		{
			Fail(FString::Printf(TEXT("upper-floor: a click on the floor at (2, 3) picked kind %d cell %d, %d, level %d"), static_cast<int32>(Pick.Kind),
				Pick.Cell.x - State->Site.x, Pick.Cell.y - State->Site.y, Pick.Cell.level));
		}
		// The architect's cell is on the level looked at, over open air too.
		const FVector Air = B->SimToWorld(State->Site.x + 11.5f, State->Site.y + 3.5f, 1) + FVector(0.0, 0.0, 1500.0);
		const FSimPick Plan = SimPicking::PickGroundFromRay(*B, Air, FVector(0.0, 0.0, -1.0), 1);
		if (Plan.Kind != ESimPickKind::Cell || Plan.Cell != At(11, 3, 1))
		{
			Fail(TEXT("upper-floor: the architect's pick over open air on level 1 did not land on level 1"));
		}
	});

	// 4. Page Down: level 1 is cut away, the ground floor's rooms show from above and clicks land on the ground.
	AddPhase(TEXT("cut-away"), [=, this]()
	{
		Overseer()->GetOverseerPawn()->StepLevel(-1);
	}, 1.5f, 1.5f, [=]()
	{
		HouseView(0, 20.0, 55.0);
	}, [=, this]()
	{
		ULevelsSubsystem* L = Levels();
		Buildings()->Refresh();
		if (L->GetViewLevel() != 0 || L->IsLevelShown(1) || L->GetTopShownLevel() != 0)
		{
			Fail(TEXT("cut-away: level 1 is still shown after Page Down"));
		}
		if (Buildings()->GetLevelInstances(1, true) != 0 || Buildings()->GetLevelInstances(1, false) < 6)
		{
			Fail(FString::Printf(TEXT("cut-away: %d of %d instances on level 1 still visible"), Buildings()->GetLevelInstances(1, true), Buildings()->GetLevelInstances(1, false)));
		}
		if (Buildings()->GetLevelInstances(0, true) < 24)
		{
			Fail(TEXT("cut-away: the ground floor's walls are not all shown"));
		}
		const ASimPawnActor* Up = Pawns()->FindPawn(State->Upstairs);
		if (!Up || !Up->IsCutAway() || !Up->IsHidden())
		{
			Fail(TEXT("cut-away: the colonist upstairs is still drawn"));
		}
		UProceduralMeshComponent* Mesh = L->GetChunkMesh(1, State->Site.x / 32, State->Site.y / 32);
		if (Mesh && Mesh->IsMeshSectionVisible(0))
		{
			Fail(TEXT("cut-away: the upper floor's mesh is still visible"));
		}
		const FSimPick Pick = PickDown(2, 3);
		if (Pick.Kind != ESimPickKind::Cell || Pick.Cell != At(2, 3, 0))
		{
			Fail(FString::Printf(TEXT("cut-away: a click at (2, 3) picked kind %d level %d, not the ground under the cut-away floor"), static_cast<int32>(Pick.Kind), Pick.Cell.level));
		}
	});

	// 5. Direct mode: the protagonist walks to the foot of the stairs and pushes into them; the sim climbs. Paused halfway for the picture.
	AddPhase(TEXT("climb-up"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!P->EnterDirectMode(State->Me))
		{
			Fail(TEXT("climb-up: could not take control of the protagonist"));
			return;
		}
		P->GetBody()->SetFirstPerson(false);
		const FVector2D Start = P->GetBody()->GetSimPosition();
		State->Route = {FVector2D(State->Low.x + 0.5, Start.Y), Centre(State->Low)};
		State->RouteIndex = 0;
		State->bPushing = false;
		State->PushYaw = 90.0f;
		State->ClimbsBefore = P->GetClimbsSent();
		B->SetSpeed(1);
	}, 7.0f, 1.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		StairsView();
		if (State->bPausedMid)
		{
			return;
		}
		Drive();
		const colony::ThingView* Me = LevelsSmokeFind(*B, State->Me);
		const double Rise = MeasureClimb();
		if (Me && Rise >= 0.0)
		{
			if (Me->climb > 0.4f && Me->climb < 0.75f && !State->bMidClimb)
			{
				State->bMidClimb = true;
				State->MidRiseCm = Rise;
				State->bPausedMid = true;
				Prot()->GetBody()->SetScriptedInput(0.0, 0.0, true);
				B->SetSpeed(0);
			}
		}
	}, [=, this]()
	{
		if (!State->bMidClimb)
		{
			const colony::ThingView* Me = LevelsSmokeFind(*Bridge(), State->Me);
			Fail(FString::Printf(TEXT("climb-up: the protagonist was never seen halfway up the stairs (route %d of %d, at %.1f, %.1f level %d, %lld climbs asked)"),
				State->RouteIndex, State->Route.Num(), Me ? Me->x - State->Site.x : -1.0f, Me ? Me->y - State->Site.y : -1.0f, Me ? Me->level : -1,
				static_cast<long long>(Prot()->GetClimbsSent() - State->ClimbsBefore)));
			return;
		}
		// Halfway along, the drawn body is about halfway up the 3 m (the climb is flat in the end cells, so a little under).
		if (State->MidRiseCm < 60.0 || State->MidRiseCm > 270.0)
		{
			Fail(FString::Printf(TEXT("climb-up: halfway up the stairs the body is drawn %.0f cm above the ground"), State->MidRiseCm));
		}
		if (Prot()->GetClimbsSent() <= State->ClimbsBefore || !Prot()->GetBody()->IsClimbing())
		{
			Fail(TEXT("climb-up: no climb was asked of the sim, or the body does not know it is climbing"));
		}
	});

	// 6. On to the top: the body stands on the upper floor at the stairs' head, on level 1.
	AddPhase(TEXT("upstairs"), [=, this]()
	{
		State->bPausedMid = false;
		Bridge()->SetSpeed(1);
	}, 4.0f, 1.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const FVector Top = B->SimToWorld(State->High.x + 0.5f, State->High.y + 0.5f, 1);
		const FVector Eye = Top + FVector(-450.0, -650.0, 420.0);
		ViewFrom(Eye, (Top + FVector(0.0, 0.0, 100.0) - Eye).Rotation());
		Prot()->GetBody()->SetScriptedInput(0.0, 0.0, true);
		MeasureClimb();
	}, [=, this]()
	{
		const colony::ThingView* Me = LevelsSmokeFind(*Bridge(), State->Me);
		AProtagonistPawn* Body = Prot()->GetBody();
		if (!Me || Me->level != 1 || colony::CellPos{FMath::FloorToInt32(Me->x), FMath::FloorToInt32(Me->y), Me->level} != State->High)
		{
			Fail(TEXT("upstairs: the protagonist is not at the head of the stairs on level 1"));
		}
		if (Body->GetSimLevel() != 1 || Body->IsClimbing())
		{
			Fail(TEXT("upstairs: the body did not take level 1 when the climb was over"));
		}
		if (State->MaxRiseCm < 250.0)
		{
			Fail(FString::Printf(TEXT("upstairs: the body was drawn at most %.0f cm up on the way"), State->MaxRiseCm));
		}
	});

	// 7. Back down: from the head, pushing towards the stairs climbs down.
	AddPhase(TEXT("climb-down"), [=, this]()
	{
		State->Route = {Centre(State->High)};
		State->RouteIndex = 0;
		State->bPushing = false;
		State->PushYaw = -90.0f;
		State->ClimbsBefore = Prot()->GetClimbsSent();
	}, 6.0f, 1.0f, [=, this]()
	{
		StairsView();
		// Down at the foot: stop pushing, or the body walks on south.
		const colony::ThingView* Me = LevelsSmokeFind(*Bridge(), State->Me);
		if (Me && Me->level == 0 && Me->climb <= 0.0f && Prot()->GetClimbsSent() > State->ClimbsBefore)
		{
			Prot()->GetBody()->SetScriptedInput(0.0, 0.0, true);
			return;
		}
		Drive();
	}, [=, this]()
	{
		const colony::ThingView* Me = LevelsSmokeFind(*Bridge(), State->Me);
		if (!Me || Me->level != 0 || colony::CellPos{FMath::FloorToInt32(Me->x), FMath::FloorToInt32(Me->y), 0} != State->Low)
		{
			Fail(FString::Printf(TEXT("climb-down: the protagonist did not get back to the foot of the stairs (level %d, %lld climbs asked)"), Me ? Me->level : -1,
				static_cast<long long>(Prot()->GetClimbsSent() - State->ClimbsBefore)));
		}
		if (Prot()->GetBody()->GetSimLevel() != 0)
		{
			Fail(TEXT("climb-down: the body is not back on the ground"));
		}
	});

	// 8. Inside the ground-floor room, looking up: direct mode shows every level, and the floor above is the ceiling.
	AddPhase(TEXT("ceiling"), [=, this]()
	{
		Prot()->GetBody()->SetScriptedInput(0.0, 0.0, true);
		Bridge()->SetSpeed(0);
	}, 1.5f, 1.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		const FVector Eye = B->SimToWorld(State->Site.x + 2.5f, State->Site.y + 3.5f, 0) + FVector(0.0, 0.0, 120.0);
		const FVector Target = B->SimToWorld(State->Site.x + 6.0f, State->Site.y + 7.5f, 0) + FVector(0.0, 0.0, 280.0);
		ViewFrom(Eye, (Target - Eye).Rotation());
	}, [=, this]()
	{
		if (!Levels()->IsLevelShown(1) || Buildings()->GetLevelInstances(1, true) < 6)
		{
			Fail(TEXT("ceiling: direct mode does not show level 1"));
		}
		const ASimPawnActor* Up = Pawns()->FindPawn(State->Upstairs);
		if (Up && Up->IsCutAway())
		{
			Fail(TEXT("ceiling: the colonist upstairs is still cut away in direct mode"));
		}
		if (State->FailedCommands > 0)
		{
			Notes.Add(FString::Printf(TEXT("%d commands failed, the first: %s"), State->FailedCommands, *State->FirstFailure));
		}
	});
}
