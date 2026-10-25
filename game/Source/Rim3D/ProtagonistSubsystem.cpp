#include "ProtagonistSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include <imgui.h>

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"

DEFINE_LOG_CATEGORY_STATIC(LogProtagonist, Log, All);

namespace
{
	FAutoConsoleCommandWithWorld GProtagonistEnterCommand(
		TEXT("Protagonist.Enter"), TEXT("Switches to direct control of the protagonist (or the colonist selected in the overseer view)."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UProtagonistSubsystem* Protagonist = World ? World->GetSubsystem<UProtagonistSubsystem>() : nullptr)
			{
				Protagonist->EnterDirectMode();
			}
		}));

	FAutoConsoleCommandWithWorld GProtagonistExitCommand(
		TEXT("Protagonist.Exit"), TEXT("Returns from direct control to the overseer view."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (UProtagonistSubsystem* Protagonist = World ? World->GetSubsystem<UProtagonistSubsystem>() : nullptr)
			{
				Protagonist->ExitDirectMode();
			}
		}));

	void ProtagonistText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	bool ProtagonistSpeedButton(const char* Label, int32 Speed, USimSubsystem& Bridge)
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

	bool ProtagonistIsColonistOnMap(const USimSubsystem& Bridge, colony::Entity Pawn)
	{
		const colony::ThingView* View = Pawn != colony::kNoEntity && Bridge.GetSim() ? Bridge.GetCurrentSnapshot().find(Pawn) : nullptr;
		return View != nullptr && !View->downed && !View->dead && Bridge.GetSim()->colonistOf(Pawn) != nullptr;
	}
}

bool UProtagonistSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UProtagonistSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UProtagonistSubsystem, STATGROUP_Tickables);
}

void UProtagonistSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		BeforeStepHandle = Bridge->OnBeforeStep.AddUObject(this, &UProtagonistSubsystem::OnBeforeStep);
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UProtagonistSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<UOverseerSubsystem>();
	Collection.InitializeDependency<USimPawnSubsystem>();
}

void UProtagonistSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnBeforeStep.Remove(BeforeStepHandle);
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Body = nullptr;
	Super::Deinitialize();
}

void UProtagonistSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (FParse::Param(FCommandLine::Get(), TEXT("Direct")))
	{
		EnterDirectMode();
	}
}

AProtagonistPawn* UProtagonistSubsystem::GetBody() const
{
	return Body;
}

bool UProtagonistSubsystem::IsDirectMode() const
{
	return bDirect && IsValid(Body.Get());
}

colony::Entity UProtagonistSubsystem::FindProtagonist() const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return colony::kNoEntity;
	}
	colony::Entity First = colony::kNoEntity;
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Pawn)
		{
			continue;
		}
		if (const colony::Colonist* Colonist = Bridge->GetSim()->colonistOf(View.id))
		{
			if (Colonist->protagonist)
			{
				return View.id;
			}
			if (First == colony::kNoEntity)
			{
				First = View.id;
			}
		}
	}
	return First;
}

FString UProtagonistSubsystem::ColonistLabel(const colony::Sim& Sim, colony::Entity Pawn)
{
	const colony::Colonist* Colonist = Sim.colonistOf(Pawn);
	if (!Colonist)
	{
		return FString();
	}
	return UTF8_TO_TCHAR(!Colonist->nickName.empty() ? Colonist->nickName.c_str() : Colonist->firstName.c_str());
}

bool UProtagonistSubsystem::EnterDirectMode(colony::Entity Pawn)
{
	UWorld* World = GetWorld();
	USimSubsystem* Bridge = BridgeSource.Get();
	APlayerController* PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
	if (!Bridge || !Bridge->GetSim() || !PC)
	{
		UE_LOG(LogProtagonist, Warning, TEXT("Direct mode needs a running sim and a player controller"));
		return false;
	}
	UOverseerSubsystem* Overseer = World->GetSubsystem<UOverseerSubsystem>();
	if (Pawn == colony::kNoEntity && Overseer && Overseer->GetSelection().Kind == ESimPickKind::Thing &&
		ProtagonistIsColonistOnMap(*Bridge, Overseer->GetSelection().Thing))
	{
		Pawn = Overseer->GetSelection().Thing;
	}
	if (Pawn == colony::kNoEntity)
	{
		Pawn = FindProtagonist();
	}
	if (!ProtagonistIsColonistOnMap(*Bridge, Pawn))
	{
		UE_LOG(LogProtagonist, Warning, TEXT("No colonist to control (start with -SimScenario=Crashlanded)"));
		return false;
	}

	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	const colony::ThingView* View = Snapshot.find(Pawn);
	const colony::DefDatabase* Defs = Bridge->GetDefs().get();
	const double MetresPerStep = Defs && View->def.valid() ? Defs->get(View->def).number("stats.moveSpeed", 0.0) : 0.0;
	const float Cruise = static_cast<float>(MetresPerStep * colony::kStepsPerRealSecondAt1x);

	// The view looks the way the colonist faces, or the way the overseer camera looked.
	float Yaw = 0.0f;
	if (const USimPawnSubsystem* Pawns = World->GetSubsystem<USimPawnSubsystem>(); Pawns && Pawns->FindPawn(Pawn))
	{
		Yaw = Pawns->FindPawn(Pawn)->GetActorRotation().Yaw;
	}
	if (Overseer && Overseer->GetOverseerPawn() && Overseer->IsOverseerActive())
	{
		Yaw = Overseer->GetOverseerPawn()->GetYaw();
	}

	if (!IsValid(Body.Get()))
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Body = World->SpawnActor<AProtagonistPawn>(AProtagonistPawn::StaticClass(), FTransform::Identity, Params);
		if (!Body)
		{
			return false;
		}
	}
	if (Controlled != colony::kNoEntity && Controlled != Pawn)
	{
		if (const USimPawnSubsystem* Pawns = World->GetSubsystem<USimPawnSubsystem>(); Pawns && Pawns->FindPawn(Controlled))
		{
			Pawns->FindPawn(Controlled)->SetBodyHidden(false);
		}
	}
	Body->Begin(Pawn, FVector2D(View->x, View->y), Cruise, Yaw);
	Controlled = Pawn;
	LastSim = Bridge->GetSim();
	if (!bDirect)
	{
		SpeedBeforeDirect = Bridge->GetSpeed();
	}
	bDirect = true;
	Bridge->SetMaxSpeed(1);  // the body moves in real time
	Bridge->Submit(colony::ControlPawnCommand{Pawn});

	if (Overseer)
	{
		Overseer->LeaveOverseerMode();
	}
	PC->Possess(Body);
	PC->SetViewTarget(Body);
	PC->bShowMouseCursor = false;
	PC->SetInputMode(FInputModeGameOnly());
	Body->SnapCamera();
	ApplyBodyVisibility();
	UE_LOG(LogProtagonist, Log, TEXT("Direct control of %s (#%u)"), *ColonistLabel(*Bridge->GetSim(), Pawn), static_cast<uint32>(entt::to_integral(Pawn)));
	return true;
}

void UProtagonistSubsystem::ExitDirectMode()
{
	if (!bDirect)
	{
		return;
	}
	UWorld* World = GetWorld();
	USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Entity Released = Controlled;
	bDirect = false;
	Controlled = colony::kNoEntity;
	if (!Bridge || !World)
	{
		return;
	}
	if (const USimPawnSubsystem* Pawns = World->GetSubsystem<USimPawnSubsystem>(); Pawns && Pawns->FindPawn(Released))
	{
		Pawns->FindPawn(Released)->SetBodyHidden(false);
	}
	if (Bridge->GetSim())
	{
		Bridge->Submit(colony::ControlPawnCommand{colony::kNoEntity});
	}
	Bridge->SetMaxSpeed(3);

	if (UOverseerSubsystem* Overseer = World->GetSubsystem<UOverseerSubsystem>())
	{
		Overseer->EnterOverseerMode();
		FVector Location;
		if (Bridge->GetBlendedLocation(Released, Location) && Overseer->GetOverseerPawn())
		{
			Overseer->GetOverseerPawn()->FocusOn(Location);
			Overseer->GetOverseerPawn()->SnapCamera();
			FSimPick Pick;
			Pick.Kind = ESimPickKind::Thing;
			Pick.Thing = Released;
			Pick.Location = Location;
			Overseer->Select(Pick);
		}
	}
	UE_LOG(LogProtagonist, Log, TEXT("Back to the overseer view; #%u is on its own again"), static_cast<uint32>(entt::to_integral(Released)));
}

void UProtagonistSubsystem::ToggleMode()
{
	if (bDirect)
	{
		ExitDirectMode();
	}
	else
	{
		EnterDirectMode();
	}
}

bool UProtagonistSubsystem::TakeOver(colony::Entity Pawn)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!bDirect || !Bridge || Pawn == Controlled || !ProtagonistIsColonistOnMap(*Bridge, Pawn))
	{
		return false;
	}
	return EnterDirectMode(Pawn);
}

colony::Entity UProtagonistSubsystem::GetLookedAtColonist(float MaxDistanceCm) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!bDirect || !IsValid(Body.Get()) || !Bridge || !Bridge->GetSim())
	{
		return colony::kNoEntity;
	}
	const FVector Origin = Body->GetViewLocation();
	const FVector Reach = Body->GetViewDirection() * MaxDistanceCm;  // the ray as a vector, as LineBoxIntersection wants it
	colony::Entity Best = colony::kNoEntity;
	double BestDistance = TNumericLimits<double>::Max();
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		FBox Box;
		if (View.category != colony::ThingCategory::Pawn || View.id == Controlled || !Bridge->GetSim()->colonistOf(View.id) ||
			!SimPicking::ThingBox(*Bridge, View, Box))
		{
			continue;
		}
		const double Distance = FVector::Dist(Origin, Box.GetCenter());
		if (Distance < BestDistance && FMath::LineBoxIntersection(Box, Origin, Origin + Reach, Reach))
		{
			Best = View.id;
			BestDistance = Distance;
		}
	}
	return Best;
}

bool UProtagonistSubsystem::TakeOverLookedAt()
{
	const colony::Entity Looked = GetLookedAtColonist();
	return Looked != colony::kNoEntity && TakeOver(Looked);
}

void UProtagonistSubsystem::OnBeforeStep()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!bDirect || !IsValid(Body.Get()) || !Bridge || !Bridge->GetSim())
	{
		return;
	}
	const FVector2D& Position = Body->GetSimPosition();
	Bridge->Submit(colony::DirectMoveCommand{Controlled, static_cast<float>(Position.X), static_cast<float>(Position.Y)});
	++MovesSent;
}

void UProtagonistSubsystem::OnSimEvent(const colony::Event& Event)
{
	// Downed or dead: the sim lets the colonist go in the same step, so the player goes back to the
	// overseer view (next frame, not in the middle of the sim's step).
	if (bDirect && Event.thing == Controlled && (Event.type == colony::EventType::PawnDowned || Event.type == colony::EventType::PawnDied))
	{
		bExitRequested = true;
	}
	// The controlled colonist stops existing (a later task's death): back to the overseer view.
	if (bDirect && Event.type == colony::EventType::ThingDestroyed && Event.thing == Controlled)
	{
		UE_LOG(LogProtagonist, Log, TEXT("The controlled colonist is gone"));
		Controlled = colony::kNoEntity;
		RequestedControl = colony::kNoEntity;
		bDirect = false;
		if (USimSubsystem* Bridge = BridgeSource.Get())
		{
			Bridge->SetMaxSpeed(3);
		}
		if (UOverseerSubsystem* Overseer = GetWorld()->GetSubsystem<UOverseerSubsystem>())
		{
			Overseer->EnterOverseerMode();
		}
	}
}

void UProtagonistSubsystem::ApplyBodyVisibility()
{
	if (!bDirect || !IsValid(Body.Get()))
	{
		return;
	}
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(Controlled) : nullptr;
	if (Actor && Actor->IsBodyHidden() != Body->IsFirstPerson())
	{
		Actor->SetBodyHidden(Body->IsFirstPerson());
	}
}

void UProtagonistSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}

	if (bExitRequested)
	{
		bExitRequested = false;
		UE_LOG(LogProtagonist, Log, TEXT("The controlled colonist went down"));
		ExitDirectMode();
	}
	if (RequestedControl != colony::kNoEntity)
	{
		const colony::Entity Pawn = RequestedControl;
		RequestedControl = colony::kNoEntity;
		EnterDirectMode(Pawn);
	}

	// A loaded game replaces the sim: keep the mode the save was made in.
	if (Bridge->GetSim() != LastSim)
	{
		LastSim = Bridge->GetSim();
		const colony::Entity Saved = Bridge->GetSim()->controlledPawn();
		if (Saved != colony::kNoEntity)
		{
			EnterDirectMode(Saved);
		}
		else if (bDirect)
		{
			if (ProtagonistIsColonistOnMap(*Bridge, Controlled))
			{
				EnterDirectMode(Controlled);
			}
			else
			{
				ExitDirectMode();
			}
		}
	}
	if (!bDirect || !IsValid(Body.Get()))
	{
		return;
	}

	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Controlled);
	if (!View)
	{
		ExitDirectMode();
		return;
	}
	// The sim has the last word: once it has taken control, the local position follows it.
	if (Bridge->GetSim()->controlledPawn() == Controlled && Body->Reconcile(FVector2D(View->x, View->y)))
	{
		++ReconcileCount;
	}
	ApplyBodyVisibility();
	DrawHud(*Bridge);
}

void UProtagonistSubsystem::DrawHud(USimSubsystem& Bridge)
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
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	const float Margin = 12.0f * Scale;
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

	// The time, and pause or 1x (a faster game would outrun the body).
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Margin), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("##DirectTime", nullptr, PanelFlags))
	{
		const colony::GameTime& Time = Sim.time();
		ProtagonistText(FString::Printf(TEXT("Day %lld   %02d:%02d"), static_cast<long long>(Time.day() + 1), Time.hourOfDay(),
			static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute)));
		ImGui::SameLine(0.0f, 24.0f * Scale);
		ProtagonistSpeedButton("Pause", 0, Bridge);
		ImGui::SameLine();
		ProtagonistSpeedButton("1x", 1, Bridge);
	}
	ImGui::End();

	// Who you are, bottom left.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Margin, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("##DirectWho", nullptr, PanelFlags))
	{
		const colony::Colonist* Colonist = Sim.colonistOf(Controlled);
		ProtagonistText(FString::Printf(TEXT("You are %s"), *ColonistLabel(Sim, Controlled)));
		if (Colonist && Colonist->protagonist)
		{
			ImGui::TextUnformatted("the protagonist");
		}
		if (Bridge.GetSpeed() == 0)
		{
			ImGui::TextUnformatted("Paused: press Space to move");
		}
	}
	ImGui::End();

	// Crosshair, and who is under it.
	const ImVec2 Centre(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f);
	ImDrawList* Draw = ImGui::GetForegroundDrawList();
	Draw->AddCircleFilled(Centre, 2.5f * Scale, IM_COL32(255, 255, 255, 220));
	Draw->AddCircle(Centre, 8.0f * Scale, IM_COL32(255, 255, 255, 110), 24, 1.5f);
	const colony::Entity Looked = GetLookedAtColonist();
	if (Looked != colony::kNoEntity)
	{
		ImGui::SetNextWindowPos(ImVec2(Centre.x, Centre.y + 40.0f * Scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##DirectLooked", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
		{
			ProtagonistText(FString::Printf(TEXT("%s   T: take over"), *ColonistLabel(Sim, Looked)));
		}
		ImGui::End();
	}

	// Keys, bottom right.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (ImGui::Begin("##DirectHints", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
	{
		ImGui::TextUnformatted("WASD walk   Ctrl slow   mouse look");
		ImGui::TextUnformatted("V first / third person   wheel zoom");
		ImGui::TextUnformatted("T take over   Tab overseer view");
	}
	ImGui::End();

	++HudFramesDrawn;
}
