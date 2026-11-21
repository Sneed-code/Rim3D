// Smoke test `newgame` (docs/design/new-game.md): the title screen, the scenario page, then a Crash Landed start on a
// random tropical site straight into "You" (the creator's default woman, naked), the same with clothes on and spinning,
// "Your companions" (traits and skills, after random traits), its Looks tab after a reroll, and the landing; one
// screenshot each, at 1080p, UI included.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"

#include "ColonyHudSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "TerrainSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
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

	// 3. Land somewhere random: "You", with the three on their creator bodies.
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
			Fail(TEXT("newgame_people: the You page is not open"));
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
			// Round 2 showed the animating mannequin through the MakeHuman body as a red and blue figure.
			else if (Actor->GetBody() && Actor->GetBody()->IsVisible())
			{
				Fail(TEXT("newgame_people: the hidden mannequin is drawn over a MakeHuman body"));
			}
			else
			{
				UE_LOG(LogTemp, Display, TEXT("NEWGAME person %s drawn as %s"), *NewGameSmokeName(*Sim, Pawn),
					Actor->GetMakeHumanMesh()->GetSkeletalMeshAsset() ? *Actor->GetMakeHumanMesh()->GetSkeletalMeshAsset()->GetName() : TEXT("nothing"));
			}
		}
		// The screen is shown by day, whatever hour the landing is at.
		const USkySubsystem* Sky = GetWorld()->GetSubsystem<USkySubsystem>();
		if (Sky && Sky->GetHourOverride() < 0.0f)
		{
			Fail(TEXT("newgame_people: the sky is not held at day on the character screen"));
		}
		else if (Sky)
		{
			UE_LOG(LogTemp, Display, TEXT("NEWGAME screen sky at %.2f h, exposure %.1f EV"), Sky->GetHourOverride(), Sky->GetExposure());
			if (Sky->GetExposure() > 12.5f)
			{
				Fail(TEXT("newgame_people: the screen's exposure is near the engine's clipping warning (12.5 EV of ours)"));
			}
		}
		const UTerrainSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainSubsystem>();
		if (Terrain && Terrain->GetPlantClearingCount() != static_cast<int32>(People.size()))
		{
			Fail(TEXT("newgame_people: no clearing of plants round each person on show"));
		}
		// You are the creator's own woman, untouched (the gun range's), so hair and body updates show on you.
		const colony::PawnLooks* YourLooks = Sim->things().registry().try_get<colony::PawnLooks>(People[0]);
		const colony::Colonist* You = Sim->colonistOf(People[0]);
		if (!You || !You->female || !YourLooks || YourLooks->creator.rfind("seed=-1;", 0) != 0)
		{
			Fail(TEXT("newgame_people: you are not the creator's default woman"));
		}
		if (You && !You->traits.empty())
		{
			Fail(TEXT("newgame_people: you have traits (who you are is cosmetic for now)"));
		}
		State->RerolledName = Sim->colonistOf(People[2]) ? Sim->colonistOf(People[2])->firstName : std::string();
	};
	Phases.Add(MoveTemp(PeoplePhase));

	// 3b. Clothes on, spinning: the cargo pack's cotton shown on the body (cosmetic), the body turning on the turntable.
	FPhase ClothesPhase;
	ClothesPhase.Name = TEXT("newgame_you_clothes");
	ClothesPhase.WarmupSeconds = 2.5f;
	ClothesPhase.MeasureSeconds = 0.3f;
	ClothesPhase.Setup = [=, this]()
	{
		Hud()->SetPeopleClothesForTest(true);
		Hud()->SetPeopleSpinForTest(240.0f);
	};
	ClothesPhase.Verify = [=, this]()
	{
		const colony::Sim* Sim = Bridge()->GetSim();
		const std::vector<colony::Entity> People = Sim ? Sim->startingColonistSlots() : std::vector<colony::Entity>();
		const ASimPawnActor* Actor = People.empty() ? nullptr : Pawns()->FindPawn(People[0]);
		UE_LOG(LogTemp, Display, TEXT("NEWGAME clothes on: %d pieces, %d cut, %d garment meshes, spin %.0f degrees"), Actor ? Actor->GetApparel().Pieces.Num() : -1,
			Actor ? Actor->GetGarmentCuts().Num() : -1, Actor ? Actor->GetWornMeshes().Num() : -1, Actor ? Actor->GetPreviewSpin() : 0.0f);
		if (!Actor || People.empty() || !Pawns()->HasApparelPreview(People[0]))
		{
			Fail(TEXT("newgame_you_clothes: the clothes-on preview is not set on you"));
		}
		// The cargo pack's cotton shirt and trousers, drawn as MakeHuman garments (#425).
		else if (Actor->GetApparel().Pieces.Num() != 2 || Actor->GetGarmentCuts().Num() != 2 || Actor->GetWornMeshes().IsEmpty())
		{
			Fail(TEXT("newgame_you_clothes: the cotton shirt and trousers are not drawn on you"));
		}
		if (Actor && FMath::Abs(Actor->GetPreviewSpin()) < 1.0f)
		{
			Fail(TEXT("newgame_you_clothes: the turntable did not turn the body"));
		}
		const colony::Gear* Worn = People.empty() ? nullptr : Sim->things().registry().try_get<colony::Gear>(People[0]);
		if (Worn && !Worn->apparel.empty())
		{
			Fail(TEXT("newgame_you_clothes: the preview put real clothes on in the sim"));
		}
		Hud()->SetPeopleClothesForTest(false);
	};
	Phases.Add(MoveTemp(ClothesPhase));

	// 3c. Your companions: random traits for the first stranger, the traits and skills shown.
	FPhase CompanionsPhase;
	CompanionsPhase.Name = TEXT("newgame_companions");
	CompanionsPhase.WarmupSeconds = 2.5f;
	CompanionsPhase.MeasureSeconds = 0.5f;
	CompanionsPhase.Setup = [=, this]()
	{
		Hud()->SetPeopleForTest(1, 0);
		colony::SetColonistTraitsCommand Dice;
		Dice.slot = 1;
		Dice.randomize = true;
		Dice.roll = 3;
		Bridge()->Submit(Dice);
	};
	CompanionsPhase.Verify = [=, this]()
	{
		const colony::Sim* Sim = Bridge()->GetSim();
		const std::vector<colony::Entity> People = Sim ? Sim->startingColonistSlots() : std::vector<colony::Entity>();
		if (Hud()->GetMainMenuPage() != 4 || People.size() != 3 || Sim->time().steps != 0)
		{
			Fail(TEXT("newgame_companions: the companions page is not open, or the game started"));
			return;
		}
		for (size_t Slot = 1; Slot < People.size(); ++Slot)
		{
			const colony::Colonist* C = Sim->colonistOf(People[Slot]);
			const int32 Spent = C ? colony::traitPointsSpent(Sim->defs(), C->traits) : -1;
			UE_LOG(LogTemp, Display, TEXT("NEWGAME companion %s: age %d, %d traits, %d of 10 points"), *NewGameSmokeName(*Sim, People[Slot]), C ? C->age : -1,
				C ? static_cast<int32>(C->traits.size()) : -1, Spent);
			if (!C || C->age != 18 || Spent != 10)
			{
				Fail(TEXT("newgame_companions: a companion is not 18 with all 10 trait points spent"));
			}
		}
	};
	Phases.Add(MoveTemp(CompanionsPhase));

	// 4. The companions' Looks tab on the first stranger, after a reroll of the second.
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
		if (const USkySubsystem* Sky = GetWorld()->GetSubsystem<USkySubsystem>(); Sky && Sky->GetHourOverride() >= 0.0f)
		{
			Fail(TEXT("newgame_landed: the sky is still held at the character screen's hour"));
		}
		if (const UTerrainSubsystem* Terrain = GetWorld()->GetSubsystem<UTerrainSubsystem>(); Terrain && Terrain->GetPlantClearingCount() > 0)
		{
			Fail(TEXT("newgame_landed: the character screen's clearings still hide plants"));
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
