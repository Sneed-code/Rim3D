#include "ProtagonistSubsystem.h"
#include "KeyBindings.h"

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
#include "SkySubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

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
	Body->Begin(Pawn, FVector2D(View->x, View->y), Cruise, Yaw, View->level);
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
	WakeUp();  // a sleeper gets up before it is let go
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

bool UProtagonistSubsystem::IsAsleep() const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	return bDirect && Bridge && Bridge->GetSim() && Bridge->GetSim()->jobs().sleepingByHand(Controlled);
}

void UProtagonistSubsystem::SleepHere()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (bDirect && Bridge && Bridge->GetSim() && !IsAsleep())
	{
		Bridge->Submit(colony::SleepCommand{Controlled, colony::kNoEntity, false});
	}
}

void UProtagonistSubsystem::WakeUp()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (Bridge && IsAsleep())
	{
		Bridge->Submit(colony::SleepCommand{Controlled, colony::kNoEntity, true});
	}
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
	// Walked into stairs or a ladder: the sim climbs them (U17). Its DirectMove after this is ignored until the climb is over.
	FVector2D ClimbDirection;
	if (Body->TakeClimbRequest(ClimbDirection))
	{
		Bridge->Submit(colony::ClimbCommand{Controlled, static_cast<float>(ClimbDirection.X), static_cast<float>(ClimbDirection.Y)});
		++ClimbsSent;
	}
	if (Bridge->GetSim()->jobs().sleepingByHand(Controlled))
	{
		return;  // lying still till morning: the sim ignores moves meanwhile
	}
	if (UnstuckSkipMoves > 0)
	{
		--UnstuckSkipMoves;
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
	// Hidden when the view is at its eyes: first person, or aimed in with the first person aim view (U21).
	if (Actor && Actor->IsBodyHidden() != Body->IsBodyHiddenInView())
	{
		Actor->SetBodyHidden(Body->IsBodyHiddenInView());
	}
}

void UProtagonistSubsystem::StartUnstuck()
{
	if (Controlled != colony::kNoEntity && UnstuckSecondsLeft <= 0.0f)
	{
		UnstuckSecondsLeft = 5.0f;
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

	if (UnstuckSecondsLeft > 0.0f)
	{
		UnstuckSecondsLeft -= DeltaTime;
		if (UnstuckSecondsLeft <= 0.0f)
		{
			UnstuckSecondsLeft = 0.0f;
			if (Controlled != colony::kNoEntity)
			{
				Bridge->Submit(colony::UnstuckCommand{Controlled});
				UnstuckSkipMoves = 3;  // no DirectMove from the old spot until the body has been pulled to the new one
				UE_LOG(LogProtagonist, Log, TEXT("Unstuck: moved to the nearest clear spot"));
			}
		}
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
	// The sim has the last word: once it has taken control, the local position follows it, up and down stairs too.
	if (Bridge->GetSim()->controlledPawn() == Controlled)
	{
		Body->FollowSim(FVector2D(View->x, View->y), View->level, View->climb > 0.0f);
		if (!Body->IsClimbing() && (View->motionClip.valid() || View->motionPush) && !View->motionFreeFeet && View->rollPhase == 0)
		{
			// A stagger or a knockdown carries the colonist: the keys wait and the body is where the sim's clip takes it,
			// so nothing snaps back a metre when it ends. (The player's own swings leave the feet to the keys.)
			Body->SetSimPosition(FVector2D(View->x, View->y));
		}
		else if (!Body->IsClimbing() && Body->Reconcile(FVector2D(View->x, View->y)))
		{
			++ReconcileCount;
		}
	}
	// Sleep (docs/design/needs.md "The protagonist sleeps the night away"): N lies down where you stand, F on a bed is
	// the hands-on layer's; the movement keys, N or Esc get up again.
	const bool bAsleep = IsAsleep();
	if (APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0))
	{
		if (bAsleep)
		{
			for (const FKey& Key : {Rim3DKeys::Get(ERim3DAction::MoveForward), Rim3DKeys::Get(ERim3DAction::MoveBack), Rim3DKeys::Get(ERim3DAction::MoveLeft),
					 Rim3DKeys::Get(ERim3DAction::MoveRight), Rim3DKeys::Get(ERim3DAction::SleepHere), FKey(EKeys::Escape)})
			{
				if (PC->WasInputKeyJustPressed(Key))
				{
					WakeUp();
					break;
				}
			}
		}
		else if (Rim3DKeys::WasPressed(PC, ERim3DAction::SleepHere))
		{
			SleepHere();
		}
		if (!bAsleep)
		{
			UpdateFocus(*PC, *Bridge);
		}
	}
	SleepFade = FMath::Clamp(SleepFade + (bAsleep ? DeltaTime : -DeltaTime) * 1.5f, 0.0f, 1.0f);
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
	float TimeBottom = Viewport->Pos.y + Margin + 60.0f * Scale;
	if (ImGui::Begin("##DirectTime", nullptr, PanelFlags))
	{
		const colony::GameTime& Time = Sim.time();
		ProtagonistText(FString::Printf(TEXT("Day %lld   %02d:%02d"), static_cast<long long>(Time.day() + 1), Time.hourOfDay(),
			static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute)));
		ImGui::SameLine(0.0f, 24.0f * Scale);
		ProtagonistSpeedButton("Pause", 0, Bridge);
		ImGui::SameLine();
		ProtagonistSpeedButton("1x", 1, Bridge);
		// The date, the temperature and the weather (U13).
		ProtagonistText(USkySubsystem::DescribeDate(Sim) + TEXT("   ") + USkySubsystem::DescribeClimate(Sim));
		TimeBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
	}
	ImGui::End();

	// The enemy you are locked on to: its name and the health of its worst body part, under the clock.
	DrawFocus(Bridge, TimeBottom + 6.0f * Scale, Scale);

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

	// Asleep: the screen goes dark while the night runs on, with the clock.
	if (SleepFade > 0.0f)
	{
		ImDrawList* Dark = ImGui::GetBackgroundDrawList();
		Dark->AddRectFilled(Viewport->Pos, ImVec2(Viewport->Pos.x + Viewport->Size.x, Viewport->Pos.y + Viewport->Size.y),
			IM_COL32(0, 0, 0, static_cast<int>(235.0f * SleepFade)));
		if (IsAsleep())
		{
			const colony::GameTime& Time = Sim.time();
			ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			ImGui::SetNextWindowBgAlpha(0.0f);
			if (ImGui::Begin("##DirectSleep", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
			{
				ProtagonistText(FString::Printf(TEXT("Sleeping...   %02d:%02d"), Time.hourOfDay(),
					static_cast<int32>((Time.seconds % colony::kSecondsPerHour) / colony::kSecondsPerMinute)));
				ImGui::TextUnformatted("WASD, N or Esc: get up");
			}
			ImGui::End();
			++HudFramesDrawn;
			return;  // no crosshair or hints over the dark
		}
	}

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
	if (!USimSubsystem::IsKeyHelpShown())
	{
		if (ImGui::Begin("##DirectHints", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
		{
			ImGui::TextUnformatted("F1  keys");
		}
	}
	else if (ImGui::Begin("##DirectHints", nullptr, PanelFlags | ImGuiWindowFlags_NoInputs))
	{
		// The keys as they are bound now (Settings > Controls changes them, KeyBindings.h).
		const auto KeyOf = [](ERim3DAction Action) { return Rim3DKeys::GetKeyLabel(Action); };
		const auto Line = [](const FString& Text) { ImGui::TextUnformatted(TCHAR_TO_UTF8(*Text)); };
		Line(FString::Printf(TEXT("F1 hide   F3 speed   WASD walk   %s slow   %s sprint on/off   mouse look"), *KeyOf(ERim3DAction::WalkSlow), *KeyOf(ERim3DAction::Sprint)));
		Line(FString::Printf(TEXT("%s jump   %s roll / dash   %s first / third person   wheel zoom"), *KeyOf(ERim3DAction::Jump), *KeyOf(ERim3DAction::RollDash), *KeyOf(ERim3DAction::ToggleView)));
		Line(FString::Printf(TEXT("%s hold: work / pick up / take weapon   %s put down   %s build"), *KeyOf(ERim3DAction::Interact), *KeyOf(ERim3DAction::PutDown), *KeyOf(ERim3DAction::BuildMode)));
		Line(FString::Printf(TEXT("%s take over   %s overseer view   resources are picked up as you walk by"), *KeyOf(ERim3DAction::TakeOver), *KeyOf(ERim3DAction::SwitchMode)));
		Line(FString::Printf(TEXT("%s sleep here (18:00 to 6:00)   %s on a bed: sleep   F5 quicksave"), *KeyOf(ERim3DAction::SleepHere), *KeyOf(ERim3DAction::Interact)));
		if (Rim3DKeys::IsLockOnFocusEnabled())
		{
			Line(FString::Printf(TEXT("%s lock on to the next enemy   %s let go"), *KeyOf(ERim3DAction::FocusNextTarget), *KeyOf(ERim3DAction::EndFocus)));
		}
	}
	ImGui::End();

	++HudFramesDrawn;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lock-on focus (docs/design/focus.md, sneed 2026-11-17): Tab locks on to the next nearest enemy, Dark Souls and WoW
// style; middle mouse locks on or lets go (sneed 2026-11-18); Settings > Controls turns it off. The sim keeps the focus (FocusCommand) and turns the body to face
// it; here are the keys, the HP bar top middle, the marker on the target and (AProtagonistPawn) the camera.

colony::Entity UProtagonistSubsystem::GetFocusTarget() const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!bDirect || !Bridge || !Bridge->GetSim() || Controlled == colony::kNoEntity)
	{
		return colony::kNoEntity;
	}
	const colony::ThingView* Me = Bridge->GetCurrentSnapshot().find(Controlled);
	return Me ? Me->focusTarget : colony::kNoEntity;
}

void UProtagonistSubsystem::SubmitFocus(USimSubsystem& Bridge, colony::FocusMode Mode)
{
	colony::FocusCommand Command;
	Command.pawn = Controlled;
	Command.mode = Mode;
	Bridge.Submit(Command);
	++FocusCommandsSent;
}

void UProtagonistSubsystem::FocusNext()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (bDirect && Bridge && Bridge->GetSim() && Controlled != colony::kNoEntity && Rim3DKeys::IsLockOnFocusEnabled())
	{
		SubmitFocus(*Bridge, colony::FocusMode::Next);
	}
}

void UProtagonistSubsystem::EndFocus()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (Bridge && GetFocusTarget() != colony::kNoEntity)
	{
		SubmitFocus(*Bridge, colony::FocusMode::Clear);
	}
}

void UProtagonistSubsystem::UpdateFocus(APlayerController& PC, USimSubsystem& Bridge)
{
	const colony::Entity Focus = GetFocusTarget();
	if (!Rim3DKeys::IsLockOnFocusEnabled())
	{
		// Turned off while locked on: let go once.
		if (Focus != colony::kNoEntity && FocusClearedFor != Focus)
		{
			SubmitFocus(Bridge, colony::FocusMode::Clear);
			FocusClearedFor = Focus;
		}
		return;
	}
	FocusClearedFor = colony::kNoEntity;
	if (Rim3DKeys::IsCapturing())
	{
		return;  // the Controls page is waiting for a new key
	}
	if (Rim3DKeys::WasPressed(&PC, ERim3DAction::FocusNextTarget))
	{
		FocusNext();
	}
	else if (Rim3DKeys::WasPressed(&PC, ERim3DAction::EndFocus))
	{
		// Middle mouse (sneed 2026-11-18, like Elden Ring's lock-on button): locks on when free, lets go when locked on.
		if (Focus != colony::kNoEntity)
		{
			EndFocus();
		}
		else
		{
			FocusNext();
		}
	}
}

bool UProtagonistSubsystem::GetFocusPoint(FVector& OutPoint) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Entity Focus = GetFocusTarget();
	const colony::ThingView* Target = Bridge && Focus != colony::kNoEntity ? Bridge->GetCurrentSnapshot().find(Focus) : nullptr;
	if (!Target)
	{
		return false;
	}
	// The middle of the body, where the swing goes: the chest standing, low over a body on the ground.
	const bool bLow = Target->downed || Target->stance == colony::Stance::Prone;
	const float HeightM = FMath::Max(0.3f, Bridge->GetSim()->bodyHeightM(Focus));
	OutPoint = Bridge->SimToWorld(Target->x, Target->y, Target->level) + FVector(0.0, 0.0, bLow ? 25.0 : HeightM * 100.0 * 0.62);
	return true;
}

void UProtagonistSubsystem::DrawFocus(USimSubsystem& Bridge, float Top, float Scale)
{
	const colony::Entity Focus = GetFocusTarget();
	const colony::ThingView* Target = Focus != colony::kNoEntity ? Bridge.GetCurrentSnapshot().find(Focus) : nullptr;
	if (!Target)
	{
		return;
	}
	const colony::Sim& Sim = *Bridge.GetSim();
	// Its name: a colonist's or raider's, a named animal's, a spawned pawn's, else its kind (as the kill feed names it).
	const FString Name = UTF8_TO_TCHAR(Sim.feedName(Focus).c_str());
	const float Health = FMath::Clamp(Target->dead ? 0.0f : Target->lowestPart, 0.0f, 1.0f);
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();

	// Top middle: the name, and a bar for its worst body part (sneed: "lowest hp body part as %").
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Top), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	if (ImGui::Begin("##FocusTarget", nullptr, Flags))
	{
		const float Width = 300.0f * Scale;
		const FTCHARToUTF8 NameUtf8(*Name);
		const float NameWidth = ImGui::CalcTextSize(NameUtf8.Get()).x;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + FMath::Max(0.0f, (Width - NameWidth) * 0.5f));
		ImGui::TextUnformatted(NameUtf8.Get());
		const FString Percent = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Health * 100.0f));
		const FTCHARToUTF8 PercentUtf8(*Percent);
		// Red, going darker as it empties.
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.55f + 0.3f * Health, 0.08f, 0.06f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.02f, 0.02f, 0.85f));
		ImGui::ProgressBar(Health, ImVec2(Width, 16.0f * Scale), PercentUtf8.Get());
		ImGui::PopStyleColor(2);
	}
	ImGui::End();
	++FocusBarFramesDrawn;

	// A small marker on the body you are locked on to.
	FVector Point;
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	FVector2D Screen;
	if (PC && GetFocusPoint(Point) && PC->ProjectWorldLocationToScreen(Point, Screen, true))
	{
		ImDrawList* Draw = ImGui::GetForegroundDrawList();
		const ImVec2 C(Viewport->Pos.x + static_cast<float>(Screen.X), Viewport->Pos.y + static_cast<float>(Screen.Y));
		const float R = 7.0f * Scale;
		Draw->AddCircleFilled(C, R + 2.0f * Scale, IM_COL32(0, 0, 0, 120), 20);
		Draw->AddCircleFilled(C, R, IM_COL32(255, 235, 200, 230), 20);
		Draw->AddCircleFilled(C, R * 0.45f, IM_COL32(200, 40, 30, 255), 12);
	}
}
