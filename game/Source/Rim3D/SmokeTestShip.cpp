// Smoke test `ship`: the ship window, the Ship button and the ending screen, driven by the calls their buttons make, in the game,
// offscreen.
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "ShipPanelSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	const FVector ShipSmokeOrigin(-830000.0, 0.0, 0.0);

	struct FShipSmokeState
	{
		colony::Entity Casket = colony::kNoEntity;
		colony::Entity Rider = colony::kNoEntity;
	};
}

void USmokeTestSubsystem::BuildShipPhases()
{
	const TSharedRef<FShipSmokeState> State = MakeShared<FShipSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Panels = [this]() { return GetWorld()->GetSubsystem<UPanelsSubsystem>(); };
	const auto Ship = [this]() { return GetWorld()->GetSubsystem<UShipPanelSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto View = [=, this]() -> const colony::ShipView& { return Bridge()->GetCurrentSnapshot().ship; };
	const auto LogHas = [=, this](const TCHAR* Text) -> bool
	{
		for (const FColonyLogLine& Line : Panels()->GetLog())
		{
			if (Line.Text.Contains(Text))
			{
				return true;
			}
		}
		return false;
	};
	const auto Spawn = [=, this](const char* Def, int32 X, int32 Y)
	{
		colony::SpawnThingCommand Command;
		Command.def = Def;
		Command.cell = colony::CellPos{X, Y, 0};
		Bridge()->Submit(std::move(Command));
	};

	// 1. A flat map with the Crashlanded colonists and a whole ship standing; the window lists it and Launch is greyed.
	FPhase Setup;
	Setup.Name = TEXT("ship_setup");
	Setup.WarmupSeconds = 3.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Panels() || !Ship() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UPanelsSubsystem, UShipPanelSubsystem or UOverseerSubsystem did not start"));
			return;
		}
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(0, ECVF_SetByCode);
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 256;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(ShipSmokeOrigin);
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
		B->StepOnce();
		if (Ship()->HasShip())
		{
			Fail(TEXT("ship_setup: the Ship button shows before any part stands"));
		}
		// The ship off to the side of the crash site: reactor, three engines, core, sensor cluster, a beam and a casket against it.
		Spawn("ShipReactor", 150, 100);
		Spawn("ShipEngine", 158, 100);
		Spawn("ShipEngine", 162, 100);
		Spawn("ShipEngine", 166, 100);
		Spawn("ShipComputerCore", 150, 110);
		Spawn("ShipSensorCluster", 154, 110);
		Spawn("ShipStructuralBeam", 170, 100);
		Spawn("ShipCryptosleepCasket", 172, 100);
		B->StepOnce();
		if (AOverseerPawn* Pawn = Overseer()->GetOverseerPawn())
		{
			Pawn->ResetView();
			Pawn->Zoom(2.0f);
			Pawn->FocusOn(B->SimToWorld(162.0f, 104.0f, 0));
			Pawn->SnapCamera();
		}
		Ship()->SetOpen(true);
	};
	Setup.Verify = [=, this]()
	{
		const colony::ShipView& S = View();
		if (S.reactors != 1 || S.engines != 3 || S.cores != 1 || S.sensors != 1 || S.beams != 1 || S.caskets != 1)
		{
			Fail(FString::Printf(TEXT("ship_setup: the ship has reactors %d, engines %d, cores %d, sensors %d, beams %d, caskets %d; wanted 1, 3, 1, 1, 1, 1"),
				S.reactors, S.engines, S.cores, S.sensors, S.beams, S.caskets));
			return;
		}
		if (!Ship()->HasShip())
		{
			Fail(TEXT("ship_setup: no Ship button with the parts standing"));
		}
		const FShipPanelStats& Stats = Ship()->GetStats();
		if (Stats.WindowFrames == 0)
		{
			Fail(TEXT("ship_setup: the ship window was never drawn (ImGui not usable)"));
		}
		if (Stats.CasketRows != 1 || Stats.bLaunchEnabled)
		{
			Fail(FString::Printf(TEXT("ship_setup: the window shows %d caskets and Launch %s; wanted 1 and greyed"), Stats.CasketRows, Stats.bLaunchEnabled ? TEXT("enabled") : TEXT("greyed")));
		}
		if (Ship()->Launch())
		{
			Fail(TEXT("ship_setup: Launch sent a command before the reactor ran"));
		}
		Notes.Add(FString::Printf(TEXT("ship_setup: launch problem '%s'"), UTF8_TO_TCHAR(S.problem.c_str())));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. Start the reactor and put a colonist in the casket.
	FPhase Reactor;
	Reactor.Name = TEXT("ship_reactor");
	Reactor.WarmupSeconds = 1.0f;
	Reactor.MeasureSeconds = 0.5f;
	Reactor.Setup = [=, this]()
	{
		UShipPanelSubsystem* S = Ship();
		if (!S->StartReactor())
		{
			Fail(TEXT("ship_reactor: Start reactor sent nothing"));
			return;
		}
		if (S->StartReactor())
		{
			Fail(TEXT("ship_reactor: Start reactor sent a second command while the reactor runs"));
		}
		TArray<colony::Entity> Caskets;
		S->GetCaskets(Caskets);
		TArray<colony::Entity> Colonists;
		Panels()->GetColonists(Colonists);
		if (Caskets.Num() != 1 || Colonists.IsEmpty())
		{
			Fail(FString::Printf(TEXT("ship_reactor: %d caskets and %d colonists"), Caskets.Num(), Colonists.Num()));
			return;
		}
		State->Casket = Caskets[0];
		State->Rider = Colonists.Last();
		if (!S->Board(State->Casket, State->Rider))
		{
			Fail(TEXT("ship_reactor: Board sent nothing"));
		}
	};
	Reactor.Verify = [=, this]()
	{
		const colony::ShipView& S = View();
		if (S.phase != 1 || S.activationLeft <= 0)
		{
			Fail(FString::Printf(TEXT("ship_reactor: phase %d with %lld s left, wanted 1 (warming up)"), S.phase, static_cast<long long>(S.activationLeft)));
		}
		if (Ship()->RiderOf(State->Casket) != State->Rider)
		{
			Fail(TEXT("ship_reactor: the colonist is not in the casket"));
		}
		if (!LogHas(TEXT("ship reactor is starting")))
		{
			Fail(TEXT("ship_reactor: the log does not say the reactor started"));
		}
		Notes.Add(FString::Printf(TEXT("ship_reactor: %.1f days to go, %d riders"), static_cast<double>(S.activationLeft) / colony::kSecondsPerDay, S.riders));
	};
	Phases.Add(MoveTemp(Reactor));

	// 3. The dev skip ends the warm-up: the Launch button wakes up.
	FPhase Ready;
	Ready.Name = TEXT("ship_ready");
	Ready.WarmupSeconds = 1.0f;
	Ready.MeasureSeconds = 0.5f;
	Ready.Setup = [=, this]()
	{
		colony::ShipCommand Command;
		Command.action = "FinishWarmUp";
		Bridge()->Submit(std::move(Command));
		Bridge()->StepOnce();
		Bridge()->StepOnce();
	};
	Ready.Verify = [=, this]()
	{
		const colony::ShipView& S = View();
		if (S.phase != 2 || !S.canLaunch)
		{
			Fail(FString::Printf(TEXT("ship_ready: phase %d, can launch %d ('%s'); wanted 2 and yes"), S.phase, S.canLaunch ? 1 : 0, UTF8_TO_TCHAR(S.problem.c_str())));
		}
		if (!Ship()->GetStats().bLaunchEnabled)
		{
			Fail(TEXT("ship_ready: the Launch button is still greyed"));
		}
		if (!LogHas(TEXT("ship reactor is active")))
		{
			Fail(TEXT("ship_ready: the log does not say the reactor is active"));
		}
	};
	Phases.Add(MoveTemp(Ready));

	// 4. Launch: the ending screen names who left, and Keep playing returns to the colony.
	FPhase Launch;
	Launch.Name = TEXT("ship_launch");
	Launch.WarmupSeconds = 1.5f;
	Launch.MeasureSeconds = 0.5f;
	Launch.Setup = [=, this]()
	{
		if (!Ship()->Launch())
		{
			Fail(TEXT("ship_launch: Launch sent nothing"));
		}
	};
	Launch.Verify = [=, this]()
	{
		UShipPanelSubsystem* S = Ship();
		if (!View().won || View().escaped != 1)
		{
			Fail(FString::Printf(TEXT("ship_launch: won %d with %d escaped, wanted won with 1"), View().won ? 1 : 0, View().escaped));
		}
		if (!S->IsEndingShown() || S->GetStats().EndingFrames == 0)
		{
			Fail(TEXT("ship_launch: the ending screen was not drawn"));
		}
		if (S->GetEscapedNames().Num() != 1)
		{
			Fail(FString::Printf(TEXT("ship_launch: the ending names %d riders, wanted 1"), S->GetEscapedNames().Num()));
		}
		if (!LogHas(TEXT("ship has launched")))
		{
			Fail(TEXT("ship_launch: the log does not say the ship launched"));
		}
		Notes.Add(FString::Printf(TEXT("ship_launch: %s left; %d commands sent by the ship panel"),
			S->GetEscapedNames().Num() > 0 ? *S->GetEscapedNames()[0] : TEXT("no one"), S->GetStats().CommandsSent));
		S->DismissEnding();
		if (S->IsEndingShown())
		{
			Fail(TEXT("ship_launch: Keep playing did not close the ending"));
		}
	};
	Phases.Add(MoveTemp(Launch));
}
