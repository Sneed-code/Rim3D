#include "PanelsSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "QuestPanelSubsystem.h"
#include "ShipPanelSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Bills.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/JobType.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace
{
	constexpr int32 MaxLogLines = 300;

	TAutoConsoleVariable<int32> GPanelsShowUi(
		TEXT("Panels.UI"), 1, TEXT("1 shows the colony panels (work and schedule tabs, colonist and bill panels, message log), 0 hides them."), ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs GPanelsTabCommand(
		TEXT("Panels.Tab"), TEXT("Panels.Tab 0|1|2 opens no tab, the Work tab or the Schedule tab."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPanelsSubsystem* Panels = World ? World->GetSubsystem<UPanelsSubsystem>() : nullptr)
			{
				const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
				Panels->SetTab(Index == 1 ? EColonyTab::Work : (Index == 2 ? EColonyTab::Schedule : EColonyTab::None));
			}
		}));

	void PText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	void PText(const std::string& Text)
	{
		ImGui::TextUnformatted(Text.c_str(), Text.c_str() + Text.size());
	}

	void PTextColored(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		PText(Text);
		ImGui::PopStyleColor();
	}

	FString PCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	int32 PId(colony::Entity Id)
	{
		return static_cast<int32>(entt::to_integral(Id));
	}

	bool PButton(const FString& Label, bool bActive)
	{
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.75f, 1.0f));
		}
		const bool bClicked = ImGui::Button(TCHAR_TO_UTF8(*Label));
		if (bActive)
		{
			ImGui::PopStyleColor();
		}
		return bClicked;
	}

	ImVec4 MoodColor(int32 Band)
	{
		switch (Band)
		{
			case 0: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
			case 1: return ImVec4(0.95f, 0.85f, 0.35f, 1.0f);
			case 2: return ImVec4(0.95f, 0.60f, 0.25f, 1.0f);
			default: return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
		}
	}

	ImVec4 SeverityColor(uint8 Severity)
	{
		switch (Severity)
		{
			case 1: return ImVec4(0.95f, 0.65f, 0.30f, 1.0f);
			case 2: return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
			case 3: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
			default: return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
		}
	}

	FString TimeText(int64 Seconds)
	{
		return FString::Printf(TEXT("Day %lld %02d:%02d"), static_cast<long long>(Seconds / colony::kSecondsPerDay + 1),
			static_cast<int32>((Seconds % colony::kSecondsPerDay) / colony::kSecondsPerHour),
			static_cast<int32>((Seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute));
	}

	const char* ScheduleLetter(colony::ScheduleKind Kind)
	{
		switch (Kind)
		{
			case colony::ScheduleKind::Anything: return "A";
			case colony::ScheduleKind::Work: return "W";
			case colony::ScheduleKind::Joy: return "J";
			default: return "S";
		}
	}

	ImVec4 ScheduleColor(colony::ScheduleKind Kind)
	{
		switch (Kind)
		{
			case colony::ScheduleKind::Anything: return ImVec4(0.38f, 0.38f, 0.40f, 1.0f);
			case colony::ScheduleKind::Work: return ImVec4(0.25f, 0.55f, 0.30f, 1.0f);
			case colony::ScheduleKind::Joy: return ImVec4(0.75f, 0.62f, 0.15f, 1.0f);
			default: return ImVec4(0.22f, 0.35f, 0.70f, 1.0f);
		}
	}

	const char* ScheduleWord(colony::ScheduleKind Kind)
	{
		switch (Kind)
		{
			case colony::ScheduleKind::Anything: return "Anything";
			case colony::ScheduleKind::Work: return "Work";
			case colony::ScheduleKind::Joy: return "Joy";
			default: return "Sleep";
		}
	}

	ImVec4 PriorityColor(int32 Priority)
	{
		switch (Priority)
		{
			case 1: return ImVec4(0.22f, 0.62f, 0.28f, 1.0f);
			case 2: return ImVec4(0.38f, 0.58f, 0.28f, 1.0f);
			case 3: return ImVec4(0.52f, 0.52f, 0.30f, 1.0f);
			case 4: return ImVec4(0.50f, 0.42f, 0.30f, 1.0f);
			default: return ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
		}
	}

	const char* BillModeWord(colony::BillMode Mode)
	{
		switch (Mode)
		{
			case colony::BillMode::Count: return "Do a number of times";
			case colony::BillMode::UntilHave: return "Do until you have";
			default: return "Do forever";
		}
	}
}

// ---- Setup -------------------------------------------------------------------------------------

bool UPanelsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UPanelsSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPanelsSubsystem, STATGROUP_Tickables);
}

void UPanelsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UPanelsSubsystem::OnSimEvent);
		EventSource = Bridge;
	}
}

void UPanelsSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = EventSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

USimSubsystem* UPanelsSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

bool UPanelsSubsystem::IsOverseerActive() const
{
	UWorld* World = GetWorld();
	const UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

void UPanelsSubsystem::ResetForNewSim()
{
	Log.Reset();
	Stats.CommandsSent = 0;
	NewBillBuilding = colony::kNoEntity;
	NewRecipe = 0;
	ThermoBuilding = colony::kNoEntity;
}

void UPanelsSubsystem::Submit(colony::Command Command)
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

FString UPanelsSubsystem::NameOf(colony::Entity Pawn) const
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::Colonist* Colonist = Bridge && Bridge->GetSim() ? Bridge->GetSim()->colonistOf(Pawn) : nullptr;
	if (!Colonist)
	{
		return TEXT("Someone");
	}
	return UTF8_TO_TCHAR((Colonist->nickName.empty() ? Colonist->firstName : Colonist->nickName).c_str());
}

void UPanelsSubsystem::GetColonists(TArray<colony::Entity>& Out) const
{
	Out.Reset();
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && Bridge->GetSim()->colonistOf(View.id))
		{
			Out.Add(View.id);
		}
	}
}

// ---- The message log ---------------------------------------------------------------------------

void UPanelsSubsystem::AddLog(const FString& Text, uint8 Severity, colony::Entity Subject)
{
	const USimSubsystem* Bridge = GetBridge();
	FColonyLogLine Line;
	Line.GameSeconds = Bridge ? Bridge->GetGameSeconds() : 0;
	Line.Text = Text;
	Line.Severity = Severity;
	Line.Subject = Subject;
	Log.Add(MoveTemp(Line));
	if (Log.Num() > MaxLogLines)
	{
		Log.RemoveAt(0, Log.Num() - MaxLogLines);
	}
}

void UPanelsSubsystem::OnSimEvent(const colony::Event& Event)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const bool bColonist = Sim.colonistOf(Event.thing) != nullptr;
	switch (Event.type)
	{
		case colony::EventType::CommandFailed:
			AddLog(FString::Printf(TEXT("Refused: %s"), UTF8_TO_TCHAR(Event.message.c_str())), 1);
			break;
		case colony::EventType::PawnDied:
			if (bColonist)
			{
				AddLog(FString::Printf(TEXT("%s died (%s)."), *NameOf(Event.thing), UTF8_TO_TCHAR(Event.message.c_str())), 2, Event.thing);
			}
			break;
		case colony::EventType::PawnDowned:
			if (bColonist)
			{
				AddLog(FString::Printf(TEXT("%s is down."), *NameOf(Event.thing)), 2, Event.thing);
			}
			break;
		case colony::EventType::PawnGotUp:
			if (bColonist)
			{
				AddLog(FString::Printf(TEXT("%s got back up."), *NameOf(Event.thing)), 3, Event.thing);
			}
			break;
		case colony::EventType::PawnInjured:
			if (bColonist && Event.def.valid())
			{
				AddLog(FString::Printf(TEXT("%s was hurt: %s."), *NameOf(Event.thing), UTF8_TO_TCHAR(Sim.defs().get(Event.def).label().c_str())), 1, Event.thing);
			}
			break;
		case colony::EventType::PawnPartLost:
			if (bColonist)
			{
				AddLog(FString::Printf(TEXT("%s lost a body part."), *NameOf(Event.thing)), 2, Event.thing);
			}
			break;
		case colony::EventType::PawnMoodBand:
			if (bColonist)
			{
				const bool bStable = Event.message == "stable";
				AddLog(bStable ? FString::Printf(TEXT("%s has calmed down."), *NameOf(Event.thing))
							   : FString::Printf(TEXT("%s is close to a %s mental break (mood %.0f)."), *NameOf(Event.thing), UTF8_TO_TCHAR(Event.message.c_str()), Event.amount),
					bStable ? 3 : (Event.message == "minor" ? 1 : 2), Event.thing);
			}
			break;
		case colony::EventType::RecipeDone:
		{
			const FString What = Event.def.valid() ? UTF8_TO_TCHAR(Sim.defs().get(Event.def).label().c_str()) : TEXT("a bill");
			AddLog(FString::Printf(TEXT("%s finished: %s%s."), *NameOf(Event.thing), *What, Event.message.empty() ? TEXT("") : TEXT(" (bill done)")), 3, Event.thing);
			break;
		}
		case colony::EventType::QuestOffered:
		case colony::EventType::QuestAccepted:
		case colony::EventType::QuestDeclined:
		case colony::EventType::QuestExpired:
		case colony::EventType::QuestCompleted:
		case colony::EventType::QuestFailed:
		{
			// Quests (docs/design/quests.md); the quest window is UQuestPanelSubsystem.
			const FString What = PCapital(Event.def.valid() ? Sim.defs().get(Event.def).label() : Event.message);
			const FString Why = UTF8_TO_TCHAR(Event.message.c_str());
			switch (Event.type)
			{
				case colony::EventType::QuestOffered: AddLog(FString::Printf(TEXT("Quest offered: %s. Open Quests to answer."), *What), 0); break;
				case colony::EventType::QuestAccepted: AddLog(FString::Printf(TEXT("Quest accepted: %s."), *What), 0); break;
				case colony::EventType::QuestDeclined: AddLog(FString::Printf(TEXT("Quest declined: %s."), *What), 0); break;
				case colony::EventType::QuestExpired: AddLog(FString::Printf(TEXT("Quest lapsed: %s."), *What), 1); break;
				case colony::EventType::QuestCompleted: AddLog(FString::Printf(TEXT("Quest done: %s (%s)."), *What, *Why), 3); break;
				default: AddLog(FString::Printf(TEXT("Quest failed: %s (%s)."), *What, *Why), 2); break;
			}
			break;
		}
		// The spaceship ending (docs/design/ship.md); the ship window is UShipPanelSubsystem.
		case colony::EventType::ShipReactorStarted:
			AddLog(FString::Printf(TEXT("The ship reactor is starting. It needs %.0f days, and raiders will come for it."), Event.amount), 1, Event.thing);
			break;
		case colony::EventType::ShipWave:
			AddLog(FString::Printf(TEXT("Raiders are coming for the ship reactor (wave %.0f)."), Event.amount), 2, Event.thing);
			break;
		case colony::EventType::ShipReactorReady:
			AddLog(TEXT("The ship reactor is active. Open Ship to launch."), 3, Event.thing);
			break;
		case colony::EventType::ShipReactorLost:
			AddLog(TEXT("The ship reactor is lost. It must be started again."), 2);
			break;
		case colony::EventType::ShipLaunched:
			AddLog(FString::Printf(TEXT("The ship has launched with %.0f aboard."), Event.amount), 3);
			break;
		default:
			break;
	}
}

// ---- What a click does -------------------------------------------------------------------------

bool UPanelsSubsystem::CyclePriority(colony::Entity Pawn, int32 WorkIndex, bool bForward)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Bridge->GetSim()->things().exists(Pawn))
	{
		return false;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::WorkSettings* Settings = Sim.things().registry().try_get<colony::WorkSettings>(Pawn);
	if (!Settings || WorkIndex < 0 || WorkIndex >= static_cast<int32>(Settings->entries.size()))
	{
		return false;
	}
	const colony::WorkPriority& Entry = Settings->entries[WorkIndex];
	if (Entry.disabled)
	{
		return false;
	}
	const int32 Count = colony::JobSystem::kMaxPriority + 1;
	const int32 Next = (static_cast<int32>(Entry.priority) + (bForward ? 1 : Count - 1)) % Count;
	Submit(colony::SetWorkPriorityCommand{Pawn, Sim.defs().get(Entry.workType).defName(), Next});
	return true;
}

bool UPanelsSubsystem::CycleSchedule(colony::Entity Pawn, int32 Hour, bool bForward)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Bridge->GetSim()->things().exists(Pawn) || Hour < 0 || Hour > 23)
	{
		return false;
	}
	const colony::Schedule* Schedule = Bridge->GetSim()->things().registry().try_get<colony::Schedule>(Pawn);
	if (!Schedule)
	{
		return false;
	}
	const int32 Kinds = 4;
	const int32 Next = (static_cast<int32>(Schedule->hours[Hour]) + (bForward ? 1 : Kinds - 1)) % Kinds;
	Submit(colony::SetScheduleCommand{Pawn, Hour, static_cast<colony::ScheduleKind>(Next)});
	return true;
}

bool UPanelsSubsystem::AddBill(colony::Entity Building, const FString& Recipe, colony::BillMode Mode, int32 Count)
{
	if (Building == colony::kNoEntity || Recipe.IsEmpty())
	{
		return false;
	}
	Submit(colony::AddBillCommand{Building, std::string(TCHAR_TO_UTF8(*Recipe)), Mode, Count});
	return true;
}

bool UPanelsSubsystem::RemoveBill(colony::Entity Building, uint32 BillId)
{
	if (Building == colony::kNoEntity || BillId == 0)
	{
		return false;
	}
	Submit(colony::RemoveBillCommand{Building, BillId});
	return true;
}

bool UPanelsSubsystem::ChangeBill(colony::Entity Building, uint32 BillId, colony::BillMode Mode, int32 Count, bool bSuspended)
{
	if (Building == colony::kNoEntity || BillId == 0)
	{
		return false;
	}
	Submit(colony::SetBillCommand{Building, BillId, Mode, Count, bSuspended});
	return true;
}

bool UPanelsSubsystem::AssignBed(colony::Entity Bed, colony::Entity Pawn)
{
	if (Bed == colony::kNoEntity)
	{
		return false;
	}
	Submit(colony::AssignBedCommand{Bed, Pawn});
	return true;
}

bool UPanelsSubsystem::SetThermostat(colony::Entity Building, float Celsius, bool bClear)
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge ? Bridge->GetCurrentSnapshot().find(Building) : nullptr;
	if (!View || !Bridge->GetSim() || !Bridge->GetSim()->defs().get(View->def).flag("heatPusher.adjustable"))
	{
		return false;
	}
	const colony::TemperatureNumbers& Numbers = Bridge->GetSim()->temperature().numbers();
	Submit(colony::SetThermostatCommand{Building, FMath::Clamp(Celsius, Numbers.thermostatMinC, Numbers.thermostatMaxC), bClear});
	return true;
}

// ---- The tick and the panels -------------------------------------------------------------------

void UPanelsSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (ContentGeneration != Bridge->GetSimGeneration())
	{
		ContentGeneration = Bridge->GetSimGeneration();
		ResetForNewSim();
	}
	if (!IsOverseerActive() || GPanelsShowUi.GetValueOnGameThread() == 0)
	{
		return;
	}
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (ShowUi && ShowUi->GetInt() == 0)
	{
		return;
	}
	DrawPanels(*Bridge);
}

void UPanelsSubsystem::DrawPanels(USimSubsystem& Bridge)
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
	const float Margin = 12.0f * Scale;

	float MenuBottom = Margin;
	DrawMenuAndColonistBar(Bridge, Scale, Margin, MenuBottom);

	if (Tab == EColonyTab::Work)
	{
		DrawWorkTab(Bridge, Scale);
	}
	else if (Tab == EColonyTab::Schedule)
	{
		DrawScheduleTab(Bridge, Scale);
	}

	// What is selected: a colonist's panel, or a work building's bills.
	const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	if (Overseer && Overseer->GetSelection().Kind == ESimPickKind::Thing)
	{
		if (const colony::ThingView* View = Bridge.GetCurrentSnapshot().find(Overseer->GetSelection().Thing))
		{
			const float Left = Margin;
			const float Top = MenuBottom + 6.0f * Scale;
			if (View->category == colony::ThingCategory::Pawn && Bridge.GetSim()->colonistOf(View->id))
			{
				DrawColonistPanel(Bridge, *View, Left, Top, Scale);
			}
			else if (View->category == colony::ThingCategory::Building && View->stage == colony::ConstructionStage::Built)
			{
				if (Bridge.GetSim()->defs().get(View->def).flag("heatPusher.adjustable"))
				{
					DrawThermostatPanel(Bridge, *View, Left, Top, Scale);
				}
				else
				{
					DrawBillsPanel(Bridge, *View, Left, Top, Scale);
				}
			}
		}
	}

	DrawLog(Bridge, Scale, Margin);
	++Stats.FramesDrawn;
}

namespace
{
	ImGuiWindowFlags PanelFlags()
	{
		return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	}
}

void UPanelsSubsystem::DrawMenuAndColonistBar(USimSubsystem& Bridge, float Scale, float Margin, float& OutBottom)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Margin, Viewport->Pos.y + Margin), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x * 0.5f, FLT_MAX));
	if (ImGui::Begin("##PanelsMenu", nullptr, PanelFlags()))
	{
		if (PButton(TEXT("Work"), Tab == EColonyTab::Work))
		{
			Tab = Tab == EColonyTab::Work ? EColonyTab::None : EColonyTab::Work;
		}
		ImGui::SameLine();
		if (PButton(TEXT("Schedule"), Tab == EColonyTab::Schedule))
		{
			Tab = Tab == EColonyTab::Schedule ? EColonyTab::None : EColonyTab::Schedule;
		}
		ImGui::SameLine();
		if (PButton(TEXT("Log"), bLogOpen))
		{
			bLogOpen = !bLogOpen;
		}
		if (UQuestPanelSubsystem* Quests = GetWorld()->GetSubsystem<UQuestPanelSubsystem>())
		{
			// "Quests (2)" while offers wait for an answer; amber until the window has been opened since the newest one.
			ImGui::SameLine();
			const int32 Waiting = Quests->CountOffered();
			const bool bUnseen = Quests->HasUnseenOffer();
			if (bUnseen)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.55f, 0.15f, 1.0f));
			}
			if (PButton(Waiting > 0 ? FString::Printf(TEXT("Quests (%d)"), Waiting) : FString(TEXT("Quests")), Quests->IsOpen()))
			{
				Quests->SetOpen(!Quests->IsOpen());
			}
			if (bUnseen)
			{
				ImGui::PopStyleColor();
			}
		}
		UShipPanelSubsystem* Ship = GetWorld()->GetSubsystem<UShipPanelSubsystem>();
		if (Ship && Ship->HasShip())
		{
			// "Ship (12.5 days)" while the reactor warms up; green once it can launch.
			ImGui::SameLine();
			const colony::ShipView& ShipView = Bridge.GetCurrentSnapshot().ship;
			const bool bReady = ShipView.canLaunch;
			if (bReady)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.6f, 0.3f, 1.0f));
			}
			const FString Label = ShipView.phase == 1
				? FString::Printf(TEXT("Ship (%.1f days)"), static_cast<double>(ShipView.activationLeft) / colony::kSecondsPerDay)
				: FString(TEXT("Ship"));
			if (PButton(Label, Ship->IsOpen()))
			{
				Ship->SetOpen(!Ship->IsOpen());
			}
			if (bReady)
			{
				ImGui::PopStyleColor();
			}
		}

		// The colonists, coloured by mood; downed ones are grey.
		TArray<colony::Entity> Colonists;
		GetColonists(Colonists);
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		for (const colony::Entity Pawn : Colonists)
		{
			const colony::ThingView* View = Snapshot.find(Pawn);
			if (!View)
			{
				continue;
			}
			const bool bSelected = Overseer && Overseer->GetSelection().Kind == ESimPickKind::Thing && Overseer->GetSelection().Thing == Pawn;
			ImGui::PushID(PId(Pawn));
			ImGui::PushStyleColor(ImGuiCol_Text, View->downed ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : MoodColor(View->moodBand));
			if (PButton(NameOf(Pawn), bSelected) && Overseer)
			{
				FSimPick Pick;
				Pick.Kind = ESimPickKind::Thing;
				Pick.Thing = Pawn;
				Overseer->Select(Pick);
			}
			if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && Overseer)
			{
				Overseer->FocusOnSelection();
			}
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
			{
				const FString Tip = View->mood >= 0.0f ? FString::Printf(TEXT("Mood %.0f   Food %.0f%%   Rest %.0f%%\nClick to select, double click to centre"), View->mood, View->food * 100.0f, View->rest * 100.0f) : FString(TEXT("Click to select"));
				ImGui::SetTooltip("%s", TCHAR_TO_UTF8(*Tip));
			}
			ImGui::PopID();
			ImGui::SameLine();
		}
		ImGui::NewLine();
		OutBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - Viewport->Pos.y;
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawWorkTab(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const auto& WorkTypes = Sim.jobs().workTypes();
	// Only work that has a scanner does anything yet.
	std::vector<int32> Columns;
	for (int32 Index = 0; Index < static_cast<int32>(WorkTypes.size()); ++Index)
	{
		if (!WorkTypes[Index].givers.empty())
		{
			Columns.push_back(Index);
		}
	}
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x * 0.8f, Viewport->Size.y * 0.6f));
	if (ImGui::Begin("##PanelsWork", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.WorkTabFrames;
		PText(TEXT("Work"));
		ImGui::SameLine();
		PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), TEXT("  1 is done first, 4 last, - is never.  Left click raises the number, right click lowers it.  X: can never do this."));
		TArray<colony::Entity> Colonists;
		GetColonists(Colonists);
		if (Colonists.IsEmpty() || Columns.empty())
		{
			PText(TEXT("No colonists or no work to set."));
		}
		else if (ImGui::BeginTable("##WorkTable", static_cast<int>(Columns.size()) + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Colonist");
			for (const int32 Index : Columns)
			{
				ImGui::TableSetupColumn(TCHAR_TO_UTF8(*PCapital(Sim.defs().get(WorkTypes[Index].def).label())));
			}
			ImGui::TableHeadersRow();
			for (const colony::Entity Pawn : Colonists)
			{
				const colony::WorkSettings* Settings = Sim.things().registry().try_get<colony::WorkSettings>(Pawn);
				const colony::Colonist* Colonist = Sim.colonistOf(Pawn);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				PText(NameOf(Pawn));
				int32 Column = 0;
				for (const int32 Index : Columns)
				{
					ImGui::TableNextColumn();
					++Column;
					ImGui::PushID(PId(Pawn) * 64 + Index);
					if (!Settings || Index >= static_cast<int32>(Settings->entries.size()))
					{
						PText(TEXT("."));
					}
					else if (Settings->entries[Index].disabled)
					{
						PTextColored(ImVec4(0.5f, 0.3f, 0.3f, 1.0f), TEXT(" X "));
					}
					else
					{
						const int32 Priority = Settings->entries[Index].priority;
						ImGui::PushStyleColor(ImGuiCol_Button, PriorityColor(Priority));
						const FString Label = Priority == 0 ? FString(TEXT("-")) : FString::FromInt(Priority);
						ImGui::Button(TCHAR_TO_UTF8(*Label), ImVec2(30.0f * Scale, 0.0f));
						ImGui::PopStyleColor();
						if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
						{
							CyclePriority(Pawn, Index, true);
						}
						else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
						{
							CyclePriority(Pawn, Index, false);
						}
						if (ImGui::IsItemHovered())
						{
							const colony::Def& WorkDef = Sim.defs().get(WorkTypes[Index].def);
							FString Tip = FString::Printf(TEXT("%s: %s"), *PCapital(WorkDef.label()), Priority == 0 ? TEXT("never") : *FString::Printf(TEXT("priority %d"), Priority));
							const std::string Skill = WorkDef.textAt("relevantSkills", 0);
							if (!Skill.empty() && Colonist)
							{
								if (const colony::SkillRecord* Record = Colonist->skill(Skill))
								{
									const colony::Def* SkillDef = Sim.defs().find("Skills", Skill);
									Tip += FString::Printf(TEXT("\n%s %d"), SkillDef ? *PCapital(SkillDef->label()) : *PCapital(Skill), Record->level);
								}
							}
							ImGui::SetTooltip("%s", TCHAR_TO_UTF8(*Tip));
						}
					}
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawScheduleTab(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const int32 Now = Sim.time().hourOfDay();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x * 0.9f, Viewport->Size.y * 0.6f));
	if (ImGui::Begin("##PanelsSchedule", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.ScheduleTabFrames;
		PText(TEXT("Schedule"));
		ImGui::SameLine();
		PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), TEXT("  Click an hour to change it, right click to go back.  "));
		for (const colony::ScheduleKind Kind : {colony::ScheduleKind::Anything, colony::ScheduleKind::Work, colony::ScheduleKind::Joy, colony::ScheduleKind::Sleep})
		{
			ImGui::SameLine();
			PTextColored(ScheduleColor(Kind), FString::Printf(TEXT("%s = %s"), UTF8_TO_TCHAR(ScheduleLetter(Kind)), UTF8_TO_TCHAR(ScheduleWord(Kind))));
		}
		TArray<colony::Entity> Colonists;
		GetColonists(Colonists);
		if (Colonists.IsEmpty())
		{
			PText(TEXT("No colonists."));
		}
		else if (ImGui::BeginTable("##ScheduleTable", 25, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Colonist");
			for (int32 Hour = 0; Hour < 24; ++Hour)
			{
				ImGui::TableSetupColumn(TCHAR_TO_UTF8(*(Hour == Now ? FString::Printf(TEXT(">%d<"), Hour) : FString::FromInt(Hour))));
			}
			ImGui::TableHeadersRow();
			for (const colony::Entity Pawn : Colonists)
			{
				const colony::Schedule* Schedule = Sim.things().registry().try_get<colony::Schedule>(Pawn);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				PText(NameOf(Pawn));
				for (int32 Hour = 0; Hour < 24; ++Hour)
				{
					ImGui::TableNextColumn();
					ImGui::PushID(PId(Pawn) * 32 + Hour);
					if (!Schedule)
					{
						PText(TEXT("."));
					}
					else
					{
						const colony::ScheduleKind Kind = Schedule->hours[Hour];
						ImGui::PushStyleColor(ImGuiCol_Button, ScheduleColor(Kind));
						ImGui::Button(ScheduleLetter(Kind), ImVec2(26.0f * Scale, 0.0f));
						ImGui::PopStyleColor();
						if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
						{
							CycleSchedule(Pawn, Hour, true);
						}
						else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
						{
							CycleSchedule(Pawn, Hour, false);
						}
						if (ImGui::IsItemHovered())
						{
							ImGui::SetTooltip("%02d:00  %s", Hour, ScheduleWord(Kind));
						}
					}
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawColonistPanel(USimSubsystem& Bridge, const colony::ThingView& View, float Left, float Top, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Colonist& Colonist = *Sim.colonistOf(View.id);
	const colony::DefDatabase& Defs = Sim.defs();
	const int64 Now = Sim.time().seconds;

	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Left, Viewport->Pos.y + Top), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * Scale, 0.0f), ImVec2(460.0f * Scale, FMath::Max(200.0f * Scale, Viewport->Size.y - Top - 330.0f * Scale)));
	if (ImGui::Begin("##PanelsColonist", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.ColonistPanelFrames;
		// Who.
		FString Full = UTF8_TO_TCHAR(Colonist.firstName.c_str());
		if (!Colonist.nickName.empty())
		{
			Full += FString::Printf(TEXT(" '%s'"), UTF8_TO_TCHAR(Colonist.nickName.c_str()));
		}
		Full += FString::Printf(TEXT(" %s"), UTF8_TO_TCHAR(Colonist.lastName.c_str()));
		PText(Full);
		PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), FString::Printf(TEXT("%s, %d"), Colonist.female ? TEXT("Female") : TEXT("Male"), Colonist.age));
		const auto BackstoryLabel = [&](const std::string& Name) -> FString
		{
			const colony::Def* Def = Defs.find("Backstories", Name);
			return PCapital(Def ? Def->label() : Name);
		};
		if (!Colonist.childhood.empty() || !Colonist.adulthood.empty())
		{
			PText(FString::Printf(TEXT("%s, %s"), *BackstoryLabel(Colonist.childhood), *BackstoryLabel(Colonist.adulthood)));
		}
		if (!Colonist.traits.empty())
		{
			FString Traits;
			for (const colony::TraitRecord& Trait : Colonist.traits)
			{
				const colony::Def* Def = Defs.find("Traits", Trait.def);
				FString Label = Def ? FString(UTF8_TO_TCHAR(Def->label().c_str())) : FString(UTF8_TO_TCHAR(Trait.def.c_str()));
				for (size_t Index = 0; Def && Index < Def->count("degreeDatas"); ++Index)
				{
					const std::string Base = "degreeDatas." + std::to_string(Index);
					if (static_cast<int>(Def->integer((Base + ".degree").c_str(), 0)) == Trait.degree)
					{
						Label = UTF8_TO_TCHAR(Def->text((Base + ".label").c_str(), Def->label()).c_str());
					}
				}
				Traits += (Traits.IsEmpty() ? TEXT("") : TEXT(", ")) + Label;
			}
			PText(PCapital(std::string(TCHAR_TO_UTF8(*Traits))));
		}
		ImGui::Separator();

		// What they are doing.
		FString Doing = View.dead ? TEXT("Dead") : (View.downed ? TEXT("Down") : (View.asleep ? TEXT("Asleep") : (View.drafted ? TEXT("Drafted") : UTF8_TO_TCHAR(colony::jobTypeName(View.job)))));
		PText(FString::Printf(TEXT("Doing: %s"), *Doing));

		// Needs and mood.
		const auto Bar = [&](const char* Name, float Fraction, const FString& Overlay)
		{
			PText(FString(UTF8_TO_TCHAR(Name)));
			ImGui::SameLine(80.0f * Scale);
			ImGui::ProgressBar(FMath::Clamp(Fraction, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), TCHAR_TO_UTF8(*Overlay));
		};
		if (View.food >= 0.0f)
		{
			Bar("Food", View.food, FString::Printf(TEXT("%.0f%%"), View.food * 100.0f));
			Bar("Rest", View.rest, FString::Printf(TEXT("%.0f%%"), View.rest * 100.0f));
		}
		if (View.mood >= 0.0f)
		{
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, MoodColor(View.moodBand));
			Bar("Mood", View.mood / 100.0f, FString::Printf(TEXT("%.0f"), View.mood));
			ImGui::PopStyleColor();
			float Minor = 0.0f;
			float Major = 0.0f;
			float Extreme = 0.0f;
			Sim.mood().breakLevels(View.id, Minor, Major, Extreme);
			PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), FString::Printf(TEXT("Breaks below %.0f (minor), %.0f (major), %.0f (extreme)"), Minor, Major, Extreme));
			const std::vector<colony::ThoughtView> Thoughts = Sim.mood().thoughts(View.id, Now);
			for (const colony::ThoughtView& Thought : Thoughts)
			{
				FString Line = FString::Printf(TEXT("%+.0f  %s"), Thought.offset, *PCapital(Thought.label));
				if (!Thought.situation && Thought.expires > Now)
				{
					const double Hours = static_cast<double>(Thought.expires - Now) / static_cast<double>(colony::kSecondsPerHour);
					Line += Hours >= 24.0 ? FString::Printf(TEXT("  (%.1f days)"), Hours / 24.0) : FString::Printf(TEXT("  (%.0f h)"), Hours);
				}
				PTextColored(Thought.offset < 0.0f ? ImVec4(0.95f, 0.5f, 0.45f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), Line);
			}
		}

		// Bed.
		const colony::Entity BedOwned = Sim.jobs().bedOf(View.id);
		if (View.food >= 0.0f)
		{
			ImGui::Separator();
			const colony::ThingView* BedView = BedOwned == colony::kNoEntity ? nullptr : Bridge.GetCurrentSnapshot().find(BedOwned);
			if (BedView)
			{
				PText(FString::Printf(TEXT("Bed: %s at %d, %d"), *PCapital(Defs.get(BedView->def).label()), FMath::FloorToInt32(BedView->x), FMath::FloorToInt32(BedView->y)));
				ImGui::SameLine();
				if (ImGui::SmallButton("Free it"))
				{
					AssignBed(BedOwned, colony::kNoEntity);
				}
			}
			else
			{
				PText(TEXT("Bed: none (sleeps on the ground)"));
			}
			if (ImGui::BeginCombo("##AssignBed", "Assign a bed"))
			{
				for (const colony::ThingView& Other : Bridge.GetCurrentSnapshot().things)
				{
					if (Other.category != colony::ThingCategory::Building || Other.stage != colony::ConstructionStage::Built || !Defs.get(Other.def).has("building.bed"))
					{
						continue;
					}
					const colony::Bed* Bed = Sim.things().registry().try_get<colony::Bed>(Other.id);
					FString Owners;
					for (const colony::Entity Owner : Bed ? Bed->owners : std::vector<colony::Entity>())
					{
						Owners += (Owners.IsEmpty() ? TEXT("") : TEXT(", ")) + NameOf(Owner);
					}
					ImGui::PushID(PId(Other.id));
					const FString Line = FString::Printf(TEXT("%s at %d, %d%s"), *PCapital(Defs.get(Other.def).label()), FMath::FloorToInt32(Other.x), FMath::FloorToInt32(Other.y),
						Owners.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  (%s)"), *Owners));
					if (ImGui::Selectable(TCHAR_TO_UTF8(*Line), Other.id == BedOwned))
					{
						AssignBed(Other.id, View.id);
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}

		// Skills.
		ImGui::Separator();
		PText(TEXT("Skills"));
		for (const colony::SkillRecord& Skill : Colonist.skills)
		{
			const colony::Def* SkillDef = Defs.find("Skills", Skill.def);
			const FString Name = PCapital(SkillDef ? SkillDef->label() : Skill.def);
			const char* Passion = Skill.passion == colony::Passion::Major ? "**" : (Skill.passion == colony::Passion::Minor ? "*" : "");
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Skill.passion == colony::Passion::Major ? ImVec4(0.90f, 0.62f, 0.15f, 1.0f) : (Skill.passion == colony::Passion::Minor ? ImVec4(0.75f, 0.70f, 0.25f, 1.0f) : ImVec4(0.40f, 0.60f, 0.85f, 1.0f)));
			const FString Overlay = Skill.disabled ? FString(TEXT("incapable")) : FString::Printf(TEXT("%d %s"), Skill.level, UTF8_TO_TCHAR(Passion));
			PText(Name);
			ImGui::SameLine(110.0f * Scale);
			ImGui::ProgressBar(Skill.disabled ? 0.0f : Skill.level / 20.0f, ImVec2(-1.0f, 0.0f), TCHAR_TO_UTF8(*Overlay));
			ImGui::PopStyleColor();
		}

		// Health.
		colony::HealthReport Report;
		if (Sim.healthReport(View.id, Report))
		{
			ImGui::Separator();
			PText(TEXT("Health"));
			if (Report.dead)
			{
				PTextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), TEXT("Dead"));
			}
			else if (Report.downed)
			{
				PTextColored(ImVec4(0.95f, 0.60f, 0.25f, 1.0f), TEXT("Down"));
			}
			if (Report.injuries.empty() && Report.missingParts.empty())
			{
				PTextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), TEXT("Healthy"));
			}
			PText(FString::Printf(TEXT("Pain %.0f%%   Bleeding %.1f/day   Blood lost %.0f%%"), Report.pain * 100.0f, Report.bleedPerDay, Report.bloodLoss * 100.0f));
			for (const colony::HealthReport::InjuryLine& Injury : Report.injuries)
			{
				PTextColored(Injury.tended ? ImVec4(0.8f, 0.8f, 0.5f, 1.0f) : ImVec4(0.95f, 0.5f, 0.45f, 1.0f),
					FString::Printf(TEXT("%s: %s %.1f%s"), *PCapital(Injury.part), UTF8_TO_TCHAR(Injury.injury.c_str()), Injury.severity, Injury.tended ? TEXT(" (tended)") : TEXT("")));
			}
			for (const std::string& Missing : Report.missingParts)
			{
				PTextColored(ImVec4(0.95f, 0.5f, 0.45f, 1.0f), FString::Printf(TEXT("%s: missing"), *PCapital(Missing)));
			}
			for (const colony::HealthReport::CapacityLine& Capacity : Report.capacities)
			{
				const colony::Def* CapacityDef = Defs.find("Capacities", Capacity.name);
				PText(FString::Printf(TEXT("%s %.0f%%"), *PCapital(CapacityDef ? CapacityDef->label() : Capacity.name), Capacity.value * 100.0f));
			}
		}
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawBillsPanel(USimSubsystem& Bridge, const colony::ThingView& View, float Left, float Top, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::BillSystem& Bills = Sim.jobs().bills();
	const colony::DefDatabase& Defs = Sim.defs();
	// The recipes this building can make; a building with none has no bills.
	std::vector<const colony::Recipe*> Usable;
	for (const colony::Recipe& Recipe : Bills.recipes())
	{
		if (Bills.canMake(View.def, Recipe))
		{
			Usable.push_back(&Recipe);
		}
	}
	if (Usable.empty())
	{
		return;
	}
	if (NewBillBuilding != View.id)
	{
		NewBillBuilding = View.id;
		NewRecipe = 0;
		NewMode = 0;
		NewCount = 1;
	}
	NewRecipe = FMath::Clamp(NewRecipe, 0, static_cast<int32>(Usable.size()) - 1);

	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Left, Viewport->Pos.y + Top), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f * Scale, 0.0f), ImVec2(560.0f * Scale, FMath::Max(200.0f * Scale, Viewport->Size.y - Top - 330.0f * Scale)));
	if (ImGui::Begin("##PanelsBills", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.BillPanelFrames;
		PText(FString::Printf(TEXT("%s: bills"), *PCapital(Defs.get(View.def).label())));
		PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), TEXT("Tried from the top; the first one that can be done is."));
		ImGui::Separator();
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		if (View.billCount == 0)
		{
			PText(TEXT("No bills yet."));
		}
		for (uint32 Index = 0; Index < View.billCount; ++Index)
		{
			const colony::BillView Bill = Snapshot.bills[View.firstBill + Index];
			const colony::Recipe* Recipe = Bills.recipe(Bill.recipe);
			ImGui::PushID(static_cast<int>(Bill.id));
			FString Title = Recipe ? PCapital(Recipe->label.empty() ? Recipe->defName : Recipe->label) : FString(TEXT("(unknown recipe)"));
			if (Bill.suspended)
			{
				Title += TEXT("  (paused)");
			}
			PTextColored(Bill.suspended ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), FString::Printf(TEXT("%u. %s"), Index + 1, *Title));
			FString HowLong;
			switch (Bill.mode)
			{
				case colony::BillMode::Count: HowLong = FString::Printf(TEXT("%d more time%s"), Bill.count, Bill.count == 1 ? TEXT("") : TEXT("s")); break;
				case colony::BillMode::UntilHave:
					HowLong = FString::Printf(TEXT("until %d on the map (%d now)"), Bill.count, Recipe && !Recipe->products.empty() ? Bills.stockOf(Recipe->products[0].item) : 0);
					break;
				default: HowLong = TEXT("forever"); break;
			}
			ImGui::SameLine();
			PTextColored(ImVec4(0.7f, 0.8f, 0.9f, 1.0f), HowLong);

			// Mode, count, pause, remove.
			int ModeIndex = static_cast<int>(Bill.mode);
			ImGui::SetNextItemWidth(190.0f * Scale);
			if (ImGui::BeginCombo("##Mode", BillModeWord(Bill.mode)))
			{
				for (int32 ModeOption = 0; ModeOption < 3; ++ModeOption)
				{
					if (ImGui::Selectable(BillModeWord(static_cast<colony::BillMode>(ModeOption)), ModeOption == ModeIndex) && ModeOption != ModeIndex)
					{
						ChangeBill(View.id, Bill.id, static_cast<colony::BillMode>(ModeOption), FMath::Max(Bill.count, 1), Bill.suspended);
					}
				}
				ImGui::EndCombo();
			}
			if (Bill.mode != colony::BillMode::Forever)
			{
				ImGui::SameLine();
				const auto Step = [&](const char* Label, int32 Delta)
				{
					if (ImGui::SmallButton(Label))
					{
						ChangeBill(View.id, Bill.id, Bill.mode, FMath::Clamp(Bill.count + Delta, 1, Bills.numbers().maxBillCount), Bill.suspended);
					}
					ImGui::SameLine();
				};
				Step("-10", -10);
				Step("-1", -1);
				Step("+1", 1);
				Step("+10", 10);
			}
			else
			{
				ImGui::SameLine();
			}
			if (ImGui::SmallButton(Bill.suspended ? "Resume" : "Pause"))
			{
				ChangeBill(View.id, Bill.id, Bill.mode, FMath::Max(Bill.count, 1), !Bill.suspended);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Remove"))
			{
				RemoveBill(View.id, Bill.id);
			}
			ImGui::PopID();
			ImGui::Separator();
		}

		// Add one.
		PText(TEXT("Add a bill"));
		ImGui::SetNextItemWidth(260.0f * Scale);
		const colony::Recipe& Picked = *Usable[NewRecipe];
		if (ImGui::BeginCombo("##Recipe", TCHAR_TO_UTF8(*PCapital(Picked.label.empty() ? Picked.defName : Picked.label))))
		{
			for (int32 Index = 0; Index < static_cast<int32>(Usable.size()); ++Index)
			{
				const colony::Recipe& Option = *Usable[Index];
				if (ImGui::Selectable(TCHAR_TO_UTF8(*PCapital(Option.label.empty() ? Option.defName : Option.label)), Index == NewRecipe))
				{
					NewRecipe = Index;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SetNextItemWidth(190.0f * Scale);
		if (ImGui::BeginCombo("##NewMode", BillModeWord(static_cast<colony::BillMode>(NewMode))))
		{
			for (int32 ModeOption = 0; ModeOption < 3; ++ModeOption)
			{
				if (ImGui::Selectable(BillModeWord(static_cast<colony::BillMode>(ModeOption)), ModeOption == NewMode))
				{
					NewMode = ModeOption;
				}
			}
			ImGui::EndCombo();
		}
		if (static_cast<colony::BillMode>(NewMode) != colony::BillMode::Forever)
		{
			ImGui::SameLine();
			const auto Step = [&](const char* Label, int32 Delta)
			{
				if (ImGui::SmallButton(Label))
				{
					NewCount = FMath::Clamp(NewCount + Delta, 1, Bills.numbers().maxBillCount);
				}
				ImGui::SameLine();
			};
			Step("-10", -10);
			Step("-1", -1);
			PText(FString::FromInt(NewCount));
			ImGui::SameLine();
			Step("+1", 1);
			Step("+10", 10);
			ImGui::NewLine();
		}
		if (ImGui::Button("Add bill"))
		{
			AddBill(View.id, UTF8_TO_TCHAR(Picked.defName.c_str()), static_cast<colony::BillMode>(NewMode), NewCount);
		}
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawThermostatPanel(USimSubsystem& Bridge, const colony::ThingView& View, float Left, float Top, float Scale)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Def& Def = Sim.defs().get(View.def);
	const colony::TemperatureSystem& Temperature = Sim.temperature();
	const colony::TemperatureNumbers& Numbers = Temperature.numbers();
	const bool bHeater = Def.number("heatPusher.heatPerSecond", 0.0) > 0.0;
	const bool bOwnTarget = Temperature.hasThermostat(View.id);
	const float Target = Temperature.thermostat(View.id);
	const double OwnLimit = Def.number(bHeater ? "heatPusher.maxTemp" : "heatPusher.minTemp", 21.0);
	// A new building: start the drag value from its target.
	if (ThermoBuilding != View.id)
	{
		ThermoBuilding = View.id;
		ThermoDragValue = Target;
		bThermoDragging = false;
	}

	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Left, Viewport->Pos.y + Top), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f * Scale, 0.0f), ImVec2(520.0f * Scale, FLT_MAX));
	if (ImGui::Begin("##PanelsThermostat", nullptr, PanelFlags()))
	{
		++Stats.ThermostatPanelFrames;
		PText(FString::Printf(TEXT("%s: thermostat"), *PCapital(Def.label())));
		PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), bHeater ? TEXT("Heats the room up to this and then rests.") : TEXT("Cools the room down to this and then rests."));
		// The room it works on, now (read only; the sim keeps it up to date every step).
		const colony::CellPos Cell{static_cast<int>(std::floor(View.x)), static_cast<int>(std::floor(View.y)), View.level};
		const float RoomNow = const_cast<colony::TemperatureSystem&>(Temperature).temperatureAt(Cell);
		PText(FString::Printf(TEXT("Room now: %.1f C"), RoomNow));
		ImGui::Separator();

		float Shown = bThermoDragging ? ThermoDragValue : Target;
		ImGui::SetNextItemWidth(300.0f * Scale);
		ImGui::SliderFloat("##Target", &Shown, Numbers.thermostatMinC, Numbers.thermostatMaxC, "%.0f C");
		if (ImGui::IsItemActive())
		{
			bThermoDragging = true;
			ThermoDragValue = Shown;
		}
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			SetThermostat(View.id, std::round(ThermoDragValue), false);
		}
		if (ImGui::IsItemDeactivated())
		{
			bThermoDragging = false;
		}
		const auto Nudge = [&](const char* Label, float Delta)
		{
			if (ImGui::SmallButton(Label))
			{
				SetThermostat(View.id, std::round(Target + Delta), false);
			}
			ImGui::SameLine();
		};
		Nudge("-5", -5.0f);
		Nudge("-1", -1.0f);
		Nudge("+1", 1.0f);
		Nudge("+5", 5.0f);
		if (ImGui::SmallButton("Reset"))
		{
			SetThermostat(View.id, Target, true);
		}
		PTextColored(ImVec4(0.7f, 0.8f, 0.9f, 1.0f), bOwnTarget ? FString::Printf(TEXT("Set to %.0f C (the building's own limit is %.0f C)."), Target, OwnLimit)
														 : FString::Printf(TEXT("At the building's own limit, %.0f C."), OwnLimit));
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawLog(USimSubsystem& Bridge, float Scale, float Margin)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	// Right side, under the alarm button.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Viewport->Pos.y + Margin + 48.0f * Scale), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f * Scale, 0.0f), ImVec2(540.0f * Scale, bLogOpen ? Viewport->Size.y * 0.5f : FLT_MAX));
	ImGui::SetNextWindowBgAlpha(bLogOpen ? 0.85f : 0.55f);
	if (ImGui::Begin("##PanelsLog", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.LogPanelFrames;
		const int32 Wanted = bLogOpen ? 40 : 5;
		const int32 First = FMath::Max(0, Log.Num() - Wanted);
		if (Log.IsEmpty())
		{
			PTextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), TEXT("Nothing has happened yet."));
		}
		for (int32 Index = First; Index < Log.Num(); ++Index)
		{
			const FColonyLogLine& Line = Log[Index];
			ImGui::PushID(Index);
			ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(Line.Severity));
			const FString Text = FString::Printf(TEXT("%s  %s"), *TimeText(Line.GameSeconds), *Line.Text);
			if (ImGui::Selectable(TCHAR_TO_UTF8(*Text)) && Overseer && Line.Subject != colony::kNoEntity && Bridge.GetCurrentSnapshot().find(Line.Subject))
			{
				FSimPick Pick;
				Pick.Kind = ESimPickKind::Thing;
				Pick.Thing = Line.Subject;
				Overseer->Select(Pick);
				Overseer->FocusOnSelection();
			}
			ImGui::PopStyleColor();
			ImGui::PopID();
		}
		if (bLogOpen && Log.Num() != LastLogCountShown)
		{
			ImGui::SetScrollHereY(1.0f);
		}
		LastLogCountShown = Log.Num();
	}
	ImGui::End();
}
