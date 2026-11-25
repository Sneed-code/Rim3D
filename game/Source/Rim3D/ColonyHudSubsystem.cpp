#include "ColonyHudSubsystem.h"
#include "AnimationRuntime.h"
#include "Engine/SkeletalMesh.h"

#include "GameFramework/PlayerInput.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Interfaces/Interface_PostProcessVolume.h"
#include "Misc/FileHelper.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "HudIcons.h"
#include "HudLayout.h"
#include "DebugScenes.h"

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
#include "ProtagonistSubsystem.h"
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

	TAutoConsoleVariable<float> GHudTextGrow(
		TEXT("UI.TextGrow"), 1.5f, TEXT("How much bigger every text is than the first sizes (sneed 2026-11-24: 1.5); boxes keep their widths and wrap."), ECVF_Default);

	TAutoConsoleVariable<int32> GHudLogBoxes(
		TEXT("UI.LogBoxes"), 0, TEXT("1 logs each box's width (BOXWIDTH) and stops boxes holding their widths, to measure them."), ECVF_Default);

	FAutoConsoleCommandWithWorldAndArgs GHudTabCommand(
		TEXT("Hud.Tab"), TEXT("Hud.Tab <name> opens a tab of the bottom bar (Build, Work, Schedule, Assign, Animals, Mechs, Wildlife, Research, Quests, World, Storage); no name closes it."),
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
	// Commands are applied by the next step; while the game is paused, one step makes the change show at once. Before
	// the game starts (the new game's character screens, game second 0) a step would start the clock: those commands
	// apply without one (docs/design/new-game.md).
	if (Bridge->GetSim() && Bridge->GetSim()->time().steps == 0 && Bridge->ApplyPregameCommands())
	{
		return;
	}
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
	FString LessonsText;
	bLessonsOn = true;
	if (GConfig)
	{
		GConfig->GetString(HudPrefsSection, TEXT("Pinned"), PinnedText, GGameUserSettingsIni);
		GConfig->GetInt(HudPrefsSection, TEXT("TextSize"), TextSize, GGameUserSettingsIni);
		GConfig->GetString(HudPrefsSection, TEXT("Lessons"), LessonsText, GGameUserSettingsIni);
		GConfig->GetBool(HudPrefsSection, TEXT("LessonsOn"), bLessonsOn, GGameUserSettingsIni);
	}
	PinnedText.ParseIntoArray(Pinned, TEXT(","), true);
	TArray<FString> LessonIds;
	LessonsText.ParseIntoArray(LessonIds, TEXT(","), true);
	LearnedLessons = TSet<FString>(LessonIds);
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
	GConfig->SetString(HudPrefsSection, TEXT("Lessons"), *FString::Join(LearnedLessons.Array(), TEXT(",")), GGameUserSettingsIni);
	GConfig->SetBool(HudPrefsSection, TEXT("LessonsOn"), bLessonsOn, GGameUserSettingsIni);
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
		case EHudTab::Inventory: return TEXT("Storage");  // sneed 2026-11-25: was "Inventory"; the enum and the icon name keep the old word
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

namespace
{
	// The lights, fog, sky and post-process volumes the world draws with, and the viewport's show flags, written to a file:
	// a fresh scene and one loaded inside the game are compared line by line (the washed-out range after a scene hop).
	void hudDescribeLighting(UWorld* World, const FString& Path)
	{
		TArray<FString> Lines;
		if (!World)
		{
			return;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TInlineComponentArray<USceneComponent*> Components;
			It->GetComponents(Components);
			for (const USceneComponent* Component : Components)
			{
				const FString Where = FString::Printf(TEXT("%s.%s visible %d"), *It->GetName(), *Component->GetName(), Component->IsVisible() ? 1 : 0);
				if (const ULightComponent* Light = Cast<ULightComponent>(Component))
				{
					Lines.Add(FString::Printf(TEXT("light %s %s intensity %.2f atmosphereSun %d"), *Light->GetClass()->GetName(), *Where, Light->Intensity,
						Light->IsUsedAsAtmosphereSunLight() ? 1 : 0));
				}
				else if (const USkyLightComponent* Sky = Cast<USkyLightComponent>(Component))
				{
					Lines.Add(FString::Printf(TEXT("skylight %s intensity %.2f realtime %d"), *Where, Sky->Intensity, Sky->IsRealTimeCaptureEnabled() ? 1 : 0));
				}
				else if (const UExponentialHeightFogComponent* Fog = Cast<UExponentialHeightFogComponent>(Component))
				{
					Lines.Add(FString::Printf(TEXT("fog %s density %.5f volumetric %d"), *Where, Fog->FogDensity, Fog->bEnableVolumetricFog ? 1 : 0));
				}
				else if (Component->IsA<USkyAtmosphereComponent>())
				{
					Lines.Add(FString::Printf(TEXT("atmosphere %s"), *Where));
				}
			}
		}
		for (IInterface_PostProcessVolume* Volume : World->PostProcessVolumes)
		{
			if (!Volume)
			{
				continue;
			}
			const FPostProcessVolumeProperties Props = Volume->GetProperties();
			const FPostProcessSettings* P = Props.Settings;
			const UObject* Object = Cast<UObject>(Volume);
			Lines.Add(FString::Printf(TEXT("postprocess %s priority %.1f weight %.2f enabled %d unbound %d method %d(%d) bias %.2f(%d) min %.2f(%d) max %.2f(%d) physical %d(%d)"),
				Object ? *Object->GetPathName() : TEXT("?"), Props.Priority, Props.BlendWeight, Props.bIsEnabled ? 1 : 0, Props.bIsUnbound ? 1 : 0,
				P ? static_cast<int32>(P->AutoExposureMethod) : -1, P ? P->bOverride_AutoExposureMethod : 0, P ? P->AutoExposureBias : 0.0f,
				P ? P->bOverride_AutoExposureBias : 0, P ? P->AutoExposureMinBrightness : 0.0f, P ? P->bOverride_AutoExposureMinBrightness : 0,
				P ? P->AutoExposureMaxBrightness : 0.0f, P ? P->bOverride_AutoExposureMaxBrightness : 0, P ? P->AutoExposureApplyPhysicalCameraExposure : 0,
				P ? P->bOverride_AutoExposureApplyPhysicalCameraExposure : 0));
		}
		if (const UGameViewportClient* Viewport = World->GetGameViewport())
		{
			const FEngineShowFlags& F = Viewport->EngineShowFlags;
			Lines.Add(FString::Printf(TEXT("showflags eyeAdaptation %d postProcessing %d fog %d atmosphere %d lighting %d lumen %d viewmode %d"),
				F.EyeAdaptation ? 1 : 0, F.PostProcessing ? 1 : 0, F.Fog ? 1 : 0, F.Atmosphere ? 1 : 0, F.Lighting ? 1 : 0,
				F.LumenGlobalIllumination ? 1 : 0, Viewport->ViewModeIndex));
		}
		Lines.Add(FString::Printf(TEXT("world %s"), *World->GetPathName()));
		FFileHelper::SaveStringArrayToFile(Lines, *Path);
	}
}

// -DebugSceneHop, the PC check of scenes loading inside the game: title, Debug > Gun range in this same game, F10 back
// to the title, then quit, with a picture of each and log lines. The stage lives in a static, so it surviving the level
// loads shows it all happened in one process.
void UColonyHudSubsystem::TickSceneHop()
{
	static int32 HopStage = 0;
	static double HopSince = 0.0;
	// -CvarDump=<seconds>: the same list from a fresh start (compared with the scene loaded in the game).
	float DumpAt = 0.0f;
	if (!bCvarDumped && FParse::Value(FCommandLine::Get(), TEXT("CvarDump="), DumpAt) && GetWorld()->GetTimeSeconds() > DumpAt)
	{
		bCvarDumped = true;
		DebugScenes::DumpConsoleVariables(FPaths::ProjectSavedDir() / TEXT("Smoke/scenehop/cvars_fresh.txt"));
		hudDescribeLighting(GetWorld(), FPaths::ProjectSavedDir() / TEXT("Smoke/scenehop/light_fresh.txt"));
		const USkySubsystem* DumpSky = GetWorld()->GetSubsystem<USkySubsystem>();
		UE_LOG(LogTemp, Display, TEXT("CVARDUMP: sky exposure %.2f EV, cuts %d, camera really at %.2f EV, heals %d"), DumpSky ? DumpSky->GetExposure() : -99.0f,
			DumpSky ? DumpSky->GetExposureCuts() : -1, DumpSky ? DumpSky->GetMeasuredExposure() : -99.0f, DumpSky ? DumpSky->GetExposureHeals() : -1);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/scenehop/fresh.png"), true, false);
	}
	if (!FParse::Param(FCommandLine::Get(), TEXT("DebugSceneHop")) || bLeavingDebugScene)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	const auto Picture = [](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/scenehop") / FString::Printf(TEXT("%s.png"), Name), true, false);
	};
	const bool bInScene = DebugScenes::IsDebugScene();
	// Each stage waits for its world (the title, or the scene) and counts from its first frame there.
	if (bInScene != (HopStage == 1 || HopStage >= 10))
	{
		return;
	}
	if (HopSince == 0.0)
	{
		HopSince = Now;
		return;
	}
	if (HopStage == 0 && bMainMenuOpen && Now - HopSince > 10.0)
	{
		Picture(TEXT("1_title"));
		for (const DebugScenes::FScene& Scene : DebugScenes::All())
		{
			if (FCString::Strcmp(Scene.Id, TEXT("gunrange")) == 0)
			{
				UE_LOG(LogTemp, Display, TEXT("SCENEHOP: title shown, loading the gun range in this game (pid %u)"), FPlatformProcess::GetCurrentProcessId());
				HopStage = 1;
				HopSince = 0.0;
				bLeavingDebugScene = DebugScenes::Launch(GetWorld(), Scene);
			}
		}
	}
	else if (HopStage == 1 && bInScene && Now - HopSince > 30.0)
	{
		Picture(TEXT("2_range"));
		DebugScenes::DumpConsoleVariables(FPaths::ProjectSavedDir() / TEXT("Smoke/scenehop/cvars_hop.txt"));
		hudDescribeLighting(GetWorld(), FPaths::ProjectSavedDir() / TEXT("Smoke/scenehop/light_hop.txt"));
		const USkySubsystem* HopSky = GetWorld()->GetSubsystem<USkySubsystem>();
		UE_LOG(LogTemp, Display, TEXT("SCENEHOP: range sky exposure %.2f EV, cuts %d, camera really at %.2f EV, heals %d"), HopSky ? HopSky->GetExposure() : -99.0f,
			HopSky ? HopSky->GetExposureCuts() : -1, HopSky ? HopSky->GetMeasuredExposure() : -99.0f, HopSky ? HopSky->GetExposureHeals() : -1);
		HopStage = 10;
		HopSince = Now;
	}
	else if (HopStage >= 10 && HopStage < 14 && Now - HopSince > 3.0)
	{
		// Toggles with a picture each, to see what the white comes from: fog, the eye adaptation, a held exposure.
		static const TCHAR* const Tries[][3] = {
			{TEXT("ShowFlag.Fog 0"), TEXT("2b_nofog"), TEXT("ShowFlag.Fog 2")},
			{TEXT("ShowFlag.EyeAdaptation 0"), TEXT("2c_noeye"), TEXT("ShowFlag.EyeAdaptation 2")},
			{TEXT("Look.Exposure 17"), TEXT("2d_ev17"), TEXT("Look.Exposure -99")},
			{TEXT("ShowFlag.Lighting 0"), TEXT("2e_nolight"), TEXT("ShowFlag.Lighting 2")},
		};
		APlayerController* HopPC = GetWorld()->GetFirstPlayerController();
		const int32 Try = HopStage - 10;
		if (HopPC && HopSince > 0.0)
		{
			if (Try > 0)
			{
				HopPC->ConsoleCommand(Tries[Try - 1][2]);
			}
			HopPC->ConsoleCommand(Tries[Try][0]);
			HopSince = -Now;  // the toggle is on; the picture comes after it has settled
		}
		else if (Now + HopSince > 3.0)
		{
			Picture(Tries[Try][1]);
			UE_LOG(LogTemp, Display, TEXT("SCENEHOP: picture %s after %s"), Tries[Try][1], Tries[Try][0]);
			++HopStage;
			HopSince = Now;
		}
	}
	else if (HopStage == 14 && Now - HopSince > 3.0)
	{
		if (APlayerController* HopPC = GetWorld()->GetFirstPlayerController())
		{
			HopPC->ConsoleCommand(TEXT("ShowFlag.Lighting 2"));
		}
		UE_LOG(LogTemp, Display, TEXT("SCENEHOP: in the gun range (pid %u), back to the title"), FPlatformProcess::GetCurrentProcessId());
		HopStage = 2;
		HopSince = 0.0;
		bLeavingDebugScene = DebugScenes::ReturnToTitle(GetWorld());
	}
	else if (HopStage == 2 && !bInScene && Now - HopSince > 10.0)
	{
		Picture(TEXT("3_title_again"));
		const auto Cvar = [](const TCHAR* Name)
		{
			const IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name);
			return Var ? Var->GetString() : FString(TEXT("(none)"));
		};
		UE_LOG(LogTemp, Display, TEXT("SCENEHOP OK: back on the title in the same game (pid %u, menu %d), Sky.Hour %s, Overseer.UI %s, Pawns.MetaHumans %s, near clip %.1f"),
			FPlatformProcess::GetCurrentProcessId(), bMainMenuOpen ? 1 : 0, *Cvar(TEXT("Sky.Hour")), *Cvar(TEXT("Overseer.UI")), *Cvar(TEXT("Pawns.MetaHumans")), GNearClippingPlane);
		HopStage = 3;
	}
	else if (HopStage == 3 && Now - HopSince > 12.0)
	{
		FPlatformMisc::RequestExit(false, TEXT("SceneHop"));
	}
}

void UColonyHudSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	// A scene started from the title screen's Debug page: F10 goes back to the title (docs/design/new-game.md).
	// Not cached: scenes load inside this game, so the command line changes while it runs.
	if (!bLeavingDebugScene && DebugScenes::IsDebugScene())
	{
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->WasInputKeyJustPressed(EKeys::F10))
		{
			bLeavingDebugScene = DebugScenes::ReturnToTitle(GetWorld());
		}
	}
	TickSceneHop();
	// No engine debug view keys (F1 wireframe, F2 unlit, F4 lighting only...): the config clear covers a fresh start, this
	// covers a player controller made later (a level loaded in the game) and any build that reads another config.
	if (APlayerController* KeysPC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr; KeysPC && KeysPC->PlayerInput &&
		!KeysPC->PlayerInput->DebugExecBindings.IsEmpty())
	{
		KeysPC->PlayerInput->DebugExecBindings.Empty();
		UE_LOG(LogTemp, Display, TEXT("Engine debug keys (F1 wireframe and the like) turned off"));
	}
	// After a level load inside the game, the player's view keeps the last level's rendering history (its exposure,
	// Lumen's cached lighting, temporal buffers): the gun range loaded from the title came up washed out white (PC
	// round 3, 2026-11-22) while a fresh start of it was fine. Its first frames reset that history, as a fresh start has.
	if (ViewResetFrames < 2 && DebugScenes::HasLoadedInGame())
	{
		if (APlayerController* ResetPC = GetWorld()->GetFirstPlayerController(); ResetPC && GetWorld()->GetTimeSeconds() > (ViewResetFrames == 0 ? 0.1f : 1.0f))
		{
			FlushRenderingCommands();
			ResetPC->ConsoleCommand(TEXT("r.ResetViewState"));
			++ViewResetFrames;
			UE_LOG(LogTemp, Display, TEXT("Level loaded in the game: view history reset (%d)"), ViewResetFrames);
		}
	}
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
		bMainMenuOpen = (Bridge->GetStartOptions().Scenario.IsEmpty() || Bridge->IsTitleBackdrop()) && !FParse::Param(Line, TEXT("NoMenu")) && !FString(Line).Contains(TEXT("-SmokeTest="));
	}
	CheckLanding(*Bridge);
	// The menus are clicked with the mouse: show the cursor and let the UI have it while one is open (the level's
	// default pawn starts with the mouse captured for looking around, so the title's buttons could not be clicked),
	// and give it back to a directly controlled body when the menu closes (the overseer sets its own).
	if (APlayerController* MenuPC = GetWorld()->GetFirstPlayerController())
	{
		if (bMainMenuOpen)
		{
			MenuPC->bShowMouseCursor = true;
			if (!bMenuMouse)
			{
				bMenuMouse = true;
				FInputModeGameAndUI MenuMode;
				MenuMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
				MenuMode.SetHideCursorDuringCapture(false);
				MenuPC->SetInputMode(MenuMode);
			}
		}
		else if (bMenuMouse)
		{
			bMenuMouse = false;
			const UProtagonistSubsystem* Direct = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
			if (Direct && Direct->IsDirectMode())
			{
				MenuPC->bShowMouseCursor = false;
				MenuPC->SetInputMode(FInputModeGameOnly());
			}
		}
	}
	if (bMainMenuOpen)
	{
		DrawMainMenu(*Bridge);
		return;
	}
	EndTitleCamera();  // the world map or the game took over from the title
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
	// ImGui sometimes never hears a release (a middle-button orbit pressed over the bar, a release outside the window), and then
	// thinks the button is still held and keeps the mouse to itself. Slate knows which buttons are really down: release the rest.
	if (FSlateApplication::IsInitialized())
	{
		const TSet<FKey>& Down = FSlateApplication::Get().GetPressedMouseButtons();
		const FKey Buttons[3] = {EKeys::LeftMouseButton, EKeys::RightMouseButton, EKeys::MiddleMouseButton};
		ImGuiIO& IO = ImGui::GetIO();
		for (int32 Button = 0; Button < 3; ++Button)
		{
			if (IO.MouseDown[Button] && !Down.Contains(Buttons[Button]))
			{
				IO.AddMouseButtonEvent(Button, false);
			}
		}
	}

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
	DrawLessons(Bridge, Scale);
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
		{
			// Canopy: a faint jade hairline along the bar's top edge instead of a box border.
			const ImVec2 BarPos = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddLine(BarPos, ImVec2(BarPos.x + ImGui::GetWindowWidth(), BarPos.y), ImGui::GetColorU32(ImVec4(0.37f, 0.83f, 0.66f, 0.35f)), 1.0f);
		}
		const auto Record = [this](const FString& Label)
		{
			const ImVec2 Min = ImGui::GetItemRectMin();
			const ImVec2 Max = ImGui::GetItemRectMax();
			ButtonRects.Add(Label, FBox2D(FVector2D(Min.x, Min.y), FVector2D(Max.x, Max.y)));
		};
		// A bar button with a drawn icon: 0 pause, 1-3 that many play arrows, 5 flag, 6 menu lines (Keys and the learning helper moved into the menu, sneed 2026-11-25).
		const auto IconButton = [&](const TCHAR* Name, int32 Icon, bool bActive) -> bool
		{
			const bool bPressed = ColonyHud::Button(FString::Printf(TEXT("##icon%s"), Name), bActive, ImVec2(ImGui::GetFrameHeight() * 1.35f, ColonyHud::BarButtonHeight()));
			const ImVec2 Min = ImGui::GetItemRectMin();
			const ImVec2 Max = ImGui::GetItemRectMax();
			const ImVec2 Mid((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
			const float R = (Max.y - Min.y) * 0.28f;  // half the icon's height
			const float Line = FMath::Max(1.5f, 1.6f * Scale);
			const ImU32 Ink = ImGui::GetColorU32(bActive ? ColonyHud::Accent() : ColonyHud::TextMain());
			ImDrawList* Draw = ImGui::GetWindowDrawList();
			if (Icon == 0)
			{
				const float W = R * 0.32f;
				Draw->AddRectFilled(ImVec2(Mid.x - R * 0.55f - W * 0.5f, Mid.y - R), ImVec2(Mid.x - R * 0.55f + W * 0.5f, Mid.y + R), Ink, 1.0f);
				Draw->AddRectFilled(ImVec2(Mid.x + R * 0.55f - W * 0.5f, Mid.y - R), ImVec2(Mid.x + R * 0.55f + W * 0.5f, Mid.y + R), Ink, 1.0f);
			}
			else if (Icon <= 3)
			{
				// Arrows side by side, centred: each R wide, overlapping a little as RimWorld's speed buttons do.
				const float Each = R * 0.95f;
				const float Total = Each * static_cast<float>(Icon) + R * 0.1f;
				float X = Mid.x - Total * 0.5f;
				for (int32 Arrow = 0; Arrow < Icon; ++Arrow)
				{
					Draw->AddTriangleFilled(ImVec2(X, Mid.y - R * 0.85f), ImVec2(X + Each, Mid.y), ImVec2(X, Mid.y + R * 0.85f), Ink);
					X += Each;
				}
			}
			else if (Icon == 5)
			{
				// A flag on its pole.
				const float PoleX = Mid.x - R * 0.8f;
				Draw->AddLine(ImVec2(PoleX, Mid.y - R * 1.1f), ImVec2(PoleX, Mid.y + R * 1.1f), Ink, Line);
				Draw->AddQuadFilled(ImVec2(PoleX, Mid.y - R * 1.05f), ImVec2(PoleX + R * 1.7f, Mid.y - R * 0.75f), ImVec2(PoleX + R * 1.7f, Mid.y + R * 0.05f),
					ImVec2(PoleX, Mid.y - R * 0.15f), Ink);
			}
			else
			{
				for (int32 Bar = -1; Bar <= 1; ++Bar)
				{
					const float Y = Mid.y + Bar * R * 0.7f;
					Draw->AddLine(ImVec2(Mid.x - R * 1.1f, Y), ImVec2(Mid.x + R * 1.1f, Y), Ink, Line);
				}
			}
			return bPressed;
		};

		// The right-hand group: speed, Factions, Menu (and Ship while there is one). Measured first so the tabs fit.
		UShipPanelSubsystem* Ship = GetWorld()->GetSubsystem<UShipPanelSubsystem>();
		const bool bShip = Ship && Ship->HasShip();
		const TCHAR* const Speeds[] = {TEXT("Pause"), TEXT("1x"), TEXT("2x"), TEXT("3x")};
		// Speed, Factions and Menu are drawn as clean line icons on square-ish buttons (their names in the tooltips); Ship stays a word.
		const float IconWidth = ImGui::GetFrameHeight() * 1.35f;
		float RightWidth = 6.0f * (IconWidth + Style.ItemSpacing.x);
		if (bShip)
		{
			RightWidth += ImGui::CalcTextSize("Ship").x + 2.0f * Style.FramePadding.x + Style.ItemSpacing.x;
		}
		RightWidth += 18.0f * Scale;  // gap before the group

		// The tabs, equal widths, from the left.
		const EHudTab Open = GetOpenTab();
		const int32 TabCount = static_cast<int32>(EHudTab::Count) - 1;
		const float TabHeight = ColonyHud::BarButtonHeight();
		UQuestPanelSubsystem* Quests = GetWorld()->GetSubsystem<UQuestPanelSubsystem>();
		// Every tab is as wide as the widest label (with its icon and padding) needs, so no label is ever drawn smaller than Build's
		// (sneed 2026-11-25). Quests is measured with a waiting count, the longest it can read.
		float TabWidth = 0.0f;
		for (int32 MeasureIndex = 1; MeasureIndex <= TabCount; ++MeasureIndex)
		{
			const EHudTab MeasureTab = static_cast<EHudTab>(MeasureIndex);
			const FString Measured = MeasureTab == EHudTab::Quests ? FString(TEXT("Quests (9)")) : FString(TabLabel(MeasureTab));
			const float MeasuredText = ImGui::CalcTextSize(TCHAR_TO_UTF8(*Measured)).x;
			const float MeasuredIcon = FMath::Max(ImGui::GetTextLineHeight() * 1.25f, TabHeight * 0.80f);
			// Snug: the small pad HudIcons::Button leaves either side, no more (sneed 2026-11-25).
			TabWidth = FMath::Max(TabWidth, MeasuredIcon + Style.ItemInnerSpacing.x + MeasuredText + 2.0f * Style.FramePadding.y);
		}
		for (int32 Index = 1; Index <= TabCount; ++Index)
		{
			const EHudTab Tab = static_cast<EHudTab>(Index);
			FString Label = TabLabel(Tab);
			bool bEmber = false;
			if (Tab == EHudTab::Quests && Quests)
			{
				// "Quests (2)" while offers wait for an answer; ember until the window has been opened since the newest one.
				const int32 Waiting = Quests->CountOffered();
				Label = Waiting > 0 ? FString::Printf(TEXT("Quests (%d)"), Waiting) : Label;
				bEmber = Quests->HasUnseenOffer() && Open != EHudTab::Quests;
			}
			if (bEmber)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, ColonyHud::EmberDark());
			}
			// The tab's painted icon sits left of its label. The icon file keeps its old name "tab_inventory" for the Storage tab.
			UTexture* TabIcon = HudIcons::Get(Tab == EHudTab::Inventory ? FString(TEXT("tab_inventory")) : TEXT("tab_") + FString(TabLabel(Tab)).ToLower(), TabHeight);
			if (TabIcon ? HudIcons::Button(TabIcon, Label + TEXT("##tab"), Open == Tab, ImVec2(TabWidth, TabHeight))
					: ColonyHud::Button(Label + TEXT("##tab"), Open == Tab, ImVec2(TabWidth, TabHeight)))
			{
				ToggleTab(Tab);
			}
			if (bEmber)
			{
				ImGui::PopStyleColor();
			}
			if (Open == Tab)
			{
				// Canopy: the open tab carries a jade line along its top, the bar's own edge.
				const ImVec2 Min = ImGui::GetItemRectMin();
				const ImVec2 Max = ImGui::GetItemRectMax();
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(Min.x, Min.y), ImVec2(Max.x, Min.y + 2.0f * Scale), ImGui::GetColorU32(ColonyHud::Accent()));
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
				case EHudTab::Inventory: ColonyHud::Tip(TEXT("Storage: everything the colony has on the map. A star pins it to the top left")); break;
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
			if (IconButton(Speeds[Speed], Speed, Bridge.GetSpeed() == Speed))
			{
				Bridge.SetSpeed(Speed);
			}
			Record(Speeds[Speed]);
			ColonyHud::Tip(Speed == 0 ? FString(TEXT("Pause (Space)")) : FString::Printf(TEXT("Speed %d (%d)"), Speed, Speed));
			ImGui::SameLine();
		}
		UTradePanelSubsystem* Trade = GetWorld()->GetSubsystem<UTradePanelSubsystem>();
		if (IconButton(TEXT("Factions"), 5, Trade && Trade->IsFactionsOpen()) && Trade)
		{
			Trade->SetFactionsOpen(!Trade->IsFactionsOpen());
		}
		Record(TEXT("Factions"));
		ColonyHud::Tip(TEXT("Factions: who they are and what they think of the colony"));
		ImGui::SameLine();
		if (IconButton(TEXT("Menu"), 6, ImGui::IsPopupOpen("##HudMenu")))
		{
			ImGui::OpenPopup("##HudMenu");
			bQuitAsked = false;
		}
		Record(TEXT("Menu"));
		ColonyHud::Tip(TEXT("Menu: settings, new colony or load, message log, quit"));
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
	if (ColonyHud::Button(TEXT("Main menu"), false, ImVec2(Width, 0.0f)))
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
	// Keys and Tutorial used to be icon buttons on the bar; they live in this menu now (sneed 2026-11-25).
	if (ColonyHud::Button(TEXT("Tutorial"), bLessonsOn, ImVec2(Width, 0.0f)))
	{
		bLessonsOn = !bLessonsOn;
		SavePrefs();
		ImGui::CloseCurrentPopup();
	}
	ColonyHud::Tip(FString::Printf(TEXT("Tutorial: the learning helper's short lessons as things happen (%d learned)"), LearnedLessons.Num()));
	if (ColonyHud::Button(TEXT("Keys"), USimSubsystem::IsKeyHelpShown(), ImVec2(Width, 0.0f)))
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
	// Fully top right, at the head of the right column (sneed 2026-11-25), with the alarm's Draft button beside the day.
	HudLayout::PlaceRight(Viewport, HudLayout::ERightSlot::Date, Scale);
	ColonyHud::HoldWidth("##HudDate", Scale);
	if (ImGui::Begin("##HudDate", nullptr, ColonyHud::BoxFlags()))
	{
		const colony::GameTime& Time = Sim.time();
		ColonyHud::PushLarge();
		ColonyHud::Text(FString::Printf(TEXT("%02d:%02d   Day %lld"), Time.hourOfDay(),
			static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute), static_cast<long long>(Time.day() + 1)));
		ColonyHud::PopLarge();
		if (UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>())
		{
			ImGui::SameLine(0.0f, 18.0f * Scale);
			Orders->DrawAlarmButton(Bridge);
		}
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
		HudLayout::ReportRight(HudLayout::ERightSlot::Date);
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
	// Fully top right, where the inspect box goes when something is selected (sneed 2026-11-24, HudLayout).
	HudLayout::PlaceRight(Viewport, HudLayout::ERightSlot::Inspect, Scale);
	ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f * Scale, 0.0f), ImVec2(420.0f * Scale, FLT_MAX));
	ColonyHud::HoldWidth("##HudHover", Scale);
	if (ImGui::Begin("##HudHover", nullptr, ColonyHud::BoxFlags() | ImGuiWindowFlags_NoInputs))
	{
		HudLayout::ReportRight(HudLayout::ERightSlot::Inspect);
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
	ColonyHud::HoldWidth("##HudInfo", Scale);
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
	ColonyHud::HoldWidth("##HudSettings", Scale);
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
	USimPawnSubsystem* Pawns = World ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
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
		// Only from the front of an upright pawn: lying in bed or downed, the camera saw the back of the head (a grey shape in
		// the bar, PC 2026-11-24). The old picture stays until they are up again.
		if (View.posture == colony::PawnPosture::Lie || View.downed)
		{
			continue;
		}
		// A new look waits for them to stand still and upright (not bent over a job or mid-stride), unless there is no
		// picture yet.
		if (Shot && Shot->Target && (View.posture != colony::PawnPosture::Stand || View.vx * View.vx + View.vy * View.vy > 0.04f))
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
			// Lit by the world the pictures came out nearly black (night, indoors, and no eye adaptation history for a one-off
			// capture, PC 2026-11-20): unlit, so a face reads the same at any hour.
			MugshotCapture->ShowFlags.SetLighting(false);
			MugshotCapture->ShowFlags.SetEyeAdaptation(false);
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
			Target->InitAutoFormat(256, 256);  // sharp at the bigger portrait size
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
		// Straight at the face (sneed 2026-11-25: front on, never whatever angle the head had turned to): the head bone's turn
		// away from its bind pose carries the face's bind direction (the actor's forward) round with it; level, so a bowed
		// head still gets a level, front-on picture.
		FVector Forward = Actor->GetActorForwardVector();
		const int32 HeadBone = Mesh ? Mesh->GetBoneIndex(TEXT("head")) : INDEX_NONE;
		if (HeadBone != INDEX_NONE && Mesh->GetSkeletalMeshAsset())
		{
			const FTransform RefHead = FAnimationRuntime::GetComponentSpaceTransformRefPose(Mesh->GetSkeletalMeshAsset()->GetRefSkeleton(), HeadBone);
			const FQuat HeadWorld = Mesh->GetBoneQuaternion(TEXT("head"), EBoneSpaces::WorldSpace);
			const FVector FaceInComponent = Mesh->GetComponentQuat().Inverse().RotateVector(Actor->GetActorForwardVector());
			FVector Face = HeadWorld.RotateVector(RefHead.GetRotation().Inverse().RotateVector(FaceInComponent));
			Face.Z = 0.0f;
			if (Face.Normalize())
			{
				Forward = Face;
			}
		}
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
