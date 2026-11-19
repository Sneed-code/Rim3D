// Smoke test `idle` (docs/design/pawns.md "Standing still" and "No pops"): a colonist stands, turns to face others,
// walks off and stops, takes hits and swings twice in a row, and every frame its bones are measured.
//
// What fails it:
// - a pop: a bone of the body (hips, chest, head) or a foot or hand jumping in one frame, measured as the bone's
//   acceleration against the actor (so the actor's own travel is not counted, but the clips' set-back is);
// - a sliding foot while standing: the feet must stay where they were planted through the standing idle, through a
//   drift from one standing idle to another, and through a small standing turn (the upper body twists over them);
// - a standing pawn not on one of the standing idles, or no drift to another idle in 50 s of standing;
// - planted feet sliding while walking and stopping: a foot on the ground moving faster than it should (the numbers are
//   reported for tuning as well).
//
// The bones are read at the end of each frame (after the pawns update), as drawn, like the `motion` test does. A frame
// longer than 0.05 s (the harness's screenshot) and the two after it are left out of the pop measures.
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
#include "ColonySim/FightResponse.h"
#include "ColonySim/Sim.h"

namespace
{
	const FVector IdleSmokeOrigin(320000.0, 0.0, 0.0);

	// The bones watched: the body (a pop there is a reset anyone sees), the feet and the hands.
	enum class EIdleSmokeGroup : uint8 { Body, Feet, Hands };
	struct FIdleSmokeBone
	{
		const TCHAR* Name;
		EIdleSmokeGroup Group;
	};
	const FIdleSmokeBone IdleSmokeBones[] = {
		{TEXT("pelvis"), EIdleSmokeGroup::Body}, {TEXT("spine_03"), EIdleSmokeGroup::Body}, {TEXT("head"), EIdleSmokeGroup::Body},
		{TEXT("foot_l"), EIdleSmokeGroup::Feet}, {TEXT("foot_r"), EIdleSmokeGroup::Feet}, {TEXT("ball_l"), EIdleSmokeGroup::Feet},
		{TEXT("ball_r"), EIdleSmokeGroup::Feet}, {TEXT("hand_l"), EIdleSmokeGroup::Hands}, {TEXT("hand_r"), EIdleSmokeGroup::Hands},
	};
	constexpr int32 IdleSmokeBoneCount = UE_ARRAY_COUNT(IdleSmokeBones);
	// The feet among them (for the slide checks): foot_l, foot_r, ball_l, ball_r.
	constexpr int32 IdleSmokeFeetFirst = 3;
	constexpr int32 IdleSmokeFeetCount = 4;

	// Pop limits (m/s^2 of a bone against the actor). A bone that jumps 5 cm in one frame at 60 fps is about 180 m/s^2;
	// a walk keeps the hips under 10, a heel strike the feet under about 60, a fast swing the hands under about 300.
	constexpr double IdleSmokePopBody = 60.0;
	constexpr double IdleSmokePopFeet = 200.0;
	constexpr double IdleSmokePopHands = 500.0;
	// A standing foot may wander this far (cm) from where it was planted (the idle's own sway is under 0.3 cm).
	constexpr double IdleSmokeStandDrift = 3.0;
	// Walking: a foot this close to its lowest height (cm) is on the ground; on the ground its mean speed may be this
	// much (cm/s; a foot that sticks is near 0, a gait played at the wrong speed slides at 30 and more).
	constexpr double IdleSmokeGroundBand = 4.0;
	constexpr double IdleSmokeWalkSlip = 25.0;

	/** The bones as drawn at the end of the last frame (FWorldDelegates::OnWorldTickStart, before the next frame moves anything). */
	struct FIdleSmokeSample
	{
		bool bValid = false;
		double Seconds = 0.0;  // the world's time then
		FVector Local[IdleSmokeBoneCount];  // against the actor, in the actor's frame (cm)
		FVector World[IdleSmokeBoneCount];
		bool bStandingIdle = false;
		float TwistDegrees = 0.0f;
		float FootHold = 0.0f;   // the feet held on the ground (they are then measured in the world, not against the actor)
		bool bStepping = false;  // a standing step under way
		float PlayRate = 1.0f;
		FName Playing;
		FVector ActorAt = FVector::ZeroVector;
		float ActorYaw = 0.0f;
		float WalkLock = 0.0f;  // the walking foot lock's weight, and the feet it holds
		int32 WalkFeetHeld = 0;
		int32 WalkHeldMask = 0;  // which feet the walking lock holds fully (bit 0 left, bit 1 right)
	};

	struct FIdleSmokeState
	{
		colony::Entity Pawn = colony::kNoEntity;
		colony::Entity Others[2] = {colony::kNoEntity, colony::kNoEntity};
		colony::CellPos Home;
		// The pop meter.
		FIdleSmokeSample Last[2];  // one and two samples ago
		int32 Have = 0;
		int32 SkipFrames = 0;
		int32 Skipped = 0;
		double MaxPop[3] = {0.0, 0.0, 0.0};  // by EIdleSmokeGroup
		FString MaxPopAt[3];
		// Standing feet.
		bool bPlanted = false;
		double PlantedFor = 0.0;
		FVector Planted[IdleSmokeFeetCount];
		double MaxStandDrift = 0.0;
		FString MaxStandDriftAt;
		int32 Frames = 0;
		int32 StandingFrames = 0;
		int32 OffIdleFrames = 0;  // standing idle, but not one of the standing idles
		int32 TurnClipFrames = 0;  // frames that played a turn, pivot or reface clip (none may)
		FString TurnClipSeen;
		int32 IdlesStartedAtStart = 0;
		int32 IdleDriftsAtStart = 0;
		float MaxTwist = 0.0f;
		// Walking feet: every sample's feet (world) and time, judged in Verify.
		TArray<double> WalkTimes;
		TArray<FName> WalkPlaying;
		TArray<FVector> WalkFeet[IdleSmokeFeetCount];
		int32 Step = 0;  // the phase's own script
		double StepAt = 0.0;
	};

	void IdleSmokeRead(const ASimPawnActor& Actor, FIdleSmokeSample& Out)
	{
		const USkeletalMeshComponent* Body = Actor.GetBody();
		Out.bValid = Body != nullptr;
		if (!Body)
		{
			return;
		}
		const FTransform ActorTransform = Actor.GetActorTransform();
		for (int32 i = 0; i < IdleSmokeBoneCount; ++i)
		{
			const FName Name(IdleSmokeBones[i].Name);
			const FVector World = Body->GetBoneIndex(Name) != INDEX_NONE ? Body->GetBoneLocation(Name) : Body->GetComponentLocation();
			Out.World[i] = World;
			Out.Local[i] = ActorTransform.InverseTransformPosition(World);
		}
		const USimPawnAnimInstance* Anim = Actor.GetAnim();
		Out.bStandingIdle = Anim && Anim->GetStats().bStandingIdle;
		Out.TwistDegrees = Anim ? Anim->GetStats().StandTwistDegrees : 0.0f;
		Out.FootHold = Anim ? Anim->GetStats().FootHold : 0.0f;
		Out.bStepping = Anim && Anim->GetStats().bFootStepping;
		Out.PlayRate = Anim ? Anim->GetStats().PlayRate : 1.0f;
		Out.Playing = Anim ? Anim->GetStats().PlayingAsset : NAME_None;
		Out.ActorAt = Actor.GetActorLocation();
		Out.ActorYaw = static_cast<float>(Actor.GetActorRotation().Yaw);
		Out.WalkLock = Anim ? Anim->GetStats().WalkLock : 0.0f;
		Out.WalkFeetHeld = Anim ? Anim->GetStats().WalkFeetHeld : 0;
		Out.WalkHeldMask = Anim ? Anim->GetStats().WalkHeldMask : 0;
	}
}

void USmokeTestSubsystem::BuildIdlePhases()
{
	const TSharedRef<FIdleSmokeState> State = MakeShared<FIdleSmokeState>();
	const TSharedRef<FIdleSmokeSample> Sample = MakeShared<FIdleSmokeSample>();
	const TSharedRef<FDelegateHandle> SampleHook = MakeShared<FDelegateHandle>();
	// The world's time of the frame the bones were drawn in, taken during that frame (whether the next frame's start
	// already counts its own time or not, the sample is stamped with the frame it shows).
	const TSharedRef<FDelegateHandle> TimeHook = MakeShared<FDelegateHandle>();
	const TSharedRef<double> DrawnSeconds = MakeShared<double>(0.0);
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto ActorOf = [=]() -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(State->Pawn) : nullptr; };
	const auto View = [=, this]()
	{
		if (const ASimPawnActor* Actor = ActorOf())
		{
			const FVector P = Actor->GetActorLocation();
			const FVector Eye = P + FVector(-420.0, 330.0, 240.0);
			ViewFrom(Eye, (P + FVector(0.0, 0.0, 80.0) - Eye).Rotation());
		}
	};
	const auto PhaseName = [this]() { return Phases.IsValidIndex(PhaseIndex) ? Phases[PhaseIndex].Name : FString(); };
	const auto IsIdle = [=](FName Asset)
	{
		const USimPawnSubsystem* P = Pawns();
		return P && P->GetIdleLoops().ContainsByPredicate([Asset](const TObjectPtr<UAnimSequence>& Idle) { return Idle && Idle->GetFName() == Asset; });
	};
	// Every frame of every phase: the pop meter and the standing feet.
	const auto Measure = [=, this]()
	{
		View();
		const ASimPawnActor* Actor = ActorOf();
		if (!Actor || !Sample->bValid)
		{
			State->Have = 0;
			return;
		}
		++State->Frames;
		const FIdleSmokeSample Now = *Sample;
		if (const FString Clip = Now.Playing.ToString(); Clip.Contains(TEXT("Turn")) || Clip.Contains(TEXT("Pivot")) || Clip.Contains(TEXT("Reface")))
		{
			++State->TurnClipFrames;
			State->TurnClipSeen = FString::Printf(TEXT("%s in phase %s at %.2f s"), *Clip, *PhaseName(), PhaseTime);
		}
		const bool bStall = GetWorld()->GetDeltaSeconds() > 0.05f;
		if (bStall)
		{
			State->SkipFrames = 3;
		}
		if (State->SkipFrames > 0)
		{
			--State->SkipFrames;
			++State->Skipped;
			State->Have = 0;
		}
		else if (State->Have >= 2 && Now.Seconds > State->Last[0].Seconds && State->Last[0].Seconds > State->Last[1].Seconds)
		{
			const double Dt1 = State->Last[0].Seconds - State->Last[1].Seconds;
			const double Dt2 = Now.Seconds - State->Last[0].Seconds;
			// Feet held on the ground are measured where they are, in the world (against a turning actor they swing
			// round although they stand still); everything else against the actor, so its own travel is not counted.
			const bool bHeld = Now.FootHold > 0.0f && State->Last[0].FootHold > 0.0f && State->Last[1].FootHold > 0.0f;
			for (int32 i = 0; i < IdleSmokeBoneCount; ++i)
			{
				const int32 Group = static_cast<int32>(IdleSmokeBones[i].Group);
				// A walking foot held down where it landed is measured in the world too (against an actor whose speed
				// wavers frame to frame it would seem to jump while it stands still): its own side, all three samples.
				const int32 SideBit = FString(IdleSmokeBones[i].Name).EndsWith(TEXT("_r")) ? 2 : 1;
				const bool bWalkHeld = (Now.WalkHeldMask & State->Last[0].WalkHeldMask & State->Last[1].WalkHeldMask & SideBit) != 0;
				const bool bWorld = (bHeld || bWalkHeld) && Group == static_cast<int32>(EIdleSmokeGroup::Feet);
				const FVector& P0 = bWorld ? State->Last[1].World[i] : State->Last[1].Local[i];
				const FVector& P1 = bWorld ? State->Last[0].World[i] : State->Last[0].Local[i];
				const FVector& P2 = bWorld ? Now.World[i] : Now.Local[i];
				const FVector V1 = (P1 - P0) / Dt1;
				const FVector V2 = (P2 - P1) / Dt2;
				const double Accel = (V2 - V1).Size() / (0.5 * (Dt1 + Dt2)) / 100.0;  // m/s^2
				const double Limits[3] = {IdleSmokePopBody, IdleSmokePopFeet, IdleSmokePopHands};
				if (Accel > 0.5 * Limits[Group])
				{
					// Every frame near or over a limit, with what was going on, for finding the cause in the log.
					UE_LOG(LogTemp, Log, TEXT("IDLETRACE %s t=%.3f bone=%s accel=%.0f %s playing=%s rate=%.2f standing=%d hold=%.2f step=%d twist=%.1f yaw=%.1f dyaw=%.2f speed=%.0f walklock=%.2f walkheld=%d dt=%.4f"),
						*PhaseName(), PhaseTime, IdleSmokeBones[i].Name, Accel, bWorld ? TEXT("world") : TEXT("local"), *Now.Playing.ToString(), Now.PlayRate,
						Now.bStandingIdle ? 1 : 0, Now.FootHold, Now.bStepping ? 1 : 0, Now.TwistDegrees, Now.ActorYaw,
						FMath::FindDeltaAngleDegrees(State->Last[0].ActorYaw, Now.ActorYaw), FVector::Dist2D(Now.ActorAt, State->Last[0].ActorAt) / Dt2, Now.WalkLock, Now.WalkFeetHeld, Dt2);
				}
				if (Accel > State->MaxPop[Group])
				{
					State->MaxPop[Group] = Accel;
					State->MaxPopAt[Group] = FString::Printf(TEXT("%s in phase %s at %.2f s, playing %s"), IdleSmokeBones[i].Name, *PhaseName(), PhaseTime,
						*Now.Playing.ToString());
				}
			}
		}
		if (State->SkipFrames == 0)
		{
			State->Last[1] = State->Last[0];
			State->Last[0] = Now;
			State->Have = FMath::Min(State->Have + 1, 2);
		}

		// Standing: the feet stay where they were planted (after the 0.4 s blend into the idle has settled), through idle
		// drifts and standing turns, until the pawn moves or its feet pivot round (a turn past the twist's reach).
		// A standing step is a foot meant to move: the place is taken again once it has landed.
		const bool bPivotLimit = Now.bStepping;
		if (Now.bStandingIdle)
		{
			++State->StandingFrames;
			State->OffIdleFrames += IsIdle(Now.Playing) ? 0 : 1;
			State->MaxTwist = FMath::Max(State->MaxTwist, FMath::Abs(Now.TwistDegrees));
		}
		if (!Now.bStandingIdle || bPivotLimit)
		{
			State->bPlanted = false;
			State->PlantedFor = 0.0;
			return;
		}
		State->PlantedFor += GetWorld()->GetDeltaSeconds();
		if (State->PlantedFor < 0.6)
		{
			return;
		}
		if (!State->bPlanted)
		{
			for (int32 i = 0; i < IdleSmokeFeetCount; ++i)
			{
				State->Planted[i] = Now.World[IdleSmokeFeetFirst + i];
			}
			State->bPlanted = true;
			return;
		}
		for (int32 i = 0; i < IdleSmokeFeetCount; ++i)
		{
			const double Drift = FVector::Dist2D(Now.World[IdleSmokeFeetFirst + i], State->Planted[i]);
			if (Drift > State->MaxStandDrift)
			{
				State->MaxStandDrift = Drift;
				State->MaxStandDriftAt = FString::Printf(TEXT("%s in phase %s at %.2f s, playing %s, twist %.0f degrees"),
					IdleSmokeBones[IdleSmokeFeetFirst + i].Name, *PhaseName(), PhaseTime, *Now.Playing.ToString(), Now.TwistDegrees);
			}
		}
	};
	// The pop limits, checked at the end of each phase (and reset for the next).
	const auto VerifyPops = [=, this](const TCHAR* What)
	{
		static const TCHAR* const GroupNames[3] = {TEXT("body"), TEXT("feet"), TEXT("hands")};
		const double Limits[3] = {IdleSmokePopBody, IdleSmokePopFeet, IdleSmokePopHands};
		for (int32 g = 0; g < 3; ++g)
		{
			PhaseNumbers.Add(FString::Printf(TEXT("maxPop_%s_ms2"), GroupNames[g]), State->MaxPop[g]);
			if (State->MaxPop[g] > Limits[g])
			{
				Fail(FString::Printf(TEXT("%s: a pop: the %s jumped (%.0f m/s^2, limit %.0f): %s"), What, GroupNames[g], State->MaxPop[g], Limits[g], *State->MaxPopAt[g]));
			}
			else if (State->MaxPop[g] > 0.0)
			{
				Notes.Add(FString::Printf(TEXT("%s: largest %s acceleration %.0f m/s^2 (%s)"), What, GroupNames[g], State->MaxPop[g], *State->MaxPopAt[g]));
			}
			State->MaxPop[g] = 0.0;
			State->MaxPopAt[g].Reset();
		}
		PhaseNumbers.Add(TEXT("stallFramesSkipped"), State->Skipped);
		State->Skipped = 0;
		PhaseNumbers.Add(TEXT("turnClipFrames"), State->TurnClipFrames);
		if (State->TurnClipFrames > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d frames played a turn/pivot clip (they spin the legs under the body): last %s"), What, State->TurnClipFrames,
				*State->TurnClipSeen));
		}
		State->TurnClipFrames = 0;
	};
	const auto VerifyStand = [=, this](const TCHAR* What)
	{
		PhaseNumbers.Add(TEXT("maxStandFootDriftCm"), State->MaxStandDrift);
		PhaseNumbers.Add(TEXT("standingFrames"), State->StandingFrames);
		PhaseNumbers.Add(TEXT("maxTwistDegrees"), State->MaxTwist);
		if (State->MaxStandDrift > IdleSmokeStandDrift)
		{
			Fail(FString::Printf(TEXT("%s: a standing foot slid %.1f cm (limit %.1f): %s"), What, State->MaxStandDrift, IdleSmokeStandDrift, *State->MaxStandDriftAt));
		}
		if (State->OffIdleFrames > 0)
		{
			Fail(FString::Printf(TEXT("%s: %d standing frames played something other than a standing idle"), What, State->OffIdleFrames));
		}
		State->MaxStandDrift = 0.0;
		State->MaxStandDriftAt.Reset();
		State->StandingFrames = 0;
		State->OffIdleFrames = 0;
		State->MaxTwist = 0.0f;
	};
	const auto Cell = [=](int32 Dx, int32 Dy) { return colony::CellPos{State->Home.x + Dx, State->Home.y + Dy, State->Home.level}; };

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
			B->SetWorldOrigin(IdleSmokeOrigin);
			if (!B->StartSim(Options))
			{
				Fail(TEXT("setup: the game's data did not load"));
				return;
			}
			B->SetSpeed(1);
			int32 Found = 0;
			for (const colony::ThingView& Thing : B->GetCurrentSnapshot().things)
			{
				if (Thing.category == colony::ThingCategory::Pawn && B->GetSim()->colonistOf(Thing.id))
				{
					if (Found == 0)
					{
						State->Pawn = Thing.id;
						State->Home = colony::CellPos{FMath::FloorToInt32(Thing.x), FMath::FloorToInt32(Thing.y), Thing.level};
					}
					else if (Found <= 2)
					{
						State->Others[Found - 1] = Thing.id;
					}
					++Found;
				}
			}
			if (Found < 3)
			{
				Fail(FString::Printf(TEXT("setup: %d colonists (3 needed)"), Found));
				return;
			}
			// The colonist stands at home; the two others stand 5 m ahead and 5 m ahead, 3 m to the side (31 degrees apart).
			for (int32 i = 0; i < 3; ++i)
			{
				B->Submit(colony::DraftCommand{i == 0 ? State->Pawn : State->Others[i - 1], true});
			}
			B->Submit(colony::MoveOrderCommand{State->Pawn, Cell(0, 0)});
			B->Submit(colony::MoveOrderCommand{State->Others[0], Cell(5, 0)});
			B->Submit(colony::MoveOrderCommand{State->Others[1], Cell(5, 3)});
			const TWeakObjectPtr<USmokeTestSubsystem> Self(this);
			*TimeHook = FWorldDelegates::OnWorldPostActorTick.AddLambda([Self, DrawnSeconds](UWorld* World, ELevelTick, float)
			{
				const USmokeTestSubsystem* Smoke = Self.Get();
				if (Smoke && World == Smoke->GetWorld())
				{
					*DrawnSeconds = World->GetTimeSeconds();
				}
			});
			*SampleHook = FWorldDelegates::OnWorldTickStart.AddLambda([Self, State, Sample, DrawnSeconds](UWorld* World, ELevelTick, float)
			{
				const USmokeTestSubsystem* Smoke = Self.Get();
				if (!Smoke || World != Smoke->GetWorld())
				{
					return;
				}
				const USimPawnSubsystem* P = World->GetSubsystem<USimPawnSubsystem>();
				const ASimPawnActor* Actor = P ? P->FindPawn(State->Pawn) : nullptr;
				Sample->bValid = false;
				if (Actor)
				{
					IdleSmokeRead(*Actor, *Sample);
					Sample->Seconds = *DrawnSeconds;
				}
			});
		};
		Phase.WarmupSeconds = 6.0f;
		Phase.MeasureSeconds = 0.5f;
		Phase.OnFrame = View;
		Phase.Verify = [=, this]()
		{
			if (!ActorOf())
			{
				Fail(TEXT("setup: the colonist has no pawn actor"));
			}
			if (Pawns() && Pawns()->GetIdleLoops().Num() < 2)
			{
				Fail(FString::Printf(TEXT("setup: %d standing idles loaded (run -run=MakeHumanArt -Clips for the Mixamo set)"), Pawns() ? Pawns()->GetIdleLoops().Num() : 0));
			}
			State->Have = 0;
		};
		Phases.Add(MoveTemp(Phase));
	}

	// Standing for 50 s: one of the standing idles all the way, the feet where they were planted, and at least one drift
	// to another idle on the same footprint (every 14 to 40 s).
	{
		FPhase Phase;
		Phase.Name = TEXT("stand");
		Phase.Setup = [=]()
		{
			const USimPawnAnimInstance* Anim = ActorOf() ? ActorOf()->GetAnim() : nullptr;
			State->IdlesStartedAtStart = Anim ? Anim->GetStats().IdlesStarted : 0;
			State->IdleDriftsAtStart = Anim ? Anim->GetStats().IdleDrifts : 0;
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 50.0f;
		Phase.OnFrame = Measure;
		Phase.Verify = [=, this]()
		{
			VerifyPops(TEXT("stand"));
			const int32 Frames = FMath::Max(State->Frames, 1);
			PhaseNumbers.Add(TEXT("standingShare"), static_cast<double>(State->StandingFrames) / Frames);
			if (State->StandingFrames * 10 < Frames * 9)
			{
				Fail(FString::Printf(TEXT("stand: the standing idle played in only %d of %d frames"), State->StandingFrames, Frames));
			}
			State->Frames = 0;
			const USimPawnAnimInstance* Anim = ActorOf() ? ActorOf()->GetAnim() : nullptr;
			const int32 Drifts = Anim ? Anim->GetStats().IdleDrifts - State->IdleDriftsAtStart : 0;
			PhaseNumbers.Add(TEXT("idleDrifts"), Drifts);
			PhaseNumbers.Add(TEXT("idleSeamBlends"), Anim ? Anim->GetStats().IdleSeams : 0);
			const USimPawnSubsystem* P = Pawns();
			if (Drifts < 1 && P && P->GetIdleLoops().Num() >= 2)
			{
				Fail(TEXT("stand: in 50 s of standing the pawn never drifted to another standing idle"));
			}
			VerifyStand(TEXT("stand"));
		};
		Phases.Add(MoveTemp(Phase));
	}

	// Standing turns: locked on to the colonist ahead, then the one 31 degrees to the side (the feet stay, the body twists),
	// then the first again, then let go.
	{
		FPhase Phase;
		Phase.Name = TEXT("turn");
		Phase.Setup = [=]() { State->Step = 0; State->StepAt = 0.0; };
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 12.0f;
		Phase.OnFrame = [=, this]()
		{
			Measure();
			USimSubsystem* B = Bridge();
			if (!B)
			{
				return;
			}
			const double Times[4] = {0.0, 3.0, 6.0, 9.0};
			if (State->Step < 4 && PhaseTime >= Times[State->Step])
			{
				switch (State->Step)
				{
					case 0: B->Submit(colony::FocusCommand{State->Pawn, State->Others[0], colony::FocusMode::Set}); break;
					case 1: B->Submit(colony::FocusCommand{State->Pawn, State->Others[1], colony::FocusMode::Set}); break;
					case 2: B->Submit(colony::FocusCommand{State->Pawn, State->Others[0], colony::FocusMode::Set}); break;
					default: B->Submit(colony::FocusCommand{State->Pawn, colony::kNoEntity, colony::FocusMode::Clear}); break;
				}
				++State->Step;
			}
		};
		Phase.Verify = [=, this]()
		{
			VerifyPops(TEXT("turn"));
			if (State->MaxTwist < 5.0f)
			{
				Notes.Add(FString::Printf(TEXT("turn: the standing body twisted at most %.0f degrees (did it turn to face the others?)"), State->MaxTwist));
			}
			VerifyStand(TEXT("turn"));
		};
		Phases.Add(MoveTemp(Phase));
	}

	// Walking off 8 m and back, stopping each time: starts and stops without a pop, planted feet that stick.
	{
		FPhase Phase;
		Phase.Name = TEXT("walk");
		Phase.Setup = [=]()
		{
			State->Step = 0;
			State->WalkTimes.Reset();
			State->WalkPlaying.Reset();
			for (TArray<FVector>& Foot : State->WalkFeet)
			{
				Foot.Reset();
			}
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 16.0f;
		Phase.OnFrame = [=, this]()
		{
			Measure();
			USimSubsystem* B = Bridge();
			if (B && State->Step == 0)
			{
				B->Submit(colony::MoveOrderCommand{State->Pawn, Cell(-8, 0)});
				State->Step = 1;
			}
			else if (B && State->Step == 1 && PhaseTime > 8.0f)
			{
				B->Submit(colony::MoveOrderCommand{State->Pawn, Cell(0, 0)});
				State->Step = 2;
			}
			if (Sample->bValid && GetWorld()->GetDeltaSeconds() <= 0.05f)
			{
				State->WalkTimes.Add(Sample->Seconds);
				State->WalkPlaying.Add(Sample->Playing);
				for (int32 i = 0; i < IdleSmokeFeetCount; ++i)
				{
					State->WalkFeet[i].Add(Sample->World[IdleSmokeFeetFirst + i]);
				}
			}
		};
		Phase.Verify = [=, this]()
		{
			VerifyPops(TEXT("walk"));
			VerifyStand(TEXT("walk"));
			// Each foot: on the ground when within a few cm of its lowest; its mean speed there.
			double WorstSlip = 0.0;
			const TCHAR* WorstFoot = TEXT("");
			for (int32 i = 0; i < IdleSmokeFeetCount; ++i)
			{
				const TArray<FVector>& Foot = State->WalkFeet[i];
				if (Foot.Num() < 10)
				{
					continue;
				}
				// The foot's lowest within half a second either side: the ground under it then (the map is flat, but
				// this way a gentle slope does not count as lifted feet).
				const auto LowestNear = [&](int32 k)
				{
					double Lowest = TNumericLimits<double>::Max();
					for (int32 j = k; j >= 0 && State->WalkTimes[k] - State->WalkTimes[j] < 0.5; --j)
					{
						Lowest = FMath::Min(Lowest, Foot[j].Z);
					}
					for (int32 j = k; j < Foot.Num() && State->WalkTimes[j] - State->WalkTimes[k] < 0.5; ++j)
					{
						Lowest = FMath::Min(Lowest, Foot[j].Z);
					}
					return Lowest;
				};
				double Moved = 0.0;
				double Time = 0.0;
				for (int32 k = 1; k < Foot.Num(); ++k)
				{
					const double Dt = State->WalkTimes[k] - State->WalkTimes[k - 1];
					const double Ground = LowestNear(k);
					if (Dt <= 0.0 || Dt > 0.05 || Foot[k].Z > Ground + IdleSmokeGroundBand || Foot[k - 1].Z > Ground + IdleSmokeGroundBand)
					{
						continue;
					}
					const double Speed = FVector::Dist2D(Foot[k], Foot[k - 1]) / Dt;
					if (Speed > 2.0 * IdleSmokeWalkSlip)
					{
						UE_LOG(LogTemp, Log, TEXT("IDLETRACE walk-slip foot=%s t=%.2f speed=%.0f cm/s playing=%s"), IdleSmokeBones[IdleSmokeFeetFirst + i].Name,
							State->WalkTimes[k] - State->WalkTimes[0], Speed, *State->WalkPlaying[k].ToString());
					}
					Moved += FVector::Dist2D(Foot[k], Foot[k - 1]);
					Time += Dt;
				}
				const double Slip = Time > 0.5 ? Moved / Time : 0.0;
				PhaseNumbers.Add(FString::Printf(TEXT("groundSlip_%s_cms"), IdleSmokeBones[IdleSmokeFeetFirst + i].Name), Slip);
				if (Slip > WorstSlip)
				{
					WorstSlip = Slip;
					WorstFoot = IdleSmokeBones[IdleSmokeFeetFirst + i].Name;
				}
			}
			Notes.Add(FString::Printf(TEXT("walk: planted feet slide at most %.1f cm/s on average (%s)"), WorstSlip, WorstFoot));
			if (WorstSlip > IdleSmokeWalkSlip)
			{
				Fail(FString::Printf(TEXT("walk: a planted foot (%s) slid %.1f cm/s on average while on the ground (limit %.0f)"), WorstFoot, WorstSlip, IdleSmokeWalkSlip));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	// Hits while standing and while walking: the react plays on the upper body without a pop, and bullets of a burst do
	// not restart it.
	{
		FPhase Phase;
		Phase.Name = TEXT("hits");
		Phase.Setup = [=]() { State->Step = 0; State->StepAt = 0.0; };
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 10.0f;
		Phase.OnFrame = [=, this]()
		{
			Measure();
			ASimPawnActor* Actor = ActorOf();
			USimSubsystem* B = Bridge();
			if (!Actor || !B)
			{
				return;
			}
			// Standing: a burst of three from the front (0.1 s apart), a stagger from the left at 2 s; then walking off at
			// 4 s, a burst from behind at 5 s and a stagger at 6.5 s.
			struct FHit { double At; FVector From; bool bStagger; };
			const FVector Fwd = Actor->GetActorForwardVector();
			const FVector Right = Actor->GetActorRightVector();
			const FHit Hits[] = {{0.5, Fwd, false}, {0.6, Fwd, false}, {0.7, Fwd, false}, {2.0, -Right, true},
				{5.0, -Fwd, false}, {5.1, -Fwd, false}, {5.2, -Fwd, false}, {6.5, Right, true}};
			if (State->Step < static_cast<int32>(UE_ARRAY_COUNT(Hits)) && PhaseTime >= Hits[State->Step].At)
			{
				// The shot travels from where it came from towards the body.
				Actor->PlayHitClip(-Hits[State->Step].From, Hits[State->Step].bStagger);
				++State->Step;
			}
			if (State->StepAt == 0.0 && PhaseTime >= 4.0f)
			{
				B->Submit(colony::MoveOrderCommand{State->Pawn, Cell(-6, 2)});
				State->StepAt = PhaseTime;
			}
		};
		Phase.Verify = [=, this]()
		{
			VerifyPops(TEXT("hits"));
			VerifyStand(TEXT("hits"));
		};
		Phases.Add(MoveTemp(Phase));
	}

	// Two swings in a row, standing: the second takes over from the first without the body jumping (the set-back of
	// the one going out is kept while it fades), then the pawn settles back into its idle without a pop.
	{
		FPhase Phase;
		Phase.Name = TEXT("swings");
		Phase.Setup = [=]() { State->Step = 0; };
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 9.0f;
		Phase.OnFrame = [=, this]()
		{
			Measure();
			USimSubsystem* B = Bridge();
			if (!B || !B->GetSim())
			{
				return;
			}
			// Back home first (2.5 s), then a jab, a cross 0.35 s later (while the jab still plays), and a hook at 2 s after.
			const double Times[3] = {2.5, 2.85, 4.5};
			const TCHAR* const Clips[3] = {TEXT("Motion_Unarmed_Jab"), TEXT("Motion_Unarmed_Cross"), TEXT("Motion_Unarmed_Hook")};
			if (State->Step == 0 && PhaseTime < 0.1f)
			{
				B->Submit(colony::MoveOrderCommand{State->Pawn, Cell(0, 0)});
			}
			if (State->Step < 3 && PhaseTime >= Times[State->Step])
			{
				colony::Sim& Sim = *const_cast<colony::Sim*>(B->GetSim());
				const colony::DefId Clip = Sim.combat().motion().clipId(TCHAR_TO_UTF8(Clips[State->Step]));
				if (!Clip.valid() || !Sim.combat().motion().play(State->Pawn, Clip, 0.0f, 0.0f, 0, false))
				{
					Notes.Add(FString::Printf(TEXT("swings: the sim would not play %s"), Clips[State->Step]));
				}
				++State->Step;
			}
		};
		Phase.Verify = [=, this]()
		{
			FWorldDelegates::OnWorldTickStart.Remove(*SampleHook);
			FWorldDelegates::OnWorldPostActorTick.Remove(*TimeHook);
			if (const USimPawnAnimInstance* Anim = ActorOf() ? ActorOf()->GetAnim() : nullptr)
			{
				// Turn, pivot and reface clips (and run clips at a walk) motion matching picked and was not let play.
				PhaseNumbers.Add(TEXT("bannedPicksAllPhases"), Anim->GetStats().BannedPicks);
			}
			VerifyPops(TEXT("swings"));
			VerifyStand(TEXT("swings"));
		};
		Phases.Add(MoveTemp(Phase));
	}
}
