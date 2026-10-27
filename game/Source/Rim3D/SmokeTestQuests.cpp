// Smoke test `quests`: the quest window and the Quests button, driven by the calls their buttons make, in the game, offscreen.
#include "SmokeTest.h"

#include "HAL/IConsoleManager.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "QuestPanelSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Trade.h"

#include <string>

namespace
{
	const FVector QuestsSmokeOrigin(-790000.0, 0.0, 0.0);

	struct FQuestsSmokeState
	{
		uint32 Wanderer = 0;
		uint32 Cache = 0;
		uint32 Declined = 0;
		int32 SteelBefore = 0;
		int32 SteelReward = 0;
		int32 ColonistsBefore = 0;
	};
}

void USmokeTestSubsystem::BuildQuestsPhases()
{
	const TSharedRef<FQuestsSmokeState> State = MakeShared<FQuestsSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Panels = [this]() { return GetWorld()->GetSubsystem<UPanelsSubsystem>(); };
	const auto Quests = [this]() { return GetWorld()->GetSubsystem<UQuestPanelSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto SimOf = [=, this]() -> const colony::Sim* { USimSubsystem* B = Bridge(); return B ? B->GetSim() : nullptr; };
	// The narrator's way to make an offer; returns the new quest's id (0 if none came).
	const auto Offer = [=, this](const char* Kind) -> uint32
	{
		colony::QuestCommand Command;
		Command.action = "offer";
		Command.kind = Kind;
		Bridge()->Submit(std::move(Command));
		Bridge()->StepOnce();
		uint32 Id = 0;
		for (const colony::QuestView& Quest : Bridge()->GetCurrentSnapshot().quests)
		{
			if (Quest.kind == Kind && Quest.state == 0)
			{
				Id = Quest.id;
			}
		}
		return Id;
	};
	const auto Colonists = [=, this]() -> int32
	{
		TArray<colony::Entity> Pawns;
		Panels()->GetColonists(Pawns);
		return Pawns.Num();
	};
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
	const auto StateOf = [=, this](uint32 Id) -> int32
	{
		const colony::QuestView* Quest = Quests()->FindQuest(Id);
		return Quest ? Quest->state : -1;
	};

	// 1. A new sim with the Crashlanded colonists; two offers come; the button counts them and the window lists them.
	FPhase Setup;
	Setup.Name = TEXT("quests_setup");
	Setup.WarmupSeconds = 3.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !Panels() || !Quests() || !Overseer())
		{
			Fail(TEXT("USimSubsystem, UPanelsSubsystem, UQuestPanelSubsystem or UOverseerSubsystem did not start"));
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
		B->SetWorldOrigin(QuestsSmokeOrigin);
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
			const colony::CellPos Home = SimOf()->stockpileCell() ? *SimOf()->stockpileCell() : colony::CellPos{128, 128, 0};
			Pawn->ResetView();
			Pawn->Zoom(2.0f);
			Pawn->FocusOn(B->SimToWorld(Home.x + 0.5f, Home.y + 0.5f, 0));
			Pawn->SnapCamera();
		}
		B->StepOnce();
		State->Wanderer = Offer("WandererAsks");
		State->Cache = Offer("SupplyCache");
		if (State->Wanderer == 0 || State->Cache == 0)
		{
			Fail(FString::Printf(TEXT("quests_setup: the offers did not come (wanderer %u, cache %u)"), State->Wanderer, State->Cache));
			return;
		}
		if (Quests()->CountOffered() != 2 || !Quests()->HasUnseenOffer())
		{
			Fail(FString::Printf(TEXT("quests_setup: the button counts %d offers (unseen %d), wanted 2 and unseen"), Quests()->CountOffered(), Quests()->HasUnseenOffer() ? 1 : 0));
		}
		if (!LogHas(TEXT("Quest offered")))
		{
			Fail(TEXT("quests_setup: the log does not say a quest was offered"));
		}
		Quests()->SetOpen(true);
		if (Quests()->HasUnseenOffer())
		{
			Fail(TEXT("quests_setup: opening the window did not mark the offers seen"));
		}
	};
	Setup.Verify = [=, this]()
	{
		const FQuestPanelStats& Stats = Quests()->GetStats();
		if (Stats.WindowFrames == 0)
		{
			Fail(TEXT("quests_setup: the quest window was never drawn (ImGui not usable)"));
		}
		if (Stats.OfferedRows != 2)
		{
			Fail(FString::Printf(TEXT("quests_setup: the window shows %d offers, wanted 2"), Stats.OfferedRows));
		}
		Notes.Add(FString::Printf(TEXT("quests_setup: offers %u (wanderer) and %u (supply cache)"), State->Wanderer, State->Cache));
	};
	Phases.Add(MoveTemp(Setup));

	// 2. Accept both: the cache's goods land on the map, the wanderer joins, and an answered offer can't be answered again.
	FPhase Accept;
	Accept.Name = TEXT("quests_accept");
	Accept.WarmupSeconds = 1.5f;
	Accept.MeasureSeconds = 1.0f;
	Accept.Setup = [=, this]()
	{
		UQuestPanelSubsystem* Q = Quests();
		if (State->Cache == 0 || !Q)
		{
			return;
		}
		const colony::QuestView* Cache = Q->FindQuest(State->Cache);
		for (const std::pair<std::string, int>& Reward : Cache ? Cache->rewards : std::vector<std::pair<std::string, int>>())
		{
			if (Reward.first == "Steel")
			{
				State->SteelReward = Reward.second;
			}
		}
		if (State->SteelReward <= 0)
		{
			Fail(TEXT("quests_accept: the supply cache offers no steel"));
		}
		State->SteelBefore = SimOf()->trade().colonyHas("Steel");
		State->ColonistsBefore = Colonists();
		if (!Q->Accept(State->Cache) || !Q->Accept(State->Wanderer))
		{
			Fail(TEXT("quests_accept: Accept sent nothing"));
			return;
		}
		if (Q->Accept(State->Cache) || Q->Decline(State->Wanderer))
		{
			Fail(TEXT("quests_accept: a quest that was already answered took another answer"));
		}
	};
	Accept.Verify = [=, this]()
	{
		const int32 Steel = SimOf()->trade().colonyHas("Steel");
		if (Steel != State->SteelBefore + State->SteelReward)
		{
			Fail(FString::Printf(TEXT("quests_accept: steel went from %d to %d, wanted +%d"), State->SteelBefore, Steel, State->SteelReward));
		}
		if (Colonists() != State->ColonistsBefore + 1)
		{
			Fail(FString::Printf(TEXT("quests_accept: colonists went from %d to %d, wanted +1"), State->ColonistsBefore, Colonists()));
		}
		if (StateOf(State->Cache) != 2 || StateOf(State->Wanderer) != 2)
		{
			Fail(FString::Printf(TEXT("quests_accept: the quests are in states %d and %d, wanted 2 (done)"), StateOf(State->Cache), StateOf(State->Wanderer)));
		}
		if (Quests()->GetOutcome(State->Cache) != TEXT("delivered") || !LogHas(TEXT("Quest done")))
		{
			Fail(FString::Printf(TEXT("quests_accept: the cache's outcome is '%s' and the log %s 'Quest done'"), *Quests()->GetOutcome(State->Cache), LogHas(TEXT("Quest done")) ? TEXT("has") : TEXT("lacks")));
		}
		if (Quests()->GetStats().FinishedRows != 2 || Quests()->GetStats().OfferedRows != 0)
		{
			Fail(FString::Printf(TEXT("quests_accept: the window shows %d offers and %d finished, wanted 0 and 2"), Quests()->GetStats().OfferedRows, Quests()->GetStats().FinishedRows));
		}
		Notes.Add(FString::Printf(TEXT("quests_accept: steel %d -> %d, colonists %d -> %d"), State->SteelBefore, Steel, State->ColonistsBefore, Colonists()));
	};
	Phases.Add(MoveTemp(Accept));

	// 3. Decline a second wanderer; a new cache is left waiting for the picture.
	FPhase Decline;
	Decline.Name = TEXT("quests_decline");
	Decline.WarmupSeconds = 1.5f;
	Decline.MeasureSeconds = 1.0f;
	Decline.Setup = [=, this]()
	{
		UQuestPanelSubsystem* Q = Quests();
		if (!Q || State->Cache == 0)
		{
			return;
		}
		State->Declined = Offer("WandererAsks");
		State->ColonistsBefore = Colonists();
		if (State->Declined == 0 || !Q->Decline(State->Declined))
		{
			Fail(TEXT("quests_decline: no second wanderer to decline"));
			return;
		}
		if (Offer("SupplyCache") == 0)
		{
			Fail(TEXT("quests_decline: a second supply cache was not offered"));
		}
	};
	Decline.Verify = [=, this]()
	{
		if (StateOf(State->Declined) != 4)
		{
			Fail(FString::Printf(TEXT("quests_decline: the declined quest is in state %d, wanted 4"), StateOf(State->Declined)));
		}
		if (Colonists() != State->ColonistsBefore)
		{
			Fail(TEXT("quests_decline: someone joined after a decline"));
		}
		const FQuestPanelStats& Stats = Quests()->GetStats();
		if (Stats.OfferedRows != 1 || Stats.FinishedRows != 3 || !LogHas(TEXT("Quest declined")))
		{
			Fail(FString::Printf(TEXT("quests_decline: the window shows %d offers and %d finished (wanted 1 and 3), log %s 'Quest declined'"), Stats.OfferedRows, Stats.FinishedRows, LogHas(TEXT("Quest declined")) ? TEXT("has") : TEXT("lacks")));
		}
		Notes.Add(FString::Printf(TEXT("quests_decline: %d commands sent by the quest panel"), Stats.CommandsSent));
	};
	Phases.Add(MoveTemp(Decline));
}
