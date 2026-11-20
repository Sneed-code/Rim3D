// Smoke test `hud` (docs/design/ui-layout.md): the overseer's bottom tab bar opens every tab in turn on a Crashlanded colony,
// then Build with its Structure tiles, the info window, the Outfits, Areas, Medical and Prisoners windows,
// the main menu and Settings > Controls; one screenshot each, in the game, offscreen.
#include "SmokeTest.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"

#include "ColonyHudSubsystem.h"
#include "OrdersSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

namespace
{
	const FVector HudSmokeOrigin(-780000.0, 0.0, 0.0);

	struct FHudSmokeState
	{
		TArray<colony::Entity> Colonists;
		int32 TabFramesBefore = 0;
	};
}

void USmokeTestSubsystem::BuildHudPhases()
{
	const TSharedRef<FHudSmokeState> State = MakeShared<FHudSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Hud = [this]() { return GetWorld()->GetSubsystem<UColonyHudSubsystem>(); };
	const auto Orders = [this]() { return GetWorld()->GetSubsystem<UOrdersSubsystem>(); };
	const auto Panels = [this]() { return GetWorld()->GetSubsystem<UPanelsSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };

	// 1. A new sim with the Crashlanded colonists in the overseer view: the bar draws, nothing open.
	FPhase Setup;
	Setup.Name = TEXT("hud_bar");
	Setup.WarmupSeconds = 4.0f;
	Setup.MeasureSeconds = 1.0f;
	Setup.Setup = [=, this]()
	{
		// Full HD even offscreen, so the pictures show the layout as a real screen does.
		GEngine->Exec(GetWorld(), TEXT("r.SetRes 1920x1080w"));
		USimSubsystem* B = Bridge();
		if (!B || !Hud() || !Orders() || !Panels() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UColonyHudSubsystem, UOrdersSubsystem, UPanelsSubsystem or UOverseerSubsystem did not start"));
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
		B->SetWorldOrigin(HudSmokeOrigin);
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
		if (AOverseerPawn* Pawn = Overseer()->GetOverseerPawn())
		{
			const colony::Sim* Sim = B->GetSim();
			const colony::CellPos Home = Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
			Pawn->ResetView();
			Pawn->Zoom(2.0f);
			Pawn->FocusOn(B->SimToWorld(Home.x + 0.5f, Home.y + 0.5f, 0));
			Pawn->SnapCamera();
		}
		Hud()->CloseTabs();
	};
	Setup.Verify = [=, this]()
	{
		Panels()->GetColonists(State->Colonists);
		if (Hud()->GetStats().BarFrames == 0)
		{
			Fail(TEXT("hud_bar: the bottom bar was never drawn"));
		}
		Notes.Add(FString::Printf(TEXT("hud_bar: %d colonists, bar drawn %d frames"), State->Colonists.Num(), Hud()->GetStats().BarFrames));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. Every tab of the bar, one at a time.
	for (int32 Index = static_cast<int32>(EHudTab::Build); Index < static_cast<int32>(EHudTab::Count); ++Index)
	{
		const EHudTab Tab = static_cast<EHudTab>(Index);
		const FString Label = UColonyHudSubsystem::TabLabel(Tab);
		FPhase Phase;
		Phase.Name = FString::Printf(TEXT("hud_tab_%s"), *Label.ToLower());
		Phase.WarmupSeconds = 1.5f;
		Phase.MeasureSeconds = 0.5f;
		Phase.Setup = [=, this]()
		{
			State->TabFramesBefore = Hud()->GetStats().TabWindowFrames;
			Hud()->OpenTab(Tab);
			if (Tab == EHudTab::Build)
			{
				Orders()->SetOpenCategory(TEXT("Orders"));
			}
		};
		Phase.Verify = [=, this]()
		{
			if (Hud()->GetOpenTab() != Tab)
			{
				Fail(FString::Printf(TEXT("hud_tab_%s: the tab did not stay open"), *Label.ToLower()));
			}
			// Build, Work, Schedule, Quests and World are drawn by their own subsystems; the rest count here.
			const bool bOwn = Tab == EHudTab::Assign || Tab == EHudTab::Animals || Tab == EHudTab::Mechs || Tab == EHudTab::Wildlife ||
				Tab == EHudTab::Research || Tab == EHudTab::Inventory;
			if (bOwn && Hud()->GetStats().TabWindowFrames == State->TabFramesBefore)
			{
				Fail(FString::Printf(TEXT("hud_tab_%s: the tab's window was never drawn"), *Label.ToLower()));
			}
			if (Tab == EHudTab::Build && !Orders()->IsBuildMenuOpen())
			{
				Fail(TEXT("hud_tab_build: the Build menu did not open"));
			}
			Hud()->CloseTabs();
		};
		Phases.Add(MoveTemp(Phase));
	}

	// 3. Build > Structure: the wall tiles and the earthworks tools, with Wall in hand (the strip with material and cost).
	FPhase Structure;
	Structure.Name = TEXT("hud_build_structure");
	Structure.WarmupSeconds = 1.5f;
	Structure.MeasureSeconds = 0.5f;
	Structure.Setup = [=, this]()
	{
		Hud()->OpenTab(EHudTab::Build);
		Orders()->SetOpenCategory(TEXT("Structure"));
		Orders()->SelectBuilding(TEXT("Wall"));
	};
	Structure.Verify = [=, this]()
	{
		FBox2D Unused;
		if (!Orders()->GetButtonRect(TEXT("Level earth"), Unused))
		{
			Fail(TEXT("hud_build_structure: no Level earth tile in Structure"));
		}
		Orders()->CancelTool();
		Hud()->CloseTabs();
	};
	Phases.Add(MoveTemp(Structure));

	// 4. The info window of the first colonist ("i" on the selected panel).
	FPhase Info;
	Info.Name = TEXT("hud_info");
	Info.WarmupSeconds = 1.5f;
	Info.MeasureSeconds = 0.5f;
	Info.Setup = [=, this]()
	{
		if (State->Colonists.Num() == 0)
		{
			Fail(TEXT("hud_info: no colonist to show"));
			return;
		}
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = State->Colonists[0];
		Overseer()->Select(Pick);
		Hud()->OpenInfo(State->Colonists[0]);
	};
	Info.Verify = [=, this]()
	{
		if (Hud()->GetStats().InfoFrames == 0)
		{
			Fail(TEXT("hud_info: the info window was never drawn"));
		}
		Hud()->CloseInfo();
	};
	Phases.Add(MoveTemp(Info));

	// 5. The manager windows from the Assign tab: Outfits, Areas (with one area made and painted), Medical, Prisoners.
	const struct
	{
		const TCHAR* Name;
		EHudScreen Screen;
	} Screens[] = {
		{TEXT("hud_outfits"), EHudScreen::Outfits},
		{TEXT("hud_areas"), EHudScreen::Areas},
		{TEXT("hud_medical"), EHudScreen::Medical},
		{TEXT("hud_prisoners"), EHudScreen::Prisoners},
	};
	for (const auto& Entry : Screens)
	{
		const FString Name = Entry.Name;
		const EHudScreen Screen = Entry.Screen;
		FPhase Phase;
		Phase.Name = Name;
		Phase.WarmupSeconds = 1.5f;
		Phase.MeasureSeconds = 0.5f;
		Phase.Setup = [=, this]()
		{
			State->TabFramesBefore = Hud()->GetStats().ScreenFrames;
			if (Screen == EHudScreen::Areas)
			{
				// One area, painted 12 x 8 round the stockpile, with the Paint tool in hand so its cells show.
				USimSubsystem* B = Bridge();
				const colony::Sim* Sim = B->GetSim();
				const colony::CellPos Home = Sim && Sim->stockpileCell() ? *Sim->stockpileCell() : colony::CellPos{128, 128, 0};
				// A fresh sim's first area is number 1 (docs/design/areas.md); both commands go in the same step.
				B->Submit(colony::CreateAreaCommand{"Home base"});
				const uint32 Area = 1;
				colony::CellRect Rect;
				Rect.minX = Home.x - 6;
				Rect.minY = Home.y - 4;
				Rect.sizeX = 12;
				Rect.sizeY = 8;
				B->Submit(colony::PaintAreaCommand{Area, Rect, true});
				B->StepOnce();
				Orders()->SetAreaTool(Area, true);
			}
			Hud()->OpenScreen(Screen);
		};
		Phase.Verify = [=, this]()
		{
			if (Hud()->GetStats().ScreenFrames == State->TabFramesBefore)
			{
				Fail(FString::Printf(TEXT("%s: the window was never drawn"), *Name));
			}
			if (Screen == EHudScreen::Areas)
			{
				const std::vector<colony::AreaView>& Areas = Bridge()->GetCurrentSnapshot().areas;
				if (Areas.empty() || Areas.back().cellCount != 96)
				{
					Fail(FString::Printf(TEXT("hud_areas: the painted area has %u cells, wanted 96"), Areas.empty() ? 0u : Areas.back().cellCount));
				}
				Orders()->SetAreaTool(0, true);
			}
			Hud()->OpenScreen(EHudScreen::None);
			Hud()->CloseTabs();
		};
		Phases.Add(MoveTemp(Phase));
	}

	// 6. The main menu's New colony page (over the running colony; nothing is started).
	FPhase Menu;
	Menu.Name = TEXT("hud_main_menu");
	Menu.WarmupSeconds = 1.5f;
	Menu.MeasureSeconds = 0.5f;
	Menu.Setup = [=, this]()
	{
		State->TabFramesBefore = Hud()->GetStats().MainMenuFrames;
		Hud()->SetMainMenuOpen(true);
	};
	Menu.Verify = [=, this]()
	{
		if (Hud()->GetStats().MainMenuFrames == State->TabFramesBefore)
		{
			Fail(TEXT("hud_main_menu: the main menu was never drawn"));
		}
		Hud()->SetMainMenuOpen(false);
	};
	Phases.Add(MoveTemp(Menu));

	// 7. Settings, the Controls page with the key rebind list.
	FPhase Settings;
	Settings.Name = TEXT("hud_settings_controls");
	Settings.WarmupSeconds = 1.5f;
	Settings.MeasureSeconds = 0.5f;
	Settings.Setup = [=, this]() { Hud()->OpenSettingsPage(TEXT("Controls")); };
	Settings.Verify = [=, this]()
	{
		if (Hud()->GetStats().SettingsFrames == 0)
		{
			Fail(TEXT("hud_settings_controls: Settings was never drawn"));
		}
		Hud()->SetSettingsOpen(false);
	};
	Phases.Add(MoveTemp(Settings));
}
