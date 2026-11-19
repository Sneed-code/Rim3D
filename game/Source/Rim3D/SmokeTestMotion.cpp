// Smoke test `motion` (docs/design/body-motion.md): a colonist plays the sim's motion clips (a roll, staggers, a
// sidestep, a knockdown, the three punches) and every frame the drawn hips are measured against the sim's position. The hips must stay
// on the sim's position through the clip and must not jump when it ends.
//
// The hips are read at the end of each frame (after the pawns update), as they are drawn: this test's own tick runs
// before the pawn subsystem's, where the body's animation has already moved on for the frame but its set-back has not.
#include "SmokeTest.h"

#include "Engine/World.h"

#include "Components/SkeletalMeshComponent.h"

#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/BodyMotion.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Sim.h"

namespace
{
	const FVector MotionSmokeOrigin(300000.0, 0.0, 0.0);

	struct FMotionSmokeState
	{
		colony::Entity Pawn = colony::kNoEntity;
		FVector2D IdleOffset = FVector2D::ZeroVector;  // the idle hips against the actor, in the actor's frame (cm)
		int32 IdleSamples = 0;
		// The clip phase now playing.
		bool bStarted = false;
		bool bSeenClip = false;
		bool bEnded = false;
		float EndedAt = 0.0f;
		FVector Start = FVector::ZeroVector;
		FVector2D LastHips = FVector2D::ZeroVector;
		FVector2D LastActor = FVector2D::ZeroVector;
		bool bHaveLast = false;
		double MaxOff = 0.0;      // hips from the sim's position while the clip plays (cm)
		double MaxOffAfter = 0.0; // and in the 0.3 s after it ends (the blend-out)
		int32 UpperFrames = 0;    // frames an upper-body clip played, and of those the frames the legs walked
		int32 LegFrames = 0;
		double MaxJump = 0.0;     // the hips' largest one-frame move beyond the actor's own (cm)
		double MaxJumpAfter = 0.0;
		double Travel = 0.0;      // how far the sim moved the body (cm)
		bool bPrevSkipped = false;
		double StartYaw = 0.0;
		FVector LastSpot = FVector::ZeroVector;
		double LastYaw = 0.0;
		int32 StillFrames = 0;
		double MaxTurn = 0.0;     // how far the drawn body turned while the clip played (degrees)
		int32 Skipped = 0;
		bool bStallBefore = false;  // a stall one and two frames ago (the end-of-frame sample is a frame old)
		bool bStallBefore2 = false;
		FVector2D StillHips = FVector2D::ZeroVector;  // the hips against the actor summed over the still frames before the clip
		int32 StillHipsSamples = 0;
	};

	/** The hips and the actor as drawn, read at the end of the last frame (FWorldDelegates::OnWorldTickStart, before the next frame moves anything). */
	struct FMotionSmokeSample
	{
		bool bValid = false;
		FVector2D Local = FVector2D::ZeroVector;  // the hips against the actor, in the actor's frame (cm)
		FVector2D Hips = FVector2D::ZeroVector;
		FVector2D Actor = FVector2D::ZeroVector;
	};

	FVector2D MotionSmokeHips(const ASimPawnActor& Actor)
	{
		const USkeletalMeshComponent* Body = Actor.GetBody();
		if (!Body)
		{
			return FVector2D(Actor.GetActorLocation());
		}
		for (const TCHAR* Name : {TEXT("pelvis"), TEXT("Hips"), TEXT("hips")})
		{
			if (Body->GetBoneIndex(FName(Name)) != INDEX_NONE)
			{
				return FVector2D(Body->GetBoneLocation(FName(Name)));
			}
		}
		return FVector2D(Body->GetComponentLocation());
	}

	/** The hips against the actor in the actor's frame (x forward, y right), cm. */
	FVector2D MotionSmokeLocal(const ASimPawnActor& Actor)
	{
		const FVector2D Delta = MotionSmokeHips(Actor) - FVector2D(Actor.GetActorLocation());
		const FVector2D F(Actor.GetActorForwardVector());
		const FVector2D R(Actor.GetActorRightVector());
		return FVector2D(FVector2D::DotProduct(Delta, F.GetSafeNormal()), FVector2D::DotProduct(Delta, R.GetSafeNormal()));
	}
}

void USmokeTestSubsystem::BuildMotionPhases()
{
	const TSharedRef<FMotionSmokeState> State = MakeShared<FMotionSmokeState>();
	const TSharedRef<FMotionSmokeSample> Sample = MakeShared<FMotionSmokeSample>();
	const TSharedRef<FDelegateHandle> SampleHook = MakeShared<FDelegateHandle>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto ActorOf = [=]() -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(State->Pawn) : nullptr; };
	const auto View = [=, this]()
	{
		if (const ASimPawnActor* Actor = ActorOf())
		{
			const FVector P = Actor->GetActorLocation();
			const FVector Eye = P + FVector(-450.0, 350.0, 260.0);
			ViewFrom(Eye, (P + FVector(0.0, 0.0, 90.0) - Eye).Rotation());
		}
	};

	{
		FPhase Phase;
		Phase.Name = TEXT("setup");
		Phase.Setup = [=, this]()
		{
			USimSubsystem* B = Bridge();
			if (!B || !Pawns())
			{
				Fail(TEXT("setup: the sim or pawn subsystem did not start"));
				return;
			}
			FSimStartOptions Options;
			Options.Seed = 42;
			Options.MapSize = 128;
			Options.bFlatMap = true;
			Options.Scenario = TEXT("Crashlanded");
			B->SetWorldOrigin(MotionSmokeOrigin);
			if (!B->StartSim(Options))
			{
				Fail(TEXT("setup: the game's data did not load"));
				return;
			}
			for (const std::string& Problem : B->GetSim()->setupErrors())
			{
				Fail(FString::Printf(TEXT("setup: sim setup: %s"), UTF8_TO_TCHAR(Problem.c_str())));
			}
			B->SetSpeed(1);
			for (const colony::ThingView& Thing : B->GetCurrentSnapshot().things)
			{
				if (Thing.category == colony::ThingCategory::Pawn && B->GetSim()->colonistOf(Thing.id))
				{
					State->Pawn = Thing.id;
					break;
				}
			}
			if (State->Pawn == colony::kNoEntity)
			{
				Fail(TEXT("setup: no colonist"));
				return;
			}
			B->Submit(colony::DraftCommand{State->Pawn, true});
			const TWeakObjectPtr<USmokeTestSubsystem> Self(this);
			*SampleHook = FWorldDelegates::OnWorldTickStart.AddLambda([Self, State, Sample](UWorld* World, ELevelTick, float)
			{
				const USmokeTestSubsystem* Smoke = Self.Get();
				const USimPawnSubsystem* P = Smoke && World == Smoke->GetWorld() ? World->GetSubsystem<USimPawnSubsystem>() : nullptr;
				const ASimPawnActor* Actor = P ? P->FindPawn(State->Pawn) : nullptr;
				if (!Smoke || World != Smoke->GetWorld())
				{
					return;
				}
				Sample->bValid = Actor != nullptr;
				if (Actor)
				{
					Sample->Local = MotionSmokeLocal(*Actor);
					Sample->Hips = MotionSmokeHips(*Actor);
					Sample->Actor = FVector2D(Actor->GetActorLocation());
				}
			});
		};
		Phase.WarmupSeconds = 3.0f;
		Phase.MeasureSeconds = 0.5f;
		Phase.OnFrame = View;
		Phase.Verify = [=, this]()
		{
			if (!ActorOf())
			{
				Fail(TEXT("setup: the colonist has no pawn actor"));
			}
			const colony::Sim* Sim = Bridge() ? Bridge()->GetSim() : nullptr;
			if (Sim && !Sim->combat().motion().clip("Motion_Roll"))
			{
				Fail(TEXT("setup: no Motion_Roll def (data/defs/MotionClips)"));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		FPhase Phase;
		Phase.Name = TEXT("idle");
		Phase.Setup = []() {};
		Phase.WarmupSeconds = 1.0f;
		Phase.MeasureSeconds = 1.0f;
		Phase.OnFrame = [=]()
		{
			View();
			if (ActorOf() && Sample->bValid)
			{
				State->IdleOffset += Sample->Local;
				++State->IdleSamples;
			}
		};
		Phase.Verify = [=, this]()
		{
			if (State->IdleSamples > 0)
			{
				State->IdleOffset /= static_cast<double>(State->IdleSamples);
			}
			PhaseNumbers.Add(TEXT("idleHipsForwardCm"), State->IdleOffset.X);
			PhaseNumbers.Add(TEXT("idleHipsRightCm"), State->IdleOffset.Y);
		};
		Phases.Add(MoveTemp(Phase));
	}

	const TCHAR* const MotionSmokeClips[] = {TEXT("Motion_Roll"), TEXT("Motion_StaggerBack"), TEXT("Motion_StaggerLeft"), TEXT("Motion_SidestepRight"),
		TEXT("Motion_KnockdownForward"), TEXT("Motion_Unarmed_Jab"), TEXT("Motion_Unarmed_Cross"), TEXT("Motion_Unarmed_Hook")};
	for (const TCHAR* ClipName : MotionSmokeClips)
	{
		const FString Name = ClipName;
		const bool bLastClip = ClipName == MotionSmokeClips[UE_ARRAY_COUNT(MotionSmokeClips) - 1];
		FPhase Phase;
		Phase.Name = Name;
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = Name.Contains(TEXT("Knockdown")) ? 8.0f : 6.0f;
		Phase.Setup = [=]()
		{
			*State = FMotionSmokeState{State->Pawn, State->IdleOffset, State->IdleSamples};
		};
		Phase.OnFrame = [=, this]()
		{
			View();
			USimSubsystem* B = Bridge();
			ASimPawnActor* Actor = ActorOf();
			if (!B || !B->GetSim() || !Actor || !Sample->bValid)
			{
				return;
			}
			if (!State->bStarted)
			{
				// Wait until the colonist stands still (after the last clip a drafted colonist walks back to its post).
				const FVector Here = Actor->GetActorLocation();
				const double Turned = FMath::Abs(FMath::FindDeltaAngleDegrees(State->LastYaw, Actor->GetActorRotation().Yaw));
				State->StillFrames = FVector::Dist2D(Here, State->LastSpot) < 0.05 && Turned < 0.05 ? State->StillFrames + 1 : 0;
				// The idle hips of the standing idle playing now (each stop picks one of several, and they stand
				// their hips a few cm differently): measured over the still frames just before the clip.
				if (State->StillFrames == 1)
				{
					State->StillHips = FVector2D::ZeroVector;
					State->StillHipsSamples = 0;
				}
				if (State->StillFrames > 0)
				{
					State->StillHips += Sample->Local;
					++State->StillHipsSamples;
				}
				State->LastSpot = Here;
				State->LastYaw = Actor->GetActorRotation().Yaw;
				if (State->StillFrames < 30 && PhaseTime < 6.0f)
				{
					return;
				}
				colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
				const colony::DefId Clip = Sim.combat().motion().clipId(TCHAR_TO_UTF8(*Name));
				if (!Clip.valid() || !Sim.combat().motion().play(State->Pawn, Clip, 0.0f, 0.0f, 0, false))
				{
					Fail(FString::Printf(TEXT("%s: the sim would not play it"), *Name));
				}
				// A stagger is drawn on the upper body over walking legs, as a blow's stagger is (BodyMotionSystem::throwBody).
				if (Name.Contains(TEXT("Stagger")))
				{
					Sim.combat().motion().setLayers(State->Pawn, true, false);
				}
				if (State->StillHipsSamples >= 10)
				{
					State->IdleOffset = State->StillHips / static_cast<double>(State->StillHipsSamples);
				}
				State->bStarted = true;
				State->Start = Actor->GetActorLocation();
				State->StartYaw = Actor->GetActorRotation().Yaw;
				return;
			}
			// The hips against the sim's position, less the idle pose's own offset.
			const FVector2D Off = Sample->Local - State->IdleOffset;
			const FVector2D Hips = Sample->Hips;
			const FVector2D At = Sample->Actor;
			const double RawJump = State->bHaveLast ? FVector2D::Distance(Hips - State->LastHips, At - State->LastActor) : 0.0;
			State->LastHips = Hips;
			State->LastActor = At;
			State->bHaveLast = true;
			const bool bInClip = Actor->IsInMotionClip();
			// A stalled frame (the harness's screenshot, about 0.1 s) and the one after it, when the sim catches up,
			// are left out: the actor and the animation move on in different orders across a stall.
			// The sample is from the end of the last frame, so a stall shows a frame later: two frames after it are left out.
			const bool bStall = GetWorld()->GetDeltaSeconds() > 0.05f;
			const bool bSkip = bStall || State->bStallBefore || State->bStallBefore2;
			State->bStallBefore2 = State->bStallBefore;
			State->bStallBefore = bStall;
			const double Jump = bSkip || State->bPrevSkipped ? 0.0 : RawJump;
			State->bPrevSkipped = bSkip;
			if (bSkip)
			{
				++State->Skipped;
			}
			if (bInClip)
			{
				State->bSeenClip = true;
				// The legs under an upper-body clip: walking (motion matching off the idle), not standing still.
				if (Actor->IsMotionClipUpper())
				{
					++State->UpperFrames;
					const USimPawnAnimInstance* Anim = Actor->GetAnim();
					State->LegFrames += Anim && !Anim->GetStats().bStandingIdle && Actor->GetGroundSpeed() > 20.0f ? 1 : 0;
				}
			}
			if (bInClip && !bSkip)
			{
				State->MaxOff = FMath::Max(State->MaxOff, Off.Size());
				State->MaxTurn = FMath::Max(State->MaxTurn, FMath::Abs(FMath::FindDeltaAngleDegrees(State->StartYaw, Actor->GetActorRotation().Yaw)));
				State->MaxJump = FMath::Max(State->MaxJump, Jump);
			}
			else if (State->bSeenClip && !bInClip)
			{
				if (!State->bEnded)
				{
					State->bEnded = true;
					State->EndedAt = PhaseTime;
				}
				if (PhaseTime - State->EndedAt < 0.3f && !bSkip)  // the blend-out; after that a drafted colonist walks back to its post
				{
					State->MaxOffAfter = FMath::Max(State->MaxOffAfter, Off.Size());
					State->MaxJumpAfter = FMath::Max(State->MaxJumpAfter, Jump);
				}
			}
			State->Travel = FMath::Max(State->Travel, FVector::Dist2D(State->Start, Actor->GetActorLocation()));
			const colony::ThingView* Now = B->GetCurrentSnapshot().find(State->Pawn);
			const FVector Moved = Actor->GetActorLocation() - State->Start;
			const FVector MotionOffset = Actor->GetMotionOffset();
			UE_LOG(LogTemp, Log, TEXT("MOTIONTRACE %s t=%.3f clip=%d simclip=%d simt=%.2f moved=(%.1f,%.1f) yaw=%.0f hipsLocal=(%.1f,%.1f) off=%.1f setback=(%.1f,%.1f)"),
				*Name, PhaseTime, bInClip ? 1 : 0, Now && Now->motionClip.valid() ? 1 : 0, Now ? Now->motionTime : -1.0f, Moved.X, Moved.Y,
				Actor->GetActorRotation().Yaw, Off.X, Off.Y, Off.Size(), MotionOffset.X, MotionOffset.Y);
		};
		Phase.Verify = [=, this]()
		{
			if (bLastClip)
			{
				FWorldDelegates::OnWorldTickStart.Remove(*SampleHook);
			}
			PhaseNumbers.Add(TEXT("simTravelCm"), State->Travel);
			PhaseNumbers.Add(TEXT("maxHipsOffCm"), State->MaxOff);
			PhaseNumbers.Add(TEXT("maxHipsOffAfterCm"), State->MaxOffAfter);
			PhaseNumbers.Add(TEXT("maxFrameJumpCm"), State->MaxJump);
			PhaseNumbers.Add(TEXT("maxFrameJumpAfterCm"), State->MaxJumpAfter);
			PhaseNumbers.Add(TEXT("stallFramesSkipped"), State->Skipped);
			PhaseNumbers.Add(TEXT("maxTurnDegrees"), State->MaxTurn);
			if (State->MaxTurn > 20.0)
			{
				Fail(FString::Printf(TEXT("%s: the body turned %.0f degrees while the clip played (it should keep the sim's facing)"), *Name, State->MaxTurn));
			}
			Notes.Add(FString::Printf(TEXT("%s: sim moved the body %.0f cm; hips off the sim position at most %.1f cm while playing, %.1f cm in the 0.3 s after; largest one-frame hip jump %.1f cm playing, %.1f cm after; turned %.0f degrees"),
				*Name, State->Travel, State->MaxOff, State->MaxOffAfter, State->MaxJump, State->MaxJumpAfter, State->MaxTurn));
			if (!State->bSeenClip)
			{
				Fail(FString::Printf(TEXT("%s: the pawn actor never played the clip"), *Name));
			}
			else if (!State->bEnded)
			{
				Fail(FString::Printf(TEXT("%s: the clip never ended"), *Name));
			}
			if (State->MaxOff > 15.0 || State->MaxOffAfter > 15.0)
			{
				Fail(FString::Printf(TEXT("%s: the hips left the sim's position by %.1f cm"), *Name, FMath::Max(State->MaxOff, State->MaxOffAfter)));
			}
			if (Name.Contains(TEXT("Stagger")))
			{
				PhaseNumbers.Add(TEXT("upperFrames"), State->UpperFrames);
				PhaseNumbers.Add(TEXT("walkingLegFrames"), State->LegFrames);
				if (State->UpperFrames == 0)
				{
					Fail(FString::Printf(TEXT("%s: the stagger was not drawn on the upper body"), *Name));
				}
				else if (State->LegFrames * 3 < State->UpperFrames)
				{
					Fail(FString::Printf(TEXT("%s: the legs walked in only %d of %d frames of the stagger (they should carry the stumble)"), *Name,
						State->LegFrames, State->UpperFrames));
				}
			}
			if (State->MaxJumpAfter > 8.0)
			{
				Fail(FString::Printf(TEXT("%s: the hips jumped %.1f cm in one frame after the clip"), *Name, State->MaxJumpAfter));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}
}
