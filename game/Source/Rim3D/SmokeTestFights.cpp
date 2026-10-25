// Smoke test `fights` (task U11): shooting and fighting on screen, in the game, offscreen.
#include "SmokeTest.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"

#include "FightSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

#include <algorithm>
#include <vector>

namespace
{
	const FVector FightSmokeOrigin(300000.0, 0.0, 0.0);

	struct FFightSmokeState
	{
		colony::Entity Protagonist = colony::kNoEntity;
		colony::Entity Dummy[3] = {colony::kNoEntity, colony::kNoEntity, colony::kNoEntity};
		colony::Entity Target = colony::kNoEntity;
		TArray<colony::Entity> Raiders;
		TArray<colony::Entity> Defenders;
		FVector2D Start = FVector2D::ZeroVector;

		// Phase bookkeeping.
		bool bEquipped = false;
		bool bUsedEquipKey = false;
		bool bCrouched = false;
		bool bStoodUp = false;
		bool bReloadTried = false;
		bool bReloadSeen = false;
		bool bFired = false;
		bool bPaused = false;
		bool bFightSetUp = false;
		double PausedAt = 0.0;
		float TracerDistanceAtPause = 0.0f;
		int32 TracersAtPause = 0;
		int32 ParticlesAtPause = 0;
		int32 BloodAtPause = 0;
		bool bLeanAtPause = false;
		int32 ShotsAtStart = 0;
		int32 HitsAtStart = 0;
		int32 PawnHitsAtStart = 0;
		int32 FlashesAtStart = 0;
		int32 TracersAtStart = 0;
		int32 ReactionsAtStart = 0;
		int32 BodyDecalsAtStart = 0;
		int32 GroundDecalsAtStart = 0;
		int32 RagdollsAtStart = 0;
		int32 ImpactsAtStart = 0;
		int32 PawnImpactsAtStart = 0;
		int32 MarkersAtStart = 0;
		int32 FireOrdersAtStart = 0;
		int32 MaxTracers = 0;
		int32 MaxParticles = 0;
		float StartHeightCm = 0.0f;
		FVector DummyLocation = FVector::ZeroVector;
	};

	const colony::ThingView* FightSmokeFind(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		return Thing == colony::kNoEntity ? nullptr : Bridge.GetCurrentSnapshot().find(Thing);
	}

	bool FightSmokeAlive(const USimSubsystem& Bridge, colony::Entity Thing)
	{
		const colony::ThingView* View = FightSmokeFind(Bridge, Thing);
		return View && !View->downed && !View->dead;
	}

	/** Items on the map made from this def. */
	TArray<colony::Entity> FightSmokeItems(const USimSubsystem& Bridge, const char* DefName)
	{
		TArray<colony::Entity> Found;
		if (!Bridge.GetDefs())
		{
			return Found;
		}
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Item && View.def.valid() && Bridge.GetDefs()->get(View.def).defName() == DefName)
			{
				Found.Add(View.id);
			}
		}
		return Found;
	}
}

void USmokeTestSubsystem::BuildFightsPhases()
{
	const TSharedRef<FFightSmokeState> State = MakeShared<FFightSmokeState>();

	const auto AddPhase = [this](const TCHAR* Name, TFunction<void()> Setup, float Warm, float Measure, TFunction<void()> EveryFrame, TFunction<void()> Check)
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
	const auto Prot = [this]() { return GetWorld()->GetSubsystem<UProtagonistSubsystem>(); };
	const auto Fight = [this]() { return GetWorld()->GetSubsystem<UFightSubsystem>(); };
	const auto Pawns = [this]() { return GetWorld()->GetSubsystem<USimPawnSubsystem>(); };

	// Turns the player's view so the crosshair is on a pawn's chest.
	const auto AimAt = [=](colony::Entity Target) -> bool
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		FVector Where;
		if (!B || !P || !P->IsDirectMode() || !B->GetBlendedLocation(Target, Where))
		{
			return false;
		}
		AProtagonistPawn* Body = P->GetBody();
		const FRotator Wanted = (Where + FVector(0.0, 0.0, 125.0) - Body->GetViewLocation()).Rotation();
		Body->Look(FRotator::NormalizeAxis(Wanted.Yaw - Body->GetYaw()), Wanted.Pitch - Body->GetPitch());
		Body->SnapCamera();
		return true;
	};

	// The first of the three dummies still standing.
	const auto PickTarget = [=]() -> colony::Entity
	{
		for (const colony::Entity Dummy : State->Dummy)
		{
			if (FightSmokeAlive(*Bridge(), Dummy))
			{
				return Dummy;
			}
		}
		return colony::kNoEntity;
	};

	// A camera to the side of the line between the protagonist and a point, looking at the point between.
	const auto SideCamera = [=, this](const FVector& A, const FVector& B, double Side, double Height)
	{
		const FVector Middle = (A + B) * 0.5;
		const FVector Along = (B - A).GetSafeNormal2D();
		const FVector Out = FVector::CrossProduct(FVector::UpVector, Along);
		const FVector Eye = Middle + Out * Side + FVector(0.0, 0.0, Height);
		ViewFrom(Eye, (Middle + FVector(0.0, 0.0, 110.0) - Eye).Rotation());
	};

	const auto Snapshot = [=](colony::Entity Thing) { return FightSmokeFind(*Bridge(), Thing); };

	const auto MarkBaselines = [=]()
	{
		UFightSubsystem* F = Fight();
		State->ShotsAtStart = F->GetShotsSeen();
		State->HitsAtStart = F->GetHitsSeen();
		State->PawnHitsAtStart = F->GetPawnHitsSeen();
		State->FlashesAtStart = F->GetFlashes();
		State->TracersAtStart = F->GetTracersSpawned();
		State->ReactionsAtStart = F->GetHitReactions();
		State->BodyDecalsAtStart = F->GetBodyDecals();
		State->GroundDecalsAtStart = F->GetGroundDecals();
		State->RagdollsAtStart = F->GetRagdollsStarted();
		State->ImpactsAtStart = F->GetImpacts();
		State->PawnImpactsAtStart = F->GetImpactsOfKind(colony::ProjectileHitKind::Pawn);
		State->MarkersAtStart = F->GetHitMarkers();
		State->FireOrdersAtStart = F->GetFireOrders();
		State->MaxTracers = 0;
		State->MaxParticles = 0;
	};

	// 1. A sim with the Crashlanded start and three plain pawns standing 12 m east of the protagonist, who is
	// taken over in third person.
	AddPhase(TEXT("setup"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		UProtagonistSubsystem* P = Prot();
		if (!B || !P || !Fight() || !Pawns())
		{
			Fail(TEXT("setup: USimSubsystem, UProtagonistSubsystem, UFightSubsystem or USimPawnSubsystem did not start"));
			return;
		}
		FSimStartOptions Options;
		Options.Seed = 42;
		Options.MapSize = 128;
		Options.bFlatMap = true;
		Options.Scenario = TEXT("Crashlanded");
		B->SetWorldOrigin(FightSmokeOrigin);
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
		State->Protagonist = P->FindProtagonist();
		const colony::ThingView* Me = Snapshot(State->Protagonist);
		if (!Me)
		{
			Fail(TEXT("setup: no protagonist"));
			return;
		}
		State->Start = FVector2D(Me->x, Me->y);
		for (int32 I = 0; I < 3; ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{FMath::FloorToInt32(Me->x) + 12, FMath::FloorToInt32(Me->y) + (I - 1) * 4, 0}});
		}
		if (!P->EnterDirectMode(State->Protagonist))
		{
			Fail(TEXT("setup: could not take control of the protagonist"));
			return;
		}
		P->GetBody()->SetFirstPerson(false);
		P->GetBody()->Look(FRotator::NormalizeAxis(-P->GetBody()->GetYaw()), -5.0f - P->GetBody()->GetPitch());
		P->GetBody()->SnapCamera();
		Fight()->SetShowHitboxes(false);
	}, 2.5f, 0.5f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		if (!B || !B->GetSim())
		{
			return;
		}
		int32 Found = 0;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && !B->GetSim()->colonistOf(View.id) && Found < 3)
			{
				State->Dummy[Found++] = View.id;
			}
		}
		if (Found < 3)
		{
			Fail(FString::Printf(TEXT("setup: only %d of the 3 plain pawns exist"), Found));
			return;
		}
		// Sorted by y so the middle one is straight ahead.
		std::sort(State->Dummy, State->Dummy + 3, [&](colony::Entity L, colony::Entity R) { return Snapshot(L)->y < Snapshot(R)->y; });
		for (const colony::Entity Dummy : State->Dummy)
		{
			if (!Pawns()->FindPawn(Dummy))
			{
				Fail(TEXT("setup: a plain pawn has no pawn actor"));
			}
		}
		if (B->GetSim()->controlledPawn() != State->Protagonist || !Prot()->IsDirectMode())
		{
			Fail(TEXT("setup: the protagonist is not under direct control"));
		}
		if (Fight()->GetShotsSeen() != 0 || Fight()->GetActiveTracers() != 0)
		{
			Fail(TEXT("setup: shots or tracers before anything was fired"));
		}
	});

	// 2. E picks up a gun, the held gun appears, the crosshair finds a pawn, C crouches.
	AddPhase(TEXT("equip_and_aim"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		const colony::ThingView* Me = Snapshot(State->Protagonist);
		if (Me && Me->weapon.valid())
		{
			Fail(TEXT("equip_and_aim: the protagonist already holds a gun before E"));
		}
		State->bUsedEquipKey = Fight()->EquipNearest();
		if (!State->bUsedEquipKey)
		{
			// The guns lie farther than 8 m from the crash site: give it the rifle by command and say so.
			const TArray<colony::Entity> Rifles = FightSmokeItems(*B, "Gun_BoltActionRifle");
			if (Rifles.IsEmpty())
			{
				Fail(TEXT("equip_and_aim: the Crashlanded start has no bolt-action rifle"));
				return;
			}
			B->Submit(colony::EquipCommand{State->Protagonist, Rifles[0]});
			Notes.Add(TEXT("equip_and_aim: no gun within 8 m of the protagonist, equipped the rifle by command"));
		}
		State->Target = State->Dummy[1];
		State->bCrouched = false;
		State->bStoodUp = false;
	}, 2.0f, 1.5f, [=, this]()
	{
		AimAt(State->Target);
		const colony::ThingView* Me = Snapshot(State->Protagonist);
		if (!Me || !Me->weapon.valid())
		{
			return;
		}
		UFightSubsystem* F = Fight();
		if (PhaseTime >= 2.2f && !State->bCrouched)
		{
			State->bCrouched = true;
			if (!F->ToggleCrouch())
			{
				Fail(TEXT("equip_and_aim: C (crouch) did nothing"));
			}
		}
		if (PhaseTime >= 3.0f && !State->bStoodUp)
		{
			State->bStoodUp = true;
			if (Me->stance != colony::Stance::Crouching)
			{
				Fail(TEXT("equip_and_aim: the sim does not have the protagonist crouching a second after C"));
			}
			if (const UStaticMeshComponent* Gun = F->GetHeldGun(State->Protagonist); !Gun || Gun->GetRelativeLocation().Z > 100.0)
			{
				Fail(TEXT("equip_and_aim: the held gun did not come down with the crouch"));
			}
			F->ToggleCrouch();
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		const colony::ThingView* Me = Snapshot(State->Protagonist);
		if (!Me || !Me->weapon.valid())
		{
			Fail(TEXT("equip_and_aim: the protagonist holds no gun after E"));
			return;
		}
		if (Me->stance != colony::Stance::Standing)
		{
			Fail(TEXT("equip_and_aim: the protagonist did not stand up again"));
		}
		const UStaticMeshComponent* Gun = F->GetHeldGun(State->Protagonist);
		if (!Gun || !Gun->IsVisible() || Gun->GetRelativeLocation().Z < 100.0)
		{
			Fail(TEXT("equip_and_aim: no placeholder gun at the protagonist's chest"));
		}
		if (F->GetHeldGuns() < 1)
		{
			Fail(TEXT("equip_and_aim: no held guns drawn"));
		}
		FVector Point;
		colony::Entity Looked = colony::kNoEntity;
		if (!F->ComputeAimPoint(Point, &Looked))
		{
			Fail(TEXT("equip_and_aim: the crosshair gave no aim point"));
			return;
		}
		// The crosshair must be on a pawn's body: the middle pawn, or a colonist who happens to stand in the line of sight.
		FVector Chest;
		const bool bFound = Looked != colony::kNoEntity && B->GetBlendedLocation(Looked, Chest);
		const double Gap = bFound ? FVector::Dist(Point, Chest + FVector(0.0, 0.0, 100.0)) : 1.0e9;
		if (!bFound || Gap > 130.0)
		{
			Fail(FString::Printf(TEXT("equip_and_aim: the crosshair found %s (%.0f cm from the middle of that pawn)"), bFound ? TEXT("a pawn") : TEXT("no pawn"), Gap));
		}
		Notes.Add(FString::Printf(TEXT("equip_and_aim: E %s; crosshair on %s"), State->bUsedEquipKey ? TEXT("picked up the gun") : TEXT("found none (command used)"),
			Looked == State->Dummy[1] ? TEXT("the middle pawn") : TEXT("another pawn in the line of sight")));
	});

	// 3. Hold the trigger on the middle pawn: orders go to the sim, shots, flashes, tracers, impacts, wounds.
	AddPhase(TEXT("fire_at_pawn"), [=, this]()
	{
		MarkBaselines();
		State->Target = State->Dummy[1];
		State->bReloadTried = false;
		State->bReloadSeen = false;
	}, 0.3f, 14.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		State->Target = FightSmokeAlive(*B, State->Target) ? State->Target : PickTarget();
		if (State->Target == colony::kNoEntity)
		{
			return;
		}
		AimAt(State->Target);
		// R once, when the gun is ready and the magazine is part empty; every other frame is the trigger.
		const bool bReady = B->GetSim()->combat().shotReadyIn(State->Protagonist) == 0;
		if (!State->bReloadTried && bReady && F->GetShotsSeen() - State->ShotsAtStart >= 2 && F->Reload())
		{
			State->bReloadTried = true;
		}
		else
		{
			F->Fire();
		}
		State->MaxTracers = FMath::Max(State->MaxTracers, F->GetActiveTracers());
		State->MaxParticles = FMath::Max(State->MaxParticles, F->GetActiveParticles());
		const colony::ThingView* Me = Snapshot(State->Protagonist);
		State->bReloadSeen = State->bReloadSeen || (Me && Me->reloading);
	}, [=, this]()
	{
		UFightSubsystem* F = Fight();
		const int32 Shots = F->GetShotsSeen() - State->ShotsAtStart;
		const int32 PawnHits = F->GetPawnHitsSeen() - State->PawnHitsAtStart;
		if (Shots < 3)
		{
			Fail(FString::Printf(TEXT("fire_at_pawn: only %d shots in 14 s of holding the trigger"), Shots));
		}
		if (F->GetFireOrders() - State->FireOrdersAtStart < Shots)
		{
			Fail(TEXT("fire_at_pawn: fewer fire orders than shots"));
		}
		if (F->GetFlashes() - State->FlashesAtStart != Shots)
		{
			Fail(FString::Printf(TEXT("fire_at_pawn: %d muzzle flashes for %d shots"), F->GetFlashes() - State->FlashesAtStart, Shots));
		}
		if (F->GetTracersSpawned() - State->TracersAtStart != Shots)
		{
			Fail(FString::Printf(TEXT("fire_at_pawn: %d tracers for %d shots"), F->GetTracersSpawned() - State->TracersAtStart, Shots));
		}
		if (F->GetTracersDropped() > 0)
		{
			Fail(FString::Printf(TEXT("fire_at_pawn: %d tracers were given up on"), F->GetTracersDropped()));
		}
		if (F->GetImpacts() - State->ImpactsAtStart < 1)
		{
			Fail(TEXT("fire_at_pawn: no bullet made an impact"));
		}
		if (PawnHits < 1)
		{
			Fail(TEXT("fire_at_pawn: no shot hit a pawn that was aimed at from 12 m"));
		}
		else
		{
			if (F->GetHitReactions() - State->ReactionsAtStart < 1)
			{
				Fail(TEXT("fire_at_pawn: a pawn was hit and nobody flinched"));
			}
			if (F->GetHitMarkers() - State->MarkersAtStart < 1)
			{
				Fail(TEXT("fire_at_pawn: the player hit a pawn and got no hit marker"));
			}
			if (F->IsDecalMaterialMissing())
			{
				Fail(TEXT("fire_at_pawn: game/Content/Rim3D/Generated/Fights/M_WoundDecal is missing (run the FightMaterials commandlet, docs/design/smoke-tests.md)"));
			}
			else if (F->GetBodyDecals() - State->BodyDecalsAtStart < 1 || F->GetGroundDecals() - State->GroundDecalsAtStart < 1)
			{
				Fail(FString::Printf(TEXT("fire_at_pawn: %d wound decals and %d blood decals for %d pawn hits"), F->GetBodyDecals() - State->BodyDecalsAtStart,
					F->GetGroundDecals() - State->GroundDecalsAtStart, PawnHits));
			}
		}
		if (!State->bReloadSeen)
		{
			Fail(TEXT("fire_at_pawn: R (reload) never showed as reloading in the snapshot"));
		}
		if (F->GetHitboxChecks() < 1)
		{
			Fail(TEXT("fire_at_pawn: no hit was checked against the hitboxes"));
		}
		if (F->GetMaxHitboxMissCm() > 5.0f)
		{
			Fail(FString::Printf(TEXT("fire_at_pawn: a hit was %.1f cm from the pawn's hitboxes"), F->GetMaxHitboxMissCm()));
		}
		// The shooter turned to face its target (the sim's facing, drawn).
		if (const ASimPawnActor* Actor = Pawns()->FindPawn(State->Protagonist); Actor && FMath::Abs(FRotator::NormalizeAxis(Actor->GetActorRotation().Yaw)) > 45.0f)
		{
			Fail(FString::Printf(TEXT("fire_at_pawn: the shooter's yaw is %.0f after shooting east"), Actor->GetActorRotation().Yaw));
		}
		Notes.Add(FString::Printf(TEXT("fire_at_pawn: %d shots, %d hits on pawns, %d impacts, %d flinches, %d wound decals, %d blood decals, hits within %.1f cm of the hitboxes, at most %d tracers and %d particles at once"),
			Shots, PawnHits, F->GetImpacts() - State->ImpactsAtStart, F->GetHitReactions() - State->ReactionsAtStart, F->GetBodyDecals() - State->BodyDecalsAtStart,
			F->GetGroundDecals() - State->GroundDecalsAtStart, F->GetMaxHitboxMissCm(), State->MaxTracers, State->MaxParticles));
	});

	// 4. One shot, frozen while its tracer is in the air: the screenshot shows the muzzle flash, the light and the streak.
	AddPhase(TEXT("frozen_shot"), [=, this]()
	{
		State->Target = PickTarget();
		State->bFired = false;
		State->bPaused = false;
		if (State->Target == colony::kNoEntity)
		{
			Fail(TEXT("frozen_shot: all three pawns are down already"));
			return;
		}
		FVector Me, Them;
		Bridge()->GetBlendedLocation(State->Protagonist, Me);
		Bridge()->GetBlendedLocation(State->Target, Them);
		// Over the shooter's right shoulder, looking along the shot.
		const FVector Along = (Them - Me).GetSafeNormal2D();
		const FVector Eye = Me - Along * 220.0 + FVector::CrossProduct(FVector::UpVector, Along) * 130.0 + FVector(0.0, 0.0, 170.0);
		ViewFrom(Eye, (Me + Along * 700.0 + FVector(0.0, 0.0, 125.0) - Eye).Rotation());
	}, 8.0f, 1.5f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		if (State->Target == colony::kNoEntity || State->bPaused)
		{
			return;
		}
		AimAt(State->Target);
		if (!State->bFired)
		{
			State->bFired = F->Fire();
			return;
		}
		// Once the streak is well on its way and the flash is still up, stop the clock.
		if (F->GetActiveTracers() > 0 && F->GetParticlesOfKind(EFightParticle::Flash) > 0 && F->GetTracers()[0].Distance > 400.0f)
		{
			State->bPaused = true;
			State->TracersAtPause = F->GetActiveTracers();
			State->ParticlesAtPause = F->GetActiveParticles();
			State->TracerDistanceAtPause = F->GetTracers()[0].Distance;
			B->SetSpeed(0);
			State->PausedAt = PhaseTime;
		}
	}, [=, this]()
	{
		UFightSubsystem* F = Fight();
		if (State->Target == colony::kNoEntity)
		{
			return;
		}
		if (!State->bFired)
		{
			Fail(TEXT("frozen_shot: the shot was never fired"));
		}
		else if (!State->bPaused)
		{
			Fail(TEXT("frozen_shot: no moment with a tracer in the air and the flash still up (a muzzle flash lasts a tenth of a second)"));
		}
		else
		{
			if (F->GetActiveTracers() != State->TracersAtPause || F->GetTracers()[0].Distance != State->TracerDistanceAtPause)
			{
				Fail(TEXT("frozen_shot: the tracer moved while the game was paused"));
			}
			Notes.Add(FString::Printf(TEXT("frozen_shot: paused with %d tracer(s) and %d particles up, the first tracer %.0f cm out"), State->TracersAtPause, State->ParticlesAtPause, State->TracerDistanceAtPause));
		}
		Bridge()->SetSpeed(1);
	});

	// 5. Shoot until a bullet hits a pawn, frozen a moment later: blood, the flinch, the wound.
	AddPhase(TEXT("frozen_hit"), [=, this]()
	{
		State->Target = PickTarget();
		State->bFired = false;
		State->bPaused = false;
		State->PausedAt = 0.0;
		State->PawnImpactsAtStart = Fight()->GetImpactsOfKind(colony::ProjectileHitKind::Pawn);
		if (State->Target == colony::kNoEntity)
		{
			Fail(TEXT("frozen_hit: all three pawns are down already"));
			return;
		}
		FVector Me, Them;
		Bridge()->GetBlendedLocation(State->Protagonist, Me);
		Bridge()->GetBlendedLocation(State->Target, Them);
		// Close to the target, a little to the side and in front of the shooter's line.
		const FVector Along = (Them - Me).GetSafeNormal2D();
		const FVector Eye = Them - Along * 260.0 + FVector::CrossProduct(FVector::UpVector, Along) * 190.0 + FVector(0.0, 0.0, 150.0);
		ViewFrom(Eye, (Them + FVector(0.0, 0.0, 105.0) - Eye).Rotation());
	}, 14.0f, 1.5f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		if (State->Target == colony::kNoEntity || State->bPaused)
		{
			return;
		}
		State->Target = FightSmokeAlive(*B, State->Target) ? State->Target : PickTarget();
		if (State->Target == colony::kNoEntity)
		{
			return;
		}
		AimAt(State->Target);
		if (F->GetImpactsOfKind(colony::ProjectileHitKind::Pawn) > State->PawnImpactsAtStart && State->PausedAt == 0.0)
		{
			State->PausedAt = PhaseTime;  // the moment of the hit; stopped a tenth of a second later
		}
		if (State->PausedAt > 0.0 && PhaseTime > State->PausedAt + 0.1 && F->GetParticlesOfKind(EFightParticle::Blood) > 0)
		{
			State->bPaused = true;
			State->BloodAtPause = F->GetParticlesOfKind(EFightParticle::Blood);
			if (const ASimPawnActor* Actor = Pawns()->FindPawn(State->Target))
			{
				State->bLeanAtPause = Actor->IsReacting();
			}
			B->SetSpeed(0);
			State->PausedAt = PhaseTime;
			return;
		}
		F->Fire();
	}, [=, this]()
	{
		if (State->Target == colony::kNoEntity)
		{
			return;
		}
		if (!State->bPaused)
		{
			Fail(TEXT("frozen_hit: no shot hit a pawn in 14 s"));
		}
		else
		{
			if (!State->bLeanAtPause)
			{
				Fail(TEXT("frozen_hit: the pawn that was hit is not flinching"));
			}
			Notes.Add(FString::Printf(TEXT("frozen_hit: paused %d blood particles in the air, a pawn flinching"), State->BloodAtPause));
		}
		Bridge()->SetSpeed(1);
	});

	// 5b. What the wounds look like: from the shooter's side, close to the pawn that was hit last.
	AddPhase(TEXT("wounds"), [=, this]()
	{
		UFightSubsystem* F = Fight();
		colony::Entity Hit = colony::kNoEntity;
		for (int32 I = F->GetHitLog().Num() - 1; I >= 0 && Hit == colony::kNoEntity; --I)
		{
			Hit = F->GetHitLog()[I].Pawn;
		}
		FVector Me, Them;
		if (Hit == colony::kNoEntity || !Bridge()->GetBlendedLocation(Hit, Them) || !Bridge()->GetBlendedLocation(State->Protagonist, Me))
		{
			Fail(TEXT("wounds: nobody has been hit yet"));
			return;
		}
		const FVector Along = (Them - Me).GetSafeNormal2D();
		const FVector Eye = Them - Along * 230.0 + FVector::CrossProduct(FVector::UpVector, Along) * 60.0 + FVector(0.0, 0.0, 135.0);
		ViewFrom(Eye, (Them + FVector(0.0, 0.0, 80.0) - Eye).Rotation());
	}, 1.0f, 1.5f, nullptr, [=, this]()
	{
		if (Fight()->GetLiveDecals() < 2)
		{
			Fail(FString::Printf(TEXT("wounds: only %d decals alive after a pawn was hit"), Fight()->GetLiveDecals()));
		}
		Notes.Add(FString::Printf(TEXT("wounds: %d decals alive (%d on bodies, %d on the ground so far)"), Fight()->GetLiveDecals(), Fight()->GetBodyDecals(), Fight()->GetGroundDecals()));
	});

	// 6. The hitbox debug view over the pawns, from the side.
	AddPhase(TEXT("hitbox_view"), [=, this]()
	{
		State->Target = PickTarget();
		if (State->Target == colony::kNoEntity)
		{
			Fail(TEXT("hitbox_view: all three pawns are down already"));
			return;
		}
		Fight()->SetShowHitboxes(true);
		FVector Them;
		Bridge()->GetBlendedLocation(State->Target, Them);
		const FVector Eye = Them + FVector(0.0, 380.0, 140.0);
		ViewFrom(Eye, (Them + FVector(0.0, 0.0, 100.0) - Eye).Rotation());
	}, 1.5f, 2.0f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		if (State->Target == colony::kNoEntity)
		{
			return;
		}
		if (F->GetHitboxesDrawn() < 12)
		{
			Fail(FString::Printf(TEXT("hitbox_view: only %d hitbox shapes drawn"), F->GetHitboxesDrawn()));
		}
		// What is drawn matches what the sim hits: the target's drawn body and its hitboxes cover the same place.
		std::vector<colony::HitboxView> Boxes;
		const ASimPawnActor* Actor = Pawns()->FindPawn(State->Target);
		if (!Actor || !B->GetSim()->combat().hitboxesOf(State->Target, Boxes) || Boxes.empty())
		{
			Fail(TEXT("hitbox_view: the target has no hitboxes or no drawn body"));
		}
		else
		{
			FBox Hit(ForceInit);
			for (const colony::HitboxView& H : Boxes)
			{
				const FVector A = B->GetWorldOrigin() + FVector(H.ax, H.ay, H.az) * 100.0;
				const double Reach = H.box ? FMath::Max3(H.bx, H.by, H.bz) * 100.0 : H.radius * 100.0;
				Hit += A + FVector(Reach);
				Hit += A - FVector(Reach);
				if (!H.box)
				{
					const FVector C = B->GetWorldOrigin() + FVector(H.bx, H.by, H.bz) * 100.0;
					Hit += C + FVector(Reach);
					Hit += C - FVector(Reach);
				}
			}
			const FBox Drawn = Actor->GetDecalMesh()->Bounds.GetBox();
			const double TopGap = FMath::Abs(Hit.Max.Z - Drawn.Max.Z);
			const double CentreGap = FVector::Dist2D(Hit.GetCenter(), Drawn.GetCenter());
			Notes.Add(FString::Printf(TEXT("hitbox_view: %d shapes drawn; hitboxes %.0f cm tall, drawn body bounds %.0f cm tall, tops %.0f cm apart, centres %.0f cm apart"), F->GetHitboxesDrawn(),
				Hit.GetSize().Z, Drawn.GetSize().Z, TopGap, CentreGap));
			if (CentreGap > 60.0)
			{
				Fail(FString::Printf(TEXT("hitbox_view: the hitboxes and the drawn body are %.0f cm apart"), CentreGap));
			}
			if (TopGap > 60.0)
			{
				Fail(FString::Printf(TEXT("hitbox_view: the top of the hitboxes and the top of the drawn body are %.0f cm apart"), TopGap));
			}
		}
		F->SetShowHitboxes(false);
	});

	// 7. A pawn goes down: it goes limp and falls to the ground.
	AddPhase(TEXT("ragdoll"), [=, this]()
	{
		State->Target = PickTarget();
		if (State->Target == colony::kNoEntity)
		{
			Fail(TEXT("ragdoll: all three pawns are down already"));
			return;
		}
		State->RagdollsAtStart = Fight()->GetRagdollsStarted();
		Bridge()->GetBlendedLocation(State->Target, State->DummyLocation);
		State->StartHeightCm = static_cast<float>(State->DummyLocation.Z);
		Bridge()->Submit(colony::DamagePawnCommand{State->Target, "Bullet", 400.0f, 0});
		const FVector Eye = State->DummyLocation + FVector(0.0, 320.0, 110.0);
		ViewFrom(Eye, (State->DummyLocation + FVector(0.0, 0.0, 40.0) - Eye).Rotation());
	}, 4.0f, 2.0f, nullptr, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		if (State->Target == colony::kNoEntity)
		{
			return;
		}
		const colony::ThingView* View = Snapshot(State->Target);
		ASimPawnActor* Actor = Pawns()->FindPawn(State->Target);
		if (!View || !Actor || (!View->downed && !View->dead))
		{
			Fail(TEXT("ragdoll: the sim does not have the pawn down after 400 damage to the torso"));
			return;
		}
		FVector Pelvis;
		if (!Actor->IsRagdoll() || !Actor->GetRagdollPelvis(Pelvis))
		{
			Fail(TEXT("ragdoll: a downed pawn is not a ragdoll"));
			return;
		}
		const double HeightAboveGround = Pelvis.Z - State->StartHeightCm;
		if (HeightAboveGround > 70.0 || HeightAboveGround < -30.0)
		{
			Fail(FString::Printf(TEXT("ragdoll: the pelvis is %.0f cm above the ground after falling for 6 s (standing it is about 95)"), HeightAboveGround));
		}
		if (FVector::Dist2D(Pelvis, State->DummyLocation) > 400.0)
		{
			Fail(FString::Printf(TEXT("ragdoll: the body slid %.0f cm from where the pawn fell"), FVector::Dist2D(Pelvis, State->DummyLocation)));
		}
		if (F->GetRagdollsStarted() - State->RagdollsAtStart < 1)
		{
			Fail(TEXT("ragdoll: no ragdoll counted"));
		}
		// Every pawn that is down is a ragdoll and every one that is up is not.
		for (const colony::ThingView& Each : B->GetCurrentSnapshot().things)
		{
			const ASimPawnActor* Other = Each.category == colony::ThingCategory::Pawn ? Pawns()->FindPawn(Each.id) : nullptr;
			if (Other && Other->IsRagdoll() != (Each.downed || Each.dead))
			{
				Fail(FString::Printf(TEXT("ragdoll: pawn #%u is %s in the sim but %s on screen"), static_cast<uint32>(entt::to_integral(Each.id)), (Each.downed || Each.dead) ? TEXT("down") : TEXT("up"),
					Other->IsRagdoll() ? TEXT("limp") : TEXT("on its feet")));
			}
		}
		Notes.Add(FString::Printf(TEXT("ragdoll: pelvis %.0f cm above the ground after 6 s, slid %.0f cm"), HeightAboveGround, FVector::Dist2D(Pelvis, State->DummyLocation)));
	});

	// 8. A firefight: four colonists against four raiders, in the open, from the side.
	AddPhase(TEXT("firefight"), [=, this]()
	{
		USimSubsystem* B = Bridge();
		// The protagonist stays under direct control (the view stays on the test camera) and takes no part.
		MarkBaselines();
		State->bFightSetUp = false;
		B->SetSpeed(1);
		// Rifles for everyone, and raiders 30 m east.
		const colony::CellPos Here{FMath::FloorToInt32(State->Start.X), FMath::FloorToInt32(State->Start.Y) + 10, 0};
		for (int32 I = 0; I < 7; ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Gun_AssaultRifle", colony::CellPos{Here.x - 3 + I, Here.y, 0}, 1});
		}
		for (int32 I = 0; I < 4; ++I)
		{
			B->Submit(colony::SpawnThingCommand{"Human", colony::CellPos{FMath::FloorToInt32(State->Start.X) + 30, FMath::FloorToInt32(State->Start.Y) - 6 + I * 4, 0}});
		}
		const FVector From = B->SimToWorld(static_cast<float>(State->Start.X), static_cast<float>(State->Start.Y), 0);
		const FVector To = B->SimToWorld(static_cast<float>(State->Start.X) + 30.0f, static_cast<float>(State->Start.Y), 0);
		SideCamera(From, To, 2600.0, 700.0);
	}, 1.0f, 16.0f, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		State->MaxTracers = FMath::Max(State->MaxTracers, F->GetActiveTracers());
		State->MaxParticles = FMath::Max(State->MaxParticles, F->GetActiveParticles());
		if (State->bFightSetUp || PhaseTime < 0.6f)
		{
			return;
		}
		State->bFightSetUp = true;
		// Who is who: the pawns that are neither colonists nor the three pawns of the earlier phases are the raiders.
		TArray<colony::Entity> Rifles = FightSmokeItems(*B, "Gun_AssaultRifle");
		TArray<colony::Entity> Colonists;
		for (const colony::ThingView& View : B->GetCurrentSnapshot().things)
		{
			if (View.category != colony::ThingCategory::Pawn || View.dead || View.downed)
			{
				continue;
			}
			if (B->GetSim()->colonistOf(View.id))
			{
				if (View.id != State->Protagonist)
				{
					Colonists.Add(View.id);
				}
			}
			else if (View.id != State->Dummy[0] && View.id != State->Dummy[1] && View.id != State->Dummy[2])
			{
				State->Raiders.Add(View.id);
			}
		}
		if (State->Raiders.Num() < 4 || Rifles.Num() < 7 || Colonists.Num() < 3)
		{
			Fail(FString::Printf(TEXT("firefight: %d raiders, %d rifles and %d colonists to set up the fight with"), State->Raiders.Num(), Rifles.Num(), Colonists.Num()));
			return;
		}
		int32 NextRifle = 0;
		for (const colony::Entity Raider : State->Raiders)
		{
			B->Submit(colony::EquipCommand{Raider, Rifles[NextRifle++]});
			B->Submit(colony::SetFactionCommand{Raider, 2});
		}
		for (int32 I = 0; I < 4 && I < Colonists.Num(); ++I)
		{
			State->Defenders.Add(Colonists[I]);
			B->Submit(colony::EquipCommand{Colonists[I], Rifles[NextRifle < Rifles.Num() ? NextRifle++ : 0]});
			B->Submit(colony::DraftCommand{Colonists[I], true});
		}
	}, [=, this]()
	{
		USimSubsystem* B = Bridge();
		UFightSubsystem* F = Fight();
		const int32 Shots = F->GetShotsSeen() - State->ShotsAtStart;
		const int32 Hits = F->GetHitsSeen() - State->HitsAtStart;
		const int32 PawnHits = F->GetPawnHitsSeen() - State->PawnHitsAtStart;
		if (!State->bFightSetUp)
		{
			return;
		}
		if (Shots < 12)
		{
			Fail(FString::Printf(TEXT("firefight: only %d shots in a firefight of 4 against 4"), Shots));
		}
		if (F->GetFlashes() - State->FlashesAtStart != Shots || F->GetTracersSpawned() - State->TracersAtStart > Shots)
		{
			Fail(FString::Printf(TEXT("firefight: %d shots, %d flashes, %d tracers"), Shots, F->GetFlashes() - State->FlashesAtStart, F->GetTracersSpawned() - State->TracersAtStart));
		}
		if (F->GetTracersDropped() > 2)
		{
			Fail(FString::Printf(TEXT("firefight: %d tracers were given up on"), F->GetTracersDropped()));
		}
		if (State->MaxTracers >= 400)
		{
			Fail(TEXT("firefight: the tracer limit was reached"));
		}
		if (PawnHits < 1)
		{
			Fail(TEXT("firefight: nobody was hit"));
		}
		if (F->GetMaxHitboxMissCm() > 5.0f)
		{
			Fail(FString::Printf(TEXT("firefight: a hit was %.1f cm from the pawn's hitboxes"), F->GetMaxHitboxMissCm()));
		}
		if (F->GetImpacts() - State->ImpactsAtStart < 1)
		{
			Fail(TEXT("firefight: no impacts drawn"));
		}
		for (const colony::ThingView& Each : B->GetCurrentSnapshot().things)
		{
			const ASimPawnActor* Other = Each.category == colony::ThingCategory::Pawn ? Pawns()->FindPawn(Each.id) : nullptr;
			if (Other && Other->IsRagdoll() != (Each.downed || Each.dead))
			{
				Fail(TEXT("firefight: a pawn is limp but up, or on its feet but down"));
				break;
			}
		}
		int32 Down = 0;
		for (const colony::Entity Fighter : State->Raiders)
		{
			Down += FightSmokeAlive(*B, Fighter) ? 0 : 1;
		}
		for (const colony::Entity Fighter : State->Defenders)
		{
			Down += FightSmokeAlive(*B, Fighter) ? 0 : 1;
		}
		Notes.Add(FString::Printf(TEXT("firefight: %d shots, %d bullets stopped (%d on pawns, %d reactions), %d went down, %d ragdolls, %d wound decals, %d blood decals (%d live), at most %d tracers and %d particles at once, %d lights, hits within %.1f cm of the hitboxes"),
			Shots, Hits, PawnHits, F->GetHitReactions() - State->ReactionsAtStart, Down, F->GetRagdollsStarted() - State->RagdollsAtStart, F->GetBodyDecals() - State->BodyDecalsAtStart,
			F->GetGroundDecals() - State->GroundDecalsAtStart, F->GetLiveDecals(), State->MaxTracers, State->MaxParticles, 6, F->GetMaxHitboxMissCm()));
		Notes.Add(FString::Printf(TEXT("hud frames drawn: %d (direct mode only)"), F->GetHudFramesDrawn()));
	});
}
