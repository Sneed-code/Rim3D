// Smoke test `gore` (docs/design/blood.md, sneed 2026-11-20): a real body shot in the groin from the front (the exit
// must be found on the body, and a close-up shows the wound with no square or shorts-shaped stain), big blood spray in
// the air on a blow and a shot, thirty hits on one spot in three seconds (a burst of pictures to see no red flashing,
// and the skin blood must stay), a head hit that visibly snaps the head, an arm shot off the living body, the corpse
// that settles and sleeps, and a blast that throws the corpse and tears it apart.
#include "SmokeTest.h"

#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#include "BlastFxSubsystem.h"
#include "CreatorPeople.h"
#include "FightSubsystem.h"
#include "GoreGibActor.h"
#include "BloodSubsystem.h"
#include "PawnActor.h"
#include "PawnAnimInstance.h"
#include "PawnSubsystem.h"
#include "SeveredPieceActor.h"
#include "SimSubsystem.h"
#include "SkinBlood.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Health.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

namespace
{
	const FVector GoreSmokeOrigin(360000.0, 0.0, 0.0);

	struct FGoreSmokeState
	{
		colony::Entity Pawn = colony::kNoEntity;
		int32 ExitsAtStart = 0;
		int32 HitsSent = 0;
		int32 SprayAtStart = 0;
		int32 MaxSprayInAir = 0;
		int32 MasksMin = 1000;
		int32 MasksAtStart = 0;
		int32 PaintsAtStart = 0;
		int32 Burst = 0;
		float MaxWobble = 0.0f;
		bool bArmOff = false;
		bool bWasAlive = false;
		bool bKilled = false;
		bool bRagdoll = false;
		int32 SleepsAtStart = 0;
		bool bBlastSent = false;
		FVector CorpseBefore = FVector::ZeroVector;
		double CorpseMoved = 0.0;
		uint16 SeveredBefore = 0;
		int32 HitPushesAtStart = 0;
		FVector HitPelvisBefore = FVector::ZeroVector;
		double HitPelvisMoved = 0.0;
		int32 CorpseBlows = 0;
		int32 GibBlasts = 0;
		int32 TrunksAtStart = 0;
		bool bGibbedSeen = false;
		int32 MostGibsInAir = 0;
	};

	void GoreSmokeRealBodies(bool bOn)
	{
		if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("Pawns.MetaHumans")))
		{
			Var->Set(bOn ? 1 : 0, ECVF_SetByCode);
		}
		if (IConsoleVariable* Hour = IConsoleManager::Get().FindConsoleVariable(TEXT("Sky.Hour")))
		{
			Hour->Set(bOn ? 12.0f : -1.0f, ECVF_SetByCode);
		}
	}

	int32 GoreSmokePart(const colony::Sim& Sim, const colony::ThingView& View, const char* Label)
	{
		const std::vector<std::string> Labels = Sim.health().partLabels(View.def);
		for (size_t i = 0; i < Labels.size(); ++i)
		{
			if (Labels[i] == Label)
			{
				return static_cast<int32>(i);
			}
		}
		return -1;
	}
}

void USmokeTestSubsystem::BuildGorePhases()
{
	const TSharedRef<FGoreSmokeState> State = MakeShared<FGoreSmokeState>();
	const auto Bridge = [this]() { return GetWorld()->GetSubsystem<USimSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };
	const auto Blood = [this]() { return GetWorld()->GetSubsystem<UBloodSubsystem>(); };
	const auto ActorOf = [=]() -> ASimPawnActor* { return Pawns() ? Pawns()->FindPawn(State->Pawn) : nullptr; };
	// The camera in front of the body at `Height` cm, `Back` cm away.
	const auto Front = [=, this](float Height, float Back)
	{
		if (const ASimPawnActor* Actor = ActorOf())
		{
			const FVector P = Actor->GetActorLocation();
			const FVector Forward = Actor->GetActorForwardVector();
			const FVector Target = P + FVector(0.0, 0.0, Height);
			const FVector Eye = Target + Forward * Back + Actor->GetActorRightVector() * (Back * 0.25) + FVector(0.0, 0.0, 10.0);
			ViewFrom(Eye, (Target - Eye).Rotation());
		}
	};
	// Where the trunk burst: the middle of the gibs once there are some, else the corpse's pelvis (a blast may have
	// thrown the ragdoll metres from its actor). The camera looks down at it from `Back` cm away, `Up` cm up.
	const auto AtGibs = [=, this](float Up, float Back)
	{
		FVector Sum = FVector::ZeroVector;
		int32 N = 0;
		for (TActorIterator<AGoreGibActor> It(GetWorld()); It; ++It)
		{
			Sum += It->GetActorLocation();
			++N;
		}
		FVector Target;
		const ASimPawnActor* Actor = ActorOf();
		if (N > 0)
		{
			Target = Sum / N;
		}
		else if (!Actor || !Actor->GetRagdollPelvis(Target))
		{
			if (!Actor)
			{
				return;
			}
			Target = Actor->GetActorLocation();
		}
		const FVector Side = Actor ? Actor->GetActorForwardVector() : FVector::ForwardVector;
		const FVector Eye = Target + Side * Back + FVector(0.0, 0.0, Up);
		ViewFrom(Eye, (Target - Eye).Rotation());
	};
	// A hit drawn at the body (blood only): from in front at `Height` (share of the body's height), along `Way` in the
	// actor's frame (X forward).
	const auto DrawnHit = [=](EBloodWound Kind, float Height, float Right, const FVector& Way, float Damage)
	{
		ASimPawnActor* Actor = ActorOf();
		UBloodSubsystem* B = Blood();
		if (!Actor || !B)
		{
			return false;
		}
		const FTransform Frame = Actor->GetActorTransform();
		const float Tall = 180.0f * Actor->GetBodyScale();
		const FVector Dir = Frame.TransformVectorNoScale(Way).GetSafeNormal();
		const FVector Aim = Frame.TransformPositionNoScale(FVector(0.0, Right, Height * Tall));
		FVector OnSkin = Aim;
		Actor->TraceDrawnBody(Aim - Dir * 80.0f, Aim + Dir * 40.0f, OnSkin);
		FBloodHit Hit;
		Hit.At = OnSkin;
		Hit.Direction = Dir;
		Hit.Kind = Kind;
		Hit.Damage = Damage;
		Hit.Victim = State->Pawn;
		Hit.Mesh = Actor->GetDecalMesh();
		B->AddHit(Hit);
		return true;
	};
	const auto Damage = [=](const char* Type, float Amount, const char* PartLabel)
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Now = B && B->GetSim() ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
		const int32 Part = Now ? GoreSmokePart(*B->GetSim(), *Now, PartLabel) : -1;
		if (Part < 0)
		{
			return false;
		}
		colony::DamagePawnCommand Command;
		Command.pawn = State->Pawn;
		Command.damageType = Type;
		Command.amount = Amount;
		Command.part = Part;
		B->Submit(Command);
		return true;
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
			GoreSmokeRealBodies(true);
			FSimStartOptions Options;
			Options.Seed = 42;
			Options.MapSize = 128;
			Options.bFlatMap = true;
			Options.Scenario = TEXT("Crashlanded");
			B->SetWorldOrigin(GoreSmokeOrigin);
			if (!B->StartSim(Options))
			{
				Fail(TEXT("setup: the game's data did not load"));
				return;
			}
			B->SetSpeed(1);
			for (const colony::ThingView& Thing : B->GetCurrentSnapshot().things)
			{
				const colony::Health* Health = B->GetSim()->things().registry().try_get<colony::Health>(Thing.id);
				if (Thing.category == colony::ThingCategory::Pawn && B->GetSim()->colonistOf(Thing.id) && Health && !Health->protagonist)
				{
					State->Pawn = Thing.id;
					break;
				}
			}
			if (State->Pawn == colony::kNoEntity)
			{
				Fail(TEXT("setup: no colonist besides the protagonist"));
				return;
			}
			// A MakeHuman body as the gun range and the creator's people have (it takes its blood in the skin; the plain
			// MetaHuman body of a bare colonist gets decals only).
			if (const colony::ThingView* View = B->GetCurrentSnapshot().find(State->Pawn))
			{
				const colony::Snapshot& Snapshot = B->GetCurrentSnapshot();
				const FSimPawnLooks From = View->looks >= 0 ? USimPawnSubsystem::MakeLooks(Snapshot.looks[static_cast<size_t>(View->looks)], B->GetDefs().get()) : FSimPawnLooks();
				// -GoreWoman: a woman's body (her anatomy body is the one whose stitch triangles drew the thigh band).
				const bool bWoman = FParse::Param(FCommandLine::Get(), TEXT("GoreWoman"));
				Pawns()->SetLooksOverride(State->Pawn, CreatorPeople::RandomLooks(4242, bWoman, From));
				PhaseNumbers.Add(TEXT("woman"), bWoman ? 1 : 0);
			}
			B->Submit(colony::DraftCommand{State->Pawn, true});
		};
		Phase.WarmupSeconds = 4.0f;
		Phase.MeasureSeconds = 0.5f;
		Phase.OnFrame = [=]() { Front(100.0f, 260.0f); };
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			if (!Actor || !Actor->GetDecalMesh())
			{
				Fail(TEXT("setup: the colonist has no drawn body"));
				return;
			}
			if (Blood() && !Blood()->IsReady())
			{
				Fail(TEXT("setup: the blood assets are missing (run -run=BloodArt -force)"));
			}
			// Whether this body takes its blood in the skin (the log line "Skin blood for <mesh>: ..." says why not).
			USkinBlood* Skin = Blood() ? Blood()->GetSkinBlood() : nullptr;
			const USkeletalMeshComponent* Mesh = Actor->GetDecalMesh();
			const bool bSkin = Skin && Skin->CanPaint(Mesh);
			PhaseNumbers.Add(TEXT("skinPaintable"), bSkin ? 1 : 0);
			if (!bSkin)
			{
				Notes.Add(FString::Printf(TEXT("setup: the colonist's body %s (%s) can't take skin blood: see the log line \"Skin blood for\""),
					*Mesh->GetName(), Mesh->GetSkeletalMeshAsset() ? *Mesh->GetSkeletalMeshAsset()->GetName() : TEXT("no mesh")));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A rifle shot into the groin from in front, through and through. The close-up must show a wound and its exit
		// behind, no square patch on the hip and no stain round the hips like shorts.
		FPhase Phase;
		Phase.Name = TEXT("groin");
		Phase.Setup = [=, this]()
		{
			State->ExitsAtStart = Blood() ? Blood()->GetExitsTraced() : 0;
			if (!DrawnHit(EBloodWound::Gunshot, 0.47f, 0.0f, FVector(-1.0, 0.0, 0.0), 18.0f))
			{
				Fail(TEXT("groin: could not draw the hit"));
			}
		};
		Phase.WarmupSeconds = 1.5f;
		Phase.MeasureSeconds = 1.5f;
		Phase.ScreenshotAt = 0.5f;
		Phase.OnFrame = [=]() { Front(88.0f, 95.0f); };
		Phase.Verify = [=, this]()
		{
			const int32 Exits = Blood() ? Blood()->GetExitsTraced() - State->ExitsAtStart : 0;
			PhaseNumbers.Add(TEXT("exitsTraced"), Exits);
			if (Exits < 1)
			{
				Notes.Add(TEXT("groin: the exit was guessed, not found on the drawn body (a weak shot may not go through)"));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// The stocking band (sneed, 2026-11-22): after torso, groin and thigh shots no ring or line of blood may go round
		// the thighs or up the body; the blood stays at the wounds. Close-ups from the front and the back.
		FPhase Phase;
		Phase.Name = TEXT("thighs");
		Phase.Setup = [=, this]()
		{
			const bool bTorso = DrawnHit(EBloodWound::Gunshot, 0.62f, 6.0f, FVector(-1.0, 0.0, 0.0), 14.0f);
			const bool bThigh = DrawnHit(EBloodWound::Gunshot, 0.38f, 11.0f, FVector(-1.0, 0.0, 0.0), 14.0f);
			const bool bHip = DrawnHit(EBloodWound::Gunshot, 0.50f, -13.0f, FVector(-1.0, 0.0, 0.0), 12.0f);
			if (!bTorso || !bThigh || !bHip)
			{
				Fail(TEXT("thighs: could not draw the hits"));
			}
		};
		Phase.WarmupSeconds = 1.5f;
		Phase.MeasureSeconds = 1.5f;
		Phase.ScreenshotAt = 0.5f;
		Phase.OnFrame = [=]() { Front(80.0f, 110.0f); };
		Phases.Add(MoveTemp(Phase));
	}

	{
		FPhase Phase;
		Phase.Name = TEXT("thighsback");
		Phase.Setup = []() {};
		Phase.WarmupSeconds = 1.0f;
		Phase.MeasureSeconds = 1.0f;
		Phase.ScreenshotAt = 0.3f;
		Phase.OnFrame = [=]() { Front(80.0f, -110.0f); };
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A sword cut across the chest and a shot through the belly: big gushes in the air, pictured mid-flight.
		FPhase Phase;
		Phase.Name = TEXT("spray");
		Phase.Setup = [=]()
		{
			State->SprayAtStart = Blood() ? Blood()->GetSpraySpawned() : 0;
			State->MaxSprayInAir = 0;
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 1.6f;
		Phase.ScreenshotAt = 0.08f;
		Phase.OnFrame = [=, this]()
		{
			Front(120.0f, 230.0f);
			if (State->HitsSent == 0)
			{
				DrawnHit(EBloodWound::Cut, 0.72f, 0.0f, FVector(-0.6, 0.8, 0.0), 20.0f);
				DrawnHit(EBloodWound::Gunshot, 0.6f, 6.0f, FVector(-0.2, -1.0, 0.0), 14.0f);
				State->HitsSent = 2;
			}
			if (const UBloodSubsystem* B = Blood())
			{
				State->MaxSprayInAir = FMath::Max(State->MaxSprayInAir, B->GetSprayInAir());
			}
		};
		Phase.Verify = [=, this]()
		{
			const int32 Spawned = Blood() ? Blood()->GetSpraySpawned() - State->SprayAtStart : 0;
			PhaseNumbers.Add(TEXT("gushes"), Spawned);
			PhaseNumbers.Add(TEXT("mostInAir"), State->MaxSprayInAir);
			PhaseNumbers.Add(TEXT("skinMasks"), Blood() ? Blood()->GetSkinMasks() : 0);
			PhaseNumbers.Add(TEXT("skinPaints"), Blood() ? Blood()->GetSkinPaints() : 0);
			if (Spawned < 6 || State->MaxSprayInAir < 4)
			{
				Fail(FString::Printf(TEXT("spray: %d gushes made, %d in the air at once (6 and 4 at least)"), Spawned, State->MaxSprayInAir));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// Thirty hits on one spot in three seconds, and a picture every 0.1 s of the last second: no flashing red, and the
		// body keeps its skin blood throughout.
		FPhase Phase;
		Phase.Name = TEXT("pileup");
		Phase.Setup = [=]()
		{
			State->HitsSent = 0;
			State->Burst = 0;
			State->MasksMin = 1000;
			State->MasksAtStart = Blood() ? Blood()->GetSkinMasks() : 0;
			State->PaintsAtStart = Blood() ? Blood()->GetSkinPaints() : 0;
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 4.2f;
		Phase.ScreenshotAt = 3.4f;
		Phase.OnFrame = [=, this]()
		{
			Front(110.0f, 150.0f);
			while (State->HitsSent < 30 && PhaseTime >= 0.1f * State->HitsSent)
			{
				DrawnHit(State->HitsSent % 2 == 0 ? EBloodWound::Gunshot : EBloodWound::Stab, 0.66f, 4.0f, FVector(-1.0, 0.1, 0.0), 12.0f);
				++State->HitsSent;
			}
			if (State->HitsSent > 2)
			{
				if (const UBloodSubsystem* B = Blood())
				{
					State->MasksMin = FMath::Min(State->MasksMin, B->GetSkinMasks());
				}
			}
			if (PhaseTime >= 3.0f && State->Burst < 10 && PhaseTime >= 3.0f + 0.1f * State->Burst)
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Smoke") / TEXT("gore") / FString::Printf(TEXT("pileup_burst%02d.png"), State->Burst), false, false);
				++State->Burst;
			}
		};
		Phase.Verify = [=, this]()
		{
			PhaseNumbers.Add(TEXT("hits"), State->HitsSent);
			PhaseNumbers.Add(TEXT("fewestMasks"), State->MasksMin);
			PhaseNumbers.Add(TEXT("masksAtStart"), State->MasksAtStart);
			const int32 Paints = Blood() ? Blood()->GetSkinPaints() - State->PaintsAtStart : 0;
			PhaseNumbers.Add(TEXT("skinPaints"), Paints);
			if (Paints < 1)
			{
				Fail(FString::Printf(TEXT("pileup: nothing was painted into the skin (%d masks at the start): the body has no blood position map or skin painting is off"), State->MasksAtStart));
			}
			else if (State->MasksMin < 1)
			{
				Fail(FString::Printf(TEXT("pileup: the body lost its skin blood while it was being hit (%d masks at the start, %d skin paints)"), State->MasksAtStart, Paints));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A shot to the head from in front: the head must snap back.
		FPhase Phase;
		Phase.Name = TEXT("head");
		Phase.Setup = [=, this]()
		{
			State->MaxWobble = 0.0f;
			USimSubsystem* B = Bridge();
			ASimPawnActor* Actor = ActorOf();
			if (B && Actor)
			{
				const FTransform Frame = Actor->GetActorTransform();
				Actor->KickZone(*B, -1, Frame.TransformPositionNoScale(FVector(0.0, 0.0, 0.93f * 180.0f * Actor->GetBodyScale())), Frame.TransformVectorNoScale(FVector(-1.0, 0.0, 0.0)), 0.45f);
			}
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 1.5f;
		Phase.OnFrame = [=, this]()
		{
			Front(160.0f, 200.0f);
			if (const ASimPawnActor* Actor = ActorOf(); Actor && Actor->GetAnim())
			{
				const float Wobble = Actor->GetAnim()->GetStats().WobbleDegrees;
				State->MaxWobble = FMath::Max(State->MaxWobble, Wobble);
				UE_LOG(LogTemp, Display, TEXT("GORETRACE head t=%.3f dt=%.4f wobble=%.2f"), PhaseTime, GetWorld()->GetDeltaSeconds(), Wobble);
			}
		};
		Phase.Verify = [=, this]()
		{
			PhaseNumbers.Add(TEXT("headSnapDegrees"), State->MaxWobble);
			if (State->MaxWobble < 12.0f)
			{
				Fail(FString::Printf(TEXT("head: a rifle hit snapped the head only %.1f degrees (12 at least)"), State->MaxWobble));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// The left arm shot again and again on the living body: it comes off and the body lives.
		FPhase Phase;
		Phase.Name = TEXT("armoff");
		Phase.Setup = [=, this]()
		{
			for (int32 i = 0; i < 14; ++i)
			{
				Damage("Bullet", 5.0f, "left arm");
			}
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 2.5f;
		Phase.ScreenshotAt = 2.0f;
		Phase.OnFrame = [=]() { Front(120.0f, 230.0f); };
		Phase.Verify = [=, this]()
		{
			const colony::ThingView* Now = Bridge() ? Bridge()->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			State->bArmOff = Now && (Now->severed & colony::kSeveredLeftArm) != 0;
			State->bWasAlive = Now && !Now->dead;
			PhaseNumbers.Add(TEXT("armOff"), State->bArmOff ? 1 : 0);
			if (!State->bArmOff || !State->bWasAlive)
			{
				Fail(FString::Printf(TEXT("armoff: arm off %d, alive %d (both expected)"), State->bArmOff ? 1 : 0, State->bWasAlive ? 1 : 0));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// Killed through the chest: it falls as a ragdoll, settles and lies still (asleep), no twitching.
		FPhase Phase;
		Phase.Name = TEXT("corpse");
		Phase.Setup = [=, this]()
		{
			State->SleepsAtStart = ActorOf() ? ActorOf()->GetRagdollSleeps() : 0;
			// Dead with its limbs on, so the blast has something to tear; 14 past the head's 0 takes the head off (a quarter of
			// 50), for the headoff phase.
			State->bKilled = Damage("Blunt", 64.0f, "head");
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 7.0f;
		Phase.ScreenshotAt = 6.0f;
		Phase.OnFrame = [=, this]()
		{
			Front(30.0f, 260.0f);
			if (const ASimPawnActor* Actor = ActorOf())
			{
				State->bRagdoll |= Actor->IsRagdoll();
				UE_LOG(LogTemp, Display, TEXT("GORETRACE corpse t=%.2f ragdoll=%d fastest=%.1f asleep=%d sleeps=%d"), PhaseTime, Actor->IsRagdoll() ? 1 : 0,
					Actor->GetRagdollFastest(), Actor->IsRagdollAsleep() ? 1 : 0, Actor->GetRagdollSleeps());
			}
		};
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			const int32 Sleeps = Actor ? Actor->GetRagdollSleeps() - State->SleepsAtStart : 0;
			PhaseNumbers.Add(TEXT("ragdoll"), State->bRagdoll ? 1 : 0);
			PhaseNumbers.Add(TEXT("sleeps"), Sleeps);
			PhaseNumbers.Add(TEXT("asleepAtEnd"), Actor && Actor->IsRagdollAsleep() ? 1 : 0);
			PhaseNumbers.Add(TEXT("fastestAtEndCmS"), Actor ? Actor->GetRagdollFastest() : -1.0f);
			if (!State->bKilled || !State->bRagdoll)
			{
				Fail(TEXT("corpse: the colonist was not killed into a ragdoll"));
			}
			else if (Sleeps < 1)
			{
				Fail(TEXT("corpse: the ragdoll never settled and slept (it keeps twitching)"));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// The head the corpse phase's blow took off (sneed, 2026-11-23: a severed head stayed tied to the neck stump by a
		// long stretched pink and white tube): a clean piece of its own, about a head long, seen close.
		FPhase Phase;
		Phase.Name = TEXT("headoff");
		Phase.Setup = []() {};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 1.0f;
		Phase.ScreenshotAt = 0.5f;
		Phase.OnFrame = [=, this]()
		{
			for (TActorIterator<ASeveredPieceActor> It(GetWorld()); It; ++It)
			{
				if (It->GetBit() == colony::kSeveredHead)
				{
					const FVector At = It->GetActorLocation();
					const FVector Eye = At + FVector(80.0, 55.0, 50.0);
					ViewFrom(Eye, (At - Eye).Rotation());
					break;
				}
			}
		};
		Phase.Verify = [=, this]()
		{
			const ASeveredPieceActor* HeadPiece = nullptr;
			for (TActorIterator<ASeveredPieceActor> It(GetWorld()); It; ++It)
			{
				if (It->GetBit() == colony::kSeveredHead)
				{
					HeadPiece = *It;
				}
			}
			const colony::ThingView* Now = Bridge() ? Bridge()->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			const bool bOff = Now && (Now->severed & colony::kSeveredHead) != 0;
			const ASimPawnActor* Actor = ActorOf();
			const float Tall = Actor ? FMath::Max(Actor->GetBodyScale(), 1.0f) : 1.0f;
			PhaseNumbers.Add(TEXT("headOff"), bOff ? 1 : 0);
			if (!bOff || !HeadPiece)
			{
				Fail(FString::Printf(TEXT("headoff: head off in the sim %d, head piece drawn %d (both expected)"), bOff ? 1 : 0, HeadPiece ? 1 : 0));
				return;
			}
			int32 PieceVertices = 0;
			const float PieceCm = HeadPiece->MeasureSkinLengthCm(PieceVertices);
			PhaseNumbers.Add(TEXT("headPieceCm"), PieceCm);
			PhaseNumbers.Add(TEXT("headPieceVertices"), PieceVertices);
			PhaseNumbers.Add(TEXT("headPieceMeshes"), HeadPiece->GetMeshCount());
			if (PieceCm < 0.0f)
			{
				Notes.Add(TEXT("headoff: the head piece's skin could not be read (no CPU copy of the vertices); look at the picture"));
			}
			else if (PieceCm > 45.0f * Tall)
			{
				Fail(FString::Printf(TEXT("headoff: the severed head's skin is drawn out %.0f cm long (45 at most): skin still tied to the body"), PieceCm));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// The settled corpse struck four times (sneed 2026-11-22: "corpses need to wriggle, have ragdoll physics when
		// hit, not just still"): each blow wakes the ragdoll and jolts the bone it hit.
		FPhase Phase;
		Phase.Name = TEXT("corpsehit");
		Phase.Setup = [=]()
		{
			const ASimPawnActor* Actor = ActorOf();
			State->HitPushesAtStart = Actor ? Actor->GetRagdollHitPushes() : 0;
			State->HitPelvisBefore = FVector::ZeroVector;
			if (Actor)
			{
				Actor->GetRagdollPelvis(State->HitPelvisBefore);
			}
			State->HitPelvisMoved = 0.0;
			State->CorpseBlows = 0;
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 2.5f;
		Phase.ScreenshotAt = 0.4f;
		Phase.OnFrame = [=, this]()
		{
			Front(30.0f, 260.0f);
			ASimPawnActor* Actor = ActorOf();
			UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
			if (!Actor || !Fight)
			{
				return;
			}
			FVector Pelvis;
			if (Actor->GetRagdollPelvis(Pelvis))
			{
				State->HitPelvisMoved = FMath::Max(State->HitPelvisMoved, FVector::Dist(Pelvis, State->HitPelvisBefore));
				while (State->CorpseBlows < 4 && PhaseTime >= 0.4f * State->CorpseBlows)
				{
					// A sword blow from the side, at the pelvis, then the chest.
					const FVector At = Pelvis + FVector(0.0, 0.0, State->CorpseBlows % 2 == 0 ? 5.0 : 25.0);
					Fight->SpawnMeleeImpact(colony::ProjectileHitKind::Pawn, colony::kNoEntity, State->Pawn, At, FVector(0.0, 1.0, -0.2), -1, 12.0f);
					++State->CorpseBlows;
				}
			}
		};
		Phase.Verify = [=, this]()
		{
			const ASimPawnActor* Actor = ActorOf();
			const int32 Pushes = Actor ? Actor->GetRagdollHitPushes() - State->HitPushesAtStart : 0;
			PhaseNumbers.Add(TEXT("corpseJolts"), Pushes);
			PhaseNumbers.Add(TEXT("pelvisMovedCm"), State->HitPelvisMoved);
			if (Pushes < 3)
			{
				Fail(FString::Printf(TEXT("corpsehit: %d of %d blows jolted the corpse's ragdoll"), Pushes, State->CorpseBlows));
			}
			else if (State->HitPelvisMoved < 2.0)
			{
				Fail(FString::Printf(TEXT("corpsehit: the corpse barely moved under the blows (%.1f cm)"), State->HitPelvisMoved));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// A rocket's blast a metre from the corpse: thrown, and torn apart.
		FPhase Phase;
		Phase.Name = TEXT("blast");
		Phase.Setup = [=, this]()
		{
			USimSubsystem* B = Bridge();
			const colony::ThingView* Now = B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			ASimPawnActor* Actor = ActorOf();
			if (!B || !Now || !Actor)
			{
				Fail(TEXT("blast: no corpse"));
				return;
			}
			State->SeveredBefore = Now->severed;
			FVector Pelvis = Actor->GetActorLocation();
			Actor->GetRagdollPelvis(Pelvis);
			State->CorpseBefore = Pelvis;
			colony::ExplodeCommand Blast;
			Blast.x = Now->x + 1.0f;
			Blast.y = Now->y;
			Blast.radius = 3.9f;
			Blast.damage = 50.0f;
			B->Submit(Blast);
			State->bBlastSent = true;
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 3.0f;
		Phase.ScreenshotAt = 0.25f;
		Phase.OnFrame = [=]()
		{
			Front(40.0f, 450.0f);
			if (const ASimPawnActor* Actor = ActorOf())
			{
				FVector Pelvis;
				if (Actor->GetRagdollPelvis(Pelvis))
				{
					State->CorpseMoved = FMath::Max(State->CorpseMoved, FVector::Dist2D(Pelvis, State->CorpseBefore));
				}
			}
		};
		Phase.Verify = [=, this]()
		{
			const colony::ThingView* Now = Bridge() ? Bridge()->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			const uint16 Fresh = Now ? static_cast<uint16>(Now->severed & ~State->SeveredBefore) : 0;
			const int32 Thrown = GetWorld()->GetSubsystem<UBlastFxSubsystem>() ? GetWorld()->GetSubsystem<UBlastFxSubsystem>()->GetRagdollsThrown() : 0;
			PhaseNumbers.Add(TEXT("corpseMovedCm"), State->CorpseMoved);
			PhaseNumbers.Add(TEXT("piecesTornOff"), FMath::CountBits(Fresh));
			PhaseNumbers.Add(TEXT("ragdollsThrown"), Thrown);
			if (Thrown < 1 || State->CorpseMoved < 40.0)
			{
				Fail(FString::Printf(TEXT("blast: the corpse moved %.0f cm (40 at least), ragdolls thrown %d"), State->CorpseMoved, Thrown));
			}
			if (Fresh == 0)
			{
				Fail(TEXT("blast: a rocket a metre away tore nothing off the corpse"));
			}
			if (State->CorpseMoved > 600.0)
			{
				Fail(FString::Printf(TEXT("blast: the corpse flew %.0f cm (cartoonish; 6 m at most)"), State->CorpseMoved));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// Rockets on what is left (sneed 2026-11-22: "keep overkill damage until it gibs ... until none"): the rest of the
		// limbs and the head come off, then the bare trunk bursts into chunks of meat, ribs and bone with sprays of blood,
		// and the body is gone.
		FPhase Phase;
		Phase.Name = TEXT("gibs");
		Phase.Setup = [=, this]()
		{
			const UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
			State->TrunksAtStart = Fight ? Fight->GetTrunksBurst() : 0;
			State->GibBlasts = 0;
		};
		Phase.WarmupSeconds = 0.0f;
		Phase.MeasureSeconds = 4.0f;
		Phase.ScreenshotAt = 0.35f;  // the chunks in the air
		Phase.OnFrame = [=, this]()
		{
			AtGibs(170.0f, 450.0f);
			USimSubsystem* B = Bridge();
			const colony::ThingView* Now = B ? B->GetCurrentSnapshot().find(State->Pawn) : nullptr;
			const bool bGone = Now && (Now->severed & colony::kSeveredTrunk) != 0;
			// A blast at the corpse every 0.6 s until the trunk has burst (4 at most).
			if (B && Now && !bGone && State->GibBlasts < 4 && PhaseTime >= 0.6f * State->GibBlasts)
			{
				colony::ExplodeCommand Blast;
				Blast.x = Now->x + 0.3f;
				Blast.y = Now->y;
				Blast.radius = 1.6f;
				Blast.damage = 120.0f;
				B->Submit(Blast);
				++State->GibBlasts;
			}
			State->bGibbedSeen |= bGone;
			int32 InAir = 0;
			for (TActorIterator<AGoreGibActor> It(GetWorld()); It; ++It)
			{
				InAir += It->HasLanded() ? 0 : 1;
			}
			State->MostGibsInAir = FMath::Max(State->MostGibsInAir, InAir);
		};
		Phase.Verify = [=, this]()
		{
			const UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
			const ASimPawnActor* Actor = ActorOf();
			const int32 Burst = Fight ? Fight->GetTrunksBurst() - State->TrunksAtStart : 0;
			const int32 Live = AGoreGibActor::CountLive(GetWorld());
			int32 Resting = 0;
			FVector Mid = FVector::ZeroVector;
			for (TActorIterator<AGoreGibActor> It(GetWorld()); It; ++It)
			{
				Resting += It->IsAtRest() ? 1 : 0;
				Mid += It->GetActorLocation();
			}
			Mid /= FMath::Max(Live, 1);
			double Spread = 0.0;
			for (TActorIterator<AGoreGibActor> It(GetWorld()); It; ++It)
			{
				Spread += FVector::Dist2D(It->GetActorLocation(), Mid) / FMath::Max(Live, 1);
			}
			PhaseNumbers.Add(TEXT("gibsSpreadCm"), Spread);
			PhaseNumbers.Add(TEXT("gibBlasts"), State->GibBlasts);
			PhaseNumbers.Add(TEXT("trunksBurst"), Burst);
			PhaseNumbers.Add(TEXT("gibsLying"), Live);
			PhaseNumbers.Add(TEXT("gibsAtRest"), Resting);
			PhaseNumbers.Add(TEXT("gibsMostInAir"), State->MostGibsInAir);
			PhaseNumbers.Add(TEXT("bodyHidden"), Actor && Actor->IsGibbed() ? 1 : 0);
			if (!State->bGibbedSeen || Burst < 1)
			{
				Fail(FString::Printf(TEXT("gibs: %d blasts on the corpse and its trunk never burst (sim %d, drawn %d)"), State->GibBlasts,
					State->bGibbedSeen ? 1 : 0, Burst));
			}
			else if (Live < 10)
			{
				Fail(FString::Printf(TEXT("gibs: only %d chunks lie about after the burst (10 at least)"), Live));
			}
			else if (!Actor || !Actor->IsGibbed() || !Actor->IsHidden())
			{
				Fail(TEXT("gibs: the body still shows after its trunk burst"));
			}
			else if (Resting < Live / 2)
			{
				Fail(FString::Printf(TEXT("gibs: %d of %d chunks came to rest on the ground in 4 s"), Resting, Live));
			}
		};
		Phases.Add(MoveTemp(Phase));
	}

	{
		// Close on the chunks lying where they came down, with the blood round them.
		FPhase Phase;
		Phase.Name = TEXT("gibsclose");
		Phase.Setup = []() {};
		Phase.WarmupSeconds = 0.5f;
		Phase.MeasureSeconds = 1.0f;
		Phase.ScreenshotAt = 0.3f;
		Phase.OnFrame = [=]() { AtGibs(150.0f, 190.0f); };
		Phase.Verify = [=, this]() { GoreSmokeRealBodies(false); };
		Phases.Add(MoveTemp(Phase));
	}
}
