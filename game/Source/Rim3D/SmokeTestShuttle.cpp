// Smoke test `shuttle` (docs/design/shuttle-draw.md): the landing zone tool and overlay, and a visitors' shuttle flying in,
// swinging its engines down, coming straight down on its jets with flames, dust and leaning grass, gear down, and the door
// opening once it is down. The climb out is checked on the pose maths (a visit's stay is too long to wait for).
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"

#include "OrdersSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "ShuttleSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Trade.h"

namespace
{
	const FVector ShuttleSmokeOrigin(-820000.0, 0.0, 0.0);

	struct FShuttleSmokeState
	{
		colony::CellPos ZoneCorner{0, 0, 0};
		uint32 Id = 0;
		float MaxUp = 0.0f;
		float MaxNacelleSwing = 0.0f;
		int32 MaxFlames = 0;
		int32 MaxDust = 0;
		float MaxWash = 0.0f;
	};
}

void USmokeTestSubsystem::BuildShuttlePhases()
{
	const TSharedRef<FShuttleSmokeState> State = MakeShared<FShuttleSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Shuttle = [this]() { return GetWorld()->GetSubsystem<UShuttleSubsystem>(); };
	const auto Orders = [this]() { return GetWorld()->GetSubsystem<UOrdersSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	// At the shuttle's landing spot once there is one, else the zone's middle.
	const auto Aim = [=, this](float Zoom)
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (Pawn)
		{
			const colony::Snapshot& Snapshot = Bridge()->GetCurrentSnapshot();
			const float X = Snapshot.shuttles.empty() ? State->ZoneCorner.x + 5.0f : Snapshot.shuttles.front().x;
			const float Y = Snapshot.shuttles.empty() ? State->ZoneCorner.y + 8.0f : Snapshot.shuttles.front().y;
			Pawn->ResetView();
			Pawn->Zoom(Zoom);
			Pawn->FocusOn(Bridge()->SimToWorld(X, Y, 0));
			Pawn->SnapCamera();
		}
	};
	// Every frame: the shuttle's highest point, the furthest its engines swung, the most flames, dust and grass push seen.
	const auto Watch = [=, this]()
	{
		UShuttleSubsystem* S = Shuttle();
		const colony::Snapshot& Snapshot = Bridge()->GetCurrentSnapshot();
		if (!S || Snapshot.shuttles.empty())
		{
			return;
		}
		State->Id = Snapshot.shuttles.front().id;
		FShuttlePose Pose;
		if (S->GetPose(State->Id, Pose))
		{
			State->MaxUp = FMath::Max(State->MaxUp, Pose.UpM);
			State->MaxNacelleSwing = FMath::Max(State->MaxNacelleSwing, Pose.Nacelles);
		}
		State->MaxFlames = FMath::Max(State->MaxFlames, S->GetFlamesDrawn());
		State->MaxDust = FMath::Max(State->MaxDust, S->GetDustDrawn());
		State->MaxWash = FMath::Max(State->MaxWash, S->GetWashStrength());
	};

	// 1. A new sim, a landing zone painted with the tool, and visitors sent by shuttle.
	FPhase Setup;
	Setup.Name = TEXT("shuttle_zone");
	Setup.WarmupSeconds = 2.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Shuttle() || !Orders() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UShuttleSubsystem, UOrdersSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(0, ECVF_SetByCode);
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.bFlatMap = true;  // a zone on a hillside has no spot level enough
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(ShuttleSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		// Noon, so the shots show something.
		if (IConsoleVariable* Hour = IConsoleManager::Get().FindConsoleVariable(TEXT("Sky.Hour")))
		{
			Hour->Set(12.0f, ECVF_SetByCode);
		}
		if (!Overseer()->EnterOverseerMode())
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		const colony::Sim* Sim = B->GetSim();
		const colony::CellPos Home = Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
		State->ZoneCorner = colony::CellPos{Home.x + 14, Home.y - 8, 0};
		// Trees and bushes off the zone (a tree in it rules the zone out); the grass round it stays for the downwash.
		if (colony::Sim* Mutable = const_cast<colony::Sim*>(Sim))
		{
			for (int32 Y = State->ZoneCorner.y - 2; Y <= State->ZoneCorner.y + 17; ++Y)
			{
				for (int32 X = State->ZoneCorner.x - 2; X <= State->ZoneCorner.x + 11; ++X)
				{
					const colony::CellPos C{X, Y, 0};
					if (Mutable->map().inBounds(C) && Mutable->plants().has(C) && Mutable->defs().get(Mutable->plants().defAt(C)).defName() != "Plant_Grass")
					{
						Mutable->plants().remove(C);
					}
				}
			}
		}
		Aim(3.0f);
		// The zone with the tool, as the player paints it: a 10 x 16 rectangle (a shuttle needs 6 x 12).
		UOrdersSubsystem* O = Orders();
		const colony::CellPos To{State->ZoneCorner.x + 9, State->ZoneCorner.y + 15, 0};
		O->SetMouseDriven(false);
		O->SetTool(EOrderTool::LandingZone);
		O->BeginDrag(State->ZoneCorner);
		O->SetHoverCell(To);
		if (O->EndDrag(To) != 1)
		{
			Fail(TEXT("shuttle_zone: the landing zone drag sent no command"));
		}
		O->SetTool(EOrderTool::None);
		O->SetMouseDriven(true);
		B->StepOnce();
	};
	Setup.Verify = [=, this]()
	{
		const int32 Cells = static_cast<int32>(Bridge()->GetCurrentSnapshot().landingZone.size());
		if (Cells < 100)
		{
			Fail(FString::Printf(TEXT("shuttle_zone: the sim has %d landing zone cells, wanted 160 (a few may be rock or water)"), Cells));
		}
		Orders()->RefreshOverlay();
		if (Orders()->GetStats().LandingCells != Cells)
		{
			Fail(FString::Printf(TEXT("shuttle_zone: %d landing zone tiles drawn for %d cells"), Orders()->GetStats().LandingCells, Cells));
		}
		Notes.Add(FString::Printf(TEXT("shuttle_zone: %d cells painted and drawn"), Cells));
		Bridge()->Submit(colony::StartVisitCommand{"FrayCompact", "Visitors", "", colony::kShuttleEdge});
		Bridge()->SetSpeed(1);
	};
	Phases.Add(MoveTemp(Setup));

	// 2. Coming in: high, engines swinging down, flames on.
	FPhase Approach;
	Approach.Name = TEXT("shuttle_approach");
	Approach.WarmupSeconds = 2.5f;
	Approach.MeasureSeconds = 2.0f;
	Approach.Setup = [=]() { Aim(6.0f); };  // StartPhase always calls Setup
	Approach.OnFrame = [=]() { Watch(); Aim(6.0f); };
	Approach.Verify = [=, this]()
	{
		if (Bridge()->GetCurrentSnapshot().shuttles.empty() || Shuttle()->GetShuttlesDrawn() == 0)
		{
			Fail(TEXT("shuttle_approach: no shuttle came (the visit did not start or found nowhere to land)"));
			return;
		}
		if (State->MaxUp < 15.0f || State->MaxFlames == 0)
		{
			Fail(FString::Printf(TEXT("shuttle_approach: highest %.1f m, %d flames; wanted over 15 m and flames"), State->MaxUp, State->MaxFlames));
		}
		Notes.Add(FString::Printf(TEXT("shuttle_approach: art %s, up to %.1f m, %d flames"), Shuttle()->HasArt() ? TEXT("yes") : TEXT("no (box parts)"), State->MaxUp, State->MaxFlames));
	};
	Phases.Add(MoveTemp(Approach));

	// 3. Low over the zone: dust and grass push.
	FPhase Low;
	Low.Name = TEXT("shuttle_low");
	Low.WarmupSeconds = 4.0f;
	Low.MeasureSeconds = 2.0f;
	Low.Setup = [=, this]() { Aim(3.0f); };
	Low.OnFrame = Watch;
	Low.Verify = [=, this]()
	{
		if (State->MaxNacelleSwing < 0.9f)
		{
			Fail(FString::Printf(TEXT("shuttle_low: the rear engines swung only %.2f of the way down"), State->MaxNacelleSwing));
		}
		if (State->MaxDust == 0 || State->MaxWash <= 0.0f)
		{
			Fail(FString::Printf(TEXT("shuttle_low: %d dust puffs, grass push %.2f; wanted both"), State->MaxDust, State->MaxWash));
		}
		Notes.Add(FString::Printf(TEXT("shuttle_low: up to %d dust puffs, grass push %.2f"), State->MaxDust, State->MaxWash));
	};
	Phases.Add(MoveTemp(Low));

	// 4. Down: on the ground, gear down, jets off, door open, roof hidden in the overseer view.
	FPhase Down;
	Down.Name = TEXT("shuttle_down");
	Down.WarmupSeconds = 6.0f;
	Down.MeasureSeconds = 1.0f;
	Down.Setup = [=, this]() { Aim(2.0f); };
	Down.Verify = [=, this]()
	{
		const colony::Snapshot& Snapshot = Bridge()->GetCurrentSnapshot();
		FShuttlePose Pose;
		FVector At;
		if (Snapshot.shuttles.empty() || !Shuttle()->GetPose(State->Id, Pose) || !Shuttle()->GetLocation(State->Id, At))
		{
			Fail(TEXT("shuttle_down: the shuttle is gone"));
			return;
		}
		const colony::ShuttleView& View = Snapshot.shuttles.front();
		const double GroundCm = Bridge()->GetWorldOrigin().Z + View.ground * 100.0;
		if (View.phase != 1 || Pose.UpM > 0.01f || FMath::Abs(At.Z - GroundCm) > 5.0 || Pose.Gear < 0.99f || Pose.Door < 0.99f || Pose.Lift > 0.01f)
		{
			Fail(FString::Printf(TEXT("shuttle_down: phase %d, up %.2f m, %.0f cm off the ground, gear %.2f, door %.2f, lift %.2f"),
				static_cast<int32>(View.phase), Pose.UpM, At.Z - GroundCm, Pose.Gear, Pose.Door, Pose.Lift));
		}
		// The climb out, on the pose maths: halfway it is in a hover with the gear in; at the end far off and high, engines back.
		const FShuttlePose Mid = Shuttle()->PoseAt(2, 0.5f, 0.0f, 0.0f);
		const FShuttlePose End = Shuttle()->PoseAt(2, 1.0f, 0.0f, 0.0f);
		if (Mid.UpM < 15.0f || Mid.Gear > 0.01f || Mid.Door > 0.01f || End.BackM > -100.0f || End.UpM < 30.0f || End.Nacelles > 0.01f)
		{
			Fail(FString::Printf(TEXT("shuttle_down: climb pose wrong (mid up %.1f gear %.2f door %.2f; end back %.0f up %.1f nacelles %.2f)"),
				Mid.UpM, Mid.Gear, Mid.Door, End.BackM, End.UpM, End.Nacelles));
		}
		const bool bInZone = View.x > State->ZoneCorner.x && View.x < State->ZoneCorner.x + 10 && View.y > State->ZoneCorner.y && View.y < State->ZoneCorner.y + 16;
		if (!bInZone)
		{
			Fail(FString::Printf(TEXT("shuttle_down: landed at (%.1f, %.1f), outside the landing zone"), View.x, View.y));
		}
		Notes.Add(FString::Printf(TEXT("shuttle_down: landed at (%.1f, %.1f) facing %d (zone from %d, %d), door open"), View.x, View.y, static_cast<int32>(View.rot), State->ZoneCorner.x, State->ZoneCorner.y));
		if (IConsoleVariable* Hour = IConsoleManager::Get().FindConsoleVariable(TEXT("Sky.Hour")))
		{
			Hour->Set(-1.0f, ECVF_SetByCode);
		}
	};
	Phases.Add(MoveTemp(Down));
}
