// Smoke test `compare` (MakeHuman characters, docs/design/makehuman.md): a MakeHuman (MPFB) woman, a local-only
// MakeHuman character (sneed's own) and the MetaHuman showcase woman side by side, standing and then walking, in the game, offscreen.
#include "SmokeTest.h"

#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Misc/Parse.h"

#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "Retargeter/IKRetargeter.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Apparel.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <vector>

namespace
{
	const FVector CompareSmokeOrigin(-1700000.0, 0.0, 0.0);
	constexpr int32 CompareSmokeWalkShots = 24;

	struct FCompareSmokeState
	{
		TArray<colony::Entity> Row;  // MakeHuman woman, local model, MetaHuman woman
		TArray<FString> Labels;
		colony::CellPos Middle = colony::CellPos{48, 48, 0};
		TWeakObjectPtr<APointLight> Light;
		int32 Leg = 0;
		float LegTime = 0.0f;
		// Jiggle measured while walking: per pawn, the soft tissue's largest lag behind the animation (cm).
		TArray<FVector> RestOffset;
		TArray<float> Jiggle;
		// The second half of the walk they jump every so often (seconds since the last), and the highest a body got (cm).
		float JumpTime = 0.0f;
		float HighestJump = 0.0f;
		int32 JumpsStarted = 0;
		int32 Errors = 0;
		FString FirstError;
		int64 Frames = 0;

		void Error(const FString& Message)
		{
			if (Errors++ == 0)
			{
				FirstError = Message;
			}
		}
	};

	void CompareSmokeSet(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Var->Set(Value, ECVF_SetByCode);
		}
	}
}

void USmokeTestSubsystem::BuildComparePhases()
{
	const TSharedRef<FCompareSmokeState> State = MakeShared<FCompareSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto SimOf = [=, this]() -> colony::Sim* { USimSubsystem* B = Bridge(); return B ? const_cast<colony::Sim*>(B->GetSim()) : nullptr; };
	const auto Cell = [=](int32 X, int32 Y) { return colony::CellPos{State->Middle.x + X, State->Middle.y + Y, 0}; };

	// Every frame: the MakeHuman characters show their own mesh (retargeted), the MetaHuman hides.
	const auto CheckFrame = [=, this]()
	{
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!Pawns || PhaseTime < 1.5f)
		{
			return;
		}
		++State->Frames;
		for (int32 i = 0; i < State->Row.Num(); ++i)
		{
			const ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]);
			if (!Pawn || !Pawn->HasLooks())
			{
				State->Error(FString::Printf(TEXT("%s has no actor or looks"), *State->Labels[i]));
				continue;
			}
			const bool bWantMakeHuman = !Pawn->GetLooks().MakeHuman.IsEmpty();
			const USkeletalMeshComponent* Mesh = Pawn->GetMakeHumanMesh();
			if (bWantMakeHuman && (!Mesh || !Mesh->GetSkeletalMeshAsset() || !Mesh->GetAnimInstance()))
			{
				State->Error(FString::Printf(TEXT("%s: no MakeHuman mesh or animation (is the MakeHumanArt commandlet run? docs/design/makehuman.md)"), *State->Labels[i]));
			}
			if (bWantMakeHuman && Pawn->GetMetaHuman() && !Pawn->GetMetaHuman()->IsHidden())
			{
				State->Error(FString::Printf(TEXT("%s: the MetaHuman still shows"), *State->Labels[i]));
			}
		}
	};
	const auto VerifyFrames = [=, this](const FString& Phase)
	{
		if (State->Frames < 3)
		{
			Fail(FString::Printf(TEXT("%s: too few frames checked (%lld)"), *Phase, State->Frames));
		}
		if (State->Errors > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d problems; the first: %s"), *Phase, State->Errors, *State->FirstError));
		}
	};
	// The camera on the three, from in front (+X, the way they face until they walk) and a little above, with a lamp beside it.
	const auto LookAtRow = [=, this](float Distance, float Height, float Side)
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			return;
		}
		FVector Mid = FVector::ZeroVector;
		int32 Count = 0;
		for (const colony::Entity Thing : State->Row)
		{
			if (const ASimPawnActor* Pawn = Pawns->FindPawn(Thing))
			{
				Mid += Pawn->GetActorLocation();
				++Count;
			}
		}
		if (Count == 0)
		{
			Mid = B->SimToWorld(State->Middle.x + 0.5f, State->Middle.y + 0.5f, 0);
			Count = 1;
		}
		else
		{
			Mid /= Count;
		}
		const FVector Target = Mid + FVector(0.0, 0.0, Height);
		const FVector From = Target + FVector(Distance, Side, 25.0);
		ViewFrom(From, (Target - From).Rotation());
		if (!State->Light.IsValid())
		{
			State->Light = GetWorld()->SpawnActor<APointLight>(From, FRotator::ZeroRotator, FActorSpawnParameters());
			Spawned.Add(State->Light.Get());
		}
		if (APointLight* Light = State->Light.Get())
		{
			Light->PointLightComponent->SetMobility(EComponentMobility::Movable);
			Light->SetActorLocation(From + FVector(0.0, 0.0, 150.0));
			Light->PointLightComponent->SetIntensity(150000.0f);
			Light->PointLightComponent->SetAttenuationRadius(2000.0f);
		}
	};

	// 1. The three standing side by side, bare (the local model in its own outfit).
	FPhase Stand;
	Stand.Name = TEXT("compare_stand");
	Stand.WarmupSeconds = 8.0f;
	Stand.MeasureSeconds = 1.0f;
	Stand.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		CompareSmokeSet(TEXT("Pawns.MetaHumans"), 1.0f);
		CompareSmokeSet(TEXT("Sky.Hour"), 7.0f);
		CompareSmokeSet(TEXT("Sky.DayOfYear"), 22.5f);
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->ConsoleCommand(TEXT("r.SetRes 1920x1080w"));
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		B->SetWorldOrigin(CompareSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		State->Middle = colony::CellPos{48, 48, 0};
		// A clear patch of ground.
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - State->Middle.x) < 24.0f && FMath::Abs(View.y - State->Middle.y) < 24.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		if (colony::Sim* Sim = SimOf())
		{
			for (int32 x = -24; x <= 24; ++x)
			{
				for (int32 y = -24; y <= 24; ++y)
				{
					Sim->plants().remove(Cell(x, y));
				}
			}
		}
		for (const int32 Y : {-2, 0, 2})
		{
			B->Submit(colony::SpawnThingCommand{"Human", Cell(0, Y)});
		}
		B->StepOnce();
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TArray<const colony::ThingView*> InRow;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0 && View.x >= State->Middle.x && View.x < State->Middle.x + 1)
			{
				InRow.Add(&View);
			}
		}
		InRow.Sort([](const colony::ThingView& A, const colony::ThingView& Bv) { return A.y < Bv.y; });
		const colony::DefDatabase* Defs = B->GetDefs().get();
		for (int32 i = 0; i < InRow.Num() && i < 3; ++i)
		{
			const colony::ThingView& View = *InRow[i];
			FSimPawnLooks Looks = View.looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View.looks)], Defs) : FSimPawnLooks();
			Looks.bFemale = true;
			Looks.Beard = TEXT("None");
			Looks.BodyHair = 0.0f;
			Looks.Tattoos.Reset();
			Looks.Scars.Reset();
			Looks.HornLength = 0.0f;
			Looks.Skin = FLinearColor(FColor(232, 205, 190));
			FString Label;
			if (i == 0)
			{
				// The MakeHuman (MPFB) example woman: her own mesh is her height.
				Looks.MakeHuman = TEXT("Woman");
				Looks.Height = 1.0f;
				Label = TEXT("MakeHuman woman");
			}
			else if (i == 1)
			{
				// sneed's own MakeHuman character (or another, -CompareSecond=<name>).
				FString Second = TEXT("PetitePale");
				FParse::Value(FCommandLine::Get(), TEXT("CompareSecond="), Second);
				Looks.MakeHuman = Second;
				Looks.Height = 1.0f;
				Label = Second;
			}
			else
			{
				// The MetaHuman showcase woman, with a ponytail.
				Looks.Height = 0.88f;
				Looks.Stature = -1.0f;
				Looks.Weight = -0.45f;
				Looks.Hair = FLinearColor(FColor(14, 12, 12));
				Looks.Eyes = FColor(79, 133, 69);
				Looks.HairStyle = TEXT("Ponytail");
				Looks.FacePreset = TEXT("Jelena");
				Looks.FaceSkin = 6;
				Label = TEXT("MetaHuman woman (ponytail)");
			}
			Pawns->SetLooksOverride(View.id, Looks);
			State->Row.Add(View.id);
			State->Labels.Add(Label);
			Notes.Add(FString::Printf(TEXT("  %d: %s"), i, *Label));
		}
		// Bare: the sim's own clothes come off; then drafted, so they walk where they are sent.
		if (colony::Sim* Sim = SimOf())
		{
			for (const colony::Entity Thing : State->Row)
			{
				const std::vector<colony::Entity> Worn = Sim->apparel().worn(Thing);
				for (const colony::Entity Item : Worn)
				{
					std::string Error;
					Sim->apparel().takeOff(Thing, Item, Error);
					B->Submit(colony::DestroyThingCommand{Item});
				}
				B->Submit(colony::DraftCommand{Thing, true});
			}
		}
		B->StepOnce();
		if (State->Row.Num() != 3)
		{
			Fail(FString::Printf(TEXT("%d test pawns, wanted 3"), State->Row.Num()));
		}
		LookAtRow(420.0f, 95.0f, 0.0f);
	};
	Stand.OnFrame = CheckFrame;
	Stand.Verify = [=, this]()
	{
		VerifyFrames(TEXT("compare_stand"));
		USimPawnSubsystem* Pawns = PawnsOf();
		for (int32 i = 0; Pawns && i < State->Row.Num(); ++i)
		{
			const ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]);
			const USkeletalMeshComponent* Mesh = Pawn ? Pawn->GetMakeHumanMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}
			const UMakeHumanAnimInstance* Anim = Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance());
			const FString Retargeter = Anim && Anim->GetRetargeter() ? Anim->GetRetargeter()->GetName() : TEXT("none");
			Notes.Add(FString::Printf(TEXT("  %s: %d soft bodies (jiggle, hair)"), *State->Labels[i], Anim ? Anim->GetPhysicsChains() : 0));
			const USkeletalMeshComponent* Source = Pawn->GetBody();
			const auto Dir = [](const USkeletalMeshComponent* M, const TCHAR* A, const TCHAR* B)
			{
				return M && M->DoesSocketExist(A) && M->DoesSocketExist(B) ? (M->GetSocketLocation(B) - M->GetSocketLocation(A)).GetSafeNormal() : FVector::ZeroVector;
			};
			const TCHAR* const Pairs[][4] = {{TEXT("thigh_l"), TEXT("calf_l"), TEXT("thigh_l"), TEXT("calf_l")}, {TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("upperarm_l"), TEXT("lowerarm_l")},
				{TEXT("thigh_l"), TEXT("calf_l"), TEXT("thigh_L"), TEXT("shin_L")}, {TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("upper_arm_L"), TEXT("forearm_L")}};
			FString Angles;
			for (const auto& P : Pairs)
			{
				const FVector S = Dir(Source, P[0], P[1]);
				const FVector T = Dir(Mesh, P[2], P[3]);
				if (!S.IsNearlyZero() && !T.IsNearlyZero())
				{
					Angles += FString::Printf(TEXT("%s %.0f deg (src %s tgt %s); "), P[2], FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(S, T), -1.0, 1.0))), *S.ToCompactString(), *T.ToCompactString());
				}
			}
			Notes.Add(FString::Printf(TEXT("  %s angles: %s"), *State->Labels[i], *Angles));
			Notes.Add(FString::Printf(TEXT("  %s: actor %s, mesh at %s, bounds %s r %.0f, visible %d, scale %s, retargeter %s, head %s"), *State->Labels[i],
				*Pawn->GetActorLocation().ToString(), *Mesh->GetComponentLocation().ToString(), *Mesh->Bounds.Origin.ToString(), Mesh->Bounds.SphereRadius,
				Mesh->IsVisible(), *Mesh->GetComponentScale().ToString(), *Retargeter,
				Mesh->DoesSocketExist(TEXT("head")) ? *Mesh->GetSocketLocation(TEXT("head")).ToString() : TEXT("-")));
		}
	};
	Phases.Add(MoveTemp(Stand));

	// 2. Closer, three-quarter view.
	FPhase Close;
	Close.Name = TEXT("compare_close");
	Close.WarmupSeconds = 1.5f;
	Close.MeasureSeconds = 1.0f;
	Close.Setup = [=, this]() { State->Frames = 0; LookAtRow(260.0f, 110.0f, 120.0f); };
	Close.OnFrame = CheckFrame;
	Close.Verify = [=, this]() { VerifyFrames(TEXT("compare_close")); };
	Phases.Add(MoveTemp(Close));

	// 2b. Each one's face and body up close (paused), lit from the camera.
	for (int32 Who = 0; Who < 3; ++Who)
	{
		for (const bool bFace : {true, false})
		{
			const FString Name = FString::Printf(TEXT("compare_%s_%d"), bFace ? TEXT("face") : TEXT("body"), Who);
			FPhase Shot;
			Shot.Name = Name;
			Shot.WarmupSeconds = 1.5f;
			Shot.MeasureSeconds = 0.5f;
			Shot.Setup = [=, this]()
			{
				State->Frames = 0;
				USimPawnSubsystem* Pawns = PawnsOf();
				const ASimPawnActor* Pawn = Pawns && State->Row.IsValidIndex(Who) ? Pawns->FindPawn(State->Row[Who]) : nullptr;
				if (!Pawn)
				{
					return;
				}
				const USkeletalMeshComponent* Mesh = Pawn->GetMakeHumanMesh() ? Pawn->GetMakeHumanMesh() : Pawn->GetFaceMesh();
				const FVector Head = Mesh && Mesh->DoesSocketExist(TEXT("head")) ? Mesh->GetSocketLocation(TEXT("head")) + FVector(0.0, 0.0, 7.0) : Pawn->GetActorLocation() + FVector(0.0, 0.0, 150.0);
				const FVector Forward = Pawn->GetActorForwardVector().GetSafeNormal2D();
				const FVector Target = bFace ? Head + Forward * 6.0 : FVector(Head.X, Head.Y, Pawn->GetActorLocation().Z + 85.0);
				const FVector From = Target + Forward * (bFace ? 70.0 : 240.0) + FVector::CrossProduct(FVector::UpVector, Forward) * (bFace ? 12.0 : 40.0);
				ViewFrom(From, (Target - From).Rotation());
				if (APointLight* Light = State->Light.Get())
				{
					Light->SetActorLocation(From + FVector(0.0, 0.0, bFace ? 30.0 : 80.0));
					Light->PointLightComponent->SetIntensity(bFace ? 20000.0f : 60000.0f);
				}
			};
			Shot.OnFrame = CheckFrame;
			Phases.Add(MoveTemp(Shot));
		}
	}

	// 3. Walking: back and forth along X, side by side; a run of shots for a short clip.
	for (int32 Shot = 0; Shot < CompareSmokeWalkShots; ++Shot)
	{
		FPhase Walk;
		Walk.Name = FString::Printf(TEXT("compare_walk_%02d"), Shot);
		Walk.WarmupSeconds = Shot == 0 ? 1.0f : 0.15f;
		Walk.MeasureSeconds = 0.2f;
		Walk.Setup = [=, this]()
		{
			USimSubsystem* B = Bridge();
			if (!B)
			{
				return;
			}
			if (Shot == 0)
			{
				B->SetSpeed(1);
				State->Leg = 0;
				State->LegTime = 99.0f;
			}
			State->Frames = 0;
		};
		Walk.OnFrame = [=, this]()
		{
			USimSubsystem* B = Bridge();
			if (!B)
			{
				return;
			}
			State->LegTime += GetWorld()->GetDeltaSeconds();
			if (State->LegTime > 3.5f)
			{
				State->LegTime = 0.0f;
				const int32 X = State->Leg % 2 == 0 ? 6 : -2;
				for (int32 i = 0; i < State->Row.Num(); ++i)
				{
					B->Submit(colony::MoveOrderCommand{State->Row[i], Cell(X, -2 + i * 2)});
				}
				++State->Leg;
			}
			LookAtRow(520.0f, 100.0f, 250.0f);
			USimPawnSubsystem* Pawns = PawnsOf();
			// Jumping (Space for the player's body) in the second half: the arc, the jump animations and the landing's bounce.
			State->JumpTime += GetWorld()->GetDeltaSeconds();
			for (int32 i = 0; Pawns && i < State->Row.Num(); ++i)
			{
				if (ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]))
				{
					if (Shot >= CompareSmokeWalkShots / 2 && State->JumpTime > 1.3f && Pawn->StartJump())
					{
						++State->JumpsStarted;
					}
					State->HighestJump = FMath::Max(State->HighestJump, Pawn->GetJumpHeight());
				}
			}
			if (State->JumpTime > 1.3f)
			{
				State->JumpTime = 0.0f;
			}
			// How far the soft tissue lags behind the animation as they walk (0 means no jiggle at all).
			State->RestOffset.SetNum(State->Row.Num());
			State->Jiggle.SetNum(State->Row.Num());
			for (int32 i = 0; Pawns && i < State->Row.Num(); ++i)
			{
				const ASimPawnActor* Pawn = Pawns->FindPawn(State->Row[i]);
				const USkeletalMeshComponent* Mesh = Pawn ? Pawn->GetMakeHumanMesh() : nullptr;
				if (UMakeHumanAnimInstance* Anim = Mesh ? Cast<UMakeHumanAnimInstance>(Mesh->GetAnimInstance()) : nullptr)
				{
					State->Jiggle[i] = FMath::Max(State->Jiggle[i], Anim->TakeLargestJiggle());
				}
			}
		};
		Walk.Verify = [=, this]()
		{
			if (Shot == CompareSmokeWalkShots - 1)
			{
				for (int32 i = 0; i < State->Jiggle.Num(); ++i)
				{
					Notes.Add(FString::Printf(TEXT("compare_walk: %s soft tissue lagged up to %.2f cm behind the animation"), *State->Labels[i], State->Jiggle[i]));
				}
				Notes.Add(FString::Printf(TEXT("compare_walk: %d jumps, the highest body %.0f cm off the ground"), State->JumpsStarted, State->HighestJump));
				if (State->JumpsStarted == 0 || State->HighestJump < 0.5f * ASimPawnActor::kJumpHeight)
				{
					Fail(TEXT("compare_walk: the pawns did not jump"));
				}
			}
			if (State->Errors > 0)
			{
				Fail(FString::Printf(TEXT("compare_walk: %d problems; the first: %s"), State->Errors, *State->FirstError));
			}
		};
		Phases.Add(MoveTemp(Walk));
	}

	FPhase Done;
	Done.Name = TEXT("compare_cleanup");
	Done.WarmupSeconds = 0.5f;
	Done.MeasureSeconds = 0.5f;
	Done.Setup = [=, this]()
	{
		if (USimPawnSubsystem* Pawns = PawnsOf())
		{
			Pawns->ClearLooksOverrides();
		}
		CompareSmokeSet(TEXT("Sky.Hour"), -1.0f);
		CompareSmokeSet(TEXT("Sky.DayOfYear"), -1.0f);
	};
	Phases.Add(MoveTemp(Done));
}

// Not a check: `showcase` (tools/play-showcase.bat) opens the compare scene for play, with the graphics turned up.
// sneed's ponytail woman is the player's (direct control: WASD, mouse, F2 pause); the long-haired example woman stands
// drafted as an NPC. It runs until the window is closed or -ShowcaseMinutes (default 240) pass. -ShowcaseDemo drives her by
// itself (run away from the view, sprint across it and towards it with jumps; for a clip and for checking sprint and jump)
// and notes how fast and how often; -ShowcaseRecord=<seconds> with it saves every frame of that long at a fixed 30 per second
// (showcase/rec_0000.png ...: `ffmpeg -framerate 30 -i rec_%04d.png -pix_fmt yuv420p clip.mp4`).
void USmokeTestSubsystem::BuildShowcasePhases()
{
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto PawnsOf = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const TSharedRef<colony::Entity> Player = MakeShared<colony::Entity>(colony::kNoEntity);
	struct FDemo
	{
		colony::Entity Body = colony::kNoEntity;
		float Time = 0.0f;
		float FastestRun = 0.0f;
		float FastestSprint = 0.0f;
		int32 Jumps = 0;
		int32 Recorded = 0;
	};
	const TSharedRef<FDemo> Demo = MakeShared<FDemo>();
	const bool bDemo = FParse::Param(FCommandLine::Get(), TEXT("ShowcaseDemo"));
	float RecordSeconds = 0.0f;
	FParse::Value(FCommandLine::Get(), TEXT("ShowcaseRecord="), RecordSeconds);

	FPhase Play;
	Play.Name = TEXT("showcase");
	Play.WarmupSeconds = 8.0f;  // control is taken during the warm-up, so the phase's screenshot is the player's view
	float Minutes = 240.0f;
	FParse::Value(FCommandLine::Get(), TEXT("ShowcaseMinutes="), Minutes);
	Play.MeasureSeconds = Minutes * 60.0f;
	Play.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		USimPawnSubsystem* Pawns = PawnsOf();
		if (!B || !Pawns)
		{
			Fail(TEXT("USimSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		// Graphics up: epic scalability, full resolution, temporal super resolution, Lumen everything, virtual shadows.
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			for (const TCHAR* Command : {TEXT("sg.ShadowQuality 4"), TEXT("sg.GlobalIlluminationQuality 4"), TEXT("sg.ReflectionQuality 4"),
				TEXT("sg.PostProcessQuality 4"), TEXT("sg.TextureQuality 4"), TEXT("sg.EffectsQuality 4"), TEXT("sg.FoliageQuality 4"),
				TEXT("sg.ShadingQuality 4"), TEXT("sg.ViewDistanceQuality 4"), TEXT("sg.AntiAliasingQuality 4"), TEXT("r.ScreenPercentage 100"),
				TEXT("r.AntiAliasingMethod 4"), TEXT("r.DynamicGlobalIlluminationMethod 1"), TEXT("r.ReflectionMethod 1"),
				TEXT("r.Lumen.Reflections.HierarchicalScreenTraces 1"), TEXT("r.Shadow.Virtual.Enable 1"), TEXT("r.SSS.Quality 1"),
				TEXT("r.ContactShadows 1"), TEXT("Pawns.FullDetail 20")})
			{
				PC->ConsoleCommand(Command);
			}
		}
		for (const TPair<const TCHAR*, float>& Var : {TPair<const TCHAR*, float>(TEXT("Pawns.MetaHumans"), 1.0f), TPair<const TCHAR*, float>(TEXT("Sky.Hour"), 9.5f),
			TPair<const TCHAR*, float>(TEXT("Sky.DayOfYear"), 22.5f)})
		{
			if (IConsoleVariable* Found = IConsoleManager::Get().FindConsoleVariable(Var.Key))
			{
				Found->Set(Var.Value, ECVF_SetByCode);
			}
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 96;
		Options.bFlatMap = true;
		// The Crashlanded start: direct control takes colonists only. Two of them become the women; the rest go.
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(CompareSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(0);
		B->StepOnce();
		const colony::CellPos Middle = B->GetSim()->stockpileCell().value_or(colony::CellPos{48, 48, 0});
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn && FMath::Abs(View.x - Middle.x) < 24.0f && FMath::Abs(View.y - Middle.y) < 24.0f)
			{
				B->Submit(colony::DestroyThingCommand{View.id});
			}
		}
		colony::Sim* Sim = const_cast<colony::Sim*>(B->GetSim());
		for (int32 x = -24; x <= 24; ++x)
		{
			for (int32 y = -24; y <= 24; ++y)
			{
				Sim->plants().remove(colony::CellPos{Middle.x + x, Middle.y + y, 0});
			}
		}
		B->StepOnce();
		const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
		TArray<const colony::ThingView*> Two;
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.lifeStage < 0)
			{
				Two.Add(&View);
			}
		}
		Two.Sort([](const colony::ThingView& A, const colony::ThingView& Bv) { return A.id < Bv.id; });
		if (Two.Num() < 2)
		{
			Fail(TEXT("the Crashlanded start has fewer than two colonists"));
			return;
		}
		for (int32 i = 2; i < Two.Num(); ++i)
		{
			B->Submit(colony::DestroyThingCommand{Two[i]->id});
		}
		const colony::DefDatabase* Defs = B->GetDefs().get();
		for (int32 i = 0; i < 2; ++i)
		{
			FSimPawnLooks Looks = Two[i]->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(Two[i]->looks)], Defs) : FSimPawnLooks();
			Looks.bFemale = true;
			Looks.Height = 1.0f;
			Looks.Skin = FLinearColor(FColor(232, 205, 190));
			Looks.MakeHuman = i == 0 ? TEXT("PetitePale") : TEXT("Woman");
			FParse::Value(FCommandLine::Get(), i == 0 ? TEXT("ShowcasePlayer=") : TEXT("ShowcaseNpc="), Looks.MakeHuman);
			Pawns->SetLooksOverride(Two[i]->id, Looks);
			const std::vector<colony::Entity> Worn = Sim->apparel().worn(Two[i]->id);
			for (const colony::Entity Item : Worn)
			{
				std::string Error;
				Sim->apparel().takeOff(Two[i]->id, Item, Error);
				B->Submit(colony::DestroyThingCommand{Item});
			}
		}
		*Player = Two[0]->id;
		B->Submit(colony::DraftCommand{Two[1]->id, true});
		B->StepOnce();
		B->SetSpeed(1);
	};
	Play.OnFrame = [=, this]()
	{
		// Take her over once her actor is there (direct control needs the pawn drawn).
		if (*Player == colony::kNoEntity || PhaseTime < 1.0f)
		{
			return;
		}
		if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
		{
			if (Protagonist->EnterDirectMode(*Player))
			{
				Notes.Add(TEXT("showcase: you control the ponytail woman"));
				Demo->Body = *Player;
				*Player = colony::kNoEntity;
			}
		}
	};
	if (bDemo)
	{
		Play.OnFrame = [=, this, Take = Play.OnFrame]()
		{
			Take();
			UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
			AProtagonistPawn* Body = Protagonist ? Protagonist->GetBody() : nullptr;
			USimPawnSubsystem* Pawns = PawnsOf();
			ASimPawnActor* Actor = Pawns && Demo->Body != colony::kNoEntity ? Pawns->FindPawn(Demo->Body) : nullptr;
			if (!Body || !Actor)
			{
				return;
			}
			// Each 11 seconds: 2.5 running away from the view; 4 sprinting across it (seen from the side) and 3.5 towards it
			// (from the front), jumping every 1.3; 1 standing. The view follows behind and does not turn, so she goes across
			// and back.
			const float Dt = GetWorld()->GetDeltaSeconds();
			if (Demo->Time == 0.0f)
			{
				Body->Zoom(3.0f);  // closer than the default view, to see her move
				if (RecordSeconds > 0.0f)
				{
					FApp::SetUseFixedTimeStep(true);
					FApp::SetFixedDeltaTime(1.0 / 30.0);
				}
			}
			Demo->Time += Dt;
			const float T = FMath::Fmod(Demo->Time, 11.0f);
			const float Side = FMath::Fmod(Demo->Time, 22.0f) < 11.0f ? 1.0f : -1.0f;
			const auto Sprinting = [&](float From)
			{
				Demo->FastestSprint = FMath::Max(Demo->FastestSprint, Actor->GetGroundSpeed());
				if (FMath::Fmod(T - From, 1.3f) < Dt && T - From > 0.6f && Body->Jump())
				{
					++Demo->Jumps;
				}
			};
			if (T < 2.5f)
			{
				Body->SetScriptedInput(0.0, 1.0, false, false);
				if (T > 1.5f)
				{
					Demo->FastestRun = FMath::Max(Demo->FastestRun, Actor->GetGroundSpeed());
				}
			}
			else if (T < 6.5f)
			{
				Body->SetScriptedInput(Side, 0.0, false, true);
				Sprinting(2.5f);
			}
			else if (T < 10.0f)
			{
				Body->SetScriptedInput(0.0, -1.0, false, true);
				Sprinting(6.5f);
			}
			else
			{
				Body->SetScriptedInput(0.0, 0.0, false, false);
			}
			if (RecordSeconds > 0.0f && Demo->Recorded < FMath::RoundToInt32(RecordSeconds * 30.0f))
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke/showcase") / FString::Printf(TEXT("rec_%04d.png"), Demo->Recorded++), false, false);
			}
		};
		Play.Verify = [=, this]()
		{
			const double Run = Demo->FastestRun / 100.0;
			const double Sprint = Demo->FastestSprint / 100.0;
			const int32 Jumps = Demo->Jumps;
			Notes.Add(FString::Printf(TEXT("showcase demo: running %.1f m/s, sprinting %.1f m/s, %d jumps"), Run, Sprint, Jumps));
			if (Demo->Jumps == 0 || Demo->FastestSprint < Demo->FastestRun * 1.25f)
			{
				Fail(TEXT("showcase demo: no jump, or the sprint was not faster than the run"));
			}
		};
	}
	Phases.Add(MoveTemp(Play));
}
