#include "QuestPanelSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"

#include <string>

namespace
{
	TAutoConsoleVariable<int32> GQuestShowUi(
		TEXT("Quests.UI"), 1, TEXT("1 shows the quest window when it is open, 0 hides it."), ECVF_Default);

	FAutoConsoleCommandWithWorld GQuestOpenCommand(
		TEXT("Quests.Open"), TEXT("Opens or closes the quest window (offers, quests under way, finished quests)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UQuestPanelSubsystem* Quests = World ? World->GetSubsystem<UQuestPanelSubsystem>() : nullptr)
			{
				Quests->SetOpen(!Quests->IsOpen());
			}
		}));

	// QuestView::state (colony::QuestState).
	constexpr uint8 QuestOffered = 0;
	constexpr uint8 QuestActive = 1;
	constexpr uint8 QuestCompleted = 2;
	constexpr uint8 QuestFailed = 3;
	constexpr uint8 QuestDeclined = 4;

	void QpText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	void QpTextColored(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		QpText(Text);
		ImGui::PopStyleColor();
	}

	void QpTextWrapped(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		ImGui::TextWrapped("%s", TCHAR_TO_UTF8(*Text));
		ImGui::PopStyleColor();
	}

	FString QpCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	ImGuiWindowFlags QpFlags()
	{
		return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	}

	/** "2 days", "5 h", "less than an hour". */
	FString QpSpan(int64 Seconds)
	{
		if (Seconds >= 2 * colony::kSecondsPerDay)
		{
			return FString::Printf(TEXT("%.1f days"), static_cast<double>(Seconds) / colony::kSecondsPerDay);
		}
		if (Seconds >= colony::kSecondsPerHour)
		{
			return FString::Printf(TEXT("%lld h"), static_cast<long long>(Seconds / colony::kSecondsPerHour));
		}
		return TEXT("less than an hour");
	}

	const char* QpStateWord(uint8 State)
	{
		switch (State)
		{
			case QuestOffered: return "waiting for an answer";
			case QuestActive: return "under way";
			case QuestCompleted: return "done";
			case QuestFailed: return "failed";
			case QuestDeclined: return "declined";
			default: return "lapsed";
		}
	}

	ImVec4 QpStateColor(uint8 State)
	{
		switch (State)
		{
			case QuestOffered: return ImVec4(0.95f, 0.85f, 0.35f, 1.0f);
			case QuestActive: return ImVec4(0.55f, 0.75f, 0.95f, 1.0f);
			case QuestCompleted: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
			case QuestFailed: return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
			default: return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
		}
	}

	const ImVec4 QpGrey(0.7f, 0.7f, 0.7f, 1.0f);
}

// ---- Setup -------------------------------------------------------------------------------------

bool UQuestPanelSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UQuestPanelSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UQuestPanelSubsystem, STATGROUP_Tickables);
}

void UQuestPanelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UQuestPanelSubsystem::OnSimEvent);
		EventSource = Bridge;
	}
}

void UQuestPanelSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = EventSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

USimSubsystem* UQuestPanelSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

bool UQuestPanelSubsystem::IsOverseerActive() const
{
	UWorld* World = GetWorld();
	const UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

void UQuestPanelSubsystem::SyncWithSim()
{
	// A new sim (a new game, a load) starts with a clean list. Checked wherever the sim's state comes in, since events
	// and clicks can come before the next tick.
	const USimSubsystem* Bridge = GetBridge();
	if (Bridge && ContentGeneration != Bridge->GetSimGeneration())
	{
		ContentGeneration = Bridge->GetSimGeneration();
		ResetForNewSim();
	}
}

void UQuestPanelSubsystem::ResetForNewSim()
{
	bOpen = false;
	bUnseenOffer = false;
	Outcomes.Reset();
	Stats.CommandsSent = 0;
}

void UQuestPanelSubsystem::Submit(colony::Command Command)
{
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge)
	{
		return;
	}
	Bridge->Submit(std::move(Command));
	++Stats.CommandsSent;
	// Commands are applied by the next step; while the game is paused, one step makes the change show at once.
	if (Bridge->GetSpeed() == 0)
	{
		Bridge->StepOnce();
	}
}

void UQuestPanelSubsystem::OnSimEvent(const colony::Event& Event)
{
	SyncWithSim();
	switch (Event.type)
	{
		case colony::EventType::QuestOffered:
			bUnseenOffer = !bOpen;
			break;
		case colony::EventType::QuestExpired:
		case colony::EventType::QuestCompleted:
		case colony::EventType::QuestFailed:
			Outcomes.Add(static_cast<uint32>(Event.count), UTF8_TO_TCHAR(Event.message.c_str()));
			break;
		default:
			break;
	}
}

// ---- What a click does -------------------------------------------------------------------------

void UQuestPanelSubsystem::SetOpen(bool bInOpen)
{
	SyncWithSim();
	bOpen = bInOpen;
	if (bOpen)
	{
		bUnseenOffer = false;
	}
}

const colony::QuestView* UQuestPanelSubsystem::FindQuest(uint32 Quest) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || Quest == 0)
	{
		return nullptr;
	}
	for (const colony::QuestView& View : Bridge->GetCurrentSnapshot().quests)
	{
		if (View.id == Quest)
		{
			return &View;
		}
	}
	return nullptr;
}

int32 UQuestPanelSubsystem::CountOffered() const
{
	const USimSubsystem* Bridge = GetBridge();
	int32 Count = 0;
	if (Bridge && Bridge->GetSim())
	{
		for (const colony::QuestView& View : Bridge->GetCurrentSnapshot().quests)
		{
			Count += View.state == QuestOffered ? 1 : 0;
		}
	}
	return Count;
}

int32 UQuestPanelSubsystem::CountActive() const
{
	const USimSubsystem* Bridge = GetBridge();
	int32 Count = 0;
	if (Bridge && Bridge->GetSim())
	{
		for (const colony::QuestView& View : Bridge->GetCurrentSnapshot().quests)
		{
			Count += View.state == QuestActive ? 1 : 0;
		}
	}
	return Count;
}

FString UQuestPanelSubsystem::GetOutcome(uint32 Quest) const
{
	const FString* Outcome = Outcomes.Find(Quest);
	return Outcome ? *Outcome : FString();
}

bool UQuestPanelSubsystem::Accept(uint32 Quest)
{
	const colony::QuestView* View = FindQuest(Quest);
	if (!View || View->state != QuestOffered)
	{
		return false;
	}
	colony::QuestCommand Command;
	Command.action = "accept";
	Command.quest = Quest;
	Submit(std::move(Command));
	return true;
}

bool UQuestPanelSubsystem::Decline(uint32 Quest)
{
	const colony::QuestView* View = FindQuest(Quest);
	if (!View || View->state != QuestOffered)
	{
		return false;
	}
	colony::QuestCommand Command;
	Command.action = "decline";
	Command.quest = Quest;
	Submit(std::move(Command));
	return true;
}

// ---- The tick and the window -------------------------------------------------------------------

void UQuestPanelSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	SyncWithSim();
	if (!bOpen || !IsOverseerActive() || GQuestShowUi.GetValueOnGameThread() == 0)
	{
		return;
	}
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (ShowUi && ShowUi->GetInt() == 0)
	{
		return;
	}
	DrawWindow(*Bridge);
}

void UQuestPanelSubsystem::DrawWindow(USimSubsystem& Bridge)
{
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;

	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(620.0f * Scale, 0.0f), ImVec2(620.0f * Scale, Viewport->Size.y * 0.8f));
	if (ImGui::Begin("##QuestWindow", nullptr, QpFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.WindowFrames;
		Stats.OfferedRows = 0;
		Stats.ActiveRows = 0;
		Stats.FinishedRows = 0;
		QpText(TEXT("Quests"));
		ImGui::SameLine();
		if (ImGui::SmallButton("Close"))
		{
			SetOpen(false);
			ImGui::End();
			return;
		}
		const std::vector<colony::QuestView>& Quests = Bridge.GetCurrentSnapshot().quests;

		ImGui::SeparatorText("Waiting for an answer");
		for (const colony::QuestView& Quest : Quests)
		{
			if (Quest.state == QuestOffered)
			{
				DrawQuest(Bridge, Quest, Scale);
				++Stats.OfferedRows;
			}
		}
		if (Stats.OfferedRows == 0)
		{
			QpTextColored(QpGrey, TEXT("No offers. Word comes now and then from travellers and neighbours."));
		}

		ImGui::SeparatorText("Under way");
		for (const colony::QuestView& Quest : Quests)
		{
			if (Quest.state == QuestActive)
			{
				DrawQuest(Bridge, Quest, Scale);
				++Stats.ActiveRows;
			}
		}
		if (Stats.ActiveRows == 0)
		{
			QpTextColored(QpGrey, TEXT("Nothing under way."));
		}

		// Finished ones, newest first (the sim keeps them in id order).
		int32 Finished = 0;
		for (const colony::QuestView& Quest : Quests)
		{
			Finished += Quest.state != QuestOffered && Quest.state != QuestActive ? 1 : 0;
		}
		if (Finished > 0 && ImGui::CollapsingHeader(TCHAR_TO_UTF8(*FString::Printf(TEXT("Finished (%d)"), Finished)), ImGuiTreeNodeFlags_DefaultOpen))
		{
			for (auto It = Quests.rbegin(); It != Quests.rend(); ++It)
			{
				if (It->state != QuestOffered && It->state != QuestActive)
				{
					DrawQuest(Bridge, *It, Scale);
					++Stats.FinishedRows;
				}
			}
		}
	}
	ImGui::End();
}

void UQuestPanelSubsystem::DrawQuest(USimSubsystem& Bridge, const colony::QuestView& Quest, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Def* Def = Sim.defs().find("Quests", Quest.kind);
	const int64 Now = Bridge.GetGameSeconds();
	const std::string Type = Def ? Def->text("questType") : std::string();

	ImGui::PushID(static_cast<int>(Quest.id));
	QpText(QpCapital(Quest.label));
	ImGui::SameLine();
	QpTextColored(QpStateColor(Quest.state), FString::Printf(TEXT("  %s"), UTF8_TO_TCHAR(QpStateWord(Quest.state))));

	if (Quest.state == QuestOffered || Quest.state == QuestActive)
	{
		if (Def)
		{
			QpTextWrapped(QpGrey, UTF8_TO_TCHAR(Def->text("text").c_str()));
		}
		// What comes of it.
		const colony::Def* Faction = Def ? Sim.defs().find("Factions", Def->text("faction", "Pirate")) : nullptr;
		const FString Raiders = Faction ? UTF8_TO_TCHAR(Faction->label().c_str()) : TEXT("raiders");
		if (Type == "Refugees")
		{
			QpText(FString::Printf(TEXT("%d %s join the colony; %s follow with a raid of %.0f points."), Quest.people, Quest.people == 1 ? TEXT("person") : TEXT("people"), *Raiders, Quest.points));
		}
		else if (Type == "Wanderer")
		{
			QpText(TEXT("One person joins the colony."));
		}
		else if (Type == "Stash")
		{
			QpText(TEXT("The goods land near the colony at once."));
		}
		else if (Type == "Defend")
		{
			QpText(FString::Printf(TEXT("%s send a raid of %.0f points; beat it within a day."), *Raiders, Quest.points));
		}
		if (!Quest.rewards.empty())
		{
			FString Rewards;
			for (const std::pair<std::string, int>& Reward : Quest.rewards)
			{
				const colony::Def* Thing = Sim.defs().find("Things", Reward.first);
				Rewards += FString::Printf(TEXT("%s%d %s"), Rewards.IsEmpty() ? TEXT("") : TEXT(", "), Reward.second,
					Thing ? UTF8_TO_TCHAR(Thing->label().c_str()) : UTF8_TO_TCHAR(Reward.first.c_str()));
			}
			QpTextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), FString::Printf(TEXT("Reward: %s"), *Rewards));
		}
	}

	if (Quest.state == QuestOffered)
	{
		QpTextColored(QpGrey, FString::Printf(TEXT("The offer stands for %s."), *QpSpan(FMath::Max<int64>(0, Quest.expiresAt - Now))));
		if (ImGui::Button("Accept"))
		{
			Accept(Quest.id);
		}
		ImGui::SameLine();
		if (ImGui::Button("Decline"))
		{
			Decline(Quest.id);
		}
	}
	else if (Quest.state == QuestActive && Quest.deadline > 0)
	{
		// The raid comes `deadlineHours` before the deadline.
		const int64 LaunchAt = Quest.deadline - static_cast<int64>((Def ? Def->number("deadlineHours", 24.0) : 24.0) * colony::kSecondsPerHour);
		if (Now < LaunchAt)
		{
			QpTextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), FString::Printf(TEXT("The raiders come in about %s."), *QpSpan(LaunchAt - Now)));
		}
		else
		{
			QpTextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f), FString::Printf(TEXT("The raid is on. Beat it within %s."), *QpSpan(FMath::Max<int64>(0, Quest.deadline - Now))));
		}
	}
	else if (Quest.state != QuestActive)
	{
		const FString Outcome = GetOutcome(Quest.id);
		if (!Outcome.IsEmpty() && Outcome != UTF8_TO_TCHAR(Quest.kind.c_str()))
		{
			QpTextColored(QpGrey, QpCapital(TCHAR_TO_UTF8(*Outcome)) + TEXT("."));
		}
	}
	ImGui::Separator();
	ImGui::PopID();
}
