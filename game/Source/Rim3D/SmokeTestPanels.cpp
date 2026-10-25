// Smoke test `panels` (task U8): the work and schedule tabs, the colonist and bills panels and the message log, driven by the calls their buttons make, in the game, offscreen.
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <string>

namespace
{
	const FVector PanelsSmokeOrigin(-760000.0, 0.0, 0.0);

	struct FPanelsSmokeState
	{
		TArray<colony::Entity> Colonists;
		colony::Entity Stove = colony::kNoEntity;
		int32 WorkTried = 0;
	};
}

void USmokeTestSubsystem::BuildPanelsPhases()
{
	const TSharedRef<FPanelsSmokeState> State = MakeShared<FPanelsSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Panels = [this]() { return GetWorld()->GetSubsystem<UPanelsSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto Select = [=, this](colony::Entity Thing)
	{
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = Thing;
		Overseer()->Select(Pick);
	};
	const auto Home = [=, this]() -> colony::CellPos
	{
		const colony::Sim* Sim = SimOf();
		return Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
	};
	const auto ViewOf = [=, this](colony::Entity Thing) -> const colony::ThingView* { return Bridge()->GetCurrentSnapshot().find(Thing); };
	const auto WorkPriorityOf = [=, this](colony::Entity Pawn, int32 Index) -> int32
	{
		const colony::WorkSettings* Settings = SimOf()->things().registry().try_get<colony::WorkSettings>(Pawn);
		return Settings && Index < static_cast<int32>(Settings->entries.size()) ? Settings->entries[Index].priority : -1;
	};
	const auto Aim = [=, this]()
	{
		AOverseerPawn* Pawn = Overseer() ? Overseer()->GetOverseerPawn() : nullptr;
		if (Pawn)
		{
			Pawn->ResetView();
			Pawn->Zoom(2.0f);
			Pawn->FocusOn(Bridge()->SimToWorld(Home().x + 0.5f, Home().y + 0.5f, 0));
			Pawn->SnapCamera();
		}
	};

	// 1. A new sim with the Crashlanded colonists, the overseer view, and the panels know them.
	FPhase Setup;
	Setup.Name = TEXT("panels_setup");
	Setup.WarmupSeconds = 3.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Panels() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UPanelsSubsystem or UOverseerSubsystem did not start"));
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
		B->SetWorldOrigin(PanelsSmokeOrigin);
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
		Aim();
		Panels()->GetColonists(State->Colonists);
		if (State->Colonists.Num() > 0)
		{
			Select(State->Colonists[0]);
		}
	};
	Setup.Verify = [=, this]()
	{
		Panels()->GetColonists(State->Colonists);
		if (State->Colonists.Num() < 3)
		{
			Fail(FString::Printf(TEXT("panels_setup: the panels list %d colonists, wanted at least 3"), State->Colonists.Num()));
		}
		if (Panels()->GetStats().FramesDrawn == 0)
		{
			Fail(TEXT("panels_setup: the panels were never drawn (ImGui not usable)"));
		}
		Notes.Add(FString::Printf(TEXT("panels_setup: %d colonists"), State->Colonists.Num()));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. The work tab: clicks go forward and back through the priorities and the sim follows; work a colonist can never do refuses.
	FPhase Work;
	Work.Name = TEXT("panels_work");
	Work.WarmupSeconds = 1.5f;
	Work.MeasureSeconds = 1.0f;
	Work.Setup = [=, this]()
	{
		UPanelsSubsystem* P = Panels();
		const colony::Sim* Sim = SimOf();
		if (!P || !Sim || State->Colonists.IsEmpty())
		{
			return;
		}
		P->SetTab(EColonyTab::Work);
		const auto& WorkTypes = Sim->jobs().workTypes();
		int32 Mining = -1;
		for (int32 Index = 0; Index < static_cast<int32>(WorkTypes.size()); ++Index)
		{
			if (Sim->defs().get(WorkTypes[Index].def).defName() == "Mining")
			{
				Mining = Index;
			}
		}
		if (Mining < 0)
		{
			Fail(TEXT("panels_work: no Mining work type"));
			return;
		}
		// A colonist who can do mining: forward from 3 goes to 4, then 0 (off), then 1; back undoes it.
		for (const colony::Entity Pawn : State->Colonists)
		{
			const colony::WorkSettings* Settings = Sim->things().registry().try_get<colony::WorkSettings>(Pawn);
			if (!Settings || Settings->entries[Mining].disabled)
			{
				continue;
			}
			const int32 Before = WorkPriorityOf(Pawn, Mining);
			P->CyclePriority(Pawn, Mining, true);
			const int32 After = WorkPriorityOf(Pawn, Mining);
			if (After != (Before + 1) % 5)
			{
				Fail(FString::Printf(TEXT("panels_work: a click moved mining from %d to %d, wanted %d"), Before, After, (Before + 1) % 5));
			}
			P->CyclePriority(Pawn, Mining, false);
			if (WorkPriorityOf(Pawn, Mining) != Before)
			{
				Fail(TEXT("panels_work: a right click did not undo the click"));
			}
			// Leave it visibly different from the others for the picture: first priority.
			while (WorkPriorityOf(Pawn, Mining) != 1)
			{
				if (++State->WorkTried > 10)
				{
					Fail(TEXT("panels_work: clicks never reached priority 1"));
					break;
				}
				P->CyclePriority(Pawn, Mining, true);
			}
			break;
		}
		// Work no one can do is refused (when a backstory has disabled some).
		for (const colony::Entity Pawn : State->Colonists)
		{
			const colony::WorkSettings* Settings = Sim->things().registry().try_get<colony::WorkSettings>(Pawn);
			for (int32 Index = 0; Settings && Index < static_cast<int32>(Settings->entries.size()); ++Index)
			{
				if (Settings->entries[Index].disabled)
				{
					const int32 Commands = P->GetStats().CommandsSent;
					if (P->CyclePriority(Pawn, Index, true) || P->GetStats().CommandsSent != Commands)
					{
						Fail(TEXT("panels_work: a click on work the colonist can never do sent a command"));
					}
					return;
				}
			}
		}
	};
	Work.Verify = [=, this]()
	{
		if (Panels()->GetStats().WorkTabFrames == 0)
		{
			Fail(TEXT("panels_work: the work tab was never drawn"));
		}
	};
	Phases.Add(MoveTemp(Work));

	// 3. The schedule tab.
	FPhase Schedule;
	Schedule.Name = TEXT("panels_schedule");
	Schedule.WarmupSeconds = 1.5f;
	Schedule.MeasureSeconds = 1.0f;
	Schedule.Setup = [=, this]()
	{
		UPanelsSubsystem* P = Panels();
		const colony::Sim* Sim = SimOf();
		if (!P || !Sim || State->Colonists.IsEmpty())
		{
			return;
		}
		P->SetTab(EColonyTab::Schedule);
		const colony::Entity Pawn = State->Colonists[0];
		const colony::Schedule* Hours = Sim->things().registry().try_get<colony::Schedule>(Pawn);
		if (!Hours)
		{
			Fail(TEXT("panels_schedule: the colonist has no schedule"));
			return;
		}
		const colony::ScheduleKind Before = Hours->hours[10];
		P->CycleSchedule(Pawn, 10, true);
		const colony::ScheduleKind After = Sim->things().registry().get<colony::Schedule>(Pawn).hours[10];
		if (static_cast<int32>(After) != (static_cast<int32>(Before) + 1) % 4)
		{
			Fail(FString::Printf(TEXT("panels_schedule: a click moved hour 10 from %d to %d"), static_cast<int32>(Before), static_cast<int32>(After)));
		}
		P->CycleSchedule(Pawn, 10, false);
		if (Sim->things().registry().get<colony::Schedule>(Pawn).hours[10] != Before)
		{
			Fail(TEXT("panels_schedule: a right click did not undo the click"));
		}
		// Joy in the evening for the picture.
		for (int32 Hour = 18; Hour < 21; ++Hour)
		{
			while (Sim->things().registry().get<colony::Schedule>(Pawn).hours[Hour] != colony::ScheduleKind::Joy)
			{
				P->CycleSchedule(Pawn, Hour, true);
			}
		}
		if (P->CycleSchedule(Pawn, 24, true))
		{
			Fail(TEXT("panels_schedule: hour 24 was accepted"));
		}
	};
	Schedule.Verify = [=, this]()
	{
		if (Panels()->GetStats().ScheduleTabFrames == 0)
		{
			Fail(TEXT("panels_schedule: the schedule tab was never drawn"));
		}
	};
	Phases.Add(MoveTemp(Schedule));

	// 4. The colonist panel: needs, mood, skills, health; a wound shows up in the panel and the log.
	FPhase Colonist;
	Colonist.Name = TEXT("panels_colonist");
	Colonist.WarmupSeconds = 1.5f;
	Colonist.MeasureSeconds = 1.0f;
	Colonist.Setup = [=, this]()
	{
		UPanelsSubsystem* P = Panels();
		if (!P || State->Colonists.IsEmpty())
		{
			return;
		}
		P->SetTab(EColonyTab::None);
		Select(State->Colonists[0]);
		Bridge()->Submit(colony::DamagePawnCommand{State->Colonists[0], "Cut", 6.0f, -1});
		Bridge()->StepOnce();
	};
	Colonist.Verify = [=, this]()
	{
		UPanelsSubsystem* P = Panels();
		if (P->GetStats().ColonistPanelFrames == 0)
		{
			Fail(TEXT("panels_colonist: the colonist panel was never drawn"));
		}
		bool bHurt = false;
		for (const FColonyLogLine& Line : P->GetLog())
		{
			bHurt = bHurt || Line.Text.Contains(TEXT("hurt"));
		}
		if (!bHurt)
		{
			Fail(TEXT("panels_colonist: a wound did not reach the message log"));
		}
		colony::HealthReport Report;
		if (!SimOf()->healthReport(State->Colonists[0], Report) || Report.injuries.empty())
		{
			Fail(TEXT("panels_colonist: the colonist has no injury to show"));
		}
	};
	Phases.Add(MoveTemp(Colonist));

	// 5. The bills panel: a stove next to the colonists; add, pause and remove bills.
	FPhase Bills;
	Bills.Name = TEXT("panels_bills");
	Bills.WarmupSeconds = 1.5f;
	Bills.MeasureSeconds = 1.0f;
	Bills.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UPanelsSubsystem* P = Panels();
		if (!B || !P)
		{
			return;
		}
		// A finished stove on the first open cell near the stockpile.
		for (int32 R = 2; R < 14 && State->Stove == colony::kNoEntity; ++R)
		{
			const colony::CellPos Cell{Home().x + R, Home().y + R, 0};
			B->Submit(colony::SpawnThingCommand{"FueledStove", Cell, 1, colony::Rot4::North});
			B->StepOnce();
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built && SimOf()->defs().get(View.def).defName() == "FueledStove")
				{
					State->Stove = View.id;
				}
			}
		}
		if (State->Stove == colony::kNoEntity)
		{
			Fail(TEXT("panels_bills: no stove could be placed"));
			return;
		}
		Select(State->Stove);
		P->AddBill(State->Stove, TEXT("CookMealSimple"), colony::BillMode::Count, 3);
		P->AddBill(State->Stove, TEXT("CookMealFine"), colony::BillMode::UntilHave, 10);
		P->AddBill(State->Stove, TEXT("CookMealSimpleBulk"), colony::BillMode::Forever, 0);
		const colony::ThingView* View = ViewOf(State->Stove);
		if (!View || View->billCount != 3)
		{
			Fail(FString::Printf(TEXT("panels_bills: %u bills after adding three"), View ? View->billCount : 0));
			return;
		}
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		const colony::BillView First = Snapshot.bills[View->firstBill];
		const colony::BillView Second = Snapshot.bills[View->firstBill + 1];
		const colony::BillView Third = Snapshot.bills[View->firstBill + 2];
		if (First.mode != colony::BillMode::Count || First.count != 3 || Second.mode != colony::BillMode::UntilHave || Second.count != 10 || Third.mode != colony::BillMode::Forever)
		{
			Fail(TEXT("panels_bills: the bills' modes or counts are not what was added"));
		}
		// Pause the first, make the second "do 5 times", take the third off, and add another for the picture.
		P->ChangeBill(State->Stove, First.id, First.mode, First.count, true);
		P->ChangeBill(State->Stove, Second.id, colony::BillMode::Count, 5, false);
		P->RemoveBill(State->Stove, Third.id);
		View = ViewOf(State->Stove);
		if (!View || View->billCount != 2)
		{
			Fail(FString::Printf(TEXT("panels_bills: %u bills after taking one off, wanted 2"), View ? View->billCount : 0));
			return;
		}
		const colony::Snapshot& After = B->GetCurrentSnapshot();
		if (!After.bills[View->firstBill].suspended || After.bills[View->firstBill + 1].mode != colony::BillMode::Count || After.bills[View->firstBill + 1].count != 5)
		{
			Fail(TEXT("panels_bills: pause or change did not reach the sim"));
		}
		P->AddBill(State->Stove, TEXT("CookMealSimpleBulk"), colony::BillMode::Forever, 0);
	};
	Bills.Verify = [=, this]()
	{
		if (Panels()->GetStats().BillPanelFrames == 0)
		{
			Fail(TEXT("panels_bills: the bills panel was never drawn"));
		}
	};
	Phases.Add(MoveTemp(Bills));

	// 6. The message log: a refused order shows up, and the open log is drawn.
	FPhase LogPhase;
	LogPhase.Name = TEXT("panels_log");
	LogPhase.WarmupSeconds = 1.5f;
	LogPhase.MeasureSeconds = 1.0f;
	LogPhase.Setup = [=, this]()
	{
		UPanelsSubsystem* P = Panels();
		if (!P || State->Colonists.IsEmpty())
		{
			return;
		}
		P->SetLogOpen(true);
		Select(State->Colonists[0]);
		// A bill on a colonist cannot be added: the sim refuses and the log says so.
		P->AddBill(State->Colonists[0], TEXT("CookMealSimple"), colony::BillMode::Count, 1);
	};
	LogPhase.Verify = [=, this]()
	{
		UPanelsSubsystem* P = Panels();
		bool bRefused = false;
		for (const FColonyLogLine& Line : P->GetLog())
		{
			bRefused = bRefused || Line.Text.StartsWith(TEXT("Refused"));
		}
		if (!bRefused)
		{
			Fail(TEXT("panels_log: a refused order did not reach the message log"));
		}
		if (P->GetStats().LogPanelFrames == 0)
		{
			Fail(TEXT("panels_log: the message log was never drawn"));
		}
		Notes.Add(FString::Printf(TEXT("panels_log: %d lines, %d commands sent by the panels"), P->GetLog().Num(), P->GetStats().CommandsSent));
	};
	Phases.Add(MoveTemp(LogPhase));
}
