// Smoke test `protagonist` (task U9): direct control of a colonist, in the game, offscreen.
#include "SmokeTest.h"

#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Sim.h"

#include <cmath>
#include <variant>

namespace
{
	const FVector ProtSmokeOrigin(300000.0, 0.0, 0.0);

	struct FProtSmokeState
	{
		colony::Entity Protagonist = colony::kNoEntity;
		colony::Entity Other = colony::kNoEntity;
		colony::Entity SavedPawn = colony::kNoEntity;
		FVector2D WalkStart = FVector2D::ZeroVector;
		FVector2D PausedAt = FVector2D::ZeroVector;
		FVector2D SlideStart = FVector2D::ZeroVector;
		int32 WallX = 0;
		int32 WallsPlaced = 0;
		bool bAimed = false;
		bool bTookOther = false;
		bool bBackToProtagonist = false;
		bool bLoaded = false;
		bool bSlideStarted = false;
		int32 ReconcilesAtStart = 0;
		int64 MovesAtStart = 0;
		int64 Frames = 0;
		int64 FullDetailFrames = 0;
		int64 SpeedSamples = 0;
		double SpeedSum = 0.0;  // sim metres per second at 1x while walking
		double MaxActorGapCm = 0.0;
		double MaxLocalGapM = 0.0;
		double MinArmCm = 1.0e9;

		void ResetCounters()
		{
			Frames = FullDetailFrames = SpeedSamples = 0;
			SpeedSum = MaxActorGapCm = MaxLocalGapM = 0.0;
			MinArmCm = 1.0e9;
		}
	};

	FVector2D ProtSmokeSimPosition(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		const colony::ThingView* View = Bridge.GetCurrentSnapshot().find(Thing);
		return View ? FVector2D(View->x, View->y) : FVector2D::ZeroVector;
	}

	int32 ProtSmokeCountControlCommands(const colony::Sim& Sim)
	{
		int32 Count = 0;
		for (const colony::LoggedCommand& Logged : Sim.commandLog())
		{
			Count += std::holds_alternative<colony::ControlPawnCommand>(Logged.command) ? 1 : 0;
		}
		return Count;
	}
}

void USmokeTestSubsystem::BuildProtagonistPhases()
{
	const TSharedRef<FProtSmokeState> State = MakeShared<FProtSmokeState>();

	// A phase: setup, warm-up and measuring seconds, then what runs every frame and what checks the result.
	const auto AddPhase = [this](const TCHAR* Name, TFunction<void()> Setup, float Warm, float Measure, TFunction<void()> EveryFrame,
								  TFunction<void()> Check)
	{
		FPhase Phase;
		Phase.Name = Name;
		Phase.Setup = MoveTemp(Setup);
		Phase.WarmupSeconds = Warm;
		Phase.MeasureSeconds = Measure;
		Phase.OnFrame = MoveTemp(EveryFrame);
		Phase.Verify = MoveTemp(Check);
		Phases.Add(MoveTemp(Phase));
	};

	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Overseer = [this]() { return GetWorld()->GetSubsystem<UOverseerSubsystem>(); };
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const auto PlayerPawn = [this]() -> APawn*
	{
		APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		return PC ? PC->GetPawn() : nullptr;
	};

	// Every frame the controlled colonist is checked against the sim: it stands on walkable
	// ground, its drawn actor is where the sim says, the local position agrees with the sim, and it
	// plays full motion matching.
	const auto CheckFrame = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!B || !B->GetSim() || !P || !P->IsDirectMode() || !Pawns)
		{
			return;
		}
		const colony::Entity Controlled = P->GetControlled();
		const colony::ThingView* View = B->GetCurrentSnapshot().find(Controlled);
		const ASimPawnActor* Actor = Pawns->FindPawn(Controlled);
		if (!View || !Actor || B->GetSim()->controlledPawn() != Controlled)
		{
			return;
		}
		++State->Frames;
		if (!B->GetSim()->map().walkable(colony::CellPos{FMath::FloorToInt32(View->x), FMath::FloorToInt32(View->y), 0}))
		{
			Fail(FString::Printf(TEXT("The controlled colonist stands on ground it can't walk: %.2f, %.2f"), View->x, View->y));
		}
		State->MaxActorGapCm = FMath::Max(State->MaxActorGapCm, FVector::Dist2D(Actor->GetActorLocation(), B->SimToWorld(View->x, View->y, View->level)));
		State->MaxLocalGapM = FMath::Max(State->MaxLocalGapM, static_cast<double>(FVector2D::Distance(P->GetBody()->GetSimPosition(), FVector2D(View->x, View->y))));
		const float Speed = FMath::Sqrt(View->vx * View->vx + View->vy * View->vy) * colony::kStepsPerRealSecondAt1x;
		if (Speed > 0.5f)
		{
			State->SpeedSum += Speed;
			++State->SpeedSamples;
		}
		if (const USimPawnAnimInstance* Anim = Actor->GetAnim(); Anim && Anim->GetStats().Detail == ESimPawnDetail::Full)
		{
			++State->FullDetailFrames;
		}
		if (!P->GetBody()->IsFirstPerson())
		{
			State->MinArmCm = FMath::Min(State->MinArmCm, static_cast<double>(FVector::Dist(P->GetBody()->GetViewLocation(), Actor->GetActorLocation())));
		}
	};

	// 1. A sim with the Crashlanded start and a wall def on a ground mesh, in the overseer view.
	AddPhase(TEXT("overseer_view"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UOverseerSubsystem* O = Overseer();
		UProtagonistSubsystem* P = Prot();
		if (!B || !O || !P)
		{
			Fail(TEXT("USimSubsystem, UOverseerSubsystem or UProtagonistSubsystem did not start"));
			return;
		}
		// The game's data plus a wall (the real data has no buildings yet), copied so the repo is untouched.
		FString Source = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("../data"));
		FPaths::CollapseRelativeDirectories(Source);
		const FString Dest = FPaths::ProjectSavedDir() / TEXT("Smoke") / TEXT("protagonist-data");
		IFileManager::Get().DeleteDirectory(*Dest, false, true);
		if (!FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*Dest, *Source, true))
		{
			Fail(FString::Printf(TEXT("Could not copy %s to %s"), *Source, *Dest));
			return;
		}
		// Since C4 the game's data has a wall of its own; add one only if it doesn't.
		bool bHasWall = false;
		TArray<FString> ThingFiles;
		IFileManager::Get().FindFiles(ThingFiles, *(Dest / TEXT("defs/Things/*.json")), true, false);
		for (const FString& File : ThingFiles)
		{
			FString Text;
			bHasWall = bHasWall || (FFileHelper::LoadFileToString(Text, *(Dest / TEXT("defs/Things") / File)) && Text.Contains(TEXT("\"defName\": \"Wall\"")));
		}
		if (!bHasWall)
		{
			FFileHelper::SaveStringToFile(
				TEXT("[{\"defName\": \"Wall\", \"label\": \"wall\", \"category\": \"Building\", \"passability\": \"Impassable\", \"fillPercent\": 1.0}]"),
				*(Dest / TEXT("defs/Things/ProtagonistSmokeWall.json")));
		}

		FSimStartOptions Options;
		Options.DataRoot = Dest;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(ProtSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		for (const std::string& Problem : B->GetSim()->setupErrors())
		{
			Fail(FString::Printf(TEXT("Sim setup: %s"), UTF8_TO_TCHAR(Problem.c_str())));
		}
		B->SetSpeed(1);
		BuildTerrainMesh(B->GetSim()->map(), FPlatformTime::Seconds());

		State->Protagonist = P->FindProtagonist();
		int32 Colonists = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			Colonists += View.category == colony::ThingCategory::Pawn && B->GetSim()->colonistOf(View.id) ? 1 : 0;
		}
		if (State->Protagonist == colony::kNoEntity || Colonists < 2)
		{
			Fail(FString::Printf(TEXT("The Crashlanded start gave %d colonists and no protagonist"), Colonists));
			return;
		}
		AOverseerPawn* OverseerBody = O->EnterOverseerMode();
		if (!OverseerBody)
		{
			Fail(TEXT("The overseer camera did not start"));
			return;
		}
		const FVector2D Start = ProtSmokeSimPosition(*B, State->Protagonist);
		OverseerBody->FocusOn(B->SimToWorld(static_cast<float>(Start.X), static_cast<float>(Start.Y), 0));
		OverseerBody->SnapCamera();
		FSimPick Pick;
		Pick.Kind = ESimPickKind::Thing;
		Pick.Thing = State->Protagonist;
		O->Select(Pick);
	}, 3.0f, 1.0f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		if (!Overseer()->IsOverseerActive() || !Cast<AOverseerPawn>(PlayerPawn()))
		{
			Fail(TEXT("The overseer view is not the active one at the start"));
		}
		if (B->GetSim()->controlledPawn() != colony::kNoEntity || Prot()->IsDirectMode())
		{
			Fail(TEXT("A colonist is controlled before the player asked for it"));
		}
		if (B->GetMaxSpeed() != 3)
		{
			Fail(TEXT("The speed cap is not 3 in the overseer view"));
		}
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!Pawns || !Pawns->AreAssetsLoaded() || !Pawns->FindPawn(State->Protagonist))
		{
			Fail(TEXT("The protagonist has no pawn actor (pawn assets missing?)"));
		}
	});

	// 2. Tab into direct mode, aim at another colonist and take it over with T, then take the
	// protagonist back.
	AddPhase(TEXT("direct_possess"), [=, this]()
	{
		UProtagonistSubsystem* P = Prot();
		P->ToggleMode();  // Tab
		if (!P->IsDirectMode() || P->GetControlled() != State->Protagonist)
		{
			Fail(TEXT("Tab did not take control of the selected protagonist"));
			return;
		}
		P->GetBody()->SetFirstPerson(true);
	}, 2.0f, 1.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim() || !P->IsDirectMode())
		{
			return;
		}
		AProtagonistPawn* Body = P->GetBody();
		if (PhaseTime >= 0.8f && !State->bAimed)
		{
			State->bAimed = true;
			// The nearest colonist that is not the controlled one.
			double Nearest = TNumericLimits<double>::Max();
			const FVector2D Mine = ProtSmokeSimPosition(*B, State->Protagonist);
			for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
			{
				if (View.category == colony::ThingCategory::Pawn && View.id != State->Protagonist && B->GetSim()->colonistOf(View.id))
				{
					const double Distance = FVector2D::Distance(Mine, FVector2D(View.x, View.y));
					if (Distance < Nearest)
					{
						Nearest = Distance;
						State->Other = View.id;
					}
				}
			}
			FBox Box;
			const colony::ThingView* OtherView = B->GetCurrentSnapshot().find(State->Other);
			if (!OtherView || !SimPicking::ThingBox(*B, *OtherView, Box))
			{
				Fail(TEXT("No other colonist to aim at"));
				return;
			}
			// First person: the camera is at the head, so aiming is one turn.
			const FRotator Look = (Box.GetCenter() - Body->GetViewLocation()).Rotation();
			Body->Look(FRotator::NormalizeAxis(Look.Yaw - Body->GetYaw()), Look.Pitch - Body->GetPitch());
			Body->SnapCamera();
			if (P->GetLookedAtColonist() != State->Other)
			{
				Fail(TEXT("Aimed at a colonist but the crosshair does not find it"));
				return;
			}
			State->bTookOther = P->TakeOverLookedAt() && P->GetControlled() == State->Other;
			if (!State->bTookOther)
			{
				Fail(TEXT("T did not take over the colonist under the crosshair"));
			}
		}
		if (PhaseTime >= 1.5f && State->bAimed && !State->bBackToProtagonist)
		{
			State->bBackToProtagonist = true;
			if (!P->TakeOver(State->Protagonist) || P->GetControlled() != State->Protagonist)
			{
				Fail(TEXT("Taking the protagonist back did not work"));
			}
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		if (B->GetSim()->controlledPawn() != State->Protagonist)
		{
			Fail(TEXT("After switching bodies the sim does not have the protagonist controlled"));
		}
		const colony::Sim* Sim = B->GetSim();
		const colony::ThingView* Left = B->GetCurrentSnapshot().find(State->Other);
		if (!Left || std::abs(Left->vx) + std::abs(Left->vy) > 0.0001f)
		{
			Fail(TEXT("The colonist that was let go is still moving by itself"));
		}
		if (ProtSmokeCountControlCommands(*Sim) < 3)
		{
			Fail(TEXT("Taking over three times should log three ControlPawn commands"));
		}
	});

	// 3. Walk forward in third person.
	AddPhase(TEXT("direct_walk_third_person"), [=, this]()
	{
		UProtagonistSubsystem* P = Prot();
		if (!P->IsDirectMode())
		{
			Fail(TEXT("Not in direct mode for the walk"));
			return;
		}
		AProtagonistPawn* Body = P->GetBody();
		Body->SetFirstPerson(false);
		Body->Look(FRotator::NormalizeAxis(-Body->GetYaw()), -10.0f - Body->GetPitch());  // looking along +X, a little down
		State->WalkStart = ProtSmokeSimPosition(*Bridge(), State->Protagonist);
		State->ResetCounters();
		State->ReconcilesAtStart = P->GetReconcileCount();
		State->MovesAtStart = P->GetMovesSent();
	}, 0.5f, 5.0f, [=, this]()
	{
		if (UProtagonistSubsystem* P = Prot(); P->IsDirectMode())
		{
			P->GetBody()->SetScriptedInput(0.0, 1.0, false);
		}
		CheckFrame();
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim() || !P->IsDirectMode())
		{
			Fail(TEXT("Direct mode ended during the walk"));
			return;
		}
		const double Moved = FVector2D::Distance(State->WalkStart, ProtSmokeSimPosition(*B, State->Protagonist));
		if (Moved < 12.0)
		{
			Fail(FString::Printf(TEXT("Walked only %.1f m in 5 s of holding W"), Moved));
		}
		const double Average = State->SpeedSamples > 0 ? State->SpeedSum / State->SpeedSamples : 0.0;
		if (Average < 3.0 || Average > 5.0)
		{
			Fail(FString::Printf(TEXT("The walking speed in the snapshot is %.2f m/s (a human runs at about 4.6)"), Average));
		}
		if (State->Frames < 100 || P->GetMovesSent() - State->MovesAtStart < 100)
		{
			Fail(FString::Printf(TEXT("Only %lld frames checked and %lld moves sent"), State->Frames, P->GetMovesSent() - State->MovesAtStart));
		}
		if (P->GetReconcileCount() - State->ReconcilesAtStart > 2)
		{
			Fail(FString::Printf(TEXT("The local position was pulled back to the sim %d times"), P->GetReconcileCount() - State->ReconcilesAtStart));
		}
		if (State->MaxLocalGapM > 1.0)
		{
			Fail(FString::Printf(TEXT("The local position got %.2f m from the sim"), State->MaxLocalGapM));
		}
		if (State->MaxActorGapCm > 60.0)
		{
			Fail(FString::Printf(TEXT("The drawn colonist got %.0f cm from the sim position"), State->MaxActorGapCm));
		}
		if (State->FullDetailFrames < State->Frames * 9 / 10)
		{
			Fail(FString::Printf(TEXT("The controlled colonist had full animation detail in %lld of %lld frames"), State->FullDetailFrames, State->Frames));
		}
		if (State->MinArmCm < 200.0)
		{
			Fail(FString::Printf(TEXT("The third person camera came within %.0f cm of the colonist"), State->MinArmCm));
		}
		if (B->GetSpeed() > 1 || B->GetMaxSpeed() != 1)
		{
			Fail(TEXT("Direct mode did not hold the game at 1x"));
		}
		if (PlayerPawn() != P->GetBody())
		{
			Fail(TEXT("The protagonist pawn does not have the player controller"));
		}
		if (P->GetHudFramesDrawn() < 10)
		{
			Fail(FString::Printf(TEXT("The direct mode HUD drew in only %d frames (ImGui not usable?)"), P->GetHudFramesDrawn()));
		}
		Notes.Add(FString::Printf(TEXT("walked %.1f m in 5 s at an average of %.2f m/s; the local position was at most %.2f m from the sim and the drawn pawn at most %.0f cm"),
			Moved, Average, State->MaxLocalGapM, State->MaxActorGapCm));
	});

	// 4. Walk into a wall: stops short of it, then slides along it.
	AddPhase(TEXT("direct_wall"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		const FVector2D Here = ProtSmokeSimPosition(*B, State->Protagonist);
		State->WallX = FMath::FloorToInt32(Here.X) + 10;
		for (int32 Y = FMath::FloorToInt32(Here.Y) - 12; Y <= FMath::FloorToInt32(Here.Y) + 12; ++Y)
		{
			B->Submit(colony::SpawnThingCommand{"Wall", colony::CellPos{State->WallX, Y, 0}});
		}
		State->bSlideStarted = false;
		State->ResetCounters();
		State->ReconcilesAtStart = Prot()->GetReconcileCount();
	}, 0.5f, 6.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim() || !P->IsDirectMode())
		{
			return;
		}
		// Forward into the wall for 3.5 s, then forward and right: along it.
		const bool bSlide = PhaseTime >= 3.5f;
		P->GetBody()->SetScriptedInput(bSlide ? 1.0 : 0.0, 1.0, false);
		if (bSlide && !State->bSlideStarted)
		{
			State->bSlideStarted = true;
			State->SlideStart = ProtSmokeSimPosition(*B, State->Protagonist);
		}
		CheckFrame();
		// Never inside the wall, in the sim or in the local copy, once the wall stands.
		const FVector2D Sim = ProtSmokeSimPosition(*B, State->Protagonist);
		const FVector2D Local = P->GetBody()->GetSimPosition();
		const colony::CellPos Cell{State->WallX, FMath::FloorToInt32(Sim.Y), 0};
		if (B->GetSim()->map().buildingAt(Cell) != colony::kNoEntity)
		{
			const double Limit = State->WallX - colony::kPawnRadius + 0.01;
			if (Sim.X > Limit || Local.X > Limit)
			{
				Fail(FString::Printf(TEXT("The colonist is inside the wall: sim x %.3f, local x %.3f, wall at %d"), Sim.X, Local.X, State->WallX));
			}
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim() || !P->IsDirectMode())
		{
			return;
		}
		int32 Walls = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			Walls += View.category == colony::ThingCategory::Building ? 1 : 0;
		}
		State->WallsPlaced = Walls;
		if (Walls < 20)
		{
			Fail(FString::Printf(TEXT("Only %d walls were placed"), Walls));
		}
		const FVector2D End = ProtSmokeSimPosition(*B, State->Protagonist);
		if (End.X < State->WallX - 1.0)
		{
			Fail(FString::Printf(TEXT("The colonist never reached the wall: x %.2f, wall at %d"), End.X, State->WallX));
		}
		if (FMath::Abs(End.Y - State->SlideStart.Y) < 2.0)
		{
			Fail(FString::Printf(TEXT("The colonist did not slide along the wall (moved %.2f m along it)"), FMath::Abs(End.Y - State->SlideStart.Y)));
		}
		if (P->GetReconcileCount() - State->ReconcilesAtStart > 2)
		{
			Fail(FString::Printf(TEXT("Against a wall the local position was pulled back %d times"), P->GetReconcileCount() - State->ReconcilesAtStart));
		}
		Notes.Add(FString::Printf(TEXT("stopped at x %.2f by a wall at %d, then slid %.1f m along it"), End.X, State->WallX, FMath::Abs(End.Y - State->SlideStart.Y)));
	});

	// 5. First person: the camera is at the head and the body is hidden; third person shows it again.
	AddPhase(TEXT("direct_first_person"), [=, this]()
	{
		UProtagonistSubsystem* P = Prot();
		if (!P->IsDirectMode())
		{
			Fail(TEXT("Not in direct mode for the first person view"));
			return;
		}
		P->GetBody()->ClearScriptedInput();
		P->GetBody()->SetFirstPerson(true);
		P->GetBody()->Look(120.0f, 5.0f);  // turn away from the wall
		State->ResetCounters();
	}, 1.0f, 1.5f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!B || !B->GetSim() || !P->IsDirectMode() || !Pawns || !Pawns->FindPawn(State->Protagonist))
		{
			Fail(TEXT("No protagonist pawn in the first person view"));
			return;
		}
		const ASimPawnActor* Actor = Pawns->FindPawn(State->Protagonist);
		const FVector Camera = P->GetBody()->GetViewLocation();
		const FVector Feet = Actor->GetActorLocation();
		if (FVector::Dist2D(Camera, Feet) > 30.0 || Camera.Z - Feet.Z < 155.0 || Camera.Z - Feet.Z > 175.0)
		{
			Fail(FString::Printf(TEXT("The first person camera is %.0f cm sideways and %.0f cm up from the feet"), FVector::Dist2D(Camera, Feet), Camera.Z - Feet.Z));
		}
		if (!Actor->IsBodyHidden())
		{
			Fail(TEXT("The player's own body is visible in first person"));
		}
		// Back to third person; the next phase checks that the body shows again.
		P->GetBody()->SetFirstPerson(false);
		P->GetBody()->SnapCamera();
	});

	// 6. Paused: the body does not move; the keys cannot make the game faster than 1x.
	AddPhase(TEXT("direct_pause_and_speed"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!P->IsDirectMode())
		{
			Fail(TEXT("Not in direct mode for the pause test"));
			return;
		}
		B->SetSpeed(0);
		State->PausedAt = ProtSmokeSimPosition(*B, State->Protagonist);
	}, 0.5f, 1.0f, [=, this]()
	{
		if (UProtagonistSubsystem* P = Prot(); P->IsDirectMode())
		{
			P->GetBody()->SetScriptedInput(0.0, 1.0, false);
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim() || !P->IsDirectMode())
		{
			return;
		}
		if (FVector2D::Distance(State->PausedAt, ProtSmokeSimPosition(*B, State->Protagonist)) > 0.01 ||
			FVector2D::Distance(State->PausedAt, P->GetBody()->GetSimPosition()) > 0.01)
		{
			Fail(TEXT("The colonist moved while the game was paused"));
		}
		P->GetBody()->ClearScriptedInput();
		B->SetSpeed(3);
		if (B->GetSpeed() != 1)
		{
			Fail(FString::Printf(TEXT("Pressing 3 in direct mode gave speed %d, not 1"), B->GetSpeed()));
		}
		// The first person view ended in the last phase: the player's own body shows again.
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (const ASimPawnActor* Actor = Pawns ? Pawns->FindPawn(State->Protagonist) : nullptr; !Actor || Actor->IsBodyHidden())
		{
			Fail(TEXT("The player's body stays hidden after going back to third person"));
		}
	});

	// 7. A save made in direct mode puts the player back in direct mode when it is loaded, even
	// from the overseer view.
	AddPhase(TEXT("direct_save_and_load"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!P->IsDirectMode())
		{
			Fail(TEXT("Not in direct mode for the save test"));
			return;
		}
		State->SavedPawn = P->GetControlled();
		State->bLoaded = false;
		if (B->SaveGame(TEXT("u9protagonist")).IsEmpty())
		{
			Fail(TEXT("The save in direct mode failed"));
			return;
		}
		P->ExitDirectMode();  // Tab to the overseer view; the save still has the colonist controlled
	}, 1.0f, 1.0f, [=, this]()
	{
		if (PhaseTime >= 0.5f && !State->bLoaded)
		{
			State->bLoaded = true;
			if (!Bridge()->LoadGame(TEXT("u9protagonist")))
			{
				Fail(TEXT("Loading the save made in direct mode failed"));
			}
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !B->GetSim())
		{
			return;
		}
		if (!P->IsDirectMode() || P->GetControlled() != State->SavedPawn || PlayerPawn() != P->GetBody())
		{
			Fail(TEXT("Loading a save made in direct mode did not put the player back in direct mode"));
		}
		if (B->GetSim()->controlledPawn() != State->SavedPawn)
		{
			Fail(TEXT("The loaded sim does not have the colonist controlled"));
		}
		if (B->GetMaxSpeed() != 1)
		{
			Fail(TEXT("Direct mode after a load does not hold the game at 1x"));
		}
	});

	// 8. Tab back to the overseer view: the colonist is let go, the speed cap is lifted, and the
	// whole session replays to the same state.
	AddPhase(TEXT("back_to_overseer"), [=, this]()
	{
		UProtagonistSubsystem* P = Prot();
		if (!P->IsDirectMode())
		{
			Fail(TEXT("Not in direct mode for the way back"));
			return;
		}
		Bridge()->SetSpeed(1);
		P->ToggleMode();  // Tab
	}, 1.5f, 1.0f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		UOverseerSubsystem* O = Overseer();
		const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
		if (!B || !B->GetSim() || !Pawns)
		{
			return;
		}
		if (P->IsDirectMode() || !O->IsOverseerActive() || !Cast<AOverseerPawn>(PlayerPawn()))
		{
			Fail(TEXT("Tab did not go back to the overseer view"));
		}
		if (B->GetSim()->controlledPawn() != colony::kNoEntity)
		{
			Fail(TEXT("The sim still has a colonist controlled after leaving direct mode"));
		}
		if (O->GetSelection().Kind != ESimPickKind::Thing || O->GetSelection().Thing != State->SavedPawn)
		{
			Fail(TEXT("The colonist that was controlled is not selected in the overseer view"));
		}
		if (const ASimPawnActor* Actor = Pawns->FindPawn(State->SavedPawn); !Actor || Actor->IsBodyHidden())
		{
			Fail(TEXT("The colonist is not visible after leaving direct mode"));
		}
		B->SetSpeed(3);
		if (B->GetSpeed() != 3)
		{
			Fail(TEXT("The overseer view cannot run the game at 3x again"));
		}
		B->SetSpeed(1);

		// The whole session, from step 0 with its commands, plays back to the same state.
		colony::Replay Recorded = colony::recordReplay(*B->GetSim());
		colony::Sim Replayed(Recorded.settings, B->GetDefs());
		colony::playReplay(Recorded, Replayed, Recorded.endStep);
		if (Replayed.stateHash() != B->GetSim()->stateHash())
		{
			Fail(TEXT("The session's replay does not reach the same state hash"));
		}
		Notes.Add(FString::Printf(TEXT("the session's %d commands replay to the same state at second %lld"), static_cast<int32>(Recorded.commands.size()),
			static_cast<long long>(Recorded.endStep)));
	});
}
