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
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "MakeHumanAnimInstance.h"
#include "PawnActor.h"
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
		};
		Walk.Verify = [=, this]()
		{
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
