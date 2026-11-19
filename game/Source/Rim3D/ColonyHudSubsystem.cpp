#include "ColonyHudSubsystem.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

#include <imgui.h>

#include "ColonyHudStyle.h"

#include "GraphicsOptionsSubsystem.h"
#include "KeyBindings.h"
#include "LevelsSubsystem.h"
#include "OrdersSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PanelsSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "QuestPanelSubsystem.h"
#include "ShipPanelSubsystem.h"
#include "ShooterFeelSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"
#include "SkySubsystem.h"
#include "SoundSubsystem.h"
#include "TradePanelSubsystem.h"
#include "WorldScreenSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <string>

namespace
{
	const TCHAR* const HudPrefsSection = TEXT("Rim3D.Hud");

	TAutoConsoleVariable<int32> GHudShowUi(
		TEXT("Hud.UI"), 1, TEXT("1 shows the overseer's bottom bar, date box, hover readout and info window; 0 hides them."), ECVF_Default);

	TAutoConsoleVariable<int32> GHudTextSize(
		TEXT("UI.TextSize"), 0, TEXT("0 normal text, 1 bigger text in every panel (Settings > Game > Text size; saved)."), ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs GHudTabCommand(
		TEXT("Hud.Tab"), TEXT("Hud.Tab <name> opens a tab of the bottom bar (Build, Work, Schedule, Assign, Animals, Mechs, Wildlife, Research, Quests, World, Inventory); no name closes it."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UColonyHudSubsystem* Hud = World ? World->GetSubsystem<UColonyHudSubsystem>() : nullptr;
			if (!Hud)
			{
				return;
			}
			if (Args.Num() == 0)
			{
				Hud->CloseTabs();
				return;
			}
			for (int32 Index = 1; Index < static_cast<int32>(EHudTab::Count); ++Index)
			{
				if (Args[0].Equals(UColonyHudSubsystem::TabLabel(static_cast<EHudTab>(Index)), ESearchCase::IgnoreCase))
				{
					Hud->OpenTab(static_cast<EHudTab>(Index));
				}
			}
		}));

	FString HudCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (Out.Len() > 0)
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	const TCHAR* HudTerrainName(colony::Terrain Terrain)
	{
		switch (Terrain)
		{
			case colony::Terrain::Water: return TEXT("Deep water");
			case colony::Terrain::Sand: return TEXT("Sand");
			case colony::Terrain::Soil: return TEXT("Soil");
			case colony::Terrain::Rock: return TEXT("Rock");
			case colony::Terrain::RoughStone: return TEXT("Rough stone");
			case colony::Terrain::ShallowWater: return TEXT("Shallow water");
			case colony::Terrain::Marsh: return TEXT("Marsh");
			case colony::Terrain::Mud: return TEXT("Mud");
			case colony::Terrain::Air: return TEXT("Open air");
			case colony::Terrain::UpperFloor: return TEXT("Upper floor");
		}
		return TEXT("Ground");
	}

	/** A row of the info window: label on the left, value on the right, every other row shaded. */
	void HudStatRow(const FString& Label, const FString& Value, int32& Row)
	{
		ImGui::TableNextRow();
		if (Row++ % 2 == 0)
		{
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ColonyHud::ButtonBg()));
		}
		ImGui::TableNextColumn();
		ColonyHud::Text(Label);
		ImGui::TableNextColumn();
		ColonyHud::Text(Value);
	}

	FString HudPercent(double Share)
	{
		return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Share * 100.0));
	}

	/** The things an apparel or look change touches, so a mugshot is remade when they change. */
	uint64 HudLookSignature(const colony::Snapshot& Snapshot, const colony::ThingView& View)
	{
		uint64 Hash = 1469598103934665603ull ^ static_cast<uint64>(View.looks + 1);
		for (uint32 Index = 0; Index < View.apparelCount; ++Index)
		{
			const colony::ApparelView& Piece = Snapshot.apparel[View.firstApparel + Index];
			Hash = (Hash ^ static_cast<uint64>(Piece.def.value + 1)) * 1099511628211ull;
		}
		Hash = (Hash ^ static_cast<uint64>(View.hideGear ? 7 : 3)) * 1099511628211ull;
		return Hash;
	}
}

// ---- Setup -------------------------------------------------------------------------------------

bool UColonyHudSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UColonyHudSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UColonyHudSubsystem, STATGROUP_Tickables);
}

void UColonyHudSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
	LoadPrefs();

	// The pages every game has. Others (Controls) are added by the code that owns them.
	AddSettingsPage(TEXT("Game"), nullptr);
	AddSettingsPage(TEXT("Display"), [](float Scale)
	{
		if (UGraphicsOptionsSubsystem* Graphics = GEngine ? GEngine->GetEngineSubsystem<UGraphicsOptionsSubsystem>() : nullptr)
		{
			Graphics->DrawOptions(Scale);
		}
	});
	AddSettingsPage(TEXT("Sound"), [this](float Scale)
	{
		if (USoundSubsystem* Sound = GetWorld() ? GetWorld()->GetSubsystem<USoundSubsystem>() : nullptr)
		{
			Sound->DrawMixerBody(Scale);
		}
	});
	AddSettingsPage(TEXT("Feel"), [this](float Scale)
	{
		if (UShooterFeelSubsystem* Feel = GetWorld() ? GetWorld()->GetSubsystem<UShooterFeelSubsystem>() : nullptr)
		{
			Feel->DrawSettingsBody(Scale);
		}
	});
	AddSettingsPage(TEXT("Controls"), [this](float Scale) { DrawKeyListPage(Scale); });
}

void UColonyHudSubsystem::Deinitialize()
{
	if (MugshotActor)
	{
		MugshotActor->Destroy();
	}
	MugshotActor = nullptr;
	MugshotCapture = nullptr;
	Mugshots.Reset();
	MugshotTargets.Reset();
	Super::Deinitialize();
}

USimSubsystem* UColonyHudSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

bool UColonyHudSubsystem::IsOverseerActive() const
{
	UWorld* World = GetWorld();
	const UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

void UColonyHudSubsystem::Submit(colony::Command Command)
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

void UColonyHudSubsystem::LoadPrefs()
{
	Pinned.Reset();
	FString PinnedText = TEXT("Silver");  // silver is pinned by default (ui-layout.md)
	int32 TextSize = 0;
	if (GConfig)
	{
		GConfig->GetString(HudPrefsSection, TEXT("Pinned"), PinnedText, GGameUserSettingsIni);
		GConfig->GetInt(HudPrefsSection, TEXT("TextSize"), TextSize, GGameUserSettingsIni);
	}
	PinnedText.ParseIntoArray(Pinned, TEXT(","), true);
	GHudTextSize.AsVariable()->Set(TextSize == 1 ? 1 : 0, ECVF_SetByCode);
}

void UColonyHudSubsystem::SavePrefs() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetString(HudPrefsSection, TEXT("Pinned"), *FString::Join(Pinned, TEXT(",")), GGameUserSettingsIni);
	GConfig->SetInt(HudPrefsSection, TEXT("TextSize"), GHudTextSize.GetValueOnGameThread(), GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void UColonyHudSubsystem::TogglePin(const FString& DefName)
{
	if (Pinned.Remove(DefName) == 0)
	{
		Pinned.Add(DefName);
	}
	SavePrefs();
}

bool UColonyHudSubsystem::SetWorkGroup(colony::Entity Pawn, const FString& Group)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Bridge->GetSim()->colonistOf(Pawn))
	{
		return false;
	}
	Submit(colony::SetWorkGroupCommand{Pawn, std::string(TCHAR_TO_UTF8(*Group.Left(colony::Colonist::kMaxWorkGroupLength)))});
	return true;
}

bool UColonyHudSubsystem::GetButtonRect(const FString& Label, FBox2D& Out) const
{
	const FBox2D* Found = ButtonRects.Find(Label);
	if (Found)
	{
		Out = *Found;
	}
	return Found != nullptr;
}

// ---- Tabs --------------------------------------------------------------------------------------

const TCHAR* UColonyHudSubsystem::TabLabel(EHudTab Tab)
{
	switch (Tab)
	{
		case EHudTab::Build: return TEXT("Build");
		case EHudTab::Work: return TEXT("Work");
		case EHudTab::Schedule: return TEXT("Schedule");
		case EHudTab::Assign: return TEXT("Assign");
		case EHudTab::Animals: return TEXT("Animals");
		case EHudTab::Mechs: return TEXT("Mechs");
		case EHudTab::Wildlife: return TEXT("Wildlife");
		case EHudTab::Research: return TEXT("Research");
		case EHudTab::Quests: return TEXT("Quests");
		case EHudTab::World: return TEXT("World");
		case EHudTab::Inventory: return TEXT("Inventory");
		default: return TEXT("");
	}
}

EHudTab UColonyHudSubsystem::GetOpenTab() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return EHudTab::None;
	}
	if (const UOrdersSubsystem* Orders = World->GetSubsystem<UOrdersSubsystem>(); Orders && Orders->IsBuildMenuOpen())
	{
		return EHudTab::Build;
	}
	if (const UPanelsSubsystem* Panels = World->GetSubsystem<UPanelsSubsystem>())
	{
		if (Panels->GetTab() == EColonyTab::Work)
		{
			return EHudTab::Work;
		}
		if (Panels->GetTab() == EColonyTab::Schedule)
		{
			return EHudTab::Schedule;
		}
	}
	if (const UQuestPanelSubsystem* Quests = World->GetSubsystem<UQuestPanelSubsystem>(); Quests && Quests->IsOpen())
	{
		return EHudTab::Quests;
	}
	if (const UWorldScreenSubsystem* WorldScreen = World->GetSubsystem<UWorldScreenSubsystem>(); WorldScreen && WorldScreen->IsOpen())
	{
		return EHudTab::World;
	}
	return OwnTab;
}

void UColonyHudSubsystem::CloseTabs()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (UOrdersSubsystem* Orders = World->GetSubsystem<UOrdersSubsystem>(); Orders && Orders->IsBuildMenuOpen())
	{
		Orders->SetBuildMenuOpen(false);
	}
	if (UPanelsSubsystem* Panels = World->GetSubsystem<UPanelsSubsystem>())
	{
		Panels->SetTab(EColonyTab::None);
	}
	if (UQuestPanelSubsystem* Quests = World->GetSubsystem<UQuestPanelSubsystem>(); Quests && Quests->IsOpen())
	{
		Quests->SetOpen(false);
	}
	if (UWorldScreenSubsystem* WorldScreen = World->GetSubsystem<UWorldScreenSubsystem>(); WorldScreen && WorldScreen->IsOpen())
	{
		WorldScreen->SetOpen(false);
	}
	OwnTab = EHudTab::None;
}

void UColonyHudSubsystem::OpenTab(EHudTab Tab)
{
	CloseTabs();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	switch (Tab)
	{
		case EHudTab::Build:
			if (UOrdersSubsystem* Orders = World->GetSubsystem<UOrdersSubsystem>())
			{
				Orders->SetBuildMenuOpen(true);
			}
			break;
		case EHudTab::Work:
		case EHudTab::Schedule:
			if (UPanelsSubsystem* Panels = World->GetSubsystem<UPanelsSubsystem>())
			{
				Panels->SetTab(Tab == EHudTab::Work ? EColonyTab::Work : EColonyTab::Schedule);
			}
			break;
		case EHudTab::Quests:
			if (UQuestPanelSubsystem* Quests = World->GetSubsystem<UQuestPanelSubsystem>())
			{
				Quests->SetOpen(true);
			}
			break;
		case EHudTab::World:
			if (UWorldScreenSubsystem* WorldScreen = World->GetSubsystem<UWorldScreenSubsystem>())
			{
				WorldScreen->SetOpen(true);
			}
			break;
		case EHudTab::None:
		case EHudTab::Count:
			break;
		default:
			OwnTab = Tab;
			break;
	}
}

void UColonyHudSubsystem::ToggleTab(EHudTab Tab)
{
	if (GetOpenTab() == Tab)
	{
		CloseTabs();
	}
	else
	{
		OpenTab(Tab);
	}
}

// ---- Settings pages ----------------------------------------------------------------------------

void UColonyHudSubsystem::AddSettingsPage(const FString& Name, TFunction<void(float Scale)> Draw)
{
	for (FSettingsPage& Page : SettingsPages)
	{
		if (Page.Name == Name)
		{
			Page.Draw = MoveTemp(Draw);
			return;
		}
	}
	SettingsPages.Add(FSettingsPage{Name, MoveTemp(Draw)});
}

void UColonyHudSubsystem::OpenSettingsPage(const FString& Name)
{
	SettingsPage = Name;
	bSettingsOpen = true;
}

// ---- The tick ----------------------------------------------------------------------------------

void UColonyHudSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (ContentGeneration != Bridge->GetSimGeneration())
	{
		// A new game or a load: windows about the old one close.
		ContentGeneration = Bridge->GetSimGeneration();
		InfoThing = colony::kNoEntity;
		OwnTab = EHudTab::None;
		ResearchPicked.Reset();
		RenamingAnimal = colony::kNoEntity;
		PatientPicked = colony::kNoEntity;
		AreaPicked = 0;
		Mugshots.Reset();
		MugshotTargets.Reset();
	}
	SendPendingNarrator(*Bridge);
	// The main menu at launch: a plain start (no scenario on the command line), not in a smoke test (-NoMenu skips it).
	if (!bMainMenuChecked)
	{
		bMainMenuChecked = true;
		const TCHAR* Line = FCommandLine::Get();
		bMainMenuOpen = Bridge->GetStartOptions().Scenario.IsEmpty() && !FParse::Param(Line, TEXT("NoMenu")) && !FString(Line).Contains(TEXT("-SmokeTest="));
	}
	if (bMainMenuOpen)
	{
		DrawMainMenu(*Bridge);
		return;
	}
	UpdateMugshots(*Bridge);
	if (!IsOverseerActive() || GHudShowUi.GetValueOnGameThread() == 0)
	{
		bHasHoverCell = false;
		return;
	}
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (ShowUi && ShowUi->GetInt() == 0)
	{
		return;
	}
	DrawHud(*Bridge);
}

void UColonyHudSubsystem::DrawHud(USimSubsystem& Bridge)
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
	const float Scale = ColonyHud::ViewScale(Viewport);
	ColonyHud::ApplyFrameStyle(Scale);
	ButtonRects.Reset();

	// The World tab is a whole screen of its own: only the bar's way back stays (Esc or M close it too).
	const EHudTab Open = GetOpenTab();
	DrawBar(Bridge, Scale);
	if (Open == EHudTab::World)
	{
		return;
	}
	DrawDateBox(Bridge, Scale);
	switch (Open)
	{
		case EHudTab::Assign: DrawAssignTab(Bridge, Scale); break;
		case EHudTab::Animals: DrawAnimalsTab(Bridge, Scale, false); break;
		case EHudTab::Wildlife: DrawAnimalsTab(Bridge, Scale, true); break;
		case EHudTab::Mechs: DrawMechsTab(Bridge, Scale); break;
		case EHudTab::Research: DrawResearchTab(Bridge, Scale); break;
		case EHudTab::Inventory: DrawInventoryTab(Bridge, Scale); break;
		default: break;
	}
	DrawScreen(Bridge, Scale);
	DrawHoverReadout(Bridge, Scale);
	DrawInfoWindow(Bridge, Scale);
	if (bSettingsOpen)
	{
		DrawSettings(Bridge, Scale);
	}
}

// ---- The bottom bar ----------------------------------------------------------------------------

void UColonyHudSubsystem::DrawBar(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Height = ColonyHud::BarHeight(Scale);
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x, Viewport->Pos.y + Viewport->Size.y - Height), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(Viewport->Size.x, Height), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * Scale, ColonyHud::BarPadY(Scale)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f * Scale, 0.0f));
	const ImGuiWindowFlags Flags = (ColonyHud::BoxFlags() & ~ImGuiWindowFlags_AlwaysAutoResize) | ImGuiWindowFlags_NoScrollbar;
	if (ImGui::Begin("##HudBar", nullptr, Flags))
	{
		++Stats.BarFrames;
		const ImGuiStyle& Style = ImGui::GetStyle();
		const auto Record = [this](const FString& Label)
		{
			const ImVec2 Min = ImGui::GetItemRectMin();
			const ImVec2 Max = ImGui::GetItemRectMax();
			ButtonRects.Add(Label, FBox2D(FVector2D(Min.x, Min.y), FVector2D(Max.x, Max.y)));
		};

		// The right-hand group: speed, key help, Factions, Menu (and Ship while there is one). Measured first so the tabs fit.
		UShipPanelSubsystem* Ship = GetWorld()->GetSubsystem<UShipPanelSubsystem>();
		const bool bShip = Ship && Ship->HasShip();
		const TCHAR* const Speeds[] = {TEXT("Pause"), TEXT("1x"), TEXT("2x"), TEXT("3x")};
		TArray<FString> RightLabels;
		if (bShip)
		{
			RightLabels.Add(TEXT("Ship"));
		}
		for (const TCHAR* Speed : Speeds)
		{
			RightLabels.Add(Speed);
		}
		RightLabels.Append({TEXT("Keys"), TEXT("Factions"), TEXT("Menu")});
		float RightWidth = 0.0f;
		for (const FString& Label : RightLabels)
		{
			RightWidth += ImGui::CalcTextSize(TCHAR_TO_UTF8(*Label)).x + 2.0f * Style.FramePadding.x + Style.ItemSpacing.x;
		}
		RightWidth += 18.0f * Scale;  // gap before the group

		// The tabs, equal widths, from the left.
		const EHudTab Open = GetOpenTab();
		const int32 TabCount = static_cast<int32>(EHudTab::Count) - 1;
		const float TabsWidth = ImGui::GetContentRegionAvail().x - RightWidth;
		const float TabWidth = FMath::Clamp((TabsWidth - Style.ItemSpacing.x * (TabCount - 1)) / TabCount, 40.0f * Scale, 150.0f * Scale);
		UQuestPanelSubsystem* Quests = GetWorld()->GetSubsystem<UQuestPanelSubsystem>();
		for (int32 Index = 1; Index <= TabCount; ++Index)
		{
			const EHudTab Tab = static_cast<EHudTab>(Index);
			FString Label = TabLabel(Tab);
			bool bAmber = false;
			if (Tab == EHudTab::Quests && Quests)
			{
				// "Quests (2)" while offers wait for an answer; amber until the window has been opened since the newest one.
				const int32 Waiting = Quests->CountOffered();
				Label = Waiting > 0 ? FString::Printf(TEXT("Quests (%d)"), Waiting) : Label;
				bAmber = Quests->HasUnseenOffer() && Open != EHudTab::Quests;
			}
			if (bAmber)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.40f, 0.12f, 1.0f));
			}
			if (ColonyHud::Button(Label + TEXT("##tab"), Open == Tab, ImVec2(TabWidth, 0.0f)))
			{
				ToggleTab(Tab);
			}
			if (bAmber)
			{
				ImGui::PopStyleColor();
			}
			Record(TabLabel(Tab));
			switch (Tab)
			{
				case EHudTab::Build: ColonyHud::Tip(TEXT("Plan buildings, floors, zones and orders (mine, chop, harvest)")); break;
				case EHudTab::Work: ColonyHud::Tip(TEXT("Who does which work, and in what order. Work groups sort the list")); break;
				case EHudTab::Schedule: ColonyHud::Tip(TEXT("Each colonist's day, hour by hour: anything, work, recreation or sleep")); break;
				case EHudTab::Assign: ColonyHud::Tip(TEXT("Outfits, drug policies, medical care and allowed areas")); break;
				case EHudTab::Animals: ColonyHud::Tip(TEXT("Tame animals: training, zones, names")); break;
				case EHudTab::Mechs: ColonyHud::Tip(TEXT("The colony's robots")); break;
				case EHudTab::Wildlife: ColonyHud::Tip(TEXT("Wild animals on the map: mark them to tame or hunt")); break;
				case EHudTab::Research: ColonyHud::Tip(TEXT("Pick what the researchers study next")); break;
				case EHudTab::Quests: ColonyHud::Tip(TEXT("Offers waiting for an answer, quests under way and finished ones")); break;
				case EHudTab::World: ColonyHud::Tip(TEXT("The world map (M)")); break;
				case EHudTab::Inventory: ColonyHud::Tip(TEXT("Everything the colony has on the map. A star pins it to the top left")); break;
				default: break;
			}
			ImGui::SameLine();
		}

		// The right-hand group.
		ImGui::SetCursorPosX(FMath::Max(ImGui::GetCursorPosX() + 18.0f * Scale, ImGui::GetWindowWidth() - RightWidth + 12.0f * Scale));
		if (bShip)
		{
			const colony::ShipView& ShipView = Bridge.GetCurrentSnapshot().ship;
			if (ShipView.canLaunch)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.6f, 0.3f, 1.0f));
			}
			if (ColonyHud::Button(TEXT("Ship"), Ship->IsOpen()))
			{
				Ship->SetOpen(!Ship->IsOpen());
			}
			if (ShipView.canLaunch)
			{
				ImGui::PopStyleColor();
			}
			ColonyHud::Tip(ShipView.phase == 1 ? FString::Printf(TEXT("The reactor warms up: %.1f days left"),
				static_cast<double>(ShipView.activationLeft) / colony::kSecondsPerDay) : FString(TEXT("The ship and its launch")));
			ImGui::SameLine();
		}
		for (int32 Speed = 0; Speed < 4; ++Speed)
		{
			if (ColonyHud::Button(Speeds[Speed], Bridge.GetSpeed() == Speed))
			{
				Bridge.SetSpeed(Speed);
			}
			Record(Speeds[Speed]);
			ColonyHud::Tip(Speed == 0 ? FString(TEXT("Pause (Space)")) : FString::Printf(TEXT("Speed %d (%d)"), Speed, Speed));
			ImGui::SameLine();
		}
		if (ColonyHud::Button(TEXT("Keys"), USimSubsystem::IsKeyHelpShown()))
		{
			if (IConsoleVariable* KeyHelp = IConsoleManager::Get().FindConsoleVariable(TEXT("UI.KeyHelp")))
			{
				KeyHelp->Set(USimSubsystem::IsKeyHelpShown() ? 0 : 1, ECVF_SetByCode);
			}
		}
		ColonyHud::Tip(TEXT("Show or hide the list of keys"));
		ImGui::SameLine();
		UTradePanelSubsystem* Trade = GetWorld()->GetSubsystem<UTradePanelSubsystem>();
		if (ColonyHud::Button(TEXT("Factions"), Trade && Trade->IsFactionsOpen()) && Trade)
		{
			Trade->SetFactionsOpen(!Trade->IsFactionsOpen());
		}
		Record(TEXT("Factions"));
		ColonyHud::Tip(TEXT("The other factions: what they are and what they think of the colony"));
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Menu"), ImGui::IsPopupOpen("##HudMenu")))
		{
			ImGui::OpenPopup("##HudMenu");
			bQuitAsked = false;
		}
		Record(TEXT("Menu"));
		DrawMenuPopup(Bridge, Scale);
	}
	ImGui::End();
	ImGui::PopStyleVar(3);
}

void UColonyHudSubsystem::DrawMenuPopup(USimSubsystem& Bridge, float Scale)
{
	if (!ImGui::BeginPopup("##HudMenu"))
	{
		return;
	}
	const float Width = 220.0f * Scale;
	if (ColonyHud::Button(TEXT("Settings"), bSettingsOpen, ImVec2(Width, 0.0f)))
	{
		bSettingsOpen = !bSettingsOpen;
		ImGui::CloseCurrentPopup();
	}
	if (ColonyHud::Button(TEXT("New colony / load"), false, ImVec2(Width, 0.0f)))
	{
		bMainMenuOpen = true;
		MainMenuPage = 0;
		ImGui::CloseCurrentPopup();
	}
	UPanelsSubsystem* Panels = GetWorld()->GetSubsystem<UPanelsSubsystem>();
	if (Panels && ColonyHud::Button(TEXT("Message log"), Panels->IsLogOpen(), ImVec2(Width, 0.0f)))
	{
		Panels->SetLogOpen(!Panels->IsLogOpen());
		ImGui::CloseCurrentPopup();
	}
	if (ColonyHud::Button(TEXT("Key list"), USimSubsystem::IsKeyHelpShown(), ImVec2(Width, 0.0f)))
	{
		if (IConsoleVariable* KeyHelp = IConsoleManager::Get().FindConsoleVariable(TEXT("UI.KeyHelp")))
		{
			KeyHelp->Set(USimSubsystem::IsKeyHelpShown() ? 0 : 1, ECVF_SetByCode);
		}
		ImGui::CloseCurrentPopup();
	}
	ImGui::Separator();
	if (!bQuitAsked)
	{
		if (ColonyHud::Button(TEXT("Quit to desktop"), false, ImVec2(Width, 0.0f)))
		{
			bQuitAsked = true;
		}
	}
	else
	{
		ColonyHud::TextColored(ColonyHud::Warn(), TEXT("Quit? Unsaved progress is lost."));
		if (ColonyHud::Button(TEXT("Quit"), false, ImVec2(Width * 0.48f, 0.0f)))
		{
			UKismetSystemLibrary::QuitGame(GetWorld(), GetWorld()->GetFirstPlayerController(), EQuitPreference::Quit, false);
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Stay"), false, ImVec2(Width * 0.48f, 0.0f)))
		{
			bQuitAsked = false;
		}
	}
	ImGui::EndPopup();
}

// ---- The date box ------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawDateBox(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - 12.0f * Scale, ColonyHud::AboveBar(Viewport, Scale)), ImGuiCond_Always,
		ImVec2(1.0f, 1.0f));
	if (ImGui::Begin("##HudDate", nullptr, ColonyHud::BoxFlags()))
	{
		const colony::GameTime& Time = Sim.time();
		ColonyHud::PushLarge();
		ColonyHud::Text(FString::Printf(TEXT("%02d:%02d   Day %lld"), Time.hourOfDay(),
			static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute), static_cast<long long>(Time.day() + 1)));
		ColonyHud::PopLarge();
		ColonyHud::TextColored(ColonyHud::TextDim(), USkySubsystem::DescribeDate(Sim) + TEXT("   ") + USkySubsystem::DescribeClimate(Sim));
		if (Bridge.GetSpeed() == 0)
		{
			if (ImGui::SmallButton("Step one second"))
			{
				Bridge.StepOnce();
			}
			ImGui::SameLine();
		}
		// The level looked at (U17), with Up and Down.
		if (ULevelsSubsystem* Levels = GetWorld()->GetSubsystem<ULevelsSubsystem>())
		{
			UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
			AOverseerPawn* Pawn = Overseer ? Overseer->GetOverseerPawn() : nullptr;
			ColonyHud::TextColored(ColonyHud::TextDim(), ULevelsSubsystem::DescribeLevel(Levels->GetViewLevel()));
			ImGui::SameLine();
			if (ImGui::SmallButton("Up") && Pawn)
			{
				Pawn->StepLevel(1);
			}
			ColonyHud::Tip(TEXT("Look at the level above (Page Up)"));
			ImGui::SameLine();
			if (ImGui::SmallButton("Down") && Pawn)
			{
				Pawn->StepLevel(-1);
			}
			ColonyHud::Tip(TEXT("Look at the level below (Page Down)"));
		}
	}
	ImGui::End();
}

// ---- Pinned resources --------------------------------------------------------------------------

bool UColonyHudSubsystem::DrawPinnedResources(USimSubsystem& Bridge)
{
	if (Pinned.IsEmpty() || !Bridge.GetSim())
	{
		return false;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	bool bFirst = true;
	for (const FString& DefName : Pinned)
	{
		const colony::DefId Def = Sim.defs().idOf("Things", TCHAR_TO_UTF8(*DefName));
		if (!Def.valid())
		{
			continue;
		}
		int32 Count = 0;
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			Count += View.category == colony::ThingCategory::Item && View.def == Def ? FMath::Max(View.stackCount, 1) : 0;
		}
		if (!bFirst)
		{
			ImGui::SameLine(0.0f, 14.0f);
		}
		bFirst = false;
		ColonyHud::TextColored(ColonyHud::TextDim(), HudCapital(Sim.defs().get(Def).label()));
		ImGui::SameLine();
		ColonyHud::Text(FString::FromInt(Count));
	}
	return !bFirst;
}

// ---- Bottom left: what the cursor is over ------------------------------------------------------

void UColonyHudSubsystem::UpdateHoverCell(USimSubsystem& Bridge)
{
	bHasHoverCell = false;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
	float MouseX = 0.0f;
	float MouseY = 0.0f;
	FVector Origin;
	FVector Direction;
	if (!PC || !PC->GetMousePosition(MouseX, MouseY) || !PC->DeprojectScreenPositionToWorld(MouseX, MouseY, Origin, Direction))
	{
		return;
	}
	const FSimPick Pick = SimPicking::PickGroundFromRay(Bridge, Origin, Direction, ULevelsSubsystem::TopShownLevel(World));
	if (Pick.Kind == ESimPickKind::Cell && Bridge.GetSim()->map().inBounds(Pick.Cell))
	{
		HoverCell = Pick.Cell;
		bHasHoverCell = true;
	}
}

void UColonyHudSubsystem::DrawHoverReadout(USimSubsystem& Bridge, float Scale)
{
	const UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>();
	const EHudTab Open = GetOpenTab();
	// Only with nothing selected and no window on the left (the Build menu) or in the middle.
	if ((Overseer && Overseer->GetSelection().IsValid()) || Open != EHudTab::None || ImGui::GetIO().WantCaptureMouse)
	{
		bHasHoverCell = false;
		return;
	}
	UpdateHoverCell(Bridge);
	if (!bHasHoverCell)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Map& Map = Sim.map();
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + 12.0f * Scale, ColonyHud::AboveBar(Viewport, Scale)), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f * Scale, 0.0f), ImVec2(420.0f * Scale, FLT_MAX));
	if (ImGui::Begin("##HudHover", nullptr, ColonyHud::BoxFlags() | ImGuiWindowFlags_NoInputs))
	{
		++Stats.HoverFrames;
		const colony::DefId Floor = Sim.floors().at(HoverCell);
		ColonyHud::Title(Floor.valid() ? HudCapital(Sim.defs().get(Floor).label()) : FString(HudTerrainName(Map.terrain(HoverCell))));
		if (!Map.walkable(HoverCell))
		{
			ColonyHud::Text(TEXT("Can't be walked on"));
		}
		else
		{
			const float Walk = colony::groundSpeedFactor(Map, HoverCell.x + 0.5f, HoverCell.y + 0.5f, 1.8f, HoverCell.level) *
				Sim.lights().moveSpeedAt(HoverCell);
			ColonyHud::Text(FString::Printf(TEXT("Walk speed %s"), *HudPercent(Walk)));
		}
		const float Fertility = HoverCell.level == 0 ? Sim.plants().fertility(HoverCell) : 0.0f;
		if (Fertility > 0.0f)
		{
			ColonyHud::Text(FString::Printf(TEXT("Fertility %s"), *HudPercent(Fertility)));
		}
		const float Light = Sim.lights().lightAt(HoverCell);
		ColonyHud::Text(FString::Printf(TEXT("Light %s (%s)"), *HudPercent(Light), Light >= 0.5f ? TEXT("lit") : TEXT("dark")));
	}
	ImGui::End();
}

// ---- The info window ---------------------------------------------------------------------------

void UColonyHudSubsystem::DrawInfoWindow(USimSubsystem& Bridge, float Scale)
{
	if (InfoThing == colony::kNoEntity)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::ThingView* View = Bridge.GetCurrentSnapshot().find(InfoThing);
	if (!View)
	{
		InfoThing = colony::kNoEntity;
		return;
	}
	const colony::Def& Def = Sim.defs().get(View->def);
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const float Width = FMath::Min(760.0f * Scale, Viewport->Size.x - 40.0f * Scale);
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + 110.0f * Scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(Width, 0.0f), ImGuiCond_Always);
	if (ImGui::Begin("##HudInfo", nullptr, ColonyHud::BoxFlags() & ~ImGuiWindowFlags_NoFocusOnAppearing))
	{
		++Stats.InfoFrames;
		FString Name = HudCapital(Def.label());
		if (const colony::Colonist* Colonist = Sim.colonistOf(View->id))
		{
			Name = FString::Printf(TEXT("%s %s"), UTF8_TO_TCHAR(Colonist->firstName.c_str()), UTF8_TO_TCHAR(Colonist->lastName.c_str()));
		}
		else if (!View->animalName.empty())
		{
			Name = FString::Printf(TEXT("%s (%s)"), UTF8_TO_TCHAR(View->animalName.c_str()), *HudCapital(Def.label()));
		}
		if (View->stuff.valid())
		{
			Name = FString::Printf(TEXT("%s (%s)"), *Name, *HudCapital(Sim.defs().get(View->stuff).label()));
		}
		ColonyHud::Title(Name);
		ImGui::Separator();
		if (ImGui::BeginTable("##HudInfoCols", 2, ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("stats", ImGuiTableColumnFlags_WidthFixed, 300.0f * Scale);
			ImGui::TableSetupColumn("about", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			// Left: Basics, then what kind of thing it is.
			ColonyHud::Heading(TEXT("Basics"));
			if (ImGui::BeginTable("##HudInfoBasics", 2, ImGuiTableFlags_SizingStretchProp))
			{
				int32 Row = 0;
				if (View->maxHitPoints > 0)
				{
					HudStatRow(TEXT("Hit points"), FString::Printf(TEXT("%d / %d"), View->hitPoints, View->maxHitPoints), Row);
				}
				if (Def.has("stats.marketValue"))
				{
					HudStatRow(TEXT("Market value"), FString::Printf(TEXT("%.0f silver"), Def.number("stats.marketValue")), Row);
				}
				if (Def.has("stats.mass"))
				{
					HudStatRow(TEXT("Mass"), FString::Printf(TEXT("%.2f kg"), Def.number("stats.mass")), Row);
				}
				if (Def.has("stats.beauty"))
				{
					HudStatRow(TEXT("Beauty"), FString::Printf(TEXT("%.0f"), Def.number("stats.beauty")), Row);
				}
				if (Def.has("stats.flammability"))
				{
					HudStatRow(TEXT("Flammability"), HudPercent(Def.number("stats.flammability")), Row);
				}
				if (View->quality >= 0)
				{
					const TCHAR* const Qualities[] = {TEXT("Awful"), TEXT("Poor"), TEXT("Normal"), TEXT("Good"), TEXT("Excellent"), TEXT("Masterwork"), TEXT("Legendary")};
					HudStatRow(TEXT("Quality"), Qualities[FMath::Clamp(View->quality, 0, 6)], Row);
				}
				ImGui::EndTable();
			}
			switch (View->category)
			{
				case colony::ThingCategory::Building:
				{
					ColonyHud::Heading(TEXT("Building"));
					if (ImGui::BeginTable("##HudInfoBuilding", 2, ImGuiTableFlags_SizingStretchProp))
					{
						int32 Row = 0;
						HudStatRow(TEXT("Size"), FString::Printf(TEXT("%d x %d"), View->sizeX, View->sizeY), Row);
						if (Def.has("stats.workToBuild"))
						{
							HudStatRow(TEXT("Work to build"), FString::Printf(TEXT("%.0f"), Def.number("stats.workToBuild")), Row);
						}
						if (View->power != colony::PowerState::None)
						{
							HudStatRow(TEXT("Power"), FString::Printf(TEXT("%+.0f W"), View->powerWatts), Row);
						}
						if (View->batteryFill >= 0.0f)
						{
							HudStatRow(TEXT("Stored"), HudPercent(View->batteryFill), Row);
						}
						if (Def.has("building.mineableThing"))
						{
							const colony::Def* Yield = Sim.defs().find("Things", Def.text("building.mineableThing"));
							HudStatRow(TEXT("Mines into"), Yield ? HudCapital(Yield->label()) : FString(UTF8_TO_TCHAR(Def.text("building.mineableThing").c_str())), Row);
						}
						ImGui::EndTable();
					}
					break;
				}
				case colony::ThingCategory::Item:
				{
					ColonyHud::Heading(TEXT("Item"));
					if (ImGui::BeginTable("##HudInfoItem", 2, ImGuiTableFlags_SizingStretchProp))
					{
						int32 Row = 0;
						HudStatRow(TEXT("In this stack"), FString::FromInt(FMath::Max(View->stackCount, 1)), Row);
						if (View->rot > 0.0f)
						{
							HudStatRow(TEXT("Rotted"), HudPercent(View->rot), Row);
						}
						if (Def.has("ingestible.nutrition"))
						{
							HudStatRow(TEXT("Nutrition"), FString::Printf(TEXT("%.2f"), Def.number("ingestible.nutrition")), Row);
						}
						ImGui::EndTable();
					}
					break;
				}
				case colony::ThingCategory::Pawn:
				{
					ColonyHud::Heading(View->lifeStage >= 0 ? TEXT("Animal") : TEXT("Person"));
					if (ImGui::BeginTable("##HudInfoPawn", 2, ImGuiTableFlags_SizingStretchProp))
					{
						int32 Row = 0;
						HudStatRow(TEXT("Kind"), HudCapital(Def.label()), Row);
						if (const colony::Colonist* Colonist = Sim.colonistOf(View->id))
						{
							HudStatRow(TEXT("Age"), FString::FromInt(Colonist->age), Row);
							if (!Colonist->workGroup.empty())
							{
								HudStatRow(TEXT("Work group"), UTF8_TO_TCHAR(Colonist->workGroup.c_str()), Row);
							}
						}
						if (View->lifeStage >= 0)
						{
							const TCHAR* const Stages[] = {TEXT("Baby"), TEXT("Young"), TEXT("Grown")};
							HudStatRow(TEXT("Life stage"), Stages[FMath::Clamp(View->lifeStage, 0, 2)], Row);
							HudStatRow(TEXT("Body size"), FString::Printf(TEXT("%.2f"), View->bodySize), Row);
						}
						if (View->sex >= 0)
						{
							HudStatRow(TEXT("Sex"), View->sex == 1 ? TEXT("Female") : TEXT("Male"), Row);
						}
						HudStatRow(TEXT("Walk speed"), HudPercent(View->moveFactor), Row);
						if (View->capacityKg > 0.0f)
						{
							HudStatRow(TEXT("Carrying"), FString::Printf(TEXT("%.0f of %.0f kg"), View->loadKg, View->capacityKg), Row);
						}
						if (View->bloodLoss > 0.0f)
						{
							HudStatRow(TEXT("Blood lost"), HudPercent(View->bloodLoss), Row);
						}
						ImGui::EndTable();
					}
					break;
				}
			}
			// Right: what it is, in words.
			ImGui::TableNextColumn();
			const std::string About = Def.text("description");
			ImGui::PushStyleColor(ImGuiCol_Text, ColonyHud::Rgb(0xd6d0c3));
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(About.empty() ? "No description yet." : About.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::EndTable();
		}
		ImGui::Spacing();
		const float CloseWidth = 140.0f * Scale;
		ImGui::SetCursorPosX((ImGui::GetWindowWidth() - CloseWidth) * 0.5f);
		ColonyHud::PushLarge();
		if (ColonyHud::Button(TEXT("Close"), false, ImVec2(CloseWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			InfoThing = colony::kNoEntity;
		}
		ColonyHud::PopLarge();
	}
	ImGui::End();
}

// ---- Settings ----------------------------------------------------------------------------------

void UColonyHudSubsystem::DrawSettings(USimSubsystem& Bridge, float Scale)
{
	++Stats.SettingsFrames;
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + 80.0f * Scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(720.0f * Scale, 320.0f * Scale), ImVec2(Viewport->Size.x * 0.9f, Viewport->Size.y * 0.8f));
	if (!ImGui::Begin("##HudSettings", nullptr, ColonyHud::BoxFlags() & ~ImGuiWindowFlags_NoFocusOnAppearing))
	{
		ImGui::End();
		return;
	}
	ColonyHud::Title(TEXT("Settings"));
	ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f * Scale);
	if (ColonyHud::Button(TEXT("Close##Settings")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		bSettingsOpen = false;
	}
	ImGui::Separator();
	// Pages down the left, the page on the right.
	ImGui::BeginGroup();
	for (const FSettingsPage& Page : SettingsPages)
	{
		if (ColonyHud::Button(Page.Name + TEXT("##page"), SettingsPage == Page.Name, ImVec2(140.0f * Scale, 0.0f)))
		{
			SettingsPage = Page.Name;
		}
	}
	ImGui::EndGroup();
	ImGui::SameLine(0.0f, 16.0f * Scale);
	ImGui::BeginGroup();
	if (SettingsPage == TEXT("Game"))
	{
		// Whole minutes, 48 (the default) to 120. Changing it applies to this game now and to new games.
		int32 Minutes = Bridge.GetDayMinutes();
		ImGui::SetNextItemWidth(260.0f * Scale);
		if (ImGui::SliderInt("Day length", &Minutes, colony::kBaseDayMinutes, colony::kMaxDayMinutes, "%d real minutes"))
		{
			Bridge.SetDayMinutes(Minutes);
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Default##Day")))
		{
			Bridge.SetDayMinutes(colony::kBaseDayMinutes);
		}
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("How long a game day lasts at normal speed. Walking, fighting and work"));
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("keep their speed; hunger, sleep, crops, animals, rot and the sun follow the day."));
		ImGui::Spacing();

		// Mouse speed when playing as a pawn (saved; 1.2 is the default).
		float MouseTurn = AProtagonistPawn::GetMouseTurn();
		ImGui::SetNextItemWidth(260.0f * Scale);
		if (ImGui::SliderFloat("Mouse speed", &MouseTurn, 0.2f, 4.0f, "%.2f"))
		{
			AProtagonistPawn::SetMouseTurn(MouseTurn);
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Default##MouseTurn")))
		{
			AProtagonistPawn::SetMouseTurn(AProtagonistPawn::kDefaultMouseTurn);
		}
		ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("How fast the mouse turns the view when you play as a pawn."));
		ImGui::Spacing();

		// Text size: the two sizes of every panel, both bigger.
		const int32 TextSize = GHudTextSize.GetValueOnGameThread();
		ColonyHud::Text(TEXT("Text size"));
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Normal"), TextSize == 0))
		{
			GHudTextSize.AsVariable()->Set(0, ECVF_SetByCode);
			SavePrefs();
		}
		ImGui::SameLine();
		if (ColonyHud::Button(TEXT("Bigger"), TextSize == 1))
		{
			GHudTextSize.AsVariable()->Set(1, ECVF_SetByCode);
			SavePrefs();
		}
	}
	else
	{
		for (const FSettingsPage& Page : SettingsPages)
		{
			if (Page.Name == SettingsPage && Page.Draw)
			{
				Page.Draw(Scale);
			}
		}
	}
	ImGui::EndGroup();
	ImGui::End();
}

void UColonyHudSubsystem::DrawKeyListPage(float Scale)
{
	// The overseer camera's keys are fixed; every key for playing as a pawn can be changed below (docs/design/controls.md).
	ColonyHud::Heading(TEXT("Overseer view"));
	ColonyHud::Text(TEXT("WASD  pan the camera        Q / E  turn        wheel  zoom        middle drag  orbit"));
	ColonyHud::Text(TEXT("Click  select        F  centre on selection        Home  reset view        Esc  deselect"));
	ColonyHud::Text(TEXT("Page Up / Page Down  level above / below        R  draft colonist / turn building"));
	ColonyHud::Text(FString::Printf(TEXT("Right click  order a drafted colonist        %s  take control of the selected colonist"),
		*Rim3DKeys::GetKeyLabel(ERim3DAction::SwitchMode)));
	ColonyHud::Text(TEXT("Space  pause        1, 2, 3  speed        M  world map"));
	ImGui::Spacing();
	ColonyHud::Heading(TEXT("Playing as a pawn (click a key to change it)"));
	Rim3DKeys::DrawControlsPage(Scale);
}

// ---- Mugshots ----------------------------------------------------------------------------------

UTexture* UColonyHudSubsystem::GetMugshot(colony::Entity Pawn) const
{
	const FMugshot* Found = Mugshots.Find(static_cast<uint32>(entt::to_integral(Pawn)));
	return Found && Found->Target ? Found->Target.Get() : nullptr;
}

void UColonyHudSubsystem::UpdateMugshots(USimSubsystem& Bridge)
{
	// One picture at most every few frames, and only in the overseer view where the colonist bar shows them.
	const double Now = FPlatformTime::Seconds();
	if (Now < NextMugshotCheck || !IsOverseerActive())
	{
		return;
	}
	NextMugshotCheck = Now + 0.25;
	UWorld* World = GetWorld();
	UPawnSubsystem* Pawns = World ? World->GetSubsystem<UPawnSubsystem>() : nullptr;
	if (!Pawns)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Pawn || View.dead || View.faction != 1 || !Sim.colonistOf(View.id))
		{
			continue;
		}
		const uint32 Key = static_cast<uint32>(entt::to_integral(View.id));
		const uint64 Signature = HudLookSignature(Snapshot, View);
		FMugshot* Shot = Mugshots.Find(Key);
		if (Shot && Shot->Target && Shot->Signature == Signature)
		{
			continue;
		}
		ASimPawnActor* Actor = Pawns->FindPawn(View.id);
		if (!Actor)
		{
			continue;
		}
		if (!MugshotActor)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			MugshotActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
			if (!MugshotActor)
			{
				return;
			}
			MugshotCapture = NewObject<USceneCaptureComponent2D>(MugshotActor);
			MugshotActor->SetRootComponent(MugshotCapture);
			MugshotCapture->RegisterComponent();
			MugshotCapture->bCaptureEveryFrame = false;
			MugshotCapture->bCaptureOnMovement = false;
			MugshotCapture->FOVAngle = 24.0f;
			MugshotCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			MugshotCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			MugshotCapture->ShowFlags.SetAtmosphere(false);
			MugshotCapture->ShowFlags.SetFog(false);
			MugshotCapture->ShowFlags.SetMotionBlur(false);
		}
		if (!Shot)
		{
			Shot = &Mugshots.Add(Key);
		}
		if (!Shot->Target)
		{
			UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(this);
			Target->RenderTargetFormat = RTF_RGBA8;
			Target->ClearColor = FLinearColor(0.07f, 0.08f, 0.09f, 1.0f);
			Target->InitAutoFormat(128, 128);
			Target->UpdateResourceImmediate(true);
			Shot->Target = Target;
			MugshotTargets.Add(Target);
		}
		// In front of the face, a little above it, looking back at it.
		USkeletalMeshComponent* Mesh = Actor->GetMakeHumanMesh() ? Actor->GetMakeHumanMesh() : Actor->GetBody();
		FVector Head = Actor->GetActorLocation() + FVector(0.0f, 0.0f, 160.0f * Actor->GetBodyScale());
		if (Mesh && Mesh->GetBoneIndex(TEXT("head")) != INDEX_NONE)
		{
			Head = Mesh->GetBoneLocation(TEXT("head")) + FVector(0.0f, 0.0f, 6.0f);
		}
		const FVector Forward = Actor->GetActorForwardVector();
		const FVector Eye = Head + Forward * 95.0f + FVector(0.0f, 0.0f, 4.0f);
		MugshotActor->SetActorLocationAndRotation(Eye, (Head - Eye).Rotation());
		MugshotCapture->ShowOnlyActors.Reset();
		MugshotCapture->ShowOnlyActors.Add(Actor);
		TArray<AActor*> Attached;
		Actor->GetAttachedActors(Attached, true, true);
		for (AActor* Child : Attached)
		{
			MugshotCapture->ShowOnlyActors.Add(Child);
		}
		MugshotCapture->TextureTarget = Shot->Target;
		MugshotCapture->CaptureScene();
		Shot->Signature = Signature;
		Shot->MadeAt = Now;
		++Stats.MugshotsMade;
		return;  // one a check
	}
}
