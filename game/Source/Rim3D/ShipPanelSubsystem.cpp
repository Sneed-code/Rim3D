#include "ShipPanelSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

#include <imgui.h>

#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"

#include <string>

namespace
{
	TAutoConsoleVariable<int32> GShipShowUi(
		TEXT("Ship.UI"), 1, TEXT("1 shows the ship window and the ending screen, 0 hides them."), ECVF_Default);

	FAutoConsoleCommandWithWorld GShipOpenCommand(
		TEXT("Ship.Open"), TEXT("Opens or closes the ship window (parts, reactor, caskets, launch)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UShipPanelSubsystem* Ship = World ? World->GetSubsystem<UShipPanelSubsystem>() : nullptr)
			{
				Ship->SetOpen(!Ship->IsOpen());
			}
		}));

	// ShipView::phase (colony::ShipPhase).
	constexpr uint8 ShipIdle = 0;
	constexpr uint8 ShipActivating = 1;
	constexpr uint8 ShipReady = 2;

	void SpText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	void SpTextColored(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		SpText(Text);
		ImGui::PopStyleColor();
	}

	void SpTextWrapped(const ImVec4& Color, const FString& Text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Color);
		ImGui::TextWrapped("%s", TCHAR_TO_UTF8(*Text));
		ImGui::PopStyleColor();
	}

	FString SpCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	ImGuiWindowFlags SpFlags()
	{
		return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	}

	/** "12.5 days", "5 h", "less than an hour". */
	FString SpSpan(int64 Seconds)
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

	const ImVec4 SpGrey(0.7f, 0.7f, 0.7f, 1.0f);
	const ImVec4 SpGood(0.55f, 0.85f, 0.55f, 1.0f);
	const ImVec4 SpBad(0.95f, 0.45f, 0.35f, 1.0f);
	const ImVec4 SpWarm(0.95f, 0.65f, 0.30f, 1.0f);

	/** "Engines  2 of 3", green when there are enough. */
	void SpPartLine(const TCHAR* Label, int32 Have, int32 Need, float Scale)
	{
		const bool bEnough = Have >= Need;
		SpTextColored(bEnough ? SpGood : SpBad, FString::Printf(TEXT("%s %s"), bEnough ? TEXT("[x]") : TEXT("[ ]"), Label));
		ImGui::SameLine(280.0f * Scale);
		SpText(FString::Printf(TEXT("%d of %d"), Have, Need));
	}
}

// ---- Setup -------------------------------------------------------------------------------------

bool UShipPanelSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UShipPanelSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UShipPanelSubsystem, STATGROUP_Tickables);
}

void UShipPanelSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UShipPanelSubsystem::OnSimEvent);
		EventSource = Bridge;
	}
}

void UShipPanelSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = EventSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

USimSubsystem* UShipPanelSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

bool UShipPanelSubsystem::IsOverseerActive() const
{
	UWorld* World = GetWorld();
	const UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

void UShipPanelSubsystem::SyncWithSim()
{
	// A new sim (a new game, a load) starts with the window shut and no ending shown.
	const USimSubsystem* Bridge = GetBridge();
	if (Bridge && ContentGeneration != Bridge->GetSimGeneration())
	{
		ContentGeneration = Bridge->GetSimGeneration();
		ResetForNewSim();
	}
}

void UShipPanelSubsystem::ResetForNewSim()
{
	bOpen = false;
	bEndingShown = false;
	EscapedNames.Reset();
	LastRiderNames.Reset();
	Stats.CommandsSent = 0;
}

void UShipPanelSubsystem::Submit(colony::Command Command)
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

void UShipPanelSubsystem::OnSimEvent(const colony::Event& Event)
{
	SyncWithSim();
	if (Event.type == colony::EventType::ShipLaunched)
	{
		// The riders are off the map by now: their names come from the last tick. A rider in a casket that touches no
		// beam stays behind, so the names are only trusted when the count agrees.
		const int32 Escaped = FMath::RoundToInt(Event.amount);
		EscapedNames = LastRiderNames.Num() == Escaped ? LastRiderNames : TArray<FString>();
		bEndingShown = true;
		bOpen = false;
	}
}

FString UShipPanelSubsystem::NameOf(colony::Entity Pawn) const
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::Sim* Sim = Bridge ? Bridge->GetSim() : nullptr;
	if (const colony::Colonist* Colonist = Sim ? Sim->colonistOf(Pawn) : nullptr)
	{
		return UTF8_TO_TCHAR((Colonist->nickName.empty() ? Colonist->firstName : Colonist->nickName).c_str());
	}
	// A prisoner or an animal: its def's label.
	const colony::ThingView* View = Bridge ? Bridge->GetCurrentSnapshot().find(Pawn) : nullptr;
	return View && View->def.valid() && Sim ? SpCapital(Sim->defs().get(View->def).label()) : FString(TEXT("Someone"));
}

// ---- What the snapshot says ------------------------------------------------------------------------

void UShipPanelSubsystem::GetParts(const char* Part, TArray<colony::Entity>& Out) const
{
	Out.Reset();
	const USimSubsystem* Bridge = GetBridge();
	const colony::Sim* Sim = Bridge ? Bridge->GetSim() : nullptr;
	if (!Sim)
	{
		return;
	}
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built && View.def.valid() &&
			Sim->defs().get(View.def).text("ship.part") == Part)
		{
			Out.Add(View.id);
		}
	}
}

void UShipPanelSubsystem::GetCaskets(TArray<colony::Entity>& Out) const
{
	GetParts("Casket", Out);
}

void UShipPanelSubsystem::GetReactors(TArray<colony::Entity>& Out) const
{
	GetParts("Reactor", Out);
}

colony::Entity UShipPanelSubsystem::RiderOf(colony::Entity Casket) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge)
	{
		return colony::kNoEntity;
	}
	for (const std::pair<colony::Entity, colony::Entity>& Aboard : Bridge->GetCurrentSnapshot().ship.aboard)
	{
		if (Aboard.first == Casket)
		{
			return Aboard.second;
		}
	}
	return colony::kNoEntity;
}

bool UShipPanelSubsystem::HasShip() const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return false;
	}
	const colony::ShipView& Ship = Bridge->GetCurrentSnapshot().ship;
	return Ship.phase != ShipIdle || Ship.won || Ship.beams + Ship.caskets + Ship.reactors + Ship.engines + Ship.cores + Ship.sensors > 0;
}

// ---- What a click does -------------------------------------------------------------------------

void UShipPanelSubsystem::SetOpen(bool bInOpen)
{
	SyncWithSim();
	bOpen = bInOpen;
}

bool UShipPanelSubsystem::StartReactor()
{
	const USimSubsystem* Bridge = GetBridge();
	TArray<colony::Entity> Reactors;
	GetReactors(Reactors);
	if (!Bridge || Reactors.IsEmpty() || Bridge->GetCurrentSnapshot().ship.phase != ShipIdle || Bridge->GetCurrentSnapshot().ship.won)
	{
		return false;
	}
	colony::ShipCommand Command;
	Command.action = "StartReactor";
	Command.thing = Reactors[0];
	Submit(std::move(Command));
	return true;
}

bool UShipPanelSubsystem::Board(colony::Entity Casket, colony::Entity Pawn)
{
	TArray<colony::Entity> Caskets;
	GetCaskets(Caskets);
	if (!Caskets.Contains(Casket))
	{
		return false;
	}
	colony::ShipCommand Command;
	Command.action = "Board";
	Command.thing = Casket;
	Command.pawn = Pawn;
	Submit(std::move(Command));
	return true;
}

bool UShipPanelSubsystem::Launch()
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Bridge->GetCurrentSnapshot().ship.canLaunch)
	{
		return false;
	}
	colony::ShipCommand Command;
	Command.action = "Launch";
	Submit(std::move(Command));
	return true;
}

// ---- The tick and the windows ------------------------------------------------------------------

void UShipPanelSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	SyncWithSim();
	LastRiderNames.Reset();
	for (const std::pair<colony::Entity, colony::Entity>& Aboard : Bridge->GetCurrentSnapshot().ship.aboard)
	{
		LastRiderNames.Add(NameOf(Aboard.second));
	}
	if ((!bOpen && !bEndingShown) || !IsOverseerActive() || GShipShowUi.GetValueOnGameThread() == 0)
	{
		return;
	}
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (ShowUi && ShowUi->GetInt() == 0)
	{
		return;
	}
	if (bEndingShown)
	{
		DrawEnding(*Bridge);
	}
	else
	{
		DrawWindow(*Bridge);
	}
}

void UShipPanelSubsystem::DrawWindow(USimSubsystem& Bridge)
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

	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::ShipView& Ship = Bridge.GetCurrentSnapshot().ship;
	const colony::Def* Rules = Sim.defs().find("Rules", "ShipRules");
	const int32 EnginesNeeded = Rules ? static_cast<int32>(Rules->integer("enginesNeeded", 3)) : 3;
	const double ActivationDays = Rules ? Rules->number("activationDays", 15.0) : 15.0;

	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f * Scale, 0.0f), ImVec2(560.0f * Scale, Viewport->Size.y * 0.8f));
	if (ImGui::Begin("##ShipWindow", nullptr, SpFlags() & ~ImGuiWindowFlags_NoScrollbar))
	{
		++Stats.WindowFrames;
		Stats.CasketRows = 0;
		Stats.bLaunchEnabled = false;
		SpText(TEXT("Ship"));
		ImGui::SameLine();
		if (ImGui::SmallButton("Close"))
		{
			SetOpen(false);
			ImGui::End();
			return;
		}

		// What a launch needs.
		ImGui::SeparatorText("Parts");
		SpPartLine(TEXT("Reactor"), Ship.reactors, 1, Scale);
		SpPartLine(TEXT("Engines"), Ship.engines, EnginesNeeded, Scale);
		SpPartLine(TEXT("Computer core"), Ship.cores, 1, Scale);
		SpPartLine(TEXT("Sensor cluster"), Ship.sensors, 1, Scale);
		SpPartLine(TEXT("Structural beams"), Ship.beams, 1, Scale);
		SpPartLine(TEXT("Cryptosleep caskets"), Ship.caskets, 1, Scale);
		SpTextColored(SpGrey, TEXT("Parts are built from the Ship tab of the build menu once researched."));

		// The reactor.
		ImGui::SeparatorText("Reactor");
		if (Ship.won)
		{
			SpText(TEXT("The ship has launched."));
		}
		else if (Ship.phase == ShipIdle)
		{
			TArray<colony::Entity> Reactors;
			GetReactors(Reactors);
			if (Reactors.IsEmpty())
			{
				SpTextColored(SpGrey, TEXT("No reactor stands yet."));
			}
			else
			{
				SpTextWrapped(SpWarm, FString::Printf(TEXT("Starting the reactor takes %.0f days, and raiders will come in waves the whole time."), ActivationDays));
				if (ImGui::Button("Start reactor"))
				{
					StartReactor();
				}
			}
		}
		else if (Ship.phase == ShipActivating)
		{
			const double Total = ActivationDays * colony::kSecondsPerDay;
			const float Done = Total > 0.0 ? FMath::Clamp(static_cast<float>(1.0 - Ship.activationLeft / Total), 0.0f, 1.0f) : 1.0f;
			SpText(FString::Printf(TEXT("Warming up: %s to go."), *SpSpan(Ship.activationLeft)));
			ImGui::ProgressBar(Done, ImVec2(-1.0f, 0.0f));
			SpTextColored(SpWarm, Ship.waves == 1 ? FString(TEXT("1 wave of raiders so far. Hold the reactor."))
												  : FString::Printf(TEXT("%d waves of raiders so far. Hold the reactor."), Ship.waves));
		}
		else if (Ship.phase == ShipReady)
		{
			SpTextColored(SpGood, TEXT("The reactor is active."));
		}

		// Who goes.
		ImGui::SeparatorText("Cryptosleep caskets");
		TArray<colony::Entity> Caskets;
		GetCaskets(Caskets);
		TArray<colony::Entity> Colonists;
		if (const UPanelsSubsystem* Panels = GetWorld()->GetSubsystem<UPanelsSubsystem>())
		{
			Panels->GetColonists(Colonists);
		}
		for (int32 Index = 0; Index < Caskets.Num(); ++Index)
		{
			const colony::Entity Casket = Caskets[Index];
			const colony::Entity Rider = RiderOf(Casket);
			ImGui::PushID(static_cast<int>(Index));
			SpText(FString::Printf(TEXT("Casket %d"), Index + 1));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(220.0f * Scale);
			const FString Current = Rider == colony::kNoEntity ? FString(TEXT("empty")) : NameOf(Rider);
			if (!Ship.won && ImGui::BeginCombo("##Rider", TCHAR_TO_UTF8(*Current)))
			{
				if (ImGui::Selectable("empty", Rider == colony::kNoEntity) && Rider != colony::kNoEntity)
				{
					Board(Casket, colony::kNoEntity);
				}
				for (int32 PawnIndex = 0; PawnIndex < Colonists.Num(); ++PawnIndex)
				{
					const colony::Entity Pawn = Colonists[PawnIndex];
					ImGui::PushID(static_cast<int>(PawnIndex));
					if (ImGui::Selectable(TCHAR_TO_UTF8(*NameOf(Pawn)), Pawn == Rider) && Pawn != Rider)
					{
						Board(Casket, Pawn);
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
			++Stats.CasketRows;
		}
		if (Caskets.IsEmpty())
		{
			SpTextColored(SpGrey, TEXT("No casket stands yet. Each one carries one person, and must touch a structural beam."));
		}
		else
		{
			SpTextColored(SpGrey, TEXT("A casket must touch a structural beam. Those not aboard stay behind."));
		}

		// Go.
		ImGui::Separator();
		if (!Ship.won)
		{
			Stats.bLaunchEnabled = Ship.canLaunch;
			ImGui::BeginDisabled(!Ship.canLaunch);
			if (ImGui::Button("Launch"))
			{
				Launch();
			}
			ImGui::EndDisabled();
			if (!Ship.canLaunch && !Ship.problem.empty())
			{
				ImGui::SameLine();
				SpTextColored(SpGrey, SpCapital(Ship.problem) + TEXT("."));
			}
		}
	}
	ImGui::End();
}

void UShipPanelSubsystem::DrawEnding(USimSubsystem& Bridge)
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
	const colony::ShipView& Ship = Bridge.GetCurrentSnapshot().ship;

	// A dark veil over the world, then the words in the middle.
	ImGui::GetBackgroundDrawList()->AddRectFilled(Viewport->Pos, ImVec2(Viewport->Pos.x + Viewport->Size.x, Viewport->Pos.y + Viewport->Size.y), IM_COL32(0, 0, 0, 170));
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f * Scale, 0.0f), ImVec2(640.0f * Scale, Viewport->Size.y * 0.8f));
	if (ImGui::Begin("##ShipEnding", nullptr, SpFlags()))
	{
		++Stats.EndingFrames;
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.8f);
		SpText(TEXT("The ship has launched"));
		ImGui::PopFont();
		ImGui::Spacing();
		FString Who;
		for (const FString& Name : EscapedNames)
		{
			Who += (Who.IsEmpty() ? TEXT("") : TEXT(", ")) + Name;
		}
		if (Ship.escaped == 0)
		{
			SpTextWrapped(SpGrey, TEXT("It left with no one aboard."));
		}
		else
		{
			SpTextWrapped(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), Who.IsEmpty()
				? FString::Printf(TEXT("%d asleep in its caskets rise out of the sky, bound for a far star. The voyage will take longer than any of them has lived."), Ship.escaped)
				: EscapedNames.Num() == 1
				? FString::Printf(TEXT("%s, asleep in a casket, rises out of the sky, bound for a far star. The voyage will take longer than %s has lived."), *Who, *Who)
				: FString::Printf(TEXT("%s, asleep in the caskets, rise out of the sky, bound for a far star. The voyage will take longer than any of them has lived."), *Who));
		}
		ImGui::Spacing();
		SpTextWrapped(SpGrey, TEXT("Those left behind carry on. You can keep playing the colony."));
		ImGui::Spacing();
		if (ImGui::Button("Keep playing"))
		{
			DismissEnding();
		}
	}
	ImGui::End();
}
