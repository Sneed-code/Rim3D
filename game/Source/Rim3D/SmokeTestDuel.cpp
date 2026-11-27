// The duel (title > Debug > Sword and shield duel, docs/design/souls-combat.md): you, with a gladius and a medium
// shield, against a hostile pawn with the same, on a flat cleared map. Elden Ring style melee: stamina, light and heavy
// attacks, combos, guard, staggers and knockdowns. A practice fight: you are healed before you would go down, and a new
// opponent walks in a few seconds after the last one falls.
//
// -DuelWatch: a check instead of play. Both sides fight on their own (the AI's souls turns), seen from the side for six
// screenshots, then you take over for a last one with the health and stamina bars; it fails if nobody swung.

#include "SmokeTest.h"

#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
	const FVector DuelSmokeOrigin(-1700000.0, 0.0, 0.0);
	// The player is healed when its worst body part falls under this share (a practice fight: no YOU DIED).
	constexpr float DuelSmokePracticeHealth = 0.35f;
	// Seconds after the opponent falls before the next one comes.
	constexpr float DuelSmokeRematchSeconds = 4.0f;
	constexpr int DuelSmokeSkill = 10;

	struct FDuelSmokeState
	{
		colony::Entity Me = colony::kNoEntity;
		colony::Entity Foe = colony::kNoEntity;
		colony::CellPos Middle{48, 48, 0};
		/** Gladius items spawned for a fighter, waiting to be equipped. */
		bool bMeArmed = false;
		bool bFoeArmed = false;
		bool bControlled = false;
		float FoeDownSince = -1.0f;
		float Clock = 0.0f;
		float LastHeal = 0.0f;
		int32 Rematches = 0;
		// Seen in the snapshot: attacks begun, heavies, staggers (stunned edges).
		uint64 SwingsAtStart = 0;
		int32 Heavies = 0;
		int32 Staggers = 0;
		uint8 LastKind[2] = {255, 255};
		bool LastStunned[2] = {false, false};
	};

	const colony::ThingView* DuelSmokeFind(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		return Thing == colony::kNoEntity ? nullptr : Bridge.GetCurrentSnapshot().find(Thing);
	}

	colony::Entity DuelSmokeItemNear(const USimSubsystem& Bridge, const char* DefName, float X, float Y, float Within)
	{
		colony::Entity Best = colony::kNoEntity;
		float BestDistance = Within;
		if (!Bridge.GetDefs())
		{
			return Best;
		}
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Item || !View.def.valid() || Bridge.GetDefs()->get(View.def).defName() != DefName)
			{
				continue;
			}
			const float Distance = FVector2D::Distance(FVector2D(X, Y), FVector2D(View.x, View.y));
			if (Distance < BestDistance)
			{
				Best = View.id;
				BestDistance = Distance;
			}
		}
		return Best;
	}

	void DuelSmokeHeal(colony::Sim& Sim, colony::Entity Pawn)
	{
		if (Pawn != colony::kNoEntity && Sim.things().spawned(Pawn))
		{
			Sim.things().registry().replace<colony::Health>(Pawn, colony::Health{});
			Sim.health().refresh(Pawn);
		}
	}

	/** A medium steel shield on the arm, and the melee skill of a fair swordsman. */
	FString DuelSmokeOutfit(colony::Sim& Sim, colony::Entity Pawn, const colony::CellPos& Cell)
	{
		std::string Why;
		const colony::Entity Shield = Sim.apparel().make(Sim.defs().idOf("Things", "Apparel_ShieldMedium"), Sim.defs().idOf("Things", "Steel"), -1, Cell, &Why);
		if (Shield == colony::kNoEntity || !Sim.apparel().putOn(Pawn, Shield, Why))
		{
			return FString::Printf(TEXT("could not put a steel medium shield on: %s"), UTF8_TO_TCHAR(Why.c_str()));
		}
		colony::Colonist& Record = Sim.things().registry().get_or_emplace<colony::Colonist>(Pawn);
		Record.skills.clear();
		Record.skills.push_back(colony::SkillRecord{"Melee", DuelSmokeSkill});
		return FString();
	}
}

void USmokeTestSubsystem::BuildDuelPhases()
{
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const TSharedRef<FDuelSmokeState> State = MakeShared<FDuelSmokeState>();
	const bool bWatch = FParse::Param(FCommandLine::Get(), TEXT("DuelWatch"));

	// A fighter with its shield on gets a gladius dropped at its feet, then equips it once the snapshot shows it.
	const auto Arm = [=](colony::Entity Pawn, bool& bArmed)
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* View = B ? DuelSmokeFind(*B, Pawn) : nullptr;
		if (bArmed || !View)
		{
			return;
		}
		if (View->weapon.valid())
		{
			bArmed = true;
			return;
		}
		const colony::Entity Blade = DuelSmokeItemNear(*B, "Melee_Gladius", View->x, View->y, 2.5f);
		if (Blade != colony::kNoEntity)
		{
			B->Submit(colony::EquipCommand{Pawn, Blade});
		}
	};

	// A new opponent: a hostile human 5 m from you, shield on, a gladius at its feet.
	const auto NewFoe = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
		const colony::ThingView* Me = DuelSmokeFind(*B, State->Me);
		const colony::CellPos Cell = Me ? colony::CellPos{static_cast<int>(Me->x) + 5, static_cast<int>(Me->y), 0} : colony::CellPos{State->Middle.x + 5, State->Middle.y, 0};
		std::vector<colony::Entity> Before;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn)
			{
				Before.push_back(View.id);
			}
		}
		B->Submit(colony::SpawnThingCommand{"Human", Cell, 1});
		B->Submit(colony::SpawnThingCommand{"Melee_Gladius", Cell, 1});
		B->StepOnce();
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && std::find(Before.begin(), Before.end(), View.id) == Before.end())
			{
				State->Foe = View.id;
			}
		}
		if (State->Foe == colony::kNoEntity)
		{
			Fail(TEXT("duel: the new opponent did not spawn"));
			return;
		}
		const FString Problem = DuelSmokeOutfit(Sim, State->Foe, Cell);
		if (!Problem.IsEmpty())
		{
			Fail(TEXT("duel: opponent ") + Problem);
		}
		B->Submit(colony::SetFactionCommand{State->Foe, 2});
		State->bFoeArmed = false;
		State->FoeDownSince = -1.0f;
	};

	// Every frame: arm whoever still needs a blade, keep you in the fight, bring a new opponent, count what is seen.
	const auto Tend = [=, this](float Delta)
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		State->Clock += Delta;
		colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
		Arm(State->Me, State->bMeArmed);
		Arm(State->Foe, State->bFoeArmed);
		const colony::ThingView* Me = DuelSmokeFind(*B, State->Me);
		if (Me && !Me->dead && (Me->downed || Me->lowestPart < DuelSmokePracticeHealth))
		{
			DuelSmokeHeal(Sim, State->Me);
		}
		// The watch keeps both on their feet a while longer, so each screenshot has a fight in it.
		if (bWatch && State->Clock - State->LastHeal > 3.0f)
		{
			State->LastHeal = State->Clock;
			const colony::ThingView* Foe = DuelSmokeFind(*B, State->Foe);
			if (Foe && !Foe->dead)
			{
				DuelSmokeHeal(Sim, State->Foe);
			}
		}
		const colony::ThingView* Foe = DuelSmokeFind(*B, State->Foe);
		if (!Foe || Foe->dead || Foe->downed)
		{
			if (State->FoeDownSince < 0.0f)
			{
				State->FoeDownSince = State->Clock;
			}
			else if (State->Clock - State->FoeDownSince > DuelSmokeRematchSeconds)
			{
				if (Foe)
				{
					B->Submit(colony::DestroyThingCommand{State->Foe});
				}
				State->Foe = colony::kNoEntity;
				++State->Rematches;
				NewFoe();
			}
		}
		const colony::Entity Two[2] = {State->Me, State->Foe};
		for (int32 i = 0; i < 2; ++i)
		{
			const colony::ThingView* View = DuelSmokeFind(*B, Two[i]);
			if (!View)
			{
				continue;
			}
			const bool bHeavy = View->meleeKind == static_cast<uint8>(colony::SoulsAttack::Heavy) ||
				View->meleeKind == static_cast<uint8>(colony::SoulsAttack::Charged) || View->meleeKind == static_cast<uint8>(colony::SoulsAttack::RunningHeavy);
			if (bHeavy && View->meleeKind != State->LastKind[i])
			{
				++State->Heavies;
			}
			if (View->meleeStunned && !State->LastStunned[i])
			{
				++State->Staggers;
			}
			State->LastKind[i] = View->meleeKind;
			State->LastStunned[i] = View->meleeStunned;
		}
	};

	FPhase Fight;
	Fight.Name = TEXT("duel");
	Fight.WarmupSeconds = 6.0f;
	float Minutes = 240.0f;
	FParse::Value(FCommandLine::Get(), TEXT("DuelMinutes="), Minutes);
	Fight.MeasureSeconds = bWatch ? 4.0f : Minutes * 60.0f;
	Fight.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !PawnsOf())
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(DuelSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		if (!B->GetCurrentSnapshot().souls)
		{
			Notes.Add(TEXT("duel: SoulsRules is off (data/defs/Rules/SoulsRules.json enabled false): the old melee"));
		}
		B->SetSpeed(0);
		B->StepOnce();
		colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
		State->Middle = Sim.stockpileCell().value_or(colony::CellPos{48, 48, 0});
		const colony::CellPos Middle = State->Middle;
		// A clear flat yard: no things, no plants within 20 m.
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - Middle.x) < 20.0f && FMath::Abs(View.y - Middle.y) < 20.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		for (int32 x = -20; x <= 20; ++x)
		{
			for (int32 y = -20; y <= 20; ++y)
			{
				Sim.plants().remove(colony::CellPos{Middle.x + x, Middle.y + y, 0});
			}
		}
		B->StepOnce();
		TArray<colony::Entity> Colonists;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0)
			{
				Colonists.Add(View.id);
			}
		}
		Colonists.Sort();
		if (Colonists.Num() < 2)
		{
			Fail(TEXT("the Crashlanded start has fewer than two colonists"));
			return;
		}
		// The first is you; the second is the first opponent (turned hostile); the rest go.
		for (int32 i = 2; i < Colonists.Num(); ++i)
		{
			B->Submit(colony::DestroyThingCommand{Colonists[i]});
		}
		State->Me = Colonists[0];
		State->Foe = Colonists[1];
		entt::registry& Registry = Sim.things().registry();
		const colony::CellPos MeCell{Middle.x - 2, Middle.y, 0};
		const colony::CellPos FoeCell{Middle.x + 3, Middle.y, 0};
		for (const TPair<colony::Entity, colony::CellPos>& Place : {TPair<colony::Entity, colony::CellPos>(State->Me, MeCell), TPair<colony::Entity, colony::CellPos>(State->Foe, FoeCell)})
		{
			colony::Position& At = Registry.get<colony::Position>(Place.Key);
			At.x = static_cast<float>(Place.Value.x) + 0.5f;
			At.y = static_cast<float>(Place.Value.y) + 0.5f;
			Registry.get<colony::Mover>(Place.Key).path.clear();
			const FString Problem = DuelSmokeOutfit(Sim, Place.Key, Place.Value);
			if (!Problem.IsEmpty())
			{
				Fail(TEXT("duel: ") + Problem);
			}
			B->Submit(colony::SpawnThingCommand{"Melee_Gladius", Place.Value, 1});
		}
		B->Submit(colony::SetFactionCommand{State->Foe, 2});
		// Drafted, so in the watch your side fights back on its own until you take over.
		B->Submit(colony::DraftCommand{State->Me, true});
		B->StepOnce();
		State->SwingsAtStart = Sim.melee().swings();
		B->SetSpeed(1);
	};
	Fight.OnFrame = [=, this]()
	{
		Tend(GetWorld()->GetDeltaSeconds());
		// Play: take over once your actor is there (direct control needs the pawn drawn) and the blade is in hand.
		if (!bWatch && !State->bControlled && State->bMeArmed && PhaseTime > 0.5f)
		{
			if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
			{
				if (Protagonist->EnterDirectMode(State->Me))
				{
					State->bControlled = true;
					Notes.Add(TEXT("duel: you hold a gladius and a shield; Shift + left mouse is a heavy attack"));
				}
			}
		}
	};
	Phases.Add(MoveTemp(Fight));
	if (!bWatch)
	{
		return;
	}

	// The watch: six looks from the side while the AI fights itself.
	const auto SideView = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		const ASimPawnActor* A = Pawns ? Pawns->FindPawn(State->Me) : nullptr;
		const ASimPawnActor* C = Pawns ? Pawns->FindPawn(State->Foe) : nullptr;
		if (!B || !A || !C)
		{
			return;
		}
		const FVector Mid = (A->GetActorLocation() + C->GetActorLocation()) * 0.5 + FVector(0.0, 0.0, 90.0);
		FVector Across = C->GetActorLocation() - A->GetActorLocation();
		Across.Z = 0.0;
		const FVector Side = FVector::CrossProduct(Across.GetSafeNormal(), FVector::UpVector);
		const FVector Eye = Mid + Side * 480.0 + FVector(0.0, 0.0, 70.0);
		ViewFrom(Eye, (Mid - Eye).Rotation());
	};
	for (int32 Look = 1; Look <= 6; ++Look)
	{
		FPhase Watch;
		Watch.Name = FString::Printf(TEXT("duel_watch_%d"), Look);
		Watch.Setup = []() {};  // the fight goes on from the first phase
		Watch.WarmupSeconds = 0.5f;
		Watch.MeasureSeconds = 3.0f;
		Watch.ScreenshotAt = 0.4f * static_cast<float>(Look % 3) + 0.6f;
		Watch.OnFrame = [=, this]()
		{
			Tend(GetWorld()->GetDeltaSeconds());
			SideView();
		};
		Phases.Add(MoveTemp(Watch));
	}
	FPhase Hud;
	Hud.Name = TEXT("duel_hud");
	Hud.Setup = []() {};
	Hud.WarmupSeconds = 1.5f;
	Hud.MeasureSeconds = 3.0f;
	Hud.OnFrame = [=, this]()
	{
		Tend(GetWorld()->GetDeltaSeconds());
		if (!State->bControlled)
		{
			if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
			{
				State->bControlled = Protagonist->EnterDirectMode(State->Me);
			}
		}
	};
	Hud.Verify = [=, this]()
	{
		const USimSubsystem* B = Bridge();
		const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
		const uint64 Swings = B && B->GetSim() ? B->GetSim()->melee().swings() - State->SwingsAtStart : 0;
		Notes.Add(FString::Printf(TEXT("duel: %llu swings, %d heavies, %d staggers, %d new opponents, stamina bar %d frames"),
			static_cast<unsigned long long>(Swings), State->Heavies, State->Staggers, State->Rematches,
			Protagonist ? Protagonist->GetVitalsFramesDrawn() : 0));
		if (Swings < 6)
		{
			Fail(FString::Printf(TEXT("duel: only %llu swings in the watch, expected at least 6"), static_cast<unsigned long long>(Swings)));
		}
		if (B && B->GetCurrentSnapshot().souls && (!Protagonist || Protagonist->GetVitalsFramesDrawn() == 0))
		{
			Fail(TEXT("duel: the health and stamina bars were never drawn"));
		}
	};
	Phases.Add(MoveTemp(Hud));
}
