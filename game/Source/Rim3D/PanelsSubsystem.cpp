#include "PanelsSubsystem.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "HudLayout.h"

#include "ColonyHudSubsystem.h"
#include "Engine/Engine.h"
#include "OverseerSubsystem.h"
#include "ProtagonistPawn.h"
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
		ColonyHud::Text(Text);  // wraps in a box of held width
	}

	void PText(const std::string& Text)
	{
		ColonyHud::Text(FString(UTF8_TO_TCHAR(Text.c_str())));
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

	/** A code name in plain words: "WearApparel" -> "Wear apparel", "HaulToStockpile" -> "Haul to stockpile". */
	FString PWords(const char* Name)
	{
		const FString In = UTF8_TO_TCHAR(Name ? Name : "");
		FString Out;
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			const TCHAR Letter = In[Index];
			if (Index > 0 && FChar::IsUpper(Letter) && !FChar::IsUpper(In[Index - 1]))
			{
				Out += TEXT(' ');
				Out += FChar::ToLower(Letter);
			}
			else
			{
				Out += Letter;
			}
		}
		return Out;
	}

	int32 PId(colony::Entity Id)
	{
		return static_cast<int32>(entt::to_integral(Id));
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
	PopupLine = -1;
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
	AddAlert(Severity == 2 ? TEXT("Danger") : Severity == 1 ? TEXT("Trouble") : Severity == 3 ? TEXT("Good news") : TEXT("Notice"), TEXT("Other"), Text, Severity, Subject);
}

void UPanelsSubsystem::AddAlert(const FString& Title, const FString& Kind, const FString& Text, uint8 Severity, colony::Entity Subject, bool bAlert)
{
	const USimSubsystem* Bridge = GetBridge();
	const double Now = FPlatformTime::Seconds();
	// The same alert again within a few seconds (a refused order clicked twice, a run of hurts) is counted on the last one.
	if (!Log.IsEmpty())
	{
		FColonyLogLine& Last = Log.Last();
		if (Last.Kind == Kind && Last.Text == Text && Last.Subject == Subject && Now - Last.RealSeconds < 5.0)
		{
			++Last.Repeats;
			Last.RealSeconds = Now;
			Last.bRead = Last.bDismissed = false;
			return;
		}
	}
	FColonyLogLine Line;
	Line.GameSeconds = Bridge ? Bridge->GetGameSeconds() : 0;
	Line.Text = Text;
	Line.Title = Title;
	Line.Kind = Kind;
	Line.Severity = Severity;
	Line.Subject = Subject;
	Line.RealSeconds = Now;
	Line.bAlert = bAlert;
	Log.Add(MoveTemp(Line));
	if (Log.Num() > MaxLogLines)
	{
		const int32 Cut = Log.Num() - MaxLogLines;
		Log.RemoveAt(0, Cut);
		PopupLine = PopupLine >= Cut ? PopupLine - Cut : -1;
	}
}

double UPanelsSubsystem::AlertSeconds(uint8 Severity)
{
	// sneed 2026-11-24: "the fade time depends on importance but ... lets do 1 min for now". One place to tune it later.
	switch (Severity)
	{
		case 2: return 60.0;  // danger
		case 1: return 60.0;  // trouble
		case 3: return 60.0;  // good news
		default: return 60.0;
	}
}

void UPanelsSubsystem::GetLiveAlerts(TArray<int32>& Out) const
{
	Out.Reset();
	const double Now = FPlatformTime::Seconds();
	for (int32 Index = Log.Num() - 1; Index >= 0; --Index)
	{
		const FColonyLogLine& Line = Log[Index];
		if (!Line.bAlert || Line.bRead || Line.bDismissed || ExcludedKinds.Contains(Line.Kind) || Now - Line.RealSeconds > AlertSeconds(Line.Severity))
		{
			continue;
		}
		Out.Add(Index);
	}
}

void UPanelsSubsystem::OpenAlert(int32 LogIndex)
{
	if (!Log.IsValidIndex(LogIndex))
	{
		return;
	}
	Log[LogIndex].bRead = true;
	PopupLine = LogIndex;
}

void UPanelsSubsystem::DismissAlert(int32 LogIndex, bool bForGood)
{
	if (!Log.IsValidIndex(LogIndex))
	{
		return;
	}
	Log[LogIndex].bDismissed = true;
	if (bForGood)
	{
		SetAlertKindOff(Log[LogIndex].Kind, true);
	}
}

void UPanelsSubsystem::SetAlertKindOff(const FString& Kind, bool bOff)
{
	LoadAlertPrefs();
	if (bOff)
	{
		ExcludedKinds.AddUnique(Kind);
	}
	else
	{
		ExcludedKinds.Remove(Kind);
	}
	SaveAlertPrefs();
}

void UPanelsSubsystem::LoadAlertPrefs()
{
	if (bAlertPrefsLoaded || !GConfig)
	{
		return;
	}
	bAlertPrefsLoaded = true;
	GConfig->GetArray(TEXT("Rim3D.Alerts"), TEXT("Off"), ExcludedKinds, GGameUserSettingsIni);
}

void UPanelsSubsystem::SaveAlertPrefs() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetArray(TEXT("Rim3D.Alerts"), TEXT("Off"), ExcludedKinds, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
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
			AddAlert(TEXT("Order refused"), TEXT("Refused"), FString::Printf(TEXT("Refused: %s"), UTF8_TO_TCHAR(Event.message.c_str())), 1);
			break;
		case colony::EventType::PawnDied:
			if (bColonist)
			{
				AddAlert(FString::Printf(TEXT("%s died"), *NameOf(Event.thing)), TEXT("ColonistDied"), FString::Printf(TEXT("%s died (%s)."), *NameOf(Event.thing), UTF8_TO_TCHAR(Event.message.c_str())), 2, Event.thing);
			}
			break;
		case colony::EventType::PawnDowned:
			if (bColonist)
			{
				AddAlert(FString::Printf(TEXT("%s down"), *NameOf(Event.thing)), TEXT("ColonistDown"), FString::Printf(TEXT("%s is down."), *NameOf(Event.thing)), 2, Event.thing);
			}
			break;
		case colony::EventType::PawnGotUp:
			if (bColonist)
			{
				AddAlert(FString::Printf(TEXT("%s up again"), *NameOf(Event.thing)), TEXT("ColonistUp"), FString::Printf(TEXT("%s got back up."), *NameOf(Event.thing)), 3, Event.thing);
			}
			break;
		case colony::EventType::PawnInjured:
			if (bColonist && Event.def.valid())
			{
				AddAlert(FString::Printf(TEXT("%s hurt"), *NameOf(Event.thing)), TEXT("ColonistHurt"), FString::Printf(TEXT("%s was hurt: %s."), *NameOf(Event.thing), UTF8_TO_TCHAR(Sim.defs().get(Event.def).label().c_str())), 1, Event.thing);
			}
			break;
		case colony::EventType::PawnPartLost:
			if (bColonist)
			{
				AddAlert(TEXT("Body part lost"), TEXT("PartLost"), FString::Printf(TEXT("%s lost a body part."), *NameOf(Event.thing)), 2, Event.thing);
			}
			break;
		case colony::EventType::PawnMoodBand:
			if (bColonist)
			{
				const bool bStable = Event.message == "stable";
				AddAlert(bStable ? FString::Printf(TEXT("%s calmer"), *NameOf(Event.thing)) : FString(TEXT("Break risk")), bStable ? TEXT("Calmed") : TEXT("BreakRisk"),
					bStable ? FString::Printf(TEXT("%s has calmed down."), *NameOf(Event.thing))
							: FString::Printf(TEXT("%s is close to a %s mental break (mood %.0f)."), *NameOf(Event.thing), UTF8_TO_TCHAR(Event.message.c_str()), Event.amount),
					bStable ? 3 : (Event.message == "minor" ? 1 : 2), Event.thing);
			}
			break;
		case colony::EventType::RecipeDone:
		{
			const FString What = Event.def.valid() ? UTF8_TO_TCHAR(Sim.defs().get(Event.def).label().c_str()) : TEXT("a bill");
			// Each finished craft goes in the log only; a whole bill finished is an alert.
			AddAlert(TEXT("Bill done"), TEXT("BillDone"), FString::Printf(TEXT("%s finished: %s%s."), *NameOf(Event.thing), *What, Event.message.empty() ? TEXT("") : TEXT(" (bill done)")), 3,
				Event.thing, !Event.message.empty());
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
				case colony::EventType::QuestOffered: AddAlert(TEXT("Quest offered"), TEXT("QuestOffered"), FString::Printf(TEXT("Quest offered: %s. Open Quests to answer."), *What), 0); break;
				case colony::EventType::QuestAccepted: AddAlert(TEXT("Quest accepted"), TEXT("QuestAccepted"), FString::Printf(TEXT("Quest accepted: %s."), *What), 0); break;
				case colony::EventType::QuestDeclined: AddAlert(TEXT("Quest declined"), TEXT("QuestDeclined"), FString::Printf(TEXT("Quest declined: %s."), *What), 0); break;
				case colony::EventType::QuestExpired: AddAlert(TEXT("Quest lapsed"), TEXT("QuestLapsed"), FString::Printf(TEXT("Quest lapsed: %s."), *What), 1); break;
				case colony::EventType::QuestCompleted: AddAlert(TEXT("Quest done"), TEXT("QuestDone"), FString::Printf(TEXT("Quest done: %s (%s)."), *What, *Why), 3); break;
				default: AddAlert(TEXT("Quest failed"), TEXT("QuestFailed"), FString::Printf(TEXT("Quest failed: %s (%s)."), *What, *Why), 2); break;
			}
			break;
		}
		// The spaceship ending (docs/design/ship.md); the ship window is UShipPanelSubsystem.
		case colony::EventType::ShipReactorStarted:
			AddAlert(TEXT("Reactor starting"), TEXT("ShipReactor"), FString::Printf(TEXT("The ship reactor is starting. It needs %.0f days, and raiders will come for it."), Event.amount), 1, Event.thing);
			break;
		case colony::EventType::ShipWave:
			AddAlert(TEXT("Raid on reactor"), TEXT("ShipWave"), FString::Printf(TEXT("Raiders are coming for the ship reactor (wave %.0f)."), Event.amount), 2, Event.thing);
			break;
		case colony::EventType::ShipReactorReady:
			AddAlert(TEXT("Ship ready"), TEXT("ShipReady"), TEXT("The ship reactor is active. Open Ship to launch."), 3, Event.thing);
			break;
		case colony::EventType::ShipReactorLost:
			AddAlert(TEXT("Reactor lost"), TEXT("ShipLost"), TEXT("The ship reactor is lost. It must be started again."), 2);
			break;
		case colony::EventType::ShipLaunched:
			AddAlert(TEXT("Ship launched"), TEXT("ShipLaunched"), FString::Printf(TEXT("The ship has launched with %.0f aboard."), Event.amount), 3);
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
	ColonyHud::ApplyFrameStyle(Scale);
	const float Margin = 12.0f * Scale;

	float MenuBottom = Margin;
	DrawMenuAndColonistBar(Bridge, Scale, Margin, MenuBottom);
	TopLeftBottom = MenuBottom;
	TopLeftFrame = GFrameCounter;

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
			const float Left = 0.0f;
			const float Top = MenuBottom;  // touching the faces' box (sneed 2026-11-25)
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
	// The colonist bar, top left (docs/design/ui-layout.md): a mugshot of each colonist with the name under it, coloured by
	// mood (grey when downed). Click selects, double click centres the camera. The tabs that used to sit here are on the
	// bottom bar now; the pinned resources show under the bar.
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	UColonyHudSubsystem* Hud = GetWorld()->GetSubsystem<UColonyHudSubsystem>();
	// Flush in the top left corner, with the selected colonist's box right under it (sneed 2026-11-25: no gaps).
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x, Viewport->Pos.y), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x * 0.55f, FLT_MAX));
	if (ImGui::Begin("##PanelsMenu", nullptr, PanelFlags()))
	{
		TArray<colony::Entity> Colonists;
		GetColonists(Colonists);
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		// The portraits grow with the text (sneed 2026-11-24: everything 50% bigger).
		const float Face = 56.0f * Scale * FMath::Max(1.0f, ImGui::GetStyle().FontScaleMain / (1.2f * Scale));
		const float Slot = Face + 16.0f * Scale;
		const int32 PerRow = FMath::Max(1, static_cast<int32>((Viewport->Size.x * 0.55f) / (Slot + ImGui::GetStyle().ItemSpacing.x)));
		int32 Shown = 0;
		for (const colony::Entity Pawn : Colonists)
		{
			const colony::ThingView* View = Snapshot.find(Pawn);
			if (!View)
			{
				continue;
			}
			if (Shown % PerRow != 0)
			{
				ImGui::SameLine();
			}
			++Shown;
			const bool bSelected = Overseer && Overseer->IsThingSelected(Pawn);
			ImGui::PushID(PId(Pawn));
			ImGui::BeginGroup();
			const ImVec2 Corner = ImGui::GetCursorScreenPos();
			UTexture* Mugshot = Hud ? Hud->GetMugshot(Pawn) : nullptr;
			ImGui::PushStyleColor(ImGuiCol_Button, ColonyHud::ButtonBg());
			ImGui::PushStyleColor(ImGuiCol_Border, bSelected ? ColonyHud::Accent() : ColonyHud::ButtonBorder());
			bool bClicked = false;
			if (Mugshot)
			{
				bClicked = ImGui::ImageButton("##face", ImTextureRef(Mugshot), ImVec2(Face, Face));
			}
			else
			{
				// Until the picture is made: the first letter of the name.
				const FString Initial = NameOf(Pawn).Left(1);
				bClicked = ImGui::Button(TCHAR_TO_UTF8(*FString::Printf(TEXT("%s##face"), *Initial)), ImVec2(Face + 2.0f * ImGui::GetStyle().FramePadding.x,
					Face + 2.0f * ImGui::GetStyle().FramePadding.y));
			}
			ImGui::PopStyleColor(2);
			const bool bDouble = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
			{
				const FString Tip = View->mood >= 0.0f ? FString::Printf(TEXT("%s\nMood %.0f   Food %.0f%%   Rest %.0f%%\nClick to select, double click to centre"), *NameOf(Pawn), View->mood,
					View->food * 100.0f, View->rest * 100.0f) : FString::Printf(TEXT("%s\nClick to select"), *NameOf(Pawn));
				ImGui::SetTooltip("%s", TCHAR_TO_UTF8(*Tip));
			}
			// Drafted: a red edge; downed: greyed.
			const ImVec2 FaceMax = ImGui::GetItemRectMax();
			if (View->drafted)
			{
				ImGui::GetWindowDrawList()->AddRect(Corner, FaceMax, ImGui::GetColorU32(ColonyHud::Bad()), 4.0f * Scale, 2.0f * Scale, 0);
			}
			// Health under the face (sneed 2026-11-24): the worst body part's share, green to amber to red, as the focus bar.
			{
				const float Health = FMath::Clamp(View->dead ? 0.0f : Bridge.GetSim()->lowestPartHealth(Pawn), 0.0f, 1.0f);
				const float BarHeight = FMath::Max(4.0f, 6.0f * Scale);
				ImGui::Dummy(ImVec2(FaceMax.x - Corner.x, BarHeight));
				const ImVec2 BarMin = ImGui::GetItemRectMin();
				const ImVec2 BarMax(FaceMax.x, BarMin.y + BarHeight);
				const ImVec4 Fill = Health > 0.6f ? ColonyHud::Good() : Health > 0.3f ? ColonyHud::Warn() : ColonyHud::Bad();
				ImDrawList* Draw = ImGui::GetWindowDrawList();
				Draw->AddRectFilled(BarMin, BarMax, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.65f)));
				Draw->AddRectFilled(BarMin, ImVec2(BarMin.x + (BarMax.x - BarMin.x) * Health, BarMax.y), ImGui::GetColorU32(Fill));
				Draw->AddRect(BarMin, BarMax, ImGui::GetColorU32(ColonyHud::ButtonBorder()));
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
				{
					ImGui::SetTooltip("Health %.0f%%", Health * 100.0f);
				}
			}
			ImGui::PushStyleColor(ImGuiCol_Text, View->downed ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : MoodColor(View->moodBand));
			// The first name only, cut to the face's width with "..." so neighbours never run together, centred under it.
			FString Name = NameOf(Pawn);
			int32 Space = INDEX_NONE;
			if (Name.FindChar(TEXT(' '), Space) && Space > 0)
			{
				Name = Name.Left(Space);
			}
			const float NameRoom = FaceMax.x - Corner.x;
			if (ImGui::CalcTextSize(TCHAR_TO_UTF8(*Name)).x > NameRoom)
			{
				while (Name.Len() > 1 && ImGui::CalcTextSize(TCHAR_TO_UTF8(*(Name + TEXT("...")))).x > NameRoom)
				{
					Name.LeftChopInline(1);
				}
				Name += TEXT("...");
			}
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + FMath::Max(0.0f, (NameRoom - ImGui::CalcTextSize(TCHAR_TO_UTF8(*Name)).x) * 0.5f));
			PText(Name);
			ImGui::PopStyleColor();
			ImGui::EndGroup();
			if (bClicked && Overseer && ImGui::GetIO().KeyShift)
			{
				Overseer->ToggleThingSelected(Pawn);  // shift click adds or removes, as in the world
			}
			else if (bClicked && Overseer)
			{
				FSimPick Pick;
				Pick.Kind = ESimPickKind::Thing;
				Pick.Thing = Pawn;
				Overseer->Select(Pick);
			}
			if (bDouble && Overseer)
			{
				Overseer->FocusOnSelection();
			}
			ImGui::PopID();
		}
		if (Shown == 0)
		{
			PTextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), TEXT("No colonists"));
		}
		// No pinned resources under the faces (sneed 2026-11-25: silver hidden); the Inventory tab lists them.
		OutBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - Viewport->Pos.y;
	}
	ImGui::End();
}

namespace
{
	/** The work table's whole width (its columns' own widths), so the auto-sized window never cuts the last columns off. */
	float WorkTableWidth(const colony::Sim& Sim, const std::vector<int32>& Columns, float Names, float Scale, float ViewportWidth)
	{
		const ImGuiStyle& Style = ImGui::GetStyle();
		const float Cell = 2.0f * Style.CellPadding.x + 1.0f;
		float Width = 110.0f * Scale + 2.0f * Style.FramePadding.x + Cell;
		Names = FMath::Max(Names, ImGui::CalcTextSize("Colonist").x);
		Width += Names + 2.0f * Style.FramePadding.x + Cell;
		const auto& WorkTypes = Sim.jobs().workTypes();
		for (const int32 Index : Columns)
		{
			const std::string Label = Sim.defs().get(WorkTypes[Index].def).label();
			Width += FMath::Max(ImGui::CalcTextSize(Label.c_str()).x, ImGui::GetFrameHeight() * 1.6f) + Cell;
		}
		Width += Style.ScrollbarSize + 2.0f * Style.WindowPadding.x;
		return FMath::Min(Width, ViewportWidth - 2.0f * Style.WindowPadding.x);
	}
}

void UPanelsSubsystem::DrawWorkTab(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const auto& WorkTypes = Sim.jobs().workTypes();
	UColonyHudSubsystem* Hud = GetWorld()->GetSubsystem<UColonyHudSubsystem>();
	// Only work that has a scanner does anything yet.
	std::vector<int32> Columns;
	for (int32 Index = 0; Index < static_cast<int32>(WorkTypes.size()); ++Index)
	{
		if (!WorkTypes[Index].givers.empty())
		{
			Columns.push_back(Index);
		}
	}
	// From the screen's left edge, its bottom right on the bar (sneed 2026-11-24, HudLayout).
	HudLayout::PlaceTab(Viewport, Scale);
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x, Viewport->Size.y * 0.7f));
	if (ImGui::Begin("##PanelsWork", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.WorkTabFrames;
		ColonyHud::Title(TEXT("Work"));
		ImGui::SameLine();
		PTextColored(ColonyHud::TextDim(), TEXT("  1 is done first, 4 last, - is never.  Left click raises the number, right click lowers it.  X: can never do this."));
		TArray<colony::Entity> Colonists;
		GetColonists(Colonists);

		// The work groups in use, as filter buttons along the top.
		TArray<FString> Groups;
		for (const colony::Entity Pawn : Colonists)
		{
			const colony::Colonist* Colonist = Sim.colonistOf(Pawn);
			if (Colonist && !Colonist->workGroup.empty())
			{
				Groups.AddUnique(UTF8_TO_TCHAR(Colonist->workGroup.c_str()));
			}
		}
		Groups.Sort();
		if (!WorkGroupFilter.IsEmpty() && !Groups.Contains(WorkGroupFilter))
		{
			WorkGroupFilter.Reset();
		}
		PTextColored(ColonyHud::TextFaint(), TEXT("Show"));
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Everyone"), WorkGroupFilter.IsEmpty()))
		{
			WorkGroupFilter.Reset();
		}
		for (const FString& Group : Groups)
		{
			ImGui::SameLine();
			if (ColonyHud::Button(Group + TEXT("##filter"), WorkGroupFilter == Group))
			{
				WorkGroupFilter = WorkGroupFilter == Group ? FString() : Group;
			}
		}

		// Prisoners have rows too, marked as such: they can't be given work.
		TArray<colony::Entity> Prisoners;
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.prisoner && !View.dead)
			{
				Prisoners.Add(View.id);
			}
		}
		const auto WorkNamesWidth = [this](const TArray<colony::Entity>& A, const TArray<colony::Entity>& B)
		{
			float Widest = 0.0f;
			for (const TArray<colony::Entity>* List : {&A, &B})
			{
				for (const colony::Entity Pawn : *List)
				{
					Widest = FMath::Max(Widest, ImGui::CalcTextSize(TCHAR_TO_UTF8(*(NameOf(Pawn) + TEXT("  (prisoner)")))).x);
				}
			}
			return Widest;
		};
		if ((Colonists.IsEmpty() && Prisoners.IsEmpty()) || Columns.empty())
		{
			PText(TEXT("No colonists or no work to set."));
		}
		else if (ImGui::BeginTable("##WorkTable", static_cast<int>(Columns.size()) + 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
			ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(WorkTableWidth(Sim, Columns, WorkNamesWidth(Colonists, Prisoners), Scale, Viewport->Size.x), FMath::Min(Viewport->Size.y * 0.52f, ImGui::GetFrameHeightWithSpacing() * (Colonists.Num() + Prisoners.Num() + 2)))))
		{
			ImGui::TableSetupScrollFreeze(2, 1);
			ImGui::TableSetupColumn("Group");
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
				const FString Group = Colonist ? FString(UTF8_TO_TCHAR(Colonist->workGroup.c_str())) : FString();
				if (!WorkGroupFilter.IsEmpty() && Group != WorkGroupFilter)
				{
					continue;
				}
				ImGui::TableNextRow();
				// The work group: blank to start; a click puts the colonist in a group (an old one or a new name).
				ImGui::TableNextColumn();
				ImGui::PushID(PId(Pawn));
				if (ColonyHud::Button(Group.IsEmpty() ? FString(TEXT("  +  ##group")) : Group + TEXT("##group"), false, ImVec2(110.0f * Scale, 0.0f)))
				{
					ImGui::OpenPopup("##GroupPopup");
					NewGroupName[0] = 0;
				}
				ColonyHud::Tip(TEXT("Put this colonist in a work group (Fighters, Growers, any name). Groups sort and filter this list"));
				if (ImGui::BeginPopup("##GroupPopup"))
				{
					for (const FString& Existing : Groups)
					{
						if (ImGui::Selectable(TCHAR_TO_UTF8(*Existing), Existing == Group) && Hud)
						{
							Hud->SetWorkGroup(Pawn, Existing);
						}
					}
					for (const TCHAR* Suggested : {TEXT("Fighters"), TEXT("Growers"), TEXT("Miners"), TEXT("Builders"), TEXT("Cooks")})
					{
						if (!Groups.Contains(Suggested) && ImGui::Selectable(TCHAR_TO_UTF8(Suggested)) && Hud)
						{
							Hud->SetWorkGroup(Pawn, Suggested);
						}
					}
					ImGui::SetNextItemWidth(180.0f * Scale);
					if (ImGui::InputTextWithHint("##newgroup", "New group, Enter", NewGroupName, sizeof(NewGroupName), ImGuiInputTextFlags_EnterReturnsTrue) && Hud &&
						NewGroupName[0] != 0)
					{
						Hud->SetWorkGroup(Pawn, UTF8_TO_TCHAR(NewGroupName));
						ImGui::CloseCurrentPopup();
					}
					if (!Group.IsEmpty() && ImGui::Selectable("No group") && Hud)
					{
						Hud->SetWorkGroup(Pawn, FString());
					}
					ImGui::EndPopup();
				}
				ImGui::PopID();
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
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
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
			if (WorkGroupFilter.IsEmpty())
			{
				for (const colony::Entity Pawn : Prisoners)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					PTextColored(ColonyHud::Warn(), TEXT("Prisoner"));
					ImGui::TableNextColumn();
					const colony::ThingView* View = Bridge.GetCurrentSnapshot().find(Pawn);
					PText(View ? PCapital(Sim.colonistOf(Pawn) ? std::string(TCHAR_TO_UTF8(*NameOf(Pawn))) : Sim.defs().get(View->def).label()) : FString(TEXT("?")));
					ImGui::TableNextColumn();
					PTextColored(ColonyHud::TextFaint(), TEXT("Prisoners don't work"));
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
	HudLayout::PlaceTab(Viewport, Scale);
	ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x, Viewport->Size.y * 0.7f));
	if (ImGui::Begin("##PanelsSchedule", nullptr, PanelFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.ScheduleTabFrames;
		ColonyHud::Title(TEXT("Schedule"));
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
	// The width it had before the text grew half as big again (sneed 2026-11-24: boxes keep their widths).
	ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * Scale, 0.0f), ImVec2(360.0f * Scale, FMath::Max(200.0f * Scale, Viewport->Size.y - Top - 330.0f * Scale)));
	ColonyHud::HeldBoxes().Add(ImHashStr("##PanelsColonist"));
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
			// Backstories carry a title ("hunter's child"), not a label.
			return PCapital(Def ? Def->text("title", Def->label()) : Name);
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
		FString Doing = View.dead ? TEXT("Dead") : (View.downed ? TEXT("Down") : (View.asleep ? TEXT("Asleep") : (View.drafted ? TEXT("Drafted") : PWords(colony::jobTypeName(View.job)))));
		PText(FString::Printf(TEXT("Doing: %s"), *Doing));

		// Needs and mood.
		const auto Bar = [&](const char* Name, float Fraction, const FString& Overlay)
		{
			PText(FString(UTF8_TO_TCHAR(Name)));
			ImGui::SameLine(80.0f * Scale * ColonyHud::TextGrow());
			ImGui::ProgressBar(FMath::Clamp(Fraction, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), TCHAR_TO_UTF8(*Overlay));
		};
		if (View.food >= 0.0f)
		{
			Bar("Food", View.food, FString::Printf(TEXT("%.0f%%"), View.food * 100.0f));
			if (View.rest >= 0.0f)  // below 0: this pawn has no rest need (never sleeps)
			{
				Bar("Rest", View.rest, FString::Printf(TEXT("%.0f%%"), View.rest * 100.0f));
			}
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
			// Canopy: jade bars, ember for a passion (deeper for a major one), as on the companions screen.
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Skill.passion == colony::Passion::Major ? ColonyHud::Ember() :
				(Skill.passion == colony::Passion::Minor ? ImVec4(0.85f, 0.55f, 0.38f, 0.85f) : ImVec4(0.30f, 0.66f, 0.53f, 0.85f)));
			const FString Overlay = Skill.disabled ? FString(TEXT("incapable")) : FString::Printf(TEXT("%d %s"), Skill.level, UTF8_TO_TCHAR(Passion));
			PText(Name);
			ImGui::SameLine(110.0f * Scale * ColonyHud::TextGrow());
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
	(void)Margin;
	LoadAlertPrefs();
	++Stats.LogPanelFrames;
	DrawAlerts(Bridge, Scale);
	DrawAlertPopup(Bridge, Scale);
	if (bLogOpen)
	{
		DrawLogWindow(Bridge, Scale);
	}
}

void UPanelsSubsystem::DrawAlerts(USimSubsystem& Bridge, float Scale)
{
	// sneed 2026-11-24, as RimWorld: short alerts down the right side, one to three words each, coloured by how bad they
	// are. Left click opens the whole message in the middle of the screen (and reads it, so it goes), right click turns it
	// off, shift right click turns its kind off for good (Menu > Message log turns it back on). Each fades out after
	// AlertSeconds; everything stays in the message log.
	(void)Bridge;
	TArray<int32> Live;
	GetLiveAlerts(Live);
	if (Live.IsEmpty())
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const double Now = FPlatformTime::Seconds();
	HudLayout::PlaceRight(Viewport, HudLayout::ERightSlot::Alerts, Scale);
	ImGui::SetNextWindowBgAlpha(0.0f);
	if (ImGui::Begin("##PanelsAlerts", nullptr, PanelFlags() | ImGuiWindowFlags_NoBackground))
	{
		const int32 Shown = FMath::Min(Live.Num(), 14);
		// All the same width, right-aligned: the widest title sets it.
		float Width = 0.0f;
		for (int32 Index = 0; Index < Shown; ++Index)
		{
			const FColonyLogLine& Line = Log[Live[Index]];
			const FString Label = Line.Repeats > 1 ? FString::Printf(TEXT("%s  x%d"), *Line.Title, Line.Repeats) : Line.Title;
			Width = FMath::Max(Width, ImGui::CalcTextSize(TCHAR_TO_UTF8(*Label)).x);
		}
		Width += 2.0f * ImGui::GetStyle().FramePadding.x + 10.0f * Scale;
		const bool bShift = ImGui::GetIO().KeyShift;
		int32 Open = -1;
		int32 Dismiss = -1;
		for (int32 Index = 0; Index < Shown; ++Index)
		{
			const int32 LogIndex = Live[Index];
			const FColonyLogLine& Line = Log[LogIndex];
			const double Age = Now - Line.RealSeconds;
			const float Fade = static_cast<float>(FMath::Clamp((AlertSeconds(Line.Severity) - Age) / 8.0, 0.15, 1.0));
			const ImVec4 Ink = SeverityColor(Line.Severity);
			ImGui::PushID(LogIndex);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, Fade);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.03f, 0.05f, 0.043f, 0.88f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColonyHud::ButtonHover());
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(Ink.x, Ink.y, Ink.z, 0.55f));
			ImGui::PushStyleColor(ImGuiCol_Text, Ink);
			ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));
			const FString Label = Line.Repeats > 1 ? FString::Printf(TEXT("%s  x%d"), *Line.Title, Line.Repeats) : Line.Title;
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + FMath::Max(0.0f, ImGui::GetContentRegionAvail().x - Width));
			ImGui::Button(TCHAR_TO_UTF8(*(Label + TEXT("##alert"))), ImVec2(Width, 0.0f));
			// A bar of the severity's colour on the left edge.
			const ImVec2 Min = ImGui::GetItemRectMin();
			const ImVec2 Max = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddRectFilled(Min, ImVec2(Min.x + 3.0f * Scale, Max.y), ImGui::GetColorU32(Ink));
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				Open = LogIndex;
			}
			else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			{
				Dismiss = LogIndex;
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
			{
				ImGui::SetTooltip("%s\n\nClick: read it.  Right click: dismiss.  Shift + right click: never show this kind again.", TCHAR_TO_UTF8(*Line.Text));
			}
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(4);
			ImGui::PopID();
		}
		if (Live.Num() > Shown)
		{
			PTextColored(ColonyHud::TextFaint(), FString::Printf(TEXT("%d more in the message log"), Live.Num() - Shown));
		}
		HudLayout::ReportRight(HudLayout::ERightSlot::Alerts);
		if (Open >= 0)
		{
			OpenAlert(Open);
		}
		if (Dismiss >= 0)
		{
			DismissAlert(Dismiss, bShift);
		}
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawAlertPopup(USimSubsystem& Bridge, float Scale)
{
	if (!Log.IsValidIndex(PopupLine))
	{
		PopupLine = -1;
		return;
	}
	const FColonyLogLine& Line = Log[PopupLine];
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Width = FMath::Min(620.0f * Scale * FMath::Max(1.0f, ImGui::GetStyle().FontScaleMain / (1.2f * Scale)), Viewport->Size.x * 0.8f);
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.42f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(Width, 0.0f), ImVec2(Width, Viewport->Size.y * 0.7f));
	if (ImGui::Begin("##PanelsAlertPopup", nullptr, PanelFlags()))
	{
		ColonyHud::PushLarge();
		PTextColored(SeverityColor(Line.Severity), Line.Title);
		ColonyHud::PopLarge();
		PTextColored(ColonyHud::TextFaint(), TimeText(Line.GameSeconds));
		ImGui::Separator();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Width - 2.0f * ImGui::GetStyle().WindowPadding.x);
		PText(Line.Text);
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
		const bool bCanJump = Overseer && Line.Subject != colony::kNoEntity && Bridge.GetCurrentSnapshot().find(Line.Subject);
		if (bCanJump && ColonyHud::Button(TEXT("Jump to it")))
		{
			FSimPick Pick;
			Pick.Kind = ESimPickKind::Thing;
			Pick.Thing = Line.Subject;
			Overseer->Select(Pick);
			Overseer->FocusOnSelection();
			PopupLine = -1;
		}
		if (bCanJump)
		{
			ImGui::SameLine();
		}
		if (ColonyHud::Button(TEXT("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			PopupLine = -1;
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Never show this kind")))
		{
			SetAlertKindOff(Line.Kind, true);
			PopupLine = -1;
		}
		ColonyHud::Tip(TEXT("Turn this kind of alert off for good. The message log turns it back on"));
	}
	ImGui::End();
}

void UPanelsSubsystem::DrawLogWindow(USimSubsystem& Bridge, float Scale)
{
	// Menu > Message log: every message with its time and title, newest last, and the alert kinds turned off for good.
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	const float Width = FMath::Min(900.0f * Scale * FMath::Max(1.0f, ImGui::GetStyle().FontScaleMain / (1.2f * Scale)), Viewport->Size.x * 0.9f);
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(Width, Viewport->Size.y * 0.7f), ImGuiCond_Always);
	if (ImGui::Begin("##PanelsLogWindow", nullptr, PanelFlags() & ~ImGuiWindowFlags_AlwaysAutoResize))
	{
		ColonyHud::Title(TEXT("Message log"));
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f * Scale);
		if (ColonyHud::Button(TEXT("Close")))
		{
			bLogOpen = false;
		}
		const float OffHeight = ExcludedKinds.IsEmpty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * (1.6f + FMath::Min(ExcludedKinds.Num(), 4));
		if (ImGui::BeginChild("##LogLines", ImVec2(0.0f, -OffHeight), ImGuiChildFlags_Borders))
		{
			if (Log.IsEmpty())
			{
				PTextColored(ColonyHud::TextFaint(), TEXT("Nothing has happened yet."));
			}
			for (int32 Index = 0; Index < Log.Num(); ++Index)
			{
				const FColonyLogLine& Line = Log[Index];
				ImGui::PushID(Index);
				PTextColored(ColonyHud::TextFaint(), TimeText(Line.GameSeconds));
				ImGui::SameLine();
				PTextColored(SeverityColor(Line.Severity), Line.Title + (Line.Repeats > 1 ? FString::Printf(TEXT(" x%d"), Line.Repeats) : FString()));
				ImGui::SameLine();
				ImGui::PushTextWrapPos(0.0f);
				const FString Text = Line.Text;
				if (ImGui::Selectable(TCHAR_TO_UTF8(*Text)) && Overseer && Line.Subject != colony::kNoEntity && Bridge.GetCurrentSnapshot().find(Line.Subject))
				{
					FSimPick Pick;
					Pick.Kind = ESimPickKind::Thing;
					Pick.Thing = Line.Subject;
					Overseer->Select(Pick);
					Overseer->FocusOnSelection();
				}
				ImGui::PopTextWrapPos();
				ImGui::PopID();
			}
			if (Log.Num() != LastLogCountShown)
			{
				ImGui::SetScrollHereY(1.0f);
			}
			LastLogCountShown = Log.Num();
		}
		ImGui::EndChild();
		if (!ExcludedKinds.IsEmpty())
		{
			PTextColored(ColonyHud::TextDim(), TEXT("Alerts turned off for good (they still go in the log):"));
			FString TurnOn;
			for (const FString& Kind : ExcludedKinds)
			{
				// The newest title of that kind names it, else the kind itself.
				FString Name = Kind;
				for (int32 Index = Log.Num() - 1; Index >= 0; --Index)
				{
					if (Log[Index].Kind == Kind)
					{
						Name = Log[Index].Title;
						break;
					}
				}
				if (ColonyHud::Button(FString::Printf(TEXT("Turn on: %s##%s"), *Name, *Kind)))
				{
					TurnOn = Kind;
				}
				ImGui::SameLine();
			}
			ImGui::NewLine();
			if (!TurnOn.IsEmpty())
			{
				SetAlertKindOff(TurnOn, false);
			}
		}
	}
	ImGui::End();
}
