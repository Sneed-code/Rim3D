// Smoke test `orders_world`: building from the overseer view with REAL mouse and key events, on the playtest's world start
// (-SimWorld -SimScenario=CrashlandedRainforest -SimOriginX=300000 -Overseer, which tools/smoke.ps1 adds for this test).
// The `orders` test calls the orders functions directly; this one sends Slate pointer events through the same path a mouse
// does: Slate's input preprocessors first (the ImGui overlay can take the click there), then the game viewport, the player
// controller and the overseer pawn's key bindings. It clicks the architect bar's Structure and Wall buttons, hovers, drags
// a line of walls on the ground near the colonists, clicks off the map, goes to direct mode and back with Tab, and builds again.
//
// Offscreen (-RenderOffScreen) the game window is a dummy that Slate's hit test skips, so after Slate has offered an event to the
// preprocessors (and to a widget holding the mouse capture), the test hands it to the game viewport widget itself: that is what
// the hit test finds on screen, since the ImGui overlay does not take hits. A faux Slate cursor stands in for the mouse so the real mouse
// cursor (wherever it is on the desktop) does not move the test pointer and is never moved itself.
#include "SmokeTest.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"

#include <imgui.h>

#include "OrdersSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/World.h"

namespace
{
	constexpr int32 OwLineLength = 6;

	struct FOwState
	{
		bool bReady = false;
		bool bListening = false;
		int32 FailedCommands = 0;
		FString FirstFailure;
		colony::CellPos Site;
		int32 Frame = 0;
		int32 WallsBefore = 0;
		int32 WallsAfterFirst = -1;
		int32 WallsAfterSecond = -1;
		int32 RowOffset = 0;
		bool bGhostSeen = false;
		int32 GhostValidDragging = -1;
		bool bWasDragging = false;
		bool bKeysRouted = true;
		bool bWentDirect = false;
		FString OffGroundMessage;
		// Pointer
		FVector2D LastAbs = FVector2D::ZeroVector;
		bool bHeld = false;
		bool bOverUiBefore = false;
		int32 PreFrames = 0;
		TArray<FString> Log;
	};

	USimSubsystem* OwBridge(UWorld* World) { return World ? World->GetSubsystem<USimSubsystem>() : nullptr; }

	TSharedPtr<SViewport> OwViewportWidget()
	{
		return GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetGameViewportWidget() : nullptr;
	}

	/** Desktop (Slate absolute) position of a viewport pixel. */
	bool OwPixelToAbs(const FVector2D& Pixel, FVector2D& OutAbs)
	{
		const TSharedPtr<SViewport> Widget = OwViewportWidget();
		if (!Widget || !GEngine->GameViewport->Viewport)
		{
			return false;
		}
		const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
		const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return false;
		}
		const FVector2D AbsSize = FVector2D(Geometry.GetAbsoluteSize());
		OutAbs = FVector2D(Geometry.GetAbsolutePosition()) + Pixel * AbsSize / FVector2D(Size);
		return true;
	}

	/** Desktop position of an ImGui point. The ImGui plugin runs with multi-viewports on (desktop builds), so ImGui's
	 *  coordinates are already desktop (Slate absolute) pixels. */
	bool OwImGuiToAbs(const FVector2D& Point, FVector2D& OutAbs)
	{
		OutAbs = Point;
		return OwViewportWidget().IsValid();
	}

	enum class EOwPointer : uint8 { Move, Down, Up };

	/** One real pointer event through Slate (preprocessors, capture), handed to the viewport widget if nothing took it. */
	FString OwSendPointer(FOwState& State, EOwPointer Kind, const FVector2D& Abs)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return TEXT("no Slate");
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const TSharedPtr<SViewport> Widget = OwViewportWidget();
		FWidgetPath ViewportPath;
		if (!Widget || !Slate.FindPathToWidget(Widget.ToSharedRef(), ViewportPath, EVisibility::All))
		{
			return TEXT("no path to the game viewport");
		}
		if (Kind == EOwPointer::Down)
		{
			State.bHeld = true;
		}
		else if (Kind == EOwPointer::Up)
		{
			State.bHeld = false;
		}
		TSet<FKey> Pressed;
		if (State.bHeld)
		{
			Pressed.Add(EKeys::LeftMouseButton);
		}
		if (const TSharedPtr<FSlateUser> CursorUser = Slate.GetUser(Slate.GetUserIndexForMouse()))
		{
			// The faux cursor (see ow_setup) only stores this; the real mouse cursor is not moved.
			CursorUser->SetCursorPosition(Abs);
		}
		const FPointerEvent Event(Slate.GetUserIndexForMouse(), FSlateApplication::CursorPointerIndex, Abs, State.LastAbs, Pressed,
			Kind == EOwPointer::Move ? EKeys::Invalid : EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
		State.LastAbs = Abs;
		const TSharedPtr<FSlateUser> User = Slate.GetUser(Slate.GetUserIndexForMouse());
		const bool bCaptured = User && User->HasCursorCapture();
		bool bTaken = false;
		bool bHanded = false;
		switch (Kind)
		{
			case EOwPointer::Move:
				bTaken = Slate.ProcessMouseMoveEvent(Event);
				if (!bTaken && !bCaptured)
				{
					bHanded = true;
					Slate.RoutePointerMoveEvent(ViewportPath, Event, false);
				}
				break;
			case EOwPointer::Down:
				bTaken = Slate.ProcessMouseButtonDownEvent(nullptr, Event);
				if (!bTaken)
				{
					bHanded = true;
					Slate.RoutePointerDownEvent(ViewportPath, Event);
				}
				break;
			case EOwPointer::Up:
				bTaken = Slate.ProcessMouseButtonUpEvent(Event);
				if (!bTaken && !bCaptured)
				{
					bHanded = true;
					Slate.RoutePointerUpEvent(ViewportPath, Event);
				}
				break;
		}
		return FString::Printf(TEXT("%s at (%.0f, %.0f) [ImGui over UI before: %s]: %s"), Kind == EOwPointer::Move ? TEXT("move") : Kind == EOwPointer::Down ? TEXT("down") : TEXT("up"),
			Abs.X, Abs.Y, State.bOverUiBefore ? TEXT("yes") : TEXT("no"), bTaken ? (bCaptured ? TEXT("taken (mouse captured)") : TEXT("taken by Slate (preprocessor or widget)")) : bHanded ? TEXT("given to the viewport") : TEXT("to the captor"));
	}

	bool OwSendKey(const FKey& Key)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return false;
		}
		FSlateApplication& Slate = FSlateApplication::Get();
		const uint32* KeyCode = nullptr;
		const uint32* CharCode = nullptr;
		FInputKeyManager::Get().GetCodesFromKey(Key, KeyCode, CharCode);
		const FKeyEvent Down(Key, FModifierKeysState(), Slate.GetUserIndexForKeyboard(), false, CharCode ? *CharCode : 0, KeyCode ? *KeyCode : 0);
		const bool bDown = Slate.ProcessKeyDownEvent(Down);
		const FKeyEvent Up(Key, FModifierKeysState(), Slate.GetUserIndexForKeyboard(), false, CharCode ? *CharCode : 0, KeyCode ? *KeyCode : 0);
		Slate.ProcessKeyUpEvent(Up);
		return bDown;
	}

	bool OwCellPixel(UWorld* World, colony::CellPos Cell, FVector2D& OutPixel)
	{
		USimSubsystem* Bridge = OwBridge(World);
		APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
		if (!Bridge || !PC)
		{
			return false;
		}
		return PC->ProjectWorldLocationToScreen(Bridge->SimToWorld(Cell.x + 0.5f, Cell.y + 0.5f, Cell.level), OutPixel, false);
	}

	int32 OwCountWalls(const USimSubsystem& Bridge)
	{
		int32 Count = 0;
		const colony::Sim& Sim = *Bridge.GetSim();
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			Count += View.category == colony::ThingCategory::Building && Sim.defs().get(View.def).defName() == "Wall" ? 1 : 0;
		}
		return Count;
	}

	/** A row of cells (and the rows beside it) a wall can go on, near `Near`: walkable, nothing built, gently sloped. */
	bool OwFindSite(const colony::Map& Map, colony::CellPos Near, colony::CellPos& Out)
	{
		for (int32 R = 3; R <= 60; ++R)
		{
			for (int32 DY = -R; DY <= R; ++DY)
			{
				for (int32 DX = -R; DX <= R; ++DX)
				{
					if (FMath::Max(FMath::Abs(DX), FMath::Abs(DY)) != R)
					{
						continue;
					}
					bool bOk = true;
					float Low = 1.0e9f;
					float High = -1.0e9f;
					for (int32 Y = -1; Y <= 3 && bOk; ++Y)
					{
						for (int32 X = -1; X <= OwLineLength && bOk; ++X)
						{
							const colony::CellPos Cell{Near.x + DX + X, Near.y + DY + Y, 0};
							bOk = Map.inBounds(Cell) && Map.walkable(Cell) && Map.buildingAt(Cell) == colony::kNoEntity && Map.groundWalkable(Cell);
							if (bOk)
							{
								Low = FMath::Min(Low, Map.groundHeight(Cell.x, Cell.y));
								High = FMath::Max(High, Map.groundHeight(Cell.x, Cell.y));
							}
						}
					}
					if (bOk && High - Low < 2.0f)
					{
						Out = colony::CellPos{Near.x + DX, Near.y + DY, 0};
						return true;
					}
				}
			}
		}
		return false;
	}
}

void USmokeTestSubsystem::BuildOrdersWorldPhases()
{
	const TSharedRef<FOwState> State = MakeShared<FOwState>();
	const auto Orders = [this]() { return GetWorld()->GetSubsystem<UOrdersSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto Note = [this, State](const FString& Line)
	{
		State->Log.Add(Line);
		UE_LOG(LogTemp, Log, TEXT("orders_world: %s"), *Line);
	};
	const auto Aim = [this, State]()
	{
		AOverseerPawn* Pawn = GetWorld()->GetSubsystem<UOverseerSubsystem>()->GetOverseerPawn();
		USimSubsystem* Bridge = OwBridge(GetWorld());
		if (Pawn && Bridge)
		{
			Pawn->ResetView();
			Pawn->Zoom(2.0f);
			Pawn->FocusOn(Bridge->SimToWorld(State->Site.x + OwLineLength * 0.5f, State->Site.y + 1.5f, 0));
			Pawn->SnapCamera();
		}
	};
	const auto Pointer = [this, State, Note](EOwPointer Kind, const FVector2D& Abs)
	{
		UOrdersSubsystem* O = GetWorld()->GetSubsystem<UOrdersSubsystem>();
		State->bOverUiBefore = O->IsMouseOverUi();
		FBox2D Unused;
		const FString Result = OwSendPointer(*State, Kind, Abs);
		Note(FString::Printf(TEXT("%s (tool %s, Wall button %s)"), *Result, O->GetTool() == EOrderTool::Build ? *O->GetBuildSummary() : TEXT("none"),
			O->GetButtonRect(TEXT("Wall"), Unused) ? TEXT("shown") : TEXT("hidden")));
	};
	const auto ButtonAbs = [this, Orders](const TCHAR* Label, FVector2D& OutAbs) -> bool
	{
		FBox2D Rect;
		return Orders()->GetButtonRect(Label, Rect) && OwImGuiToAbs(Rect.GetCenter(), OutAbs);
	};
	const auto CellAbs = [this](colony::CellPos Cell, FVector2D& OutAbs) -> bool
	{
		FVector2D Pixel;
		return OwCellPixel(GetWorld(), Cell, Pixel) && OwPixelToAbs(Pixel, OutAbs);
	};
	const auto Row = [State](int32 X) { return colony::CellPos{State->Site.x + X, State->Site.y + State->RowOffset, 0}; };

	// Clicks the Structure button, then the Wall button, with real events a few frames apart (ImGui needs a frame to see each).
	const auto PickWallFrame = [this, State, Pointer, ButtonAbs](const FString& PhaseName)
	{
		const int32 F = ++State->Frame;
		FVector2D Abs;
		const TCHAR* Label = F < 14 ? TEXT("Structure") : TEXT("Wall");
		const bool bPointerFrame = F == 2 || F == 4 || F == 6 || F == 9 || F == 16 || F == 18 || F == 20 || F == 23;
		if (!bPointerFrame)
		{
			return;
		}
		if (!ButtonAbs(Label, Abs))
		{
			Fail(FString::Printf(TEXT("%s: the architect bar has no %s button on screen"), *PhaseName, Label));
			State->Frame = 1000;
			return;
		}
		if (F == 2 || F == 4 || F == 16 || F == 18)
		{
			Pointer(EOwPointer::Move, Abs);
		}
		else if (F == 6 || F == 20)
		{
			Pointer(EOwPointer::Down, Abs);
		}
		else
		{
			Pointer(EOwPointer::Up, Abs);
		}
	};
	const auto VerifyWallInHand = [this, Orders](const FString& PhaseName)
	{
		UOrdersSubsystem* O = Orders();
		if (O->GetTool() != EOrderTool::Build || !O->GetBuildSummary().StartsWith(TEXT("Wall")))
		{
			Fail(FString::Printf(TEXT("%s: clicking Structure then Wall left the tool at '%s' (wanted a wall in hand)"), *PhaseName,
				O->GetTool() == EOrderTool::Build ? *O->GetBuildSummary() : TEXT("none")));
		}
	};
	// Hovers the row's first cell, presses, drags to its last cell and holds.
	const auto DragFrame = [this, State, Pointer, CellAbs, Row, Orders](const FString& PhaseName)
	{
		const int32 F = ++State->Frame;
		FVector2D From;
		FVector2D To;
		if (!CellAbs(Row(0), From) || !CellAbs(Row(OwLineLength - 1), To))
		{
			if (F == 1)
			{
				Fail(FString::Printf(TEXT("%s: the wall row is not on screen"), *PhaseName));
			}
			return;
		}
		if (F <= 6)
		{
			Pointer(EOwPointer::Move, From);
		}
		else if (F == 8)
		{
			Pointer(EOwPointer::Down, From);
		}
		else if (F > 8 && F <= 20)
		{
			Pointer(EOwPointer::Move, FMath::Lerp(From, To, (F - 8) / 12.0));
		}
		else if (F > 20 && F % 10 == 0)
		{
			Pointer(EOwPointer::Move, To);
		}
		if (F == 6)
		{
			State->bGhostSeen = Orders()->HasHoverCell() && Orders()->GetStats().GhostValid > 0;
		}
	};
	const auto VerifyDrag = [this, State, Orders](const FString& PhaseName)
	{
		UOrdersSubsystem* O = Orders();
		State->bWasDragging = O->IsDragging();
		State->GhostValidDragging = O->GetStats().GhostValid;
		PhaseNumbers.Add(TEXT("ghost_valid_dragging"), State->GhostValidDragging);
		if (!State->bGhostSeen)
		{
			Fail(FString::Printf(TEXT("%s: hovering the ground with a wall in hand drew no ghost (hover %s, mouse over UI %s)"), *PhaseName,
				O->HasHoverCell() ? TEXT("set") : TEXT("never set"), O->IsMouseOverUi() ? TEXT("yes") : TEXT("no")));
		}
		if (!O->IsDragging())
		{
			Fail(FString::Printf(TEXT("%s: a real left press on the ground started no drag (see LogOverseerInput / LogOrders lines)"), *PhaseName));
		}
		else if (State->GhostValidDragging != OwLineLength)
		{
			Fail(FString::Printf(TEXT("%s: dragging a line of %d walls showed %d green ghosts"), *PhaseName, OwLineLength, State->GhostValidDragging));
		}
	};
	const auto Release = [this, State, Pointer, CellAbs, Row]()
	{
		FVector2D To;
		if (CellAbs(Row(OwLineLength - 1), To))
		{
			Pointer(EOwPointer::Up, To);
		}
	};

	// 1. The world start is up: the colony on its rainforest tile, the overseer view, a spot near the colonists.
	FPhase Setup;
	Setup.Name = TEXT("ow_setup");
	Setup.WarmupSeconds = 6.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [this, State, Overseer, Note]()
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Terrain.Roofs")))
		{
			Var->Set(0, ECVF_SetByCode);
		}
		if (FSlateApplication::IsInitialized())
		{
			// Offscreen the app is never the active one; on screen it is while sneed plays.
			FSlateApplication::Get().SetHandleDeviceInputWhenApplicationNotActive(true);
			// A faux cursor for the mouse user: Slate's synthetic cursor moves follow the test's pointer, not the desktop mouse.
			FSlateApplication::Get().UsePlatformCursorForCursorUser(false);
		}
		USimSubsystem* Bridge = OwBridge(GetWorld());
		if (!Bridge || !Bridge->GetSim())
		{
			Fail(TEXT("ow_setup: no sim (start with -SimWorld -SimScenario=CrashlandedRainforest -SimOriginX=300000 -Overseer)"));
			return;
		}
		const colony::Sim& Sim = *Bridge->GetSim();
		if (!Sim.world() || Sim.world()->empty())
		{
			Fail(TEXT("ow_setup: the sim has no world (missing -SimWorld?)"));
		}
		if (!FMath::IsNearlyEqual(Bridge->GetWorldOrigin().X, 300000.0))
		{
			Fail(FString::Printf(TEXT("ow_setup: world origin X is %.0f, wanted 300000 (-SimOriginX)"), Bridge->GetWorldOrigin().X));
		}
		if (!Overseer() || !Overseer()->IsOverseerActive())
		{
			Fail(TEXT("ow_setup: the overseer view is not on (-Overseer)"));
			return;
		}
		// The game runs at normal speed, as in the playtest.
		Bridge->SetSpeed(1);
		// The colonists' middle.
		FVector2D Sum = FVector2D::ZeroVector;
		int32 Count = 0;
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && Sim.colonistOf(View.id))
			{
				Sum += FVector2D(View.x, View.y);
				++Count;
			}
		}
		if (Count == 0)
		{
			Fail(TEXT("ow_setup: no colonists on the map"));
			return;
		}
		const colony::CellPos Middle{FMath::FloorToInt32(Sum.X / Count), FMath::FloorToInt32(Sum.Y / Count), 0};
		if (!OwFindSite(Sim.map(), Middle, State->Site))
		{
			Fail(TEXT("ow_setup: no open ground for a row of walls near the colonists"));
			return;
		}
		// Clear wild plants off the rows so the picture shows the ghosts (a dev placement; walls can go over plants anyway).
		for (int32 Y = -1; Y <= 3; ++Y)
		{
			for (int32 X = -1; X <= OwLineLength; ++X)
			{
				const_cast<colony::Sim&>(Sim).plants().remove(colony::CellPos{State->Site.x + X, State->Site.y + Y, 0});
			}
		}
		State->WallsBefore = OwCountWalls(*Bridge);
		State->bReady = true;
		Note(FString::Printf(TEXT("%d colonists around cell %d, %d; wall row at %d, %d; map %d x %d; origin %.0f"), Count, Middle.x, Middle.y,
			State->Site.x, State->Site.y, Sim.map().width(), Sim.map().height(), Bridge->GetWorldOrigin().X));
		if (!State->bListening)
		{
			State->bListening = true;
			Bridge->OnSimEvent.AddLambda([State](const colony::Event& Event)
			{
				if (Event.type == colony::EventType::CommandFailed)
				{
					++State->FailedCommands;
					if (State->FirstFailure.IsEmpty())
					{
						State->FirstFailure = UTF8_TO_TCHAR(Event.message.c_str());
					}
				}
			});
		}
	};
	Setup.OnFrame = [State, Aim]() { if (State->bReady) { Aim(); } };
	Phases.Add(MoveTemp(Setup));

	// 2. Structure, then Wall, clicked on the architect bar with real mouse events.
	FPhase PickWall;
	PickWall.Name = TEXT("ow_pick_wall");
	PickWall.WarmupSeconds = 2.0f;
	PickWall.MeasureSeconds = 0.5f;
	PickWall.Setup = [State]() { State->Frame = 0; };
	PickWall.OnFrame = [State, PickWallFrame]() { if (State->bReady) { PickWallFrame(TEXT("ow_pick_wall")); } };
	PickWall.Verify = [State, VerifyWallInHand]() { if (State->bReady) { VerifyWallInHand(TEXT("ow_pick_wall")); } };
	Phases.Add(MoveTemp(PickWall));

	// 3. Hover the ground (the ghost), press, drag a line of walls and hold it: the picture shows the ghost line.
	FPhase DragPhase;
	DragPhase.Name = TEXT("ow_drag");
	DragPhase.WarmupSeconds = 2.5f;
	DragPhase.MeasureSeconds = 1.0f;
	DragPhase.Setup = [State]() { State->Frame = 0; State->RowOffset = 0; };
	DragPhase.OnFrame = [State, DragFrame]() { if (State->bReady) { DragFrame(TEXT("ow_drag")); } };
	DragPhase.Verify = [State, VerifyDrag]() { if (State->bReady) { VerifyDrag(TEXT("ow_drag")); } };
	Phases.Add(MoveTemp(DragPhase));

	// 4. Let go: six wall plans.
	FPhase ReleasePhase;
	ReleasePhase.Name = TEXT("ow_release");
	ReleasePhase.WarmupSeconds = 1.0f;
	ReleasePhase.MeasureSeconds = 0.5f;
	ReleasePhase.Setup = [State, Release]() { if (State->bReady) { Release(); } };
	ReleasePhase.Verify = [this, State]()
	{
		USimSubsystem* Bridge = OwBridge(GetWorld());
		if (!State->bReady || !Bridge)
		{
			return;
		}
		State->WallsAfterFirst = OwCountWalls(*Bridge);
		PhaseNumbers.Add(TEXT("walls_planned"), State->WallsAfterFirst - State->WallsBefore);
		if (State->WallsAfterFirst - State->WallsBefore != OwLineLength)
		{
			Fail(FString::Printf(TEXT("ow_release: %d wall plans after dragging a line of %d (failed commands %d: %s)"), State->WallsAfterFirst - State->WallsBefore,
				OwLineLength, State->FailedCommands, *State->FirstFailure));
		}
	};
	Phases.Add(MoveTemp(ReleasePhase));

	// 5. A click off the map with the wall in hand says why nothing happens.
	FPhase OffGround;
	OffGround.Name = TEXT("ow_off_ground");
	OffGround.WarmupSeconds = 1.0f;
	OffGround.MeasureSeconds = 0.5f;
	OffGround.Setup = [this, State, Orders]()
	{
		State->Frame = 0;
		// Look at the map's south-west corner: the lower left of the screen is off the map.
		AOverseerPawn* Pawn = GetWorld()->GetSubsystem<UOverseerSubsystem>()->GetOverseerPawn();
		USimSubsystem* Bridge = OwBridge(GetWorld());
		if (State->bReady && Pawn && Bridge)
		{
			Pawn->ResetView();
			Pawn->FocusOn(Bridge->SimToWorld(0.0f, 0.0f, 0));
			Pawn->SnapCamera();
		}
	};
	OffGround.OnFrame = [this, State, Pointer]()
	{
		const int32 F = ++State->Frame;
		const FIntPoint Size = GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport ? GEngine->GameViewport->Viewport->GetSizeXY() : FIntPoint(0, 0);
		FVector2D Abs;
		if (!State->bReady || Size.X <= 0 || !OwPixelToAbs(FVector2D(Size.X * 0.3, Size.Y * 0.62), Abs))
		{
			return;
		}
		if (F == 3 || F == 5)
		{
			Pointer(EOwPointer::Move, Abs);
		}
		else if (F == 8)
		{
			Pointer(EOwPointer::Down, Abs);
		}
		else if (F == 11)
		{
			State->OffGroundMessage = GetWorld()->GetSubsystem<UOrdersSubsystem>()->GetLastMessage();
			Pointer(EOwPointer::Up, Abs);
		}
	};
	OffGround.Verify = [this, State]()
	{
		if (State->bReady && State->OffGroundMessage != TEXT("Click on the ground"))
		{
			Fail(FString::Printf(TEXT("ow_off_ground: a click off the map with a wall in hand said '%s', wanted 'Click on the ground'"), *State->OffGroundMessage));
		}
	};
	Phases.Add(MoveTemp(OffGround));

	// 6. Tab to direct mode and Tab back, by key events.
	FPhase TabPhase;
	TabPhase.Name = TEXT("ow_tab");
	TabPhase.WarmupSeconds = 3.0f;
	TabPhase.MeasureSeconds = 0.5f;
	TabPhase.Setup = [State]() { State->Frame = 0; };
	TabPhase.OnFrame = [this, State, Note]()
	{
		const int32 F = ++State->Frame;
		UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
		if (!State->bReady || !Protagonist)
		{
			return;
		}
		if (F == 3 || F == 60)
		{
			const bool bTaken = OwSendKey(EKeys::Tab);
			Note(FString::Printf(TEXT("Tab key (%s): Slate %s"), F == 3 ? TEXT("to direct") : TEXT("back to overseer"), bTaken ? TEXT("handled it") : TEXT("did not report it handled")));
		}
		else if (F == 30)
		{
			State->bWentDirect = Protagonist->IsDirectMode();
			Note(FString::Printf(TEXT("after the first Tab: %s"), State->bWentDirect ? TEXT("direct mode") : TEXT("still the overseer view")));
		}
	};
	TabPhase.Verify = [this, State, Note]()
	{
		UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
		if (!State->bReady || !Protagonist)
		{
			return;
		}
		State->bKeysRouted = State->bWentDirect;
		if (Protagonist->IsDirectMode() || !GetWorld()->GetSubsystem<UOverseerSubsystem>()->IsOverseerActive())
		{
			// The key events did not get to the pawn offscreen: put the view back so the rest of the test runs.
			Note(TEXT("Tab key events left the game in direct mode; toggling back by hand"));
			State->bKeysRouted = false;
			Protagonist->ToggleMode();
		}
	};
	Phases.Add(MoveTemp(TabPhase));

	// 7. After Tab and back: Structure, Wall and a second row, all by real events again.
	FPhase PickAgain;
	PickAgain.Name = TEXT("ow_pick_again");
	PickAgain.WarmupSeconds = 2.0f;
	PickAgain.MeasureSeconds = 0.5f;
	PickAgain.Setup = [State, Aim]()
	{
		State->Frame = 0;
		if (State->bReady)
		{
			Aim();
		}
	};
	PickAgain.OnFrame = [State, PickWallFrame, Orders]()
	{
		if (!State->bReady)
		{
			return;
		}
		// A wall still in hand (it stays through direct mode): clicking Wall again would put it down, so there is nothing to click.
		// Else if the Structure category is still open, the first click would close it; go straight to the Wall button.
		FBox2D Unused;
		if (State->Frame == 0 && Orders()->GetTool() == EOrderTool::Build && Orders()->GetBuildSummary().StartsWith(TEXT("Wall")))
		{
			State->Frame = 1000;
		}
		else if (State->Frame == 0 && Orders()->GetButtonRect(TEXT("Wall"), Unused))
		{
			State->Frame = 13;
		}
		PickWallFrame(TEXT("ow_pick_again"));
	};
	PickAgain.Verify = [State, VerifyWallInHand]() { if (State->bReady) { VerifyWallInHand(TEXT("ow_pick_again")); } };
	Phases.Add(MoveTemp(PickAgain));

	FPhase DragAgain;
	DragAgain.Name = TEXT("ow_drag_again");
	DragAgain.WarmupSeconds = 2.5f;
	DragAgain.MeasureSeconds = 1.0f;
	DragAgain.Setup = [State, Note]()
	{
		State->Frame = 0;
		State->RowOffset = 2;
		State->bGhostSeen = false;
		State->PreFrames = 0;
	};
	DragAgain.OnFrame = [State, DragFrame, Pointer, ButtonAbs, Note]()
	{
		if (!State->bReady)
		{
			return;
		}
		// First: a middle press that starts over the architect bar and whose release ImGui never hears (lost while the mouse was
		// captured, or outside the window). ImGui then keeps its want-the-mouse flag on everywhere, and its input handler would
		// swallow every press on the ground. The ground drag after it must still work and draw its ghost.
		if (State->PreFrames < 8)
		{
			const int32 Pre = ++State->PreFrames;
			FVector2D Abs;
			if ((Pre == 1 || Pre == 3) && ButtonAbs(TEXT("Structure"), Abs))
			{
				Pointer(EOwPointer::Move, Abs);
			}
			if (Pre == 6)
			{
				const ImGui::FScopedContext ScopedContext;
				if (ScopedContext)
				{
					ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
					Note(TEXT("ImGui now thinks the middle button is held (pressed over the bar)"));
				}
			}
			return;
		}
		DragFrame(TEXT("ow_drag_again"));
	};
	DragAgain.Verify = [State, VerifyDrag]() { if (State->bReady) { VerifyDrag(TEXT("ow_drag_again")); } };
	Phases.Add(MoveTemp(DragAgain));

	FPhase ReleaseAgain;
	ReleaseAgain.Name = TEXT("ow_release_again");
	ReleaseAgain.WarmupSeconds = 1.0f;
	ReleaseAgain.MeasureSeconds = 0.5f;
	ReleaseAgain.Setup = [State, Release]() { if (State->bReady) { Release(); } };
	ReleaseAgain.Verify = [this, State]()
	{
		USimSubsystem* Bridge = OwBridge(GetWorld());
		if (!State->bReady || !Bridge)
		{
			return;
		}
		{
			const ImGui::FScopedContext ScopedContext;
			if (ScopedContext)
			{
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
			}
		}
		State->WallsAfterSecond = OwCountWalls(*Bridge);
		PhaseNumbers.Add(TEXT("walls_planned_again"), State->WallsAfterSecond - State->WallsAfterFirst);
		if (State->WallsAfterSecond - State->WallsAfterFirst != OwLineLength)
		{
			Fail(FString::Printf(TEXT("ow_release_again: %d wall plans after the second line (after Tab and back)"), State->WallsAfterSecond - State->WallsAfterFirst));
		}
		if (State->FailedCommands > 0)
		{
			Fail(FString::Printf(TEXT("orders_world: %d commands failed, first: %s"), State->FailedCommands, *State->FirstFailure));
		}
		Notes.Add(FString::Printf(TEXT("orders_world: Tab key events %s"), State->bKeysRouted ? TEXT("reached the pawns") : TEXT("did NOT reach the pawns offscreen (toggled by hand)")));
		for (const FString& Line : State->Log)
		{
			Notes.Add(TEXT("orders_world: ") + Line);
		}
	};
	Phases.Add(MoveTemp(ReleaseAgain));
}
