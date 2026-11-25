#include "OverseerSubsystem.h"
#include "KeyBindings.h"

#include "Camera/CameraComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "ColonyHudSubsystem.h"
#include "HudLayout.h"

#include "BuildingSubsystem.h"
#include "LevelsSubsystem.h"
#include "OrdersSubsystem.h"
#include "OverseerPawn.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Map.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Sim.h"

#include <cmath>

DEFINE_LOG_CATEGORY_STATIC(LogOverseer, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GOverseerSeeThrough(
		TEXT("Overseer.SeeThrough"), 1, TEXT("1 outlines people and buildings hidden behind trees, roofs and walls in the overseer view (SeeThroughArt)."), ECVF_Default);
	TAutoConsoleVariable<float> GOverseerBrighter(
		TEXT("Overseer.BrighterEV"), 0.8f, TEXT("Stops the overseer view is brighter than the third-person view (the ground under trees read as black)."), ECVF_Default);
	const TCHAR* const OverseerSeeThroughPath = TEXT("/Game/Rim3D/Generated/Overseer/M_SeeThrough.M_SeeThrough");

	/** Marks every primitive of an actor (and what hangs off it) for the see-through outline with a stencil value. */
	void OverseerMarkSeeThrough(AActor* Actor, int32 Stencil)
	{
		if (!Actor)
		{
			return;
		}
		TArray<UPrimitiveComponent*> Parts;
		Actor->GetComponents<UPrimitiveComponent>(Parts, true);
		for (UPrimitiveComponent* Part : Parts)
		{
			if (Part && Part->IsVisible())
			{
				Part->SetRenderCustomDepth(true);
				Part->SetCustomDepthStencilValue(Stencil);
			}
		}
	}

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
			case colony::Terrain::ShallowWater: return TEXT("Shallow water");
			case colony::Terrain::Marsh: return TEXT("Marsh");
			case colony::Terrain::Mud: return TEXT("Mud");
			case colony::Terrain::Air: return TEXT("Air");
			case colony::Terrain::UpperFloor: return TEXT("Upper floor");
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
		// A colonist by name ("Theo 'Rusty' Ito"); anything else by its label, capitalised ("Human", "Wood log").
		if (const colony::Sim* Sim = Bridge.GetSim())
		{
			if (const colony::Colonist* Colonist = Sim->colonistOf(View.id))
			{
				std::string Name = Colonist->firstName;
				if (!Colonist->nickName.empty())
				{
					Name += " '" + Colonist->nickName + "'";
				}
				if (!Colonist->lastName.empty())
				{
					Name += " " + Colonist->lastName;
				}
				if (!Name.empty())
				{
					return Name;
				}
			}
		}
		std::string Label = "thing";
		if (const colony::DefDatabase* Defs = Bridge.GetDefs().get())
		{
			Label = Defs->get(View.def).label();
			// A building made of something says what (sneed 2026-11-25): "wooden wall", "sandstone wall", as RimWorld's
			// stuffAdjective (blocks drop the " blocks").
			if (View.category == colony::ThingCategory::Building && View.stuff.valid())
			{
				const colony::Def& Stuff = Defs->get(View.stuff);
				std::string Adjective = Stuff.text("stuffProps.stuffAdjective", Stuff.label());
				const std::string Blocks = " blocks";
				if (Adjective.size() > Blocks.size() && Adjective.compare(Adjective.size() - Blocks.size(), Blocks.size(), Blocks) == 0)
				{
					Adjective.resize(Adjective.size() - Blocks.size());
				}
				if (!Adjective.empty())
				{
					Label = Adjective + " " + Label;
				}
			}
		}
		if (!Label.empty() && Label[0] >= 'a' && Label[0] <= 'z')
		{
			Label[0] = static_cast<char>(Label[0] - 'a' + 'A');
		}
		return Label;
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
	Extra.Reset();
}

void UOverseerSubsystem::SelectThings(const TArray<colony::Entity>& Things)
{
	Extra.Reset();
	Selection = FSimPick();
	for (const colony::Entity Thing : Things)
	{
		if (Thing == colony::kNoEntity || IsThingSelected(Thing))
		{
			continue;
		}
		if (!Selection.IsValid())
		{
			Selection.Kind = ESimPickKind::Thing;
			Selection.Thing = Thing;
		}
		else
		{
			Extra.Add(Thing);
		}
	}
}

void UOverseerSubsystem::ToggleThingSelected(colony::Entity Thing)
{
	TArray<colony::Entity> Things;
	GetSelectedThings(Things);
	if (Things.Contains(Thing))
	{
		Things.Remove(Thing);
	}
	else
	{
		Things.Add(Thing);
	}
	SelectThings(Things);
}

void UOverseerSubsystem::GetSelectedThings(TArray<colony::Entity>& Out) const
{
	Out.Reset();
	if (Selection.Kind == ESimPickKind::Thing)
	{
		Out.Add(Selection.Thing);
		Out.Append(Extra);
	}
}

bool UOverseerSubsystem::IsThingSelected(colony::Entity Thing) const
{
	return Selection.Kind == ESimPickKind::Thing && (Selection.Thing == Thing || Extra.Contains(Thing));
}

void UOverseerSubsystem::ClearSelection()
{
	Selection = FSimPick();
	Extra.Reset();
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

bool UOverseerSubsystem::FocusOnColonists(bool bSelect)
{
	const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !IsValid(Pawn.Get()))
	{
		return false;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	colony::Entity Found = colony::kNoEntity;
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		const colony::Colonist* Colonist = View.category == colony::ThingCategory::Pawn && !View.dead ? Sim.colonistOf(View.id) : nullptr;
		if (!Colonist || View.faction != 1)
		{
			continue;
		}
		if (Found == colony::kNoEntity || Colonist->protagonist)
		{
			Found = View.id;
		}
		if (Colonist->protagonist)
		{
			break;
		}
	}
	FVector Location;
	if (Found == colony::kNoEntity || !Bridge->GetBlendedLocation(Found, Location))
	{
		return false;
	}
	Pawn->FocusOn(Location);
	Pawn->SnapCamera();
	if (bSelect)
	{
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = Found;
		Select(Pick);
	}
	UE_LOG(LogOverseer, Log, TEXT("Camera on the colonists: #%u"), OverseerIdNumber(Found));
	return true;
}

void UOverseerSubsystem::FindThingsInBox(const FVector2D& A, const FVector2D& B, TArray<colony::Entity>& Out) const
{
	Out.Reset();
	const USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	const APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!Bridge || !Bridge->GetSim() || !PC)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const FVector2D Min(FMath::Min(A.X, B.X), FMath::Min(A.Y, B.Y));
	const FVector2D Max(FMath::Max(A.X, B.X), FMath::Max(A.Y, B.Y));
	const int32 TopLevel = ULevelsSubsystem::TopShownLevel(GetWorld());
	// Sorted by how near the box's start they are on screen, so the main selection is the one the drag began on.
	struct FBoxHit
	{
		colony::Entity Id;
		int32 Group;  // 0 your colonists, 1 other people, 2 animals, 3 anything else
		colony::DefId Def;
		double Distance;
	};
	TArray<FBoxHit> Hits;
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.level > TopLevel)
		{
			continue;
		}
		FVector Location;
		if (!Bridge->GetBlendedLocation(View.id, Location))
		{
			continue;
		}
		if (View.category == colony::ThingCategory::Pawn)
		{
			Location.Z += 80.0;  // the body's middle, not its feet
		}
		FVector2D Screen;
		if (!PC->ProjectWorldLocationToScreen(Location, Screen, true) || Screen.X < Min.X || Screen.X > Max.X || Screen.Y < Min.Y || Screen.Y > Max.Y)
		{
			continue;
		}
		int32 Group = 3;
		if (View.category == colony::ThingCategory::Pawn)
		{
			const bool bPerson = Sim.looksOf(View.id) != nullptr || Sim.colonistOf(View.id) != nullptr;
			Group = bPerson ? (Sim.colonistOf(View.id) && View.faction == 1 && !View.dead ? 0 : 1) : 2;
			if (View.dead)
			{
				Group = 3;  // bodies are things
			}
		}
		Hits.Add({View.id, Group, View.def, FVector2D::DistSquared(Screen, A)});
	}
	if (Hits.IsEmpty())
	{
		return;
	}
	Hits.Sort([](const FBoxHit& L, const FBoxHit& R) { return L.Distance < R.Distance; });
	int32 Best = 3;
	for (const FBoxHit& Hit : Hits)
	{
		Best = FMath::Min(Best, Hit.Group);
	}
	// People: your colonists if any are in the box, with anyone else of yours (prisoners and guests stay out unless no
	// colonist is in it). Animals: every animal. Else the kind of thing nearest the start (all the logs, all the walls).
	const colony::DefId Kind = Hits[0].Def;
	for (const FBoxHit& Hit : Hits)
	{
		if (Best <= 2 ? Hit.Group == Best : Hit.Def == Kind)
		{
			Out.Add(Hit.Id);
		}
	}
}

void UOverseerSubsystem::BeginSelectPress(const FVector2D& Screen)
{
	bBoxPressed = true;
	bBoxActive = false;
	BoxStart = BoxEnd = Screen;
}

void UOverseerSubsystem::UpdateSelectPress(const FVector2D& Screen)
{
	if (!bBoxPressed)
	{
		return;
	}
	BoxEnd = Screen;
	if (!bBoxActive && FVector2D::Distance(BoxStart, BoxEnd) > 10.0)
	{
		bBoxActive = true;
	}
}

bool UOverseerSubsystem::EndSelectPress(const FVector2D& Screen, bool bAdd)
{
	if (!bBoxPressed)
	{
		return false;
	}
	UpdateSelectPress(Screen);
	bBoxPressed = false;
	if (bBoxActive)
	{
		bBoxActive = false;
		TArray<colony::Entity> Found;
		FindThingsInBox(BoxStart, BoxEnd, Found);
		UE_LOG(LogOverseer, Log, TEXT("Drag box (%.0f, %.0f) to (%.0f, %.0f): %d selected"), BoxStart.X, BoxStart.Y, BoxEnd.X, BoxEnd.Y, Found.Num());
		if (bAdd)
		{
			TArray<colony::Entity> Things;
			GetSelectedThings(Things);
			for (const colony::Entity Thing : Found)
			{
				Things.AddUnique(Thing);
			}
			SelectThings(Things);
		}
		else if (!Found.IsEmpty())
		{
			SelectThings(Found);
		}
		else
		{
			ClearSelection();
		}
		return true;
	}
	const FSimPick Pick = IsValid(Pawn.Get()) ? Pawn->PickAtScreenPosition(Screen) : FSimPick();
	if (bAdd && Pick.Kind == ESimPickKind::Thing && (Selection.Kind == ESimPickKind::Thing || !Selection.IsValid()))
	{
		ToggleThingSelected(Pick.Thing);
	}
	else
	{
		Select(Pick);
	}
	return true;
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
	Extra.RemoveAll([Bridge](colony::Entity Thing) { return !Bridge->GetCurrentSnapshot().find(Thing); });
	if (Selection.Kind == ESimPickKind::Thing && !Bridge->GetCurrentSnapshot().find(Selection.Thing))
	{
		// The main one went: the next selected thing takes its place.
		TArray<colony::Entity> Rest = Extra;
		SelectThings(Rest);
	}
	UpdateSeeThrough(*Bridge);
	DrawSelectionHighlight(*Bridge);
	if (GOverseerShowUi.GetValueOnGameThread() != 0)
	{
		DrawPanels(*Bridge);
	}
}

void UOverseerSubsystem::UpdateSeeThrough(const USimSubsystem& Bridge)
{
	// The material goes on the overseer camera only, so the third-person view never shows it.
	if (!bSeeThroughLoaded)
	{
		bSeeThroughLoaded = true;
		SeeThroughMaterial = LoadObject<UMaterialInterface>(nullptr, OverseerSeeThroughPath);
		if (!SeeThroughMaterial)
		{
			UE_LOG(LogOverseer, Display, TEXT("No %s (art step see-through-art): no outlines through trees"), OverseerSeeThroughPath);
		}
	}
	UCameraComponent* Camera = Pawn ? Pawn->GetCamera() : nullptr;
	if (!Camera)
	{
		return;
	}
	// A touch brighter than the third-person view: from above, the ground under the canopy read as black (sneed 2026-11-25).
	Camera->PostProcessSettings.bOverride_AutoExposureBias = true;
	Camera->PostProcessSettings.AutoExposureBias = GOverseerBrighter.GetValueOnGameThread();
	if (!SeeThroughMaterial)
	{
		return;
	}
	const bool bWanted = GOverseerSeeThrough.GetValueOnGameThread() != 0;
	if (bWanted != bSeeThroughOnCamera)
	{
		TArray<FWeightedBlendable>& Blendables = Camera->PostProcessSettings.WeightedBlendables.Array;
		Blendables.RemoveAll([this](const FWeightedBlendable& Blend) { return Blend.Object.Get() == SeeThroughMaterial.Get(); });
		if (bWanted)
		{
			Blendables.Add(FWeightedBlendable(1.0f, SeeThroughMaterial.Get()));
		}
		bSeeThroughOnCamera = bWanted;
	}
	if (!bWanted)
	{
		return;
	}
	// New pawns, clothes and buildings come and go: mark them all again twice a second (a mark that is already set costs
	// nothing). Stencils: 1 the colony's own, 2 someone fighting, 3 other people and animals, 4 buildings.
	const double Now = FPlatformTime::Seconds();
	if (Now < NextSeeThroughMark)
	{
		return;
	}
	NextSeeThroughMark = Now + 0.5;
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
	{
		for (const TPair<uint32, TObjectPtr<ASimPawnActor>>& Entry : Pawns->GetPawns())
		{
			ASimPawnActor* Actor = Entry.Value.Get();
			if (!IsValid(Actor))
			{
				continue;
			}
			const colony::ThingView* View = Snapshot.find(Actor->GetThing());
			int32 Stencil = 3;
			if (View && View->faction == 1)
			{
				Stencil = 1;
			}
			else if (View && (View->fighting || View->fightTarget != colony::kNoEntity))
			{
				Stencil = 2;
			}
			if (View && View->dead)
			{
				continue;
			}
			OverseerMarkSeeThrough(Actor, Stencil);
			TArray<AActor*> Attached;
			Actor->GetAttachedActors(Attached, true, true);
			for (AActor* Child : Attached)
			{
				OverseerMarkSeeThrough(Child, Stencil);
			}
		}
	}
	if (const UBuildingSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingSubsystem>())
	{
		OverseerMarkSeeThrough(Buildings->GetDrawActor(), 4);
	}
}

void UOverseerSubsystem::DrawSelectionHighlight(const USimSubsystem& Bridge) const
{
	UWorld* World = GetWorld();
	if (Selection.Kind == ESimPickKind::Thing)
	{
		// A flat ring on the ground round each selected thing (sneed 2026-11-24: "a unit circle, no 3 dimensions"), sized to
		// what it is: a person's ring is about a metre across, a building's covers its footprint.
		const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
		TArray<colony::Entity> Things;
		GetSelectedThings(Things);
		const FColor Ring(170, 240, 205);
		for (const colony::Entity Thing : Things)
		{
			const colony::ThingView* View = Snapshot.find(Thing);
			FBox Box;
			if (!View || !SimPicking::ThingBox(Bridge, *View, Box))
			{
				continue;
			}
			FVector Feet;
			if (!Bridge.GetBlendedLocation(View->id, Feet))
			{
				Feet = FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z);
			}
			const FVector Extent = Box.GetExtent();
			float Radius = static_cast<float>(FMath::Max(Extent.X, Extent.Y));
			Radius = View->category == colony::ThingCategory::Pawn ? FMath::Clamp(Radius * 0.85f, 45.0f, 260.0f) : FMath::Max(Radius * 0.95f, 35.0f);
			const FVector Centre(Box.GetCenter().X, Box.GetCenter().Y, Feet.Z + 6.0);
			DrawDebugCircle(World, Centre, Radius, 48, Ring, false, -1.0f, SDPG_World, FMath::Max(2.5f, Radius * 0.045f), FVector(1.0, 0.0, 0.0),
				FVector(0.0, 1.0, 0.0), false);
			// A moving pawn's route ahead, from its feet along the waypoints, flat on the ground.
			if (View->category == colony::ThingCategory::Pawn && View->waypointCount > 0 && View->faction == 1)
			{
				FVector From = Feet + FVector(0.0, 0.0, 8.0);
				for (uint32 Index = 0; Index < View->waypointCount; ++Index)
				{
					const colony::PathPointView& Point = Snapshot.waypoints[View->firstWaypoint + Index];
					const FVector To = Bridge.SimToWorld(Point.x, Point.y, View->level) + FVector(0.0, 0.0, 8.0);
					DrawDebugLine(World, From, To, FColor(120, 220, 255, 200), false, -1.0f, SDPG_World, 2.0f);
					From = To;
				}
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
	ColonyHud::ApplyFrameStyle(Scale);
	const float Margin = 12.0f * Scale;
	if (bBoxActive)
	{
		// The drag box: a faint jade fill with a bright edge, over the world and under the boxes.
		ImDrawList* Draw = ImGui::GetBackgroundDrawList();
		const ImVec2 A(Viewport->Pos.x + static_cast<float>(FMath::Min(BoxStart.X, BoxEnd.X)), Viewport->Pos.y + static_cast<float>(FMath::Min(BoxStart.Y, BoxEnd.Y)));
		const ImVec2 B(Viewport->Pos.x + static_cast<float>(FMath::Max(BoxStart.X, BoxEnd.X)), Viewport->Pos.y + static_cast<float>(FMath::Max(BoxStart.Y, BoxEnd.Y)));
		Draw->AddRectFilled(A, B, ImGui::GetColorU32(ImVec4(0.37f, 0.83f, 0.66f, 0.10f)));
		Draw->AddRect(A, B, ImGui::GetColorU32(ImVec4(0.55f, 0.95f, 0.78f, 0.85f)), 0.0f, 1.5f * Scale, 0);
	}
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

	// The speed buttons, the date and the level looked at are on the bottom bar now (UColonyHudSubsystem).

	// Inspect panel, fully top right (sneed 2026-11-24): what is selected. A selected ground cell's box is left out while a
	// bar window (Build, Work...) is open, so nothing but that window shows over the world.
	const UColonyHudSubsystem* TabsHud = GetWorld()->GetSubsystem<UColonyHudSubsystem>();
	const bool bTabOpen = TabsHud && TabsHud->GetOpenTab() != EHudTab::None;
	if (Selection.IsValid() && !(bTabOpen && Selection.Kind == ESimPickKind::Cell))
	{
		HudLayout::PlaceRight(Viewport, HudLayout::ERightSlot::Inspect, Scale);
		ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f * Scale, 0.0f), ImVec2(560.0f * Scale, FLT_MAX));
		if (ImGui::Begin("##OverseerInspect", nullptr, PanelFlags))
		{
			if (Selection.Kind == ESimPickKind::Thing)
			{
				if (const colony::ThingView* View = Snapshot.find(Selection.Thing))
				{
					// The name large, with the info button ("i", the full info window) and close at the right.
					// The buttons sit centred on the big title's line, inside the box (they used to hang off its top edge).
					ColonyHud::PushLarge();
					const float TitleTop = ImGui::GetCursorPosY();
					OverseerText(OverseerLabel(Bridge, *View));
					const float TitleHeight = ImGui::GetItemRectSize().y;
					ColonyHud::PopLarge();
					ImGui::SameLine(0.0f, 16.0f * Scale);
					ImGui::SetCursorPosY(TitleTop + FMath::Max(0.0f, (TitleHeight - ImGui::GetFontSize()) * 0.5f));
					if (ImGui::SmallButton(" i "))
					{
						if (UColonyHudSubsystem* Hud = GetWorld()->GetSubsystem<UColonyHudSubsystem>())
						{
							Hud->OpenInfo(View->id);
						}
					}
					ColonyHud::Tip(TEXT("Everything about it"));
					ImGui::SameLine();
					ImGui::SetCursorPosY(TitleTop + FMath::Max(0.0f, (TitleHeight - ImGui::GetFontSize()) * 0.5f));
					if (ImGui::SmallButton(" x "))
					{
						ClearSelection();
					}
					ColonyHud::Tip(TEXT("Deselect (Esc)"));
					if (Extra.Num() > 0)
					{
						ColonyHud::TextColored(ColonyHud::Accent(), FString::Printf(TEXT("and %d more selected"), Extra.Num()));
					}
					ImGui::Separator();
					// No kind and number, cell, ground height or footprint lines (sneed 2026-11-25): only what matters to play.
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
								if (ImGui::Button(TCHAR_TO_UTF8(*FString::Printf(TEXT("Take control (%s)##TakeControl"), *Rim3DKeys::GetKeyLabel(ERim3DAction::SwitchMode)))))
								{
									if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
									{
										Protagonist->RequestControl(View->id);
									}
								}
								// Draft, stand down and the hint for move and attack orders (U7).
								if (UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>())
								{
									Orders->DrawDraftControls(*View);
								}
							}
							break;
						}
						case colony::ThingCategory::Item:
							OverseerText(FString::Printf(TEXT("Stack of %d"), View->stackCount));
							break;
						case colony::ThingCategory::Building:
							break;
					}
				}
			}
			else
			{
				const colony::CellPos Cell = Selection.Cell;
				const colony::Map& Map = Sim.map();
				OverseerText(Cell.level > 0 ? FString::Printf(TEXT("Cell %d, %d   %s"), Cell.x, Cell.y, *ULevelsSubsystem::DescribeLevel(Cell.level))
					: FString::Printf(TEXT("Cell %d, %d"), Cell.x, Cell.y));
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
					if (View.category != colony::ThingCategory::Building && View.level == Cell.level && FMath::FloorToInt32(View.x) == Cell.x && FMath::FloorToInt32(View.y) == Cell.y)
					{
						OverseerText(FString::Printf(TEXT("%s: %s"), OverseerCategoryName(View.category), UTF8_TO_TCHAR(OverseerLabel(Bridge, View).c_str())));
					}
				}
			}
			HudLayout::ReportRight(HudLayout::ERightSlot::Inspect);
		}
		ImGui::End();
	}

	// Key hints, bottom right, only while the key list is switched on (F1, or Menu > Key list). No folded "F1 keys" box any
	// more (sneed 2026-11-25).
	if (USimSubsystem::IsKeyHelpShown())
	{
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, ColonyHud::AboveBar(Viewport, Scale) - ColonyHud::DateBoxHeight(Scale)),
			ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##OverseerHints", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
		{
			ImGui::TextUnformatted("F1 hide   F3 speed   WASD pan   Q/E turn   wheel zoom");
			ImGui::TextUnformatted("middle-drag orbit   click select");
			ImGui::TextUnformatted("F centre   Home reset   Esc deselect");
			ImGui::TextUnformatted("Page Up / Page Down level above / below");
			ImGui::TextUnformatted(TCHAR_TO_UTF8(*FString::Printf(TEXT("%s take control of the selected colonist"), *Rim3DKeys::GetKeyLabel(ERim3DAction::SwitchMode))));
			ImGui::TextUnformatted("R draft colonist / turn building   right click order");
		}
		ImGui::End();
	}

	// The shooter feel and sound settings are pages of Settings now (Menu > Settings).

	++PanelFramesDrawn;
}
