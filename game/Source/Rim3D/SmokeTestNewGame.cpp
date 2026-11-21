// Smoke test `newgame` (docs/design/new-game.md): the title screen, the scenario page, then a Crash Landed start on a
// random tropical site straight into the people page (the three colonists on their creator bodies), its Looks tab after
// a reroll, and the landing; one screenshot each, at 1080p, UI included.
#include "SmokeTest.h"

#include "Engine/Engine.h"

#include "ColonyHudSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <string>
#include <vector>

namespace
{
	const FVector NewGameSmokeOrigin(-820000.0, 0.0, 0.0);

	struct FNewGameSmokeState
	{
		int32 FramesBefore = 0;
		std::string RerolledName;
	};

	FString NewGameSmokeName(const colony::Sim& Sim, colony::Entity Pawn)
	{
		const colony::Colonist* C = Sim.colonistOf(Pawn);
		return C ? FString(UTF8_TO_TCHAR((C->firstName + " " + C->lastName).c_str())) : FString(TEXT("?"));
	}
}

void USmokeTestSubsystem::BuildNewGamePhases()
{
	const TSharedRef<FNewGameSmokeState> State = MakeShared<FNewGameSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Hud = [this]() { return GetWorld()->GetSubsystem<UColonyHudSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };

	// 1. The title screen over a running Crash Landed map.
	FPhase TitlePhase;
	TitlePhase.Name = TEXT("newgame_title");
	TitlePhase.WarmupSeconds = 4.0f;
	TitlePhase.MeasureSeconds = 0.5f;
	TitlePhase.Setup = [=, this]()
	{
		GEngine->Exec(GetWorld(), TEXT("r.SetRes 1920x1080w"));
		USimSubsystem* B = Bridge();
		if (!B || !Hud() || !Pawns())
		{
			Fail(TEXT("USimSubsystem, UColonyHudSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 7;
		Options.MapSize = 256;
		Options.Scenario = TEXT("CrashLanded");
		B->SetWorldOrigin(NewGameSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(1);
		State->FramesBefore = Hud()->GetStats().MainMenuFrames;
		Hud()->SetMainMenuPageForTest(0);
		Hud()->SetMainMenuOpen(true);
	};
	TitlePhase.Verify = [=, this]()
	{
		if (Hud()->GetStats().MainMenuFrames == State->FramesBefore)
		{
			Fail(TEXT("newgame_title: the title screen was never drawn"));
		}
	};
	Phases.Add(MoveTemp(TitlePhase));

	// 2. The scenario page: Crash Landed with its kit.
	FPhase ScenarioPhase;
	ScenarioPhase.Name = TEXT("newgame_scenario");
	ScenarioPhase.WarmupSeconds = 1.5f;
	ScenarioPhase.MeasureSeconds = 0.5f;
	ScenarioPhase.Setup = [=, this]() { Hud()->SetMainMenuPageForTest(1); };
	Phases.Add(MoveTemp(ScenarioPhase));

	// 3. Land somewhere random: the people page with the three on their creator bodies.
	FPhase PeoplePhase;
	PeoplePhase.Name = TEXT("newgame_people");
	PeoplePhase.WarmupSeconds = 8.0f;
	PeoplePhase.MeasureSeconds = 1.0f;
	PeoplePhase.Setup = [=, this]()
	{
		Hud()->SetNewGameForTest(42, 256);
		if (!Hud()->StartNewColony(false))
		{
			Fail(TEXT("newgame_people: the colony did not start"));
		}
	};
	PeoplePhase.Verify = [=, this]()
	{
		const colony::Sim* Sim = Bridge()->GetSim();
		if (!Sim || Hud()->GetMainMenuPage() != 3 || !Hud()->IsMainMenuOpen())
		{
			Fail(TEXT("newgame_people: the people page is not open"));
			return;
		}
		if (Sim->time().steps != 0)
		{
			Fail(FString::Printf(TEXT("newgame_people: the game ran %lld steps behind the people page"), static_cast<long long>(Sim->time().steps)));
		}
		const std::vector<colony::Entity> People = Sim->startingColonistSlots();
		if (People.size() != 3)
		{
			Fail(FString::Printf(TEXT("newgame_people: %d colonists, not 3"), static_cast<int32>(People.size())));
			return;
		}
		for (const colony::Entity Pawn : People)
		{
			const colony::PawnLooks* Looks = Sim->things().registry().try_get<colony::PawnLooks>(Pawn);
			const ASimPawnActor* Actor = Pawns()->FindPawn(Pawn);
			UE_LOG(LogTemp, Display, TEXT("NEWGAME person %s: creator '%s', MakeHuman %s"), *NewGameSmokeName(*Sim, Pawn),
				Looks ? UTF8_TO_TCHAR(Looks->creator.c_str()) : TEXT("-"), Actor && Actor->GetMakeHumanMesh() ? TEXT("on") : TEXT("off"));
			if (!Looks || Looks->creator.empty())
			{
				Fail(TEXT("newgame_people: a colonist has no creator look"));
			}
			if (!Actor || !Actor->GetMakeHumanMesh())
			{
				Fail(TEXT("newgame_people: a colonist is not drawn on its MakeHuman body"));
			}
		}
		State->RerolledName = Sim->colonistOf(People[2]) ? Sim->colonistOf(People[2])->firstName : std::string();
	};
	Phases.Add(MoveTemp(PeoplePhase));

	// 4. The Looks tab on the first stranger, after a reroll of the second.
	FPhase LooksPhase;
	LooksPhase.Name = TEXT("newgame_looks");
	LooksPhase.WarmupSeconds = 3.0f;
	LooksPhase.MeasureSeconds = 0.5f;
	LooksPhase.Setup = [=, this]()
	{
		Hud()->SetPeopleForTest(1, 1);
		Bridge()->Submit(colony::RerollColonistCommand{2});
	};
	LooksPhase.Verify = [=, this]()
	{
		const colony::Sim* Sim = Bridge()->GetSim();
		const std::vector<colony::Entity> People = Sim ? Sim->startingColonistSlots() : std::vector<colony::Entity>();
		if (People.size() != 3 || Sim->time().steps != 0)
		{
			Fail(TEXT("newgame_looks: the reroll lost a colonist or the game started"));
			return;
		}
		const colony::Colonist* Third = Sim->colonistOf(People[2]);
		UE_LOG(LogTemp, Display, TEXT("NEWGAME reroll: %s -> %s"), UTF8_TO_TCHAR(State->RerolledName.c_str()), *NewGameSmokeName(*Sim, People[2]));
		const colony::PawnLooks* ThirdLooks = Sim->things().registry().try_get<colony::PawnLooks>(People[2]);
		if (!Third || !ThirdLooks || ThirdLooks->creator.empty())
		{
			Fail(TEXT("newgame_looks: the rerolled colonist has no creator look"));
		}
	};
	Phases.Add(MoveTemp(LooksPhase));

	// 5. Start: the game runs, the people keep their looks.
	FPhase LandedPhase;
	LandedPhase.Name = TEXT("newgame_landed");
	LandedPhase.WarmupSeconds = 4.0f;
	LandedPhase.MeasureSeconds = 0.5f;
	LandedPhase.Setup = [=, this]() { Hud()->LandColonyForTest(); };
	LandedPhase.Verify = [=, this]()
	{
		const colony::Sim* Sim = Bridge()->GetSim();
		if (!Sim || Hud()->IsMainMenuOpen() || Sim->time().steps == 0)
		{
			Fail(TEXT("newgame_landed: the game did not start after Start"));
			return;
		}
		for (const colony::Entity Pawn : Sim->startingColonistSlots())
		{
			const colony::PawnLooks* PawnLooks = Sim->things().registry().try_get<colony::PawnLooks>(Pawn);
			if (!PawnLooks || PawnLooks->creator.empty())
			{
				Fail(TEXT("newgame_landed: a colonist lost its creator look"));
			}
		}
	};
	Phases.Add(MoveTemp(LandedPhase));
}
