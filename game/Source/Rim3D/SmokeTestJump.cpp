// Smoke test `jump` (sneed 2026-11-28: "whenever I jump the landing is messed up, I float then it glitches; make the jump
// 100% fluid, a natural landing"): the protagonist jumps standing, walking and running, in third person; every frame the
// feet are measured against the ground and the hips' height is followed, so a float (the body down, the feet still up)
// and a glitch (the hips jumping in one frame) show as numbers, with pictures of each landing.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"

#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Snapshot.h"

namespace
{
	const FVector JumpSmokeOrigin(-1750000.0, 0.0, 0.0);

	struct FJumpSmokeState
	{
		colony::Entity Me = colony::kNoEntity;
		bool bJumped = false;
		bool bWasJumping = false;
		bool bLanded = false;
		float Age = 0.0f;
		float JumpAt = -1.0f;
		float LandedAt = -1.0f;
		double GroundZ = 0.0;      // the lower foot standing (cm, world)
		bool bHaveGround = false;
		double FootAtTouchdown = 0.0;   // the lower foot above the ground the frame the jump's arc ends (cm)
		double MaxFootLastTenth = 0.0;  // the most it is above the ground in the last 0.1 s before that
		double MinFootInAir = 1.0e9;    // below nought: the feet went into the ground
		double MaxPelvisSpeedAfter = 0.0;  // the hips' fastest up or down in the 0.6 s after the touchdown (cm/s)
		double MaxPelvisStepAfter = 0.0;   // their biggest one-frame move up or down then (cm)
		double MaxHeight = 0.0;
		double LastPelvisZ = 0.0;
		bool bHavePelvis = false;
		TArray<double> FootTrail;  // the lower foot over the last frames before the touchdown

		void Reset()
		{
			bJumped = bWasJumping = bLanded = false;
			Age = 0.0f;
			JumpAt = LandedAt = -1.0f;
			FootAtTouchdown = MaxFootLastTenth = MaxPelvisSpeedAfter = MaxPelvisStepAfter = MaxHeight = 0.0;
			MinFootInAir = 1.0e9;
			bHavePelvis = false;
			FootTrail.Reset();
		}
	};
}

void USmokeTestSubsystem::BuildJumpPhases()
{
	const TSharedRef<FJumpSmokeState> State = MakeShared<FJumpSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto Actor = [=]() -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(State->Me) : nullptr; };

	const auto LowerFoot = [=](const ASimPawnActor& A) -> double
	{
		const USkeletalMeshComponent* Mesh = A.GetBody();
		if (!Mesh)
		{
			return 0.0;
		}
		return FMath::Min(Mesh->GetSocketLocation(TEXT("foot_l")).Z, Mesh->GetSocketLocation(TEXT("foot_r")).Z);
	};

	// Every frame: jumps once JumpAfter seconds into the phase and follows the feet and hips to well after the landing.
	const auto Watch = [=, this](float JumpAfter)
	{
		ASimPawnActor* A = Actor();
		AProtagonistPawn* Body = Prot() ? Prot()->GetBody() : nullptr;
		if (!A || !Body)
		{
			return;
		}
		const float Delta = GetWorld()->GetDeltaSeconds();
		State->Age += Delta;
		const double Foot = LowerFoot(*A);
		const double Pelvis = A->GetBody() ? A->GetBody()->GetSocketLocation(TEXT("pelvis")).Z : 0.0;
		if (!State->bJumped)
		{
			// Standing (or running) on the flat before the jump: where the lower foot is lowest is the ground.
			if (State->Age > JumpAfter - 0.5f)
			{
				State->GroundZ = State->bHaveGround ? FMath::Min(State->GroundZ, Foot) : Foot;
				State->bHaveGround = true;
			}
			if (State->Age >= JumpAfter)
			{
				State->bJumped = Body->Jump();
				State->JumpAt = State->Age;
				State->bWasJumping = State->bJumped;
				if (!State->bJumped)
				{
					Fail(TEXT("jump: the protagonist would not jump"));
				}
			}
			return;
		}
		const double Above = Foot - State->GroundZ;
		const bool bJumping = A->IsJumping();
		if (bJumping)
		{
			State->MaxHeight = FMath::Max(State->MaxHeight, static_cast<double>(A->GetJumpHeight()));
			State->MinFootInAir = FMath::Min(State->MinFootInAir, Above);
			State->FootTrail.Add(Above);
			const int32 Keep = FMath::Max(1, FMath::RoundToInt32(0.1f / FMath::Max(0.004f, Delta)));
			while (State->FootTrail.Num() > Keep)
			{
				State->FootTrail.RemoveAt(0);
			}
		}
		if (State->bWasJumping && !bJumping && !State->bLanded)
		{
			State->bLanded = true;
			State->LandedAt = State->Age;
			State->FootAtTouchdown = Above;
			for (const double F : State->FootTrail)
			{
				State->MaxFootLastTenth = FMath::Max(State->MaxFootLastTenth, F);
			}
		}
		State->bWasJumping = bJumping;
		if (State->bLanded && State->Age - State->LandedAt <= 0.6f && State->bHavePelvis && Delta > 0.0f)
		{
			const double Step = FMath::Abs(Pelvis - State->LastPelvisZ);
			State->MaxPelvisStepAfter = FMath::Max(State->MaxPelvisStepAfter, Step);
			State->MaxPelvisSpeedAfter = FMath::Max(State->MaxPelvisSpeedAfter, Step / Delta);
		}
		State->LastPelvisZ = Pelvis;
		State->bHavePelvis = true;
	};

	const auto Report = [=, this](const TCHAR* Name)
	{
		const ASimPawnActor* A = Actor();
		Notes.Add(FString::Printf(TEXT("%s: %s; height %.0f cm; lower foot %.1f cm off the ground at the touchdown, up to %.1f cm in the last 0.1 s, "
									   "lowest in the air %.1f cm; hips after: fastest %.0f cm/s, biggest frame %.1f cm"),
			Name, A ? *A->GetLastJumpTiming() : TEXT("?"), State->MaxHeight, State->FootAtTouchdown, State->MaxFootLastTenth, State->MinFootInAir,
			State->MaxPelvisSpeedAfter, State->MaxPelvisStepAfter));
		if (!State->bLanded)
		{
			Fail(FString::Printf(TEXT("%s: the jump never landed"), Name));
			return;
		}
		// Feet on the ground the moment the body is (the float: the body came down with the falling pose's feet held up).
		if (State->FootAtTouchdown > 8.0)
		{
			Fail(FString::Printf(TEXT("%s: the feet were %.1f cm off the ground when the body landed (a float)"), Name, State->FootAtTouchdown));
		}
		// Not through the ground either.
		if (State->MinFootInAir < -6.0)
		{
			Fail(FString::Printf(TEXT("%s: a foot went %.1f cm into the ground in the jump"), Name, -State->MinFootInAir));
		}
		// No snap after it (the glitch): the hips settle, they don't jump.
		if (State->MaxPelvisStepAfter > 6.0)
		{
			Fail(FString::Printf(TEXT("%s: the hips jumped %.1f cm in one frame after the landing (a glitch)"), Name, State->MaxPelvisStepAfter));
		}
	};

	// 1. A flat map, the protagonist under direct control in third person, empty handed, the camera beside it.
	FPhase Setup;
	Setup.Name = TEXT("jump_setup");
	Setup.WarmupSeconds = 4.0f;
	Setup.MeasureSeconds = 0.5f;
	Setup.Setup = [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !P || !Pawns())
		{
			Fail(TEXT("USimSubsystem, UProtagonistSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		if (IConsoleVariable* Hour = IConsoleManager::Get().FindConsoleVariable(TEXT("Sky.Hour")))
		{
			Hour->Set(11.0f, ECVF_SetByCode);
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(JumpSmokeOrigin);
		if (!B->StartSim(Options))
		{
			Fail(TEXT("The game's data did not load"));
			return;
		}
		B->SetSpeed(1);
		State->Me = P->FindProtagonist();
		B->StepOnce();
		if (State->Me == colony::kNoEntity || !P->EnterDirectMode(State->Me))
		{
			Fail(TEXT("could not take control of the protagonist"));
			return;
		}
		P->GetBody()->SetFirstPerson(false);
		// Side on, so the feet and the ground show in the pictures.
		P->GetBody()->Look(FRotator::NormalizeAxis(90.0f - P->GetBody()->GetYaw()), -8.0f - P->GetBody()->GetPitch());
		P->GetBody()->SnapCamera();
	};
	Phases.Add(MoveTemp(Setup));

	struct FJumpKind
	{
		const TCHAR* Name;
		double Forward;
		bool bWalk;
		bool bSprint;
	};
	const FJumpKind Kinds[] = {
		{TEXT("jump_stand"), 0.0, false, false},
		{TEXT("jump_walk"), 1.0, true, false},
		{TEXT("jump_run"), 1.0, false, false},
		{TEXT("jump_sprint"), 1.0, false, true},
	};
	for (const FJumpKind& Kind : Kinds)
	{
		FPhase Jump;
		Jump.Name = Kind.Name;
		Jump.WarmupSeconds = 0.1f;
		Jump.MeasureSeconds = 3.2f;
		// The picture just after the touchdown (the jump goes up 1.5 s in and lands about 0.65 s later).
		Jump.ScreenshotAt = 2.2f;
		const FJumpKind K = Kind;
		Jump.Setup = [=, this]()
		{
			State->Reset();
			State->bHaveGround = false;
			if (AProtagonistPawn* Body = Prot() ? Prot()->GetBody() : nullptr)
			{
				if (K.Forward != 0.0)
				{
					Body->SetScriptedInput(0.0, K.Forward, K.bWalk, K.bSprint);
				}
				else
				{
					Body->ClearScriptedInput();
				}
			}
		};
		Jump.OnFrame = [=]() { Watch(1.5f); };
		Jump.Verify = [=, this]()
		{
			Report(K.Name);
			if (AProtagonistPawn* Body = Prot() ? Prot()->GetBody() : nullptr)
			{
				Body->ClearScriptedInput();
			}
		};
		Phases.Add(MoveTemp(Jump));
	}
}
