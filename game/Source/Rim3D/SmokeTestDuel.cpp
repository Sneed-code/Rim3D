// The duel (title > Debug > Sword and shield duel, docs/design/souls-combat.md): you, with a gladius and a medium
// shield, against a hostile pawn with the same, on a flat cleared map. Elden Ring style melee: stamina, light and heavy
// attacks, combos, guard, staggers and knockdowns. A practice fight: you are healed before you would go down, and a new
// opponent walks in a few seconds after the last one falls.
//
// -DuelWatch: a check instead of play. Both sides fight on their own (the AI's souls turns), seen from the side for six
// screenshots, then you take over for a last one with the health and stamina bars; it fails if nobody swung.
//
// -DuelLooks: close looks instead of a fight (PC 2026-11-28): both on your side and standing still, you guard with the shield
// and without it, the ready stance from the front, the opponent knocked down each of the four ways and getting up, the
// impale on a poise-broken opponent, and a light and a heavy jump attack, several screenshots each. Fails only if a
// pose could not be set up.

#include "SmokeTest.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "UnrealClient.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/BodyMotion.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Looks.h"
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
	// The player is healed when its worst body part falls under this share (a practice fight: no YOU DIED); in the watch the
	// opponent too.
	constexpr float DuelSmokePracticeHealth = 0.9f;  // a charged blow to the head takes most of it at once
	// In play you see your health go down to half before the practice heal.
	constexpr float DuelSmokePlayHealth = 0.5f;
	// Many small cuts bleed to death while no one part looks hurt: healed past this much blood lost too.
	constexpr float DuelSmokePracticeBlood = 0.1f;
	// Seconds after the opponent falls before the next one comes (sneed 2026-11-28: "the pawns despawn too fast when I down
	// them, make it take 10 sec just in that demo": time to walk up and finish them).
	constexpr float DuelSmokeRematchSeconds = 10.0f;
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
		bool bMeDeadNoted = false;
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

	/** A woman drawn as one of the creator's random people (sneed 2026-11-27: female pawns fight in the duel): the look
	 *  travels through the sim as creator text, the way a new game's colonists do. */
	void DuelSmokeWoman(colony::Sim& Sim, colony::Entity Pawn, int32 Seed)
	{
		entt::registry& Registry = Sim.things().registry();
		if (colony::Colonist* Record = Registry.try_get<colony::Colonist>(Pawn))
		{
			Record->female = true;
		}
		if (colony::PawnLooks* Looks = Registry.try_get<colony::PawnLooks>(Pawn))
		{
			Looks->female = true;
			Looks->beard = "None";
			Looks->naturalBeard = "None";
			Looks->creator = "seed=" + std::to_string(Seed) + ";female=1;";
		}
	}

	/** A medium steel shield on the arm, and the melee skill of a fair swordsman. */
	FString DuelSmokeOutfit(colony::Sim& Sim, colony::Entity Pawn, const colony::CellPos& Cell)
	{
		std::string Why;
		// The same plain clothes for both (a spawned opponent came with none and fought naked, PC 2026-11-27).
		const std::vector<colony::Entity> Worn = Sim.apparel().worn(Pawn);
		for (const colony::Entity Item : Worn)
		{
			std::string Ignored;
			Sim.apparel().takeOff(Pawn, Item, Ignored);
		}
		for (const char* Garment : {"Apparel_TShirt", "Apparel_Pants"})
		{
			const colony::Entity Piece = Sim.apparel().make(Sim.defs().idOf("Things", Garment), Sim.defs().idOf("Things", "Cloth"), 2, Cell, &Why);
			if (Piece == colony::kNoEntity || !Sim.apparel().putOn(Pawn, Piece, Why))
			{
				return FString::Printf(TEXT("could not put on %s: %s"), UTF8_TO_TCHAR(Garment), UTF8_TO_TCHAR(Why.c_str()));
			}
		}
		const colony::Entity Shield = Sim.apparel().make(Sim.defs().idOf("Things", "Apparel_ShieldMedium"), Sim.defs().idOf("Things", "Steel"), -1, Cell, &Why);
		if (Shield == colony::kNoEntity || !Sim.apparel().putOn(Pawn, Shield, Why))
		{
			return FString::Printf(TEXT("could not put a steel medium shield on: %s"), UTF8_TO_TCHAR(Why.c_str()));
		}
		colony::Colonist& Record = Sim.things().registry().get_or_emplace<colony::Colonist>(Pawn);
		// An even fight, and one the practice heal can keep up with: the protagonist's double hit points and death in place
		// of going down (protagonist.md "Death") are off for the duel.
		Record.protagonist = false;
		DuelSmokeHeal(Sim, Pawn);
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
	const bool bLooks = FParse::Param(FCommandLine::Get(), TEXT("DuelLooks"));
	const bool bWatch = FParse::Param(FCommandLine::Get(), TEXT("DuelWatch")) || bLooks;

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
		DuelSmokeWoman(Sim, State->Foe, 7100 + State->Rematches * 37);
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
		// A practice fight: the opponent's impale (a riposte, 3x a blow, always the torso) can kill outright from full health,
		// before the heal below sees a scratch (PC 2026-11-28: the watch died to one at 23 s), so it is not thrown at you
		// here; you can still impale the opponent.
		if (Me && !Me->dead && Sim.things().spawned(State->Me))
		{
			colony::Melee& Guarded = Sim.things().registry().get_or_emplace<colony::Melee>(State->Me);
			Guarded.critImmuneUntil = std::max(Guarded.critImmuneUntil, static_cast<int64_t>(Sim.time().steps) + 30);
		}
		if (Me && !Me->dead && (Me->downed || Me->lowestPart < (bWatch ? DuelSmokePracticeHealth : DuelSmokePlayHealth) || Me->bloodLoss > DuelSmokePracticeBlood))
		{
			DuelSmokeHeal(Sim, State->Me);
		}
		// The watch keeps both on their feet, so each screenshot has a fight in it.
		if (bWatch)
		{
			const colony::ThingView* Foe = DuelSmokeFind(*B, State->Foe);
			if (Foe && !Foe->dead && (Foe->downed || Foe->lowestPart < DuelSmokePracticeHealth || Foe->bloodLoss > DuelSmokePracticeBlood))
			{
				DuelSmokeHeal(Sim, State->Foe);
			}
		}
		if (Me && Me->dead && !State->bMeDeadNoted)
		{
			State->bMeDeadNoted = true;
			const colony::Health* Dead = Sim.things().registry().try_get<colony::Health>(State->Me);
			const colony::ThingView* Killer = DuelSmokeFind(*B, State->Foe);
			Notes.Add(FString::Printf(TEXT("duel: you died of %s at %.1f s (the opponent's attack: %s, %llu ripostes so far)"),
				Dead ? UTF8_TO_TCHAR(colony::deathCauseName(Dead->cause)) : TEXT("?"), State->Clock,
				Killer && Killer->meleeKind < static_cast<uint8>(colony::SoulsAttack::Count) ? UTF8_TO_TCHAR(colony::soulsAttackName(static_cast<colony::SoulsAttack>(Killer->meleeKind))) : TEXT("none"),
				static_cast<unsigned long long>(Sim.melee().ripostes())));
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
		// MakeHuman bodies (not the grey mannequins) in daylight.
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), 10.0f)})
		{
			if (IConsoleVariable* Found = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Found->Set(Var.Value, ECVF_SetByCode);
			}
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
			DuelSmokeWoman(Sim, Place.Key, Place.Key == State->Me ? 4242 : 7100);
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
					Notes.Add(TEXT("duel: you hold a gladius and a shield; hold left click for a heavy attack"));
				}
			}
		}
	};
	Phases.Add(MoveTemp(Fight));
	if (!bWatch)
	{
		return;
	}
	if (bLooks)
	{
		// One screenshot: when (seconds after the move starts), its name, and the camera: from the anchor (metres along the
		// line from you to the opponent), Fwd along that line, Right of it and Up (cm), looking at the anchor AimUp cm up.
		struct FLookShot
		{
			float At;
			FString Name;
			double Fwd, Right, Up, AimUp;
		};
		struct FLook
		{
			FString Name;
			float Gap = 2.2f;      // metres from you to the opponent, you facing it
			float Seconds = 3.0f;
			float AnchorX = 0.0f;  // the camera's anchor, metres along the line from you
			bool bHold = false;    // held in place the whole time (a pose), not only until the move starts
			TFunction<void(colony::Sim&, USimSubsystem&)> Start;
			TFunction<void(colony::Sim&, USimSubsystem&, float)> Frame;
			TArray<FLookShot> Shots;
		};
		const TSharedRef<int32> NextShot = MakeShared<int32>(0);
		const auto SimOf = [=]() -> colony::Sim*
		{
			USimSubsystem* B = Bridge();
			return B && B->GetSim() ? const_cast<colony::Sim*>(B->GetSim()) : nullptr;
		};
		const auto ShieldOf = [](colony::Sim& Sim, colony::Entity Pawn)
		{
			for (const colony::Entity Item : Sim.apparel().worn(Pawn))
			{
				const colony::Thing* Piece = Sim.things().registry().try_get<colony::Thing>(Item);
				if (Piece && Piece->def.valid() && Sim.defs().get(Piece->def).defName() == "Apparel_ShieldMedium")
				{
					return Item;
				}
			}
			return colony::kNoEntity;
		};
		const TSharedRef<colony::Entity> LooseShield = MakeShared<colony::Entity>(colony::kNoEntity);
		const auto ShieldBackOn = [=](colony::Sim& Sim)
		{
			if (*LooseShield != colony::kNoEntity && Sim.things().spawned(*LooseShield))
			{
				std::string Why;
				Sim.apparel().putOn(State->Me, *LooseShield, Why);
			}
			*LooseShield = colony::kNoEntity;
		};
		TArray<FLook> Looks;
		// You guard with the shield: the sword hand held up ready behind it.
		{
			FLook L;
			L.Name = TEXT("guard_shield");
			L.bHold = true;
			L.Seconds = 4.0f;
			L.Frame = [=](colony::Sim&, USimSubsystem& B, float)
			{
				const colony::ThingView* Me = DuelSmokeFind(B, State->Me);
				if (Me && !Me->shieldUp)
				{
					B.Submit(colony::ShieldCommand{State->Me, true, true, 1.0f, 0.0f});
				}
			};
			L.Shots = {{2.2f, TEXT("guard_shield_front"), 190.0, 110.0, 155.0, 120.0}, {2.8f, TEXT("guard_shield_side"), 15.0, 250.0, 135.0, 115.0},
				{3.4f, TEXT("guard_shield_left"), 60.0, -240.0, 140.0, 115.0}};
			Looks.Add(L);
		}
		// You guard with the sword alone (the shield taken off): the blade raised across in front.
		{
			FLook L;
			L.Name = TEXT("guard_blade");
			L.bHold = true;
			L.Seconds = 3.5f;
			L.Start = [=, this](colony::Sim& Sim, USimSubsystem& B)
			{
				B.Submit(colony::ShieldCommand{State->Me, false, false, 0.0f, 0.0f});
				const colony::Entity Shield = ShieldOf(Sim, State->Me);
				std::string Why;
				if (Shield != colony::kNoEntity && Sim.apparel().takeOff(State->Me, Shield, Why))
				{
					*LooseShield = Shield;
				}
				else
				{
					Notes.Add(FString::Printf(TEXT("duel looks: could not take the shield off: %s"), UTF8_TO_TCHAR(Why.c_str())));
				}
			};
			L.Frame = [=](colony::Sim&, USimSubsystem& B, float)
			{
				B.Submit(colony::MeleeCommand{State->Me, colony::kNoEntity, colony::MeleeMode::Block});
			};
			L.Shots = {{2.2f, TEXT("guard_blade_front"), 190.0, 110.0, 155.0, 120.0}, {2.8f, TEXT("guard_blade_side"), 15.0, 250.0, 135.0, 115.0}};
			Looks.Add(L);
		}
		// The ready stance with the shield, guard down: the sword clear of the shield.
		{
			FLook L;
			L.Name = TEXT("stance");
			L.bHold = true;
			L.Seconds = 4.0f;
			L.Start = [=](colony::Sim& Sim, USimSubsystem&) { ShieldBackOn(Sim); };
			L.Shots = {{2.2f, TEXT("stance_front"), 190.0, 110.0, 155.0, 115.0}, {2.8f, TEXT("stance_left"), 60.0, -240.0, 140.0, 110.0},
				{3.4f, TEXT("stance_right"), 40.0, 240.0, 140.0, 110.0}};
			Looks.Add(L);
		}
		// The opponent thrown down each way (the blow from the side given), seen side on through the fall and the get-up.
		const TPair<const TCHAR*, FVector2D> Falls[4] = {
			{TEXT("knock_back"), FVector2D(-1.0, 0.0)}, {TEXT("knock_forward"), FVector2D(1.0, 0.0)},
			{TEXT("knock_left"), FVector2D(0.0, -1.0)}, {TEXT("knock_right"), FVector2D(0.0, 1.0)}};
		for (const TPair<const TCHAR*, FVector2D>& Fall : Falls)
		{
			FLook L;
			L.Name = Fall.Key;
			L.Gap = 2.6f;
			L.AnchorX = 2.6f;
			L.Seconds = 7.5f;
			const FVector2D From = Fall.Value;
			const FString N = Fall.Key;
			L.Start = [=, this](colony::Sim& Sim, USimSubsystem&)
			{
				const colony::Position* At = Sim.things().registry().try_get<colony::Position>(State->Foe);
				colony::HitFrom Hit;
				Hit.known = true;
				Hit.blow = true;
				Hit.x = At ? At->x + static_cast<float>(From.X) : 0.0f;
				Hit.y = At ? At->y + static_cast<float>(From.Y) : 0.0f;
				if (!At || !Sim.combat().knockDown(State->Foe, Hit, 40.0f, 45))
				{
					Fail(FString::Printf(TEXT("duel looks: could not knock the opponent down (%s)"), *N));
				}
			};
			// Side on to the line, from your right; from the left for a fall to the right (it lies toward the camera).
			const double Side = From.Y > 0.5 ? -330.0 : 330.0;
			for (int32 i = 0; i < 9; ++i)
			{
				const float T = 0.4f + 0.75f * static_cast<float>(i);
				L.Shots.Add({T, FString::Printf(TEXT("%s_%02d"), *N, i), 0.0, Side, 230.0, 30.0});
			}
			Looks.Add(L);
		}
		// The impale: the opponent's poise broken a moment ago, any press.
		{
			FLook L;
			L.Name = TEXT("riposte");
			L.Gap = 1.4f;
			L.AnchorX = 0.7f;
			L.Seconds = 4.0f;
			L.Start = [=, this](colony::Sim& Sim, USimSubsystem&)
			{
				colony::Melee& Foe = Sim.things().registry().get_or_emplace<colony::Melee>(State->Foe);
				Foe.stunUntil = Sim.time().steps + Sim.melee().souls().postureBreakSteps;
				Foe.guardStun = false;
				std::string Why;
				if (!Sim.melee().soulsAttack(State->Me, State->Foe, true, colony::SoulsPress{}, &Why))
				{
					Fail(FString::Printf(TEXT("duel looks: no impale: %s"), UTF8_TO_TCHAR(Why.c_str())));
				}
				else
				{
					Notes.Add(FString::Printf(TEXT("duel looks: the press at the reeling opponent is a %s"),
						UTF8_TO_TCHAR(colony::soulsAttackName(Sim.melee().attackKind(State->Me)))));
				}
			};
			for (int32 i = 0; i < 10; ++i)
			{
				const float T = 0.1f + 0.2f * static_cast<float>(i);
				L.Shots.Add({T, FString::Printf(TEXT("riposte_%02d"), i), 0.0, 280.0, 130.0, 110.0});
			}
			Looks.Add(L);
		}
		// Jump attacks, light then heavy: up, and a third of a second in the jump gives way to the attack's clip.
		for (int32 Heavy = 0; Heavy < 2; ++Heavy)
		{
			FLook L;
			L.Name = Heavy ? TEXT("jump_heavy") : TEXT("jump_light");
			L.Gap = 3.2f;
			L.AnchorX = 1.6f;
			L.Seconds = 4.5f;
			const TSharedRef<bool> Sent = MakeShared<bool>(false);
			L.Start = [=, this](colony::Sim&, USimSubsystem&)
			{
				*Sent = false;
				USimPawnSubsystem* Pawns = PawnsOf();
				ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(State->Me) : nullptr;
				if (!Actor || !Actor->StartJump())
				{
					Notes.Add(TEXT("duel looks: the jump did not start"));
				}
			};
			L.Frame = [=](colony::Sim&, USimSubsystem& B, float T)
			{
				if (*Sent || T < 0.3f)
				{
					return;
				}
				*Sent = true;
				colony::MeleeCommand Order{State->Me, State->Foe, colony::MeleeMode::Attack};
				Order.heavy = Heavy != 0;
				Order.jumping = true;
				USimPawnSubsystem* Pawns = PawnsOf();
				if (ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(State->Me) : nullptr)
				{
					Actor->CancelJump();
				}
				B.Submit(Order);
			};
			for (int32 i = 0; i < 12; ++i)
			{
				const float T = 0.1f + 0.2f * static_cast<float>(i);
				L.Shots.Add({T, FString::Printf(TEXT("%s_%02d"), *L.Name, i), 0.0, 330.0, 120.0, 100.0});
			}
			Looks.Add(L);
		}

		for (const FLook& Look : Looks)
		{
			FPhase P;
			P.Name = TEXT("looks_") + Look.Name;
			P.WarmupSeconds = 1.2f;  // placed and settled before the move starts
			P.MeasureSeconds = Look.Seconds;
			P.ScreenshotAt = 1000.0f;  // its own shots below
			const TSharedRef<bool> Started = MakeShared<bool>(false);
			const TSharedRef<FVector> Origin = MakeShared<FVector>(FVector::ZeroVector);
			const TSharedRef<FVector> Frame = MakeShared<FVector>(FVector::ZeroVector);
			// Both placed Gap apart facing each other, held there (a drafted pawn walks back to where it was drafted).
			const auto Pin = [=](colony::Sim& Sim)
			{
				entt::registry& Registry = Sim.things().registry();
				const colony::Entity Two[2] = {State->Me, State->Foe};
				for (int32 i = 0; i < 2; ++i)
				{
					colony::Position& At = Registry.get<colony::Position>(Two[i]);
					At.x = static_cast<float>(State->Middle.x) + 0.5f + (i == 0 ? 0.0f : Look.Gap);
					At.y = static_cast<float>(State->Middle.y) + 0.5f;
					if (colony::Mover* M = Registry.try_get<colony::Mover>(Two[i]))
					{
						M->path.clear();
					}
					// Held here: a drafted pawn walks back to where it was drafted.
					if (colony::Fighter* Held = Registry.try_get<colony::Fighter>(Two[i]))
					{
						Held->anchor = colony::CellPos{static_cast<int>(At.x), static_cast<int>(At.y), 0};
						Held->moveOrdered = false;
						Held->forcedTarget = colony::kNoEntity;
						Held->target = colony::kNoEntity;
						Held->advances = false;
						Held->leash = 0.6f;
					}
					if (colony::Posture* Pose = Registry.try_get<colony::Posture>(Two[i]))
					{
						Pose->facingX = i == 0 ? 1.0f : -1.0f;
						Pose->facingY = 0.0f;
						Pose->stance = colony::Stance::Standing;  // the fight's cover AI leaves a drafted pawn crouched
					}
				}
			};
			P.Setup = [=, this]()
			{
				*Started = false;
				*Frame = FVector::ZeroVector;
				*NextShot = 0;
				colony::Sim* Sim = SimOf();
				USimSubsystem* B = Bridge();
				if (!Sim || !B)
				{
					return;
				}
				if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
				{
					PC->ConsoleCommand(TEXT("r.SetRes 1280x720w"));  // close looks: bigger than the offscreen 888x500
				}
				// Both on your side, drafted, standing still and healed; placed Gap apart facing each other.
				B->Submit(colony::SetFactionCommand{State->Foe, colony::kPlayerFaction});
				B->Submit(colony::DraftCommand{State->Foe, true});
				B->Submit(colony::DraftCommand{State->Me, true});
				for (const colony::Entity Who : {State->Me, State->Foe})
				{
					if (!Sim->things().spawned(Who))
					{
						Fail(TEXT("duel looks: a fighter is gone"));
						return;
					}
					DuelSmokeHeal(*Sim, Who);
				}
				Pin(*Sim);
			};
			P.OnFrame = [=, this]()
			{
				Tend(GetWorld()->GetDeltaSeconds());
				colony::Sim* Sim = SimOf();
				USimSubsystem* B = Bridge();
				if (!Sim || !B)
				{
					return;
				}
				const float T = PhaseTime - 1.2f;
				if (T < 0.0f || Look.bHold)
				{
					Pin(*Sim);  // held in place until the move starts
				}
				if (T >= 0.0f && !*Started)
				{
					*Started = true;
					if (const colony::Position* FoeAt = Sim->things().registry().try_get<colony::Position>(State->Foe))
					{
						const colony::Fighter* Held = Sim->things().registry().try_get<colony::Fighter>(State->Foe);
						Notes.Add(FString::Printf(TEXT("%s start: foe %u at (%.2f, %.2f), anchor (%d, %d), middle (%d, %d)"), *Look.Name,
							static_cast<uint32>(entt::to_integral(State->Foe)), FoeAt->x, FoeAt->y, Held ? Held->anchor.x : -1, Held ? Held->anchor.y : -1,
							State->Middle.x, State->Middle.y));
					}
					if (Look.Start)
					{
						Look.Start(*Sim, *B);
					}
				}
				if (T >= 0.0f && Look.Frame)
				{
					Look.Frame(*Sim, *B, T);
				}
				// The camera for the next shot.
				const int32 Index = FMath::Min(*NextShot, Look.Shots.Num() - 1);
				if (Index < 0)
				{
					return;
				}
				const FLookShot& Shot = Look.Shots[Index];
				const float Y = static_cast<float>(State->Middle.y) + 0.5f;
				const float X0 = static_cast<float>(State->Middle.x) + 0.5f;
				// Along the line from your body to the opponent's as drawn when the move starts (before: as drawn now).
				if (!*Started || Frame->IsNearlyZero())
				{
					USimPawnSubsystem* Pawns = PawnsOf();
					const ASimPawnActor* MeActor = Pawns ? Pawns->FindPawn(State->Me) : nullptr;
					const ASimPawnActor* FoeActor = Pawns ? Pawns->FindPawn(State->Foe) : nullptr;
					if (MeActor && FoeActor)
					{
						*Origin = MeActor->GetActorLocation();
						Origin->Z = FMath::Min(MeActor->GetActorLocation().Z, FoeActor->GetActorLocation().Z);  // the feet
						*Frame = (FoeActor->GetActorLocation() - MeActor->GetActorLocation()).GetSafeNormal2D();
					}
				}
				if (Frame->IsNearlyZero())
				{
					return;
				}
				const FVector F = *Frame;
				const FVector A = *Origin + F * (Look.AnchorX * 100.0);
				const FVector R = FVector::CrossProduct(FVector::UpVector, F);
				const FVector Eye = A + F * Shot.Fwd + R * Shot.Right + FVector::UpVector * Shot.Up;
				const FVector Aim = A + FVector::UpVector * Shot.AimUp;
				ViewFrom(Eye, (Aim - Eye).Rotation());
				if (*NextShot < Look.Shots.Num() && T >= Shot.At)
				{
					FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke") / TestName / (Shot.Name + TEXT(".png")), false, false);
					++*NextShot;
					const colony::ThingView* Me = DuelSmokeFind(*B, State->Me);
					const colony::ThingView* Foe = DuelSmokeFind(*B, State->Foe);
					const auto ClipName = [&](const colony::ThingView* V)
					{
						const colony::MotionClip* C = V && V->motionClip.valid() ? Sim->combat().motion().clip(V->motionClip) : nullptr;
						return C ? FString(UTF8_TO_TCHAR(C->name.c_str())) : FString(TEXT("-"));
					};
					if (Me && Foe)
					{
						Notes.Add(FString::Printf(TEXT("%s: you (%.2f, %.2f) shield %d block %d phase %d clip %s; foe (%.2f, %.2f) facing (%.2f, %.2f) clip %s %.2f down %d"),
							*Shot.Name, Me->x - X0, Me->y - Y, Me->shieldUp ? 1 : 0, Me->blocking ? 1 : 0, static_cast<int32>(Me->meleePhase), *ClipName(Me),
							Foe->x - X0, Foe->y - Y, Foe->facingX, Foe->facingY, *ClipName(Foe), Foe->motionTime, Foe->downed ? 1 : 0));
						if (const UWeaponDrawSubsystem* Draw = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>())
						{
							Notes.Add(FString::Printf(TEXT("%s: your weapon: %s"), *Shot.Name, *Draw->DescribePawn(State->Me)));
							if (Look.Name == TEXT("riposte"))
							{
								Notes.Add(FString::Printf(TEXT("%s: impale frames %d, blade line %.1f cm from the chest"), *Shot.Name, Draw->GetImpaleFrames(), Draw->GetImpaleMiss()));
							}
						}
						USimPawnSubsystem* ShotPawns = PawnsOf();
						if (const ASimPawnActor* MeActor = ShotPawns ? ShotPawns->FindPawn(State->Me) : nullptr; MeActor && Look.Name.StartsWith(TEXT("jump")))
						{
							Notes.Add(FString::Printf(TEXT("%s: height %.1f cm, jumping %d, fitted %d: %s"), *Shot.Name, MeActor->GetJumpHeight(),
								MeActor->IsJumping() ? 1 : 0, MeActor->GetJumpsFitted(), *MeActor->GetLastJumpFit()));
						}
					}
				}
			};
			Phases.Add(MoveTemp(P));
		}
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
		const colony::ThingView* Me = B ? DuelSmokeFind(*B, State->Me) : nullptr;
		Notes.Add(FString::Printf(TEXT("duel: %llu swings, %d heavies, %d staggers, %d new opponents, stamina bar %d frames, hud %d frames, direct %d, you %s"),
			static_cast<unsigned long long>(Swings), State->Heavies, State->Staggers, State->Rematches,
			Protagonist ? Protagonist->GetVitalsFramesDrawn() : 0, Protagonist ? Protagonist->GetHudFramesDrawn() : 0,
			Protagonist && Protagonist->IsDirectMode() ? 1 : 0, !Me ? TEXT("gone") : (Me->dead ? TEXT("dead") : (Me->downed ? TEXT("downed") : TEXT("standing")))));
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
