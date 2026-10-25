#include "OverseerSubsystem.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include <imgui.h>

#include "OverseerPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

#include <cmath>

DEFINE_LOG_CATEGORY_STATIC(LogOverseer, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GOverseerShowUi(
		TEXT("Overseer.UI"), 1, TEXT("1 shows the overseer's panels (speed, inspect, key hints), 0 hides them."), ECVF_Default);

	FAutoConsoleCommandWithWorld GOverseerEnterCommand(
		TEXT("Overseer.Enter"), TEXT("Switches to the overseer camera."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr)
			{
				Overseer->EnterOverseerMode();
			}
		}));

	void OverseerText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	void OverseerText(const std::string& Text)
	{
		ImGui::TextUnformatted(Text.c_str(), Text.c_str() + Text.size());
	}

	uint32 OverseerIdNumber(colony::Entity Id)
	{
		return static_cast<uint32>(entt::to_integral(Id));
	}

	const TCHAR* OverseerTerrainName(colony::Terrain Terrain)
	{
		switch (Terrain)
		{
			case colony::Terrain::Water: return TEXT("Water");
			case colony::Terrain::Sand: return TEXT("Sand");
			case colony::Terrain::Soil: return TEXT("Soil");
			case colony::Terrain::Rock: return TEXT("Rock");
			case colony::Terrain::RoughStone: return TEXT("Rough stone");
		}
		return TEXT("?");
	}

	const TCHAR* OverseerCategoryName(colony::ThingCategory Category)
	{
		switch (Category)
		{
			case colony::ThingCategory::Pawn: return TEXT("Pawn");
			case colony::ThingCategory::Item: return TEXT("Item");
			case colony::ThingCategory::Building: return TEXT("Building");
		}
		return TEXT("Thing");
	}

	std::string OverseerLabel(const USimSubsystem& Bridge, const colony::ThingView& View)
	{
		if (const colony::DefDatabase* Defs = Bridge.GetDefs().get())
		{
			return Defs->get(View.def).label();
		}
		return "thing";
	}

	bool OverseerSpeedButton(const char* Label, int32 Speed, USimSubsystem& Bridge)
	{
		const bool bActive = Bridge.GetSpeed() == Speed;
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.75f, 1.0f));
		}
		const bool bClicked = ImGui::Button(Label);
		if (bActive)
		{
			ImGui::PopStyleColor();
		}
		if (bClicked)
		{
			Bridge.SetSpeed(Speed);
		}
		return bClicked;
	}
}

bool UOverseerSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UOverseerSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOverseerSubsystem, STATGROUP_Tickables);
}

void UOverseerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// -Direct (the protagonist subsystem) takes the player controller itself.
	if (FParse::Param(FCommandLine::Get(), TEXT("Overseer")) && !FParse::Param(FCommandLine::Get(), TEXT("Direct")))
	{
		EnterOverseerMode();
	}
}

AOverseerPawn* UOverseerSubsystem::GetOverseerPawn() const
{
	return Pawn;
}

bool UOverseerSubsystem::IsOverseerActive() const
{
	return bActive && IsValid(Pawn.Get());
}

AOverseerPawn* UOverseerSubsystem::EnterOverseerMode()
{
	UWorld* World = GetWorld();
	APlayerController* PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
	if (!PC)
	{
		UE_LOG(LogOverseer, Warning, TEXT("No player controller to give the overseer camera to"));
		return nullptr;
	}
	if (!IsValid(Pawn.Get()))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Pawn = World->SpawnActor<AOverseerPawn>(AOverseerPawn::StaticClass(), FTransform::Identity, Params);
		if (!Pawn)
		{
			return nullptr;
		}
		if (const USimSubsystem* Bridge = World->GetSubsystem<USimSubsystem>(); Bridge && Bridge->GetSim())
		{
			const colony::Map& Map = Bridge->GetSim()->map();
			Pawn->FocusOn(Bridge->SimToWorld(Map.width() * 0.5f, Map.height() * 0.5f, 0));
		}
		Pawn->SnapCamera();
	}
	bActive = true;
	PC->Possess(Pawn);
	PC->SetViewTarget(Pawn);
	PC->bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	InputMode.SetHideCursorDuringCapture(false);
	PC->SetInputMode(InputMode);
	UE_LOG(LogOverseer, Log, TEXT("Overseer camera active"));
	return Pawn;
}

void UOverseerSubsystem::Select(const FSimPick& Pick)
{
	Selection = Pick;
}

void UOverseerSubsystem::ClearSelection()
{
	Selection = FSimPick();
}

void UOverseerSubsystem::FocusOnSelection()
{
	const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !IsValid(Pawn.Get()))
	{
		return;
	}
	FVector Location;
	if (Selection.Kind == ESimPickKind::Thing && Bridge->GetBlendedLocation(Selection.Thing, Location))
	{
		Pawn->FocusOn(Location);
	}
	else if (Selection.Kind == ESimPickKind::Cell)
	{
		Pawn->FocusOn(Bridge->SimToWorld(Selection.Cell.x + 0.5f, Selection.Cell.y + 0.5f, Selection.Cell.level));
	}
}

void UOverseerSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !IsOverseerActive())
	{
		return;
	}

	// A selected thing that left the map (picked up, destroyed) is no longer selected.
	if (Selection.Kind == ESimPickKind::Thing && !Bridge->GetCurrentSnapshot().find(Selection.Thing))
	{
		ClearSelection();
	}
	DrawSelectionHighlight(*Bridge);
	if (GOverseerShowUi.GetValueOnGameThread() != 0)
	{
		DrawPanels(*Bridge);
	}
}

void UOverseerSubsystem::DrawSelectionHighlight(const USimSubsystem& Bridge) const
{
	UWorld* World = GetWorld();
	if (Selection.Kind == ESimPickKind::Thing)
	{
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		const colony::ThingView* View = Snapshot.find(Selection.Thing);
		FBox Box;
		if (!View || !SimPicking::ThingBox(Bridge, *View, Box))
		{
			return;
		}
		DrawDebugBox(World, Box.GetCenter(), Box.GetExtent(), FColor::White, false, -1.0f, SDPG_Foreground, 4.0f);
		// A moving pawn's route ahead, from its feet along the waypoints.
		if (View->category == colony::ThingCategory::Pawn && View->waypointCount > 0)
		{
			FVector From;
			Bridge.GetBlendedLocation(View->id, From);
			From.Z += 20.0;
			for (uint32 Index = 0; Index < View->waypointCount; ++Index)
			{
				const colony::PathPointView& Point = Snapshot.waypoints[View->firstWaypoint + Index];
				const FVector To = Bridge.SimToWorld(Point.x, Point.y, View->level) + FVector(0.0, 0.0, 20.0);
				DrawDebugLine(World, From, To, FColor::Cyan, false, -1.0f, SDPG_Foreground, 3.0f);
				From = To;
			}
		}
	}
	else if (Selection.Kind == ESimPickKind::Cell)
	{
		// The cell's outline lying on the ground surface.
		const colony::CellPos Cell = Selection.Cell;
		const FVector Lift(0.0, 0.0, 6.0);
		const FVector Corner[4] = {
			Bridge.SimToWorld(Cell.x, Cell.y, Cell.level) + Lift,
			Bridge.SimToWorld(Cell.x + 1.0f, Cell.y, Cell.level) + Lift,
			Bridge.SimToWorld(Cell.x + 1.0f, Cell.y + 1.0f, Cell.level) + Lift,
			Bridge.SimToWorld(Cell.x, Cell.y + 1.0f, Cell.level) + Lift,
		};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			DrawDebugLine(World, Corner[Index], Corner[(Index + 1) % 4], FColor::Yellow, false, -1.0f, SDPG_Foreground, 5.0f);
		}
	}
}

void UOverseerSubsystem::DrawPanels(USimSubsystem& Bridge)
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
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();

	// Readable from 1080p to 4K.
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	const float Margin = 12.0f * Scale;
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

	// Speed controls, top middle. The same speeds the keys set.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Margin), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	if (ImGui::Begin("##OverseerSpeed", nullptr, PanelFlags))
	{
		const colony::GameTime& Time = Sim.time();
		OverseerText(FString::Printf(TEXT("Day %lld   %02d:%02d"), static_cast<long long>(Time.day() + 1), Time.hourOfDay(),
			static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute)));
		ImGui::SameLine(0.0f, 24.0f * Scale);
		OverseerSpeedButton("Pause", 0, Bridge);
		ImGui::SameLine();
		OverseerSpeedButton("1x", 1, Bridge);
		ImGui::SameLine();
		OverseerSpeedButton("2x", 2, Bridge);
		ImGui::SameLine();
		OverseerSpeedButton("3x", 3, Bridge);
		if (Bridge.GetSpeed() == 0)
		{
			ImGui::SameLine();
			if (ImGui::Button("Step"))
			{
				Bridge.StepOnce();
			}
		}
	}
	ImGui::End();

	// Inspect panel, bottom left: what is selected.
	if (Selection.IsValid())
	{
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Margin, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * Scale, 0.0f), ImVec2(560.0f * Scale, FLT_MAX));
		if (ImGui::Begin("##OverseerInspect", nullptr, PanelFlags))
		{
			if (Selection.Kind == ESimPickKind::Thing)
			{
				if (const colony::ThingView* View = Snapshot.find(Selection.Thing))
				{
					OverseerText(OverseerLabel(Bridge, *View));
					ImGui::Separator();
					OverseerText(FString::Printf(TEXT("%s   #%u"), OverseerCategoryName(View->category), OverseerIdNumber(View->id)));
					OverseerText(FString::Printf(TEXT("Cell %d, %d   ground %.1f m"), FMath::FloorToInt32(View->x), FMath::FloorToInt32(View->y), View->z));
					if (View->maxHitPoints > 0)
					{
						const FString Overlay = FString::Printf(TEXT("%d / %d"), View->hitPoints, View->maxHitPoints);
						const FTCHARToUTF8 Utf8(*Overlay);
						ImGui::ProgressBar(static_cast<float>(View->hitPoints) / static_cast<float>(View->maxHitPoints), ImVec2(-1.0f, 0.0f), Utf8.Get());
					}
					switch (View->category)
					{
						case colony::ThingCategory::Pawn:
						{
							const float Speed = std::sqrt(View->vx * View->vx + View->vy * View->vy);
							OverseerText(FString::Printf(TEXT("Moving at %.1f m/s"), Speed));
							if (View->waypointCount > 0)
							{
								const colony::PathPointView& End = Snapshot.waypoints[View->firstWaypoint + View->waypointCount - 1];
								OverseerText(FString::Printf(TEXT("Heading to cell %d, %d"), FMath::FloorToInt32(End.x), FMath::FloorToInt32(End.y)));
							}
							if (Sim.colonistOf(View->id))
							{
								if (ImGui::Button("Take control (Tab)"))
								{
									if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
									{
										Protagonist->RequestControl(View->id);
									}
								}
							}
							break;
						}
						case colony::ThingCategory::Item:
							OverseerText(FString::Printf(TEXT("Stack of %d"), View->stackCount));
							break;
						case colony::ThingCategory::Building:
							OverseerText(FString::Printf(TEXT("Footprint %d x %d"), View->sizeX, View->sizeY));
							break;
					}
				}
			}
			else
			{
				const colony::CellPos Cell = Selection.Cell;
				const colony::Map& Map = Sim.map();
				OverseerText(FString::Printf(TEXT("Cell %d, %d"), Cell.x, Cell.y));
				ImGui::Separator();
				OverseerText(FString::Printf(TEXT("%s, ground %.1f m"), OverseerTerrainName(Map.terrain(Cell)), Map.groundHeight(Cell)));
				OverseerText(FString(Map.isDoor(Cell) ? TEXT("Door: walkable, own region") : (Map.walkable(Cell) ? TEXT("Walkable") : TEXT("Not walkable"))));
				const colony::Entity Building = Map.buildingAt(Cell);
				if (const colony::ThingView* View = Building == colony::kNoEntity ? nullptr : Snapshot.find(Building))
				{
					OverseerText(FString::Printf(TEXT("Building: %s"), UTF8_TO_TCHAR(OverseerLabel(Bridge, *View).c_str())));
				}
				for (const colony::ThingView& View : Snapshot.things)
				{
					if (View.category != colony::ThingCategory::Building && FMath::FloorToInt32(View.x) == Cell.x && FMath::FloorToInt32(View.y) == Cell.y)
					{
						OverseerText(FString::Printf(TEXT("%s: %s"), OverseerCategoryName(View.category), UTF8_TO_TCHAR(OverseerLabel(Bridge, View).c_str())));
					}
				}
			}
		}
		ImGui::End();
	}

	// Key hints, bottom right.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("##OverseerHints", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
	{
		ImGui::TextUnformatted("WASD pan   Q/E turn   wheel zoom");
		ImGui::TextUnformatted("middle-drag orbit   click select");
		ImGui::TextUnformatted("F centre   Home reset   Esc deselect");
		ImGui::TextUnformatted("Tab take control of the selected colonist");
	}
	ImGui::End();

	++PanelFramesDrawn;
}
